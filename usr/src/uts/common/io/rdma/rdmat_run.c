/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * The work of rdmat: objects for a session, QP connection, the operations
 * and teardown.  Every value from the ioctl is checked here before it
 * sizes or indexes anything; the remote address and rkey of a WRITE or
 * READ are passed to the device as given, since rejecting them is the
 * peer's job and the point of some tests.
 *
 * Each QP has a buffer of RDMAT_CHUNK sized DMA buffers, and two fast
 * registration MRs over all of it: one for local access and one that the
 * peer may use, with the access and key of the last REG.  SEND and RECV
 * may instead use the local DMA lkey within one chunk.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/random.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/systm.h>

#include "rdmat_impl.h"

/* A wait sleeps in slices this long so it notices signals and teardown. */
#define	RDMAT_SLICE_US		(100 * 1000)
/* Empty polls a DIRECT wait spins before it sleeps one tick. */
#define	RDMAT_SPIN		2000
#define	RDMAT_LAT_SAMPLES	(1U << 20)

static uint8_t
rdmat_pattern(uint64_t seed, uint64_t off)
{
	uint64_t x = seed ^ ((off >> 3) * 0x9e3779b97f4a7c15ULL);

	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return ((uint8_t)(x >> ((off & 7) * 8)));
}

static rdmat_qp_t *
rdmat_qp(rdmat_sess_t *ts, uint32_t idx)
{
	if (idx >= ts->ts_nqp)
		return (NULL);
	return (&ts->ts_qp[idx]);
}

/*
 * Completions.
 */
static void
rdmat_wc_err(rdmat_qp_t *tq, const struct rdk_wc *wc)
{
	ASSERT(MUTEX_HELD(&tq->tq_lock));
	if (tq->tq_errors++ == 0) {
		tq->tq_err_status = wc->status;
		tq->tq_err_opcode = wc->opcode;
		tq->tq_err_vendor = wc->vendor_err;
	}
}

static void
rdmat_send_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	rdmat_qp_t *tq = cq->cq_context;

	mutex_enter(&tq->tq_lock);
	if (wc->status != RDK_WC_SUCCESS)
		rdmat_wc_err(tq, wc);
	else
		tq->tq_send_done++;
	tq->tq_last_ns = gethrtime();
	cv_broadcast(&tq->tq_cv);
	mutex_exit(&tq->tq_lock);
}

static void
rdmat_recv_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	rdmat_qp_t *tq = cq->cq_context;
	uint32_t want = tq->tq_expect_len;

	mutex_enter(&tq->tq_lock);
	if (wc->status != RDK_WC_SUCCESS) {
		rdmat_wc_err(tq, wc);
	} else {
		tq->tq_recv_done++;
		tq->tq_bytes += wc->byte_len;
		tq->tq_last_len = wc->byte_len;
		tq->tq_wc_flags = (uint32_t)wc->wc_flags;
		if ((wc->wc_flags & RDK_WC_WITH_INVALIDATE) != 0) {
			tq->tq_inv_rkey = wc->ex.invalidate_rkey;
			if (tq->tq_rmr != NULL &&
			    wc->ex.invalidate_rkey == tq->tq_rmr->rkey)
				tq->tq_rmr_bound = B_FALSE;
		}
		if (want != 0 && wc->byte_len != want)
			tq->tq_len_mismatch++;
	}
	tq->tq_last_ns = gethrtime();
	cv_broadcast(&tq->tq_cv);
	mutex_exit(&tq->tq_lock);
}

static void
rdmat_reg_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	rdmat_qp_t *tq = cq->cq_context;

	mutex_enter(&tq->tq_lock);
	if (wc->status != RDK_WC_SUCCESS)
		rdmat_wc_err(tq, wc);
	else
		tq->tq_reg_done++;
	cv_broadcast(&tq->tq_cv);
	mutex_exit(&tq->tq_lock);
}

static void
rdmat_qp_event(struct rdk_event *ev, void *arg)
{
	rdmat_qp_t *tq = arg;

	mutex_enter(&tq->tq_lock);
	tq->tq_events++;
	tq->tq_last_event = ev->event;
	cv_broadcast(&tq->tq_cv);
	mutex_exit(&tq->tq_lock);
}

static void
rdmat_reset_counts(rdmat_qp_t *tq, uint32_t expect_len)
{
	mutex_enter(&tq->tq_lock);
	tq->tq_send_done = tq->tq_recv_done = tq->tq_reg_done = 0;
	tq->tq_bytes = 0;
	tq->tq_errors = 0;
	tq->tq_err_status = tq->tq_err_opcode = tq->tq_err_vendor = 0;
	tq->tq_last_len = tq->tq_len_mismatch = tq->tq_wc_flags = 0;
	tq->tq_inv_rkey = 0;
	tq->tq_expect_len = expect_len;
	mutex_exit(&tq->tq_lock);
}

/*
 * Wait until *ctr reaches want.  Returns EIO once a completion failed,
 * EINTR on a signal, ETIMEDOUT past the deadline and ENXIO when the
 * session is being torn down.
 */
static int
rdmat_wait(rdmat_qp_t *tq, uint64_t *ctr, uint64_t want, hrtime_t deadline)
{
	rdmat_sess_t *ts = tq->tq_sess;
	uint_t spins = 0;
	int ret = 0;

	for (;;) {
		if (ts->ts_poll == RDMAT_POLL_DIRECT) {
			(void) rdk_process_cq_direct(tq->tq_scq, -1);
			(void) rdk_process_cq_direct(tq->tq_rcq, -1);
		}
		mutex_enter(&tq->tq_lock);
		if (tq->tq_errors != 0)
			ret = EIO;
		else if (*ctr >= want)
			ret = 0;
		else if (ts->ts_dying)
			ret = ENXIO;
		else if (gethrtime() >= deadline)
			ret = ETIMEDOUT;
		else
			ret = -1;
		if (ret >= 0) {
			mutex_exit(&tq->tq_lock);
			return (ret);
		}
		if (ts->ts_poll == RDMAT_POLL_DIRECT && ++spins < RDMAT_SPIN) {
			mutex_exit(&tq->tq_lock);
			continue;
		}
		spins = 0;
		if (cv_reltimedwait_sig(&tq->tq_cv, &tq->tq_lock,
		    ts->ts_poll == RDMAT_POLL_DIRECT ? 1 :
		    drv_usectohz(RDMAT_SLICE_US), TR_CLOCK_TICK) == 0) {
			mutex_exit(&tq->tq_lock);
			return (EINTR);
		}
		mutex_exit(&tq->tq_lock);
	}
}

/*
 * Objects.
 */
static int
rdmat_buf_alloc(rdmat_sess_t *ts, rdmat_qp_t *tq, uint64_t len)
{
	uint_t i;
	int ret;

	tq->tq_nchunks = (uint_t)(len / RDMAT_CHUNK);
	tq->tq_cookies = kmem_zalloc(sizeof (ddi_dma_cookie_t) *
	    tq->tq_nchunks, KM_SLEEP);
	for (i = 0; i < tq->tq_nchunks; i++) {
		ret = rdk_dma_buf_alloc(ts->ts_dev, RDMAT_CHUNK,
		    &tq->tq_chunks[i]);
		if (ret != 0)
			return (ret);
		tq->tq_cookies[i].dmac_laddress = tq->tq_chunks[i].rdb_pa;
		tq->tq_cookies[i].dmac_size = RDMAT_CHUNK;
	}
	tq->tq_len = len;
	return (0);
}

static void
rdmat_buf_free(rdmat_sess_t *ts, rdmat_qp_t *tq)
{
	uint_t i;

	for (i = 0; i < tq->tq_nchunks; i++)
		rdk_dma_buf_free(ts->ts_dev, &tq->tq_chunks[i]);
	if (tq->tq_cookies != NULL) {
		kmem_free(tq->tq_cookies,
		    sizeof (ddi_dma_cookie_t) * tq->tq_nchunks);
		tq->tq_cookies = NULL;
	}
	tq->tq_nchunks = 0;
}

static int
rdmat_qp_create(rdmat_sess_t *ts, rdmat_qp_t *tq, uint64_t len)
{
	struct rdk_device *dev = ts->ts_dev;
	struct rdk_qp_init_attr init;
	enum rdk_poll_context pc;
	uint32_t pages = (uint32_t)(len / PAGESIZE);
	int ret;

	mutex_init(&tq->tq_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&tq->tq_cv, NULL, CV_DRIVER, NULL);
	tq->tq_sess = ts;
	tq->tq_send_cqe.done = rdmat_send_done;
	tq->tq_recv_cqe.done = rdmat_recv_done;
	tq->tq_reg_cqe.done = rdmat_reg_done;
	(void) random_get_pseudo_bytes((uint8_t *)&tq->tq_psn,
	    sizeof (tq->tq_psn));
	tq->tq_psn &= 0xffffff;

	pc = ts->ts_poll == RDMAT_POLL_TASKQ ? RDK_POLL_TASKQ :
	    RDK_POLL_DIRECT;
	if ((ret = rdk_alloc_cq(dev, tq, (int)ts->ts_depth * 2 + 8, 0, pc,
	    &tq->tq_scq)) != 0)
		return (ret);
	if ((ret = rdk_alloc_cq(dev, tq, (int)ts->ts_depth + 8, 0, pc,
	    &tq->tq_rcq)) != 0)
		return (ret);

	bzero(&init, sizeof (init));
	init.event_handler = rdmat_qp_event;
	init.qp_context = tq;
	init.send_cq = tq->tq_scq;
	init.recv_cq = tq->tq_rcq;
	init.cap.max_send_wr = ts->ts_depth + 4;
	init.cap.max_recv_wr = ts->ts_depth + 1;
	init.cap.max_send_sge = 1;
	init.cap.max_recv_sge = 1;
	init.sq_sig_type = RDK_SIGNAL_REQ_WR;
	init.qp_type = ts->ts_qpt == RDMAT_QPT_UD ? RDK_QPT_UD : RDK_QPT_RC;
	init.port_num = 1;
	if ((ret = rdk_create_qp(ts->ts_pd, &init, &tq->tq_qp)) != 0)
		return (ret);

	if ((ret = rdmat_buf_alloc(ts, tq, len)) != 0)
		return (ret);
	if (ts->ts_qpt == RDMAT_QPT_RC) {
		if ((ret = rdk_alloc_mr(ts->ts_pd, RDK_MR_TYPE_MEM_REG, pages,
		    &tq->tq_lmr)) != 0)
			return (ret);
		if ((ret = rdk_alloc_mr(ts->ts_pd, RDK_MR_TYPE_MEM_REG, pages,
		    &tq->tq_rmr)) != 0)
			return (ret);
		tq->tq_rkey_next = rdk_inc_rkey(tq->tq_rmr->rkey);
	}
	return (0);
}

int
rdmat_setup(rdmat_sess_t *ts, rdmat_setup_t *rs)
{
	struct rdk_device *dev = ts->ts_dev;
	struct rdk_port_attr pa;
	rdk_gid_t gid;
	uint_t i;
	int ret;

	if ((rs->rs_qpt != RDMAT_QPT_RC && rs->rs_qpt != RDMAT_QPT_UD) ||
	    rs->rs_nqp == 0 || rs->rs_nqp > RDMAT_MAX_QPS ||
	    (rs->rs_poll != RDMAT_POLL_DIRECT &&
	    rs->rs_poll != RDMAT_POLL_TASKQ) ||
	    rs->rs_buf_len == 0 || rs->rs_buf_len > RDMAT_MAX_BUF ||
	    (rs->rs_buf_len % RDMAT_CHUNK) != 0 ||
	    rs->rs_depth == 0 || rs->rs_depth > RDMAT_MAX_DEPTH ||
	    rs->rs_depth + 4 > (uint32_t)dev->rd_attr.max_qp_wr ||
	    rs->rs_buf_len / PAGESIZE > dev->rd_attr.max_fast_reg_page_list_len)
		return (EINVAL);
	if ((ret = rdk_query_port(dev, 1, &pa)) != 0)
		return (ret);

	ts->ts_qpt = rs->rs_qpt;
	ts->ts_poll = rs->rs_poll;
	ts->ts_depth = rs->rs_depth;
	ts->ts_setup = B_TRUE;

	rdk_gid_from_ipv4(&gid, rs->rs_ipv4);
	if ((ret = rdk_add_gid(dev, 1, &gid, RDK_VLAN_NONE, pa.mac,
	    &ts->ts_gid_index)) != 0)
		goto fail;
	if ((ret = rdk_alloc_pd(dev, 0, &ts->ts_pd)) != 0)
		goto fail;
	for (i = 0; i < rs->rs_nqp; i++) {
		ts->ts_nqp = i + 1;
		ts->ts_qp[i].tq_idx = i;
		if ((ret = rdmat_qp_create(ts, &ts->ts_qp[i],
		    rs->rs_buf_len)) != 0)
			goto fail;
	}

	bcopy(gid.raw, rs->rs_gid, sizeof (rs->rs_gid));
	bcopy(pa.mac, rs->rs_mac, sizeof (rs->rs_mac));
	rs->rs_gid_index = ts->ts_gid_index;
	for (i = 0; i < ts->ts_nqp; i++) {
		rdmat_qp_t *tq = &ts->ts_qp[i];
		rdmat_qpinfo_t *qi = &rs->rs_qp[i];

		qi->rqi_qpn = tq->tq_qp->qp_num;
		qi->rqi_psn = tq->tq_psn;
		qi->rqi_qkey = RDMAT_QKEY;
		qi->rqi_len = tq->tq_len;
		if (tq->tq_rmr != NULL) {
			qi->rqi_rkey = tq->tq_rkey_next;
			qi->rqi_addr = tq->tq_cookies[0].dmac_laddress;
		}
	}
	return (0);

fail:
	rdmat_teardown(ts, B_TRUE);
	ts->ts_setup = B_FALSE;
	return (ret);
}

/* Invalidate the remote MR with a LOCAL_INV work request. */
static int
rdmat_local_inv(rdmat_qp_t *tq, hrtime_t deadline)
{
	struct rdk_send_wr wr;
	int ret;

	bzero(&wr, sizeof (wr));
	wr.wr_cqe = &tq->tq_reg_cqe;
	wr.opcode = RDK_WR_LOCAL_INV;
	wr.send_flags = RDK_SEND_SIGNALED;
	wr.ex.invalidate_rkey = tq->tq_rmr->rkey;
	rdmat_reset_counts(tq, 0);
	if ((ret = rdk_post_send(tq->tq_qp, &wr, NULL)) != 0)
		return (ret);
	if ((ret = rdmat_wait(tq, &tq->tq_reg_done, 1, deadline)) == 0)
		tq->tq_rmr_bound = B_FALSE;
	return (ret);
}

/*
 * Bind an MR over the whole buffer with a REG_MR work request.  A fast
 * register needs an MR that is not bound.
 */
static int
rdmat_reg(rdmat_qp_t *tq, struct rdk_mr *mr, int access, uint32_t key)
{
	struct rdk_reg_wr wr;
	uint64_t off = 0;
	int n, ret;

	rdk_update_fast_reg_key(mr, (uint8_t)key);
	n = rdk_map_mr_sg(mr, tq->tq_cookies, tq->tq_nchunks, &off, PAGESIZE);
	if (n != (int)tq->tq_nchunks || mr->length != tq->tq_len)
		return (n < 0 ? -n : EIO);

	bzero(&wr, sizeof (wr));
	wr.wr.wr_cqe = &tq->tq_reg_cqe;
	wr.wr.opcode = RDK_WR_REG_MR;
	wr.wr.send_flags = RDK_SEND_SIGNALED;
	wr.mr = mr;
	wr.key = mr->rkey;
	wr.access = access;
	rdmat_reset_counts(tq, 0);
	if ((ret = rdk_post_send(tq->tq_qp, &wr.wr, NULL)) != 0)
		return (ret);
	return (rdmat_wait(tq, &tq->tq_reg_done, 1,
	    gethrtime() + SEC2NSEC(10)));
}

static int
rdmat_modify(rdmat_qp_t *tq, struct rdk_qp_attr *a, int mask)
{
	return (rdk_modify_qp(tq->tq_qp, a, mask));
}

int
rdmat_connect(rdmat_sess_t *ts, rdmat_connect_t *rc)
{
	rdmat_qp_t *tq;
	struct rdk_qp_attr a;
	struct rdk_ah_attr ah;
	int ret, acc;

	if ((tq = rdmat_qp(ts, rc->rc_qp)) == NULL || tq->tq_connected ||
	    rc->rc_rqpn > 0xffffff || rc->rc_retry > 7 ||
	    rc->rc_rnr_retry > 7 ||
	    (rc->rc_path_mtu != 256 && rc->rc_path_mtu != 512 &&
	    rc->rc_path_mtu != 1024 && rc->rc_path_mtu != 2048 &&
	    rc->rc_path_mtu != 4096))
		return (EINVAL);

	bzero(&ah, sizeof (ah));
	ah.type = RDK_AH_ATTR_TYPE_ROCE;
	ah.ah_flags = RDK_AH_GRH;
	ah.port_num = 1;
	ah.grh.sgid_index = (uint8_t)ts->ts_gid_index;
	ah.grh.hop_limit = 64;
	bcopy(rc->rc_dgid, ah.grh.dgid.raw, sizeof (ah.grh.dgid.raw));
	bcopy(rc->rc_dmac, ah.roce.dmac, sizeof (ah.roce.dmac));

	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_INIT;
	a.pkey_index = 0;
	a.port_num = 1;
	if (ts->ts_qpt == RDMAT_QPT_RC) {
		a.qp_access_flags = RDK_ACCESS_LOCAL_WRITE |
		    RDK_ACCESS_REMOTE_WRITE | RDK_ACCESS_REMOTE_READ;
		ret = rdmat_modify(tq, &a, RDK_QP_STATE | RDK_QP_PKEY_INDEX |
		    RDK_QP_PORT | RDK_QP_ACCESS_FLAGS);
	} else {
		a.qkey = RDMAT_QKEY;
		ret = rdmat_modify(tq, &a, RDK_QP_STATE | RDK_QP_PKEY_INDEX |
		    RDK_QP_PORT | RDK_QP_QKEY);
	}
	if (ret != 0)
		return (ret);

	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_RTR;
	if (ts->ts_qpt == RDMAT_QPT_RC) {
		a.ah_attr = ah;
		a.path_mtu = rdk_mtu_int_to_enum((int)rc->rc_path_mtu);
		a.dest_qp_num = rc->rc_rqpn;
		a.rq_psn = rc->rc_rpsn & 0xffffff;
		a.max_dest_rd_atomic = 16;
		a.min_rnr_timer = 12;
		ret = rdmat_modify(tq, &a, RDK_QP_STATE | RDK_QP_AV |
		    RDK_QP_PATH_MTU | RDK_QP_DEST_QPN | RDK_QP_RQ_PSN |
		    RDK_QP_MAX_DEST_RD_ATOMIC | RDK_QP_MIN_RNR_TIMER);
	} else {
		ret = rdmat_modify(tq, &a, RDK_QP_STATE);
	}
	if (ret != 0)
		return (ret);

	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_RTS;
	a.sq_psn = tq->tq_psn;
	if (ts->ts_qpt == RDMAT_QPT_RC) {
		a.timeout = 14;
		a.retry_cnt = rc->rc_retry;
		a.rnr_retry = rc->rc_rnr_retry;
		a.max_rd_atomic = 16;
		ret = rdmat_modify(tq, &a, RDK_QP_STATE | RDK_QP_TIMEOUT |
		    RDK_QP_RETRY_CNT | RDK_QP_RNR_RETRY | RDK_QP_SQ_PSN |
		    RDK_QP_MAX_QP_RD_ATOMIC);
	} else {
		ret = rdmat_modify(tq, &a, RDK_QP_STATE | RDK_QP_SQ_PSN);
	}
	if (ret != 0)
		return (ret);

	if (ts->ts_qpt == RDMAT_QPT_UD) {
		if ((ret = rdk_create_ah(ts->ts_pd, &ah, &tq->tq_ah)) != 0)
			return (ret);
		tq->tq_rqpn = rc->rc_rqpn;
		tq->tq_rqkey = rc->rc_rqkey;
		tq->tq_connected = B_TRUE;
		return (0);
	}

	if ((ret = rdmat_reg(tq, tq->tq_lmr, RDK_ACCESS_LOCAL_WRITE,
	    rdk_inc_rkey(tq->tq_lmr->rkey))) != 0)
		return (ret);
	tq->tq_lmr_bound = B_TRUE;
	acc = RDK_ACCESS_LOCAL_WRITE | RDK_ACCESS_REMOTE_WRITE |
	    RDK_ACCESS_REMOTE_READ;
	if ((ret = rdmat_reg(tq, tq->tq_rmr, acc, tq->tq_rkey_next)) != 0)
		return (ret);
	tq->tq_rmr_bound = B_TRUE;
	tq->tq_rkey_next = rdk_inc_rkey(tq->tq_rmr->rkey);
	tq->tq_connected = B_TRUE;
	return (0);
}

/*
 * The local SGE for [off, off + len) of the buffer.  With the DMA lkey the
 * range must lie in one chunk.
 */
static int
rdmat_sge(rdmat_sess_t *ts, rdmat_qp_t *tq, uint64_t off, uint32_t len,
    boolean_t dma_lkey, struct rdk_sge *sge)
{
	uint64_t c;

	if (off > tq->tq_len || len > tq->tq_len - off)
		return (EINVAL);
	if (dma_lkey || tq->tq_lmr == NULL) {
		c = off / RDMAT_CHUNK;
		if ((off % RDMAT_CHUNK) + len > RDMAT_CHUNK)
			return (EINVAL);
		sge->addr = tq->tq_chunks[c].rdb_pa + (off % RDMAT_CHUNK);
		sge->lkey = ts->ts_pd->local_dma_lkey;
	} else {
		if (!tq->tq_lmr_bound)
			return (ENXIO);
		sge->addr = tq->tq_lmr->iova + off;
		sge->lkey = tq->tq_lmr->lkey;
	}
	sge->length = len;
	return (0);
}

static int
rdmat_post_recvs(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    uint32_t n, uint64_t *posted)
{
	struct rdk_recv_wr wr;
	struct rdk_sge sge;
	uint32_t size = rr->rr_size;
	uint64_t off;
	int ret;

	if (ts->ts_qpt == RDMAT_QPT_UD)
		size += RDMAT_GRH_LEN;
	while (n-- > 0) {
		off = rr->rr_offset + (*posted % ts->ts_depth) * size;
		if ((ret = rdmat_sge(ts, tq, off, size,
		    (rr->rr_flags & RDMAT_F_DMA_LKEY) != 0, &sge)) != 0)
			return (ret);
		bzero(&wr, sizeof (wr));
		wr.wr_cqe = &tq->tq_recv_cqe;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		if ((ret = rdk_post_recv(tq->tq_qp, &wr, NULL)) != 0)
			return (ret);
		(*posted)++;
	}
	return (0);
}

/* Post one send-queue work request for index i of a run. */
static int
rdmat_post_one(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    uint64_t i, boolean_t signal)
{
	struct rdk_ud_wr ud;
	struct rdk_rdma_wr rw;
	struct rdk_send_wr *wr;
	struct rdk_sge sge;
	uint64_t off, roff;
	int ret;

	off = rr->rr_offset;
	roff = 0;
	if (rr->rr_op == RDMAT_OP_WRITE || rr->rr_op == RDMAT_OP_READ) {
		/* Successive transfers walk the local and remote windows. */
		uint64_t span = MAX(rr->rr_rlen / rr->rr_size, 1);

		roff = (i % span) * rr->rr_size;
		off = rr->rr_offset + ((i % ts->ts_depth) * rr->rr_size) %
		    MAX(tq->tq_len - rr->rr_offset - rr->rr_size + 1, 1);
	}
	if ((ret = rdmat_sge(ts, tq, off, rr->rr_size,
	    (rr->rr_flags & RDMAT_F_DMA_LKEY) != 0, &sge)) != 0)
		return (ret);

	bzero(&rw, sizeof (rw));
	bzero(&ud, sizeof (ud));
	wr = ts->ts_qpt == RDMAT_QPT_UD ? &ud.wr : &rw.wr;
	wr->wr_cqe = &tq->tq_send_cqe;
	wr->sg_list = &sge;
	wr->num_sge = rr->rr_size != 0 ? 1 : 0;
	wr->send_flags = signal ? RDK_SEND_SIGNALED : 0;
	switch (rr->rr_op) {
	case RDMAT_OP_SEND:
	case RDMAT_OP_PING:
	case RDMAT_OP_PONG:
		wr->opcode = RDK_WR_SEND;
		break;
	case RDMAT_OP_SEND_INV:
		wr->opcode = RDK_WR_SEND_WITH_INV;
		wr->ex.invalidate_rkey = rr->rr_rkey;
		break;
	case RDMAT_OP_WRITE:
		wr->opcode = RDK_WR_RDMA_WRITE;
		rw.remote_addr = rr->rr_raddr + roff;
		rw.rkey = rr->rr_rkey;
		break;
	case RDMAT_OP_READ:
		wr->opcode = RDK_WR_RDMA_READ;
		rw.remote_addr = rr->rr_raddr + roff;
		rw.rkey = rr->rr_rkey;
		break;
	default:
		return (EINVAL);
	}
	if (ts->ts_qpt == RDMAT_QPT_UD) {
		ud.ah = tq->tq_ah;
		ud.remote_qpn = tq->tq_rqpn;
		ud.remote_qkey = tq->tq_rqkey;
	}
	return (rdk_post_send(tq->tq_qp, wr, NULL));
}

/*
 * A windowed stream of count send-queue operations with at most depth in
 * flight.  Unsignaled runs signal every depth'th and the last.
 */
static int
rdmat_stream(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	boolean_t unsig = (rr->rr_flags & RDMAT_F_UNSIGNALED) != 0;
	uint64_t posted = 0, sig = 0, waited = 0;
	boolean_t signal;
	int ret;

	while (posted < rr->rr_count) {
		while (posted < rr->rr_count &&
		    posted - waited < rr->rr_depth) {
			signal = !unsig || posted + 1 == rr->rr_count ||
			    ((posted + 1) % rr->rr_depth) == 0;
			if ((ret = rdmat_post_one(ts, tq, rr, posted,
			    signal)) != 0)
				return (ret);
			posted++;
			if (signal)
				sig++;
		}
		/* Wait for the oldest signaled request to finish. */
		ret = rdmat_wait(tq, &tq->tq_send_done,
		    unsig ? sig : waited + 1, deadline);
		if (ret != 0)
			return (ret);
		waited = unsig ? posted : waited + 1;
	}
	return (rdmat_wait(tq, &tq->tq_send_done, sig, deadline));
}

/* Shell sort; the kernel has no public qsort(). */
static void
rdmat_sort(uint64_t *v, uint64_t n)
{
	uint64_t gap, i, j, x;

	for (gap = n / 2; gap > 0; gap /= 2) {
		for (i = gap; i < n; i++) {
			x = v[i];
			for (j = i; j >= gap && v[j - gap] > x; j -= gap)
				v[j] = v[j - gap];
			v[j] = x;
		}
	}
}

static int
rdmat_pingpong(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	uint64_t *lat = NULL, n, i, rposted = 0, sum = 0;
	boolean_t ping = rr->rr_op == RDMAT_OP_PING;
	hrtime_t t0;
	int ret;

	n = MIN(rr->rr_count, RDMAT_LAT_SAMPLES);
	if (ping)
		lat = kmem_zalloc(sizeof (uint64_t) * n, KM_SLEEP);
	if ((ret = rdmat_post_recvs(ts, tq, rr,
	    (uint32_t)MIN(rr->rr_count, rr->rr_depth), &rposted)) != 0)
		goto out;

	for (i = 0; i < rr->rr_count; i++) {
		t0 = gethrtime();
		if (!ping && (ret = rdmat_wait(tq, &tq->tq_recv_done, i + 1,
		    deadline)) != 0)
			break;
		if ((ret = rdmat_post_one(ts, tq, rr, i, B_TRUE)) != 0)
			break;
		if (ping && (ret = rdmat_wait(tq, &tq->tq_recv_done, i + 1,
		    deadline)) != 0)
			break;
		if ((ret = rdmat_wait(tq, &tq->tq_send_done, i + 1,
		    deadline)) != 0)
			break;
		if (ping && i < n) {
			lat[i] = (uint64_t)(gethrtime() - t0);
			sum += lat[i];
		}
		if (rposted < rr->rr_count && (ret = rdmat_post_recvs(ts, tq,
		    rr, 1, &rposted)) != 0)
			break;
	}
	if (ping && ret == 0 && n > 0) {
		rdmat_sort(lat, n);
		rr->rr_lat_min = lat[0];
		rr->rr_lat_max = lat[n - 1];
		rr->rr_lat_avg = sum / n;
		rr->rr_lat_p50 = lat[n / 2];
		rr->rr_lat_p99 = lat[MIN(n - 1, (n * 99) / 100)];
	}
out:
	if (lat != NULL)
		kmem_free(lat, sizeof (uint64_t) * n);
	return (ret);
}

int
rdmat_run(rdmat_sess_t *ts, rdmat_run_t *rr)
{
	struct rdk_qp_attr qa;
	struct rdk_qp_init_attr qi;
	rdmat_qp_t *tq;
	hrtime_t start, deadline;
	uint64_t posted = 0, len;
	uint32_t timeout;
	int ret;

	if ((tq = rdmat_qp(ts, rr->rr_qp)) == NULL || !tq->tq_connected)
		return (ENXIO);
	timeout = rr->rr_timeout_ms != 0 ? rr->rr_timeout_ms : 10000;
	if (timeout > RDMAT_MAX_TIMEOUT_MS || rr->rr_count > RDMAT_MAX_COUNT ||
	    rr->rr_depth > ts->ts_depth ||
	    (rr->rr_flags & ~(RDMAT_F_UNSIGNALED | RDMAT_F_DMA_LKEY)) != 0)
		return (EINVAL);
	if (rr->rr_depth == 0)
		rr->rr_depth = 1;

	/* The run's footprint in the local buffer. */
	len = (uint64_t)rr->rr_size;
	if (ts->ts_qpt == RDMAT_QPT_UD)
		len += RDMAT_GRH_LEN;
	if (rr->rr_op == RDMAT_OP_POST_RECV || rr->rr_op == RDMAT_OP_PING ||
	    rr->rr_op == RDMAT_OP_PONG)
		len *= ts->ts_depth;
	if (rr->rr_offset > tq->tq_len || len > tq->tq_len - rr->rr_offset)
		return (EINVAL);
	if (ts->ts_qpt == RDMAT_QPT_UD && rr->rr_op != RDMAT_OP_SEND &&
	    rr->rr_op != RDMAT_OP_POST_RECV &&
	    rr->rr_op != RDMAT_OP_WAIT_RECV && rr->rr_op != RDMAT_OP_PING &&
	    rr->rr_op != RDMAT_OP_PONG)
		return (ENOTSUP);

	if (rr->rr_op != RDMAT_OP_WAIT_RECV)
		rdmat_reset_counts(tq, rr->rr_op == RDMAT_OP_POST_RECV ?
		    rr->rr_size + (ts->ts_qpt == RDMAT_QPT_UD ?
		    RDMAT_GRH_LEN : 0) : 0);
	start = gethrtime();
	deadline = start + MSEC2NSEC(timeout);

	switch (rr->rr_op) {
	case RDMAT_OP_SEND:
	case RDMAT_OP_SEND_INV:
	case RDMAT_OP_WRITE:
	case RDMAT_OP_READ:
		ret = rdmat_stream(ts, tq, rr, deadline);
		break;
	case RDMAT_OP_POST_RECV:
		ret = rr->rr_count > ts->ts_depth ? EINVAL :
		    rdmat_post_recvs(ts, tq, rr, rr->rr_count, &posted);
		break;
	case RDMAT_OP_WAIT_RECV:
		ret = rdmat_wait(tq, &tq->tq_recv_done, rr->rr_count,
		    deadline);
		break;
	case RDMAT_OP_PING:
	case RDMAT_OP_PONG:
		ret = rdmat_pingpong(ts, tq, rr, deadline);
		break;
	case RDMAT_OP_REG:
		if ((rr->rr_access & ~(RDMAT_ACC_REMOTE_WRITE |
		    RDMAT_ACC_REMOTE_READ)) != 0) {
			ret = EINVAL;
			break;
		}
		if (tq->tq_rmr_bound &&
		    (ret = rdmat_local_inv(tq, deadline)) != 0)
			break;
		ret = rdmat_reg(tq, tq->tq_rmr, RDK_ACCESS_LOCAL_WRITE |
		    ((rr->rr_access & RDMAT_ACC_REMOTE_WRITE) != 0 ?
		    RDK_ACCESS_REMOTE_WRITE : 0) |
		    ((rr->rr_access & RDMAT_ACC_REMOTE_READ) != 0 ?
		    RDK_ACCESS_REMOTE_READ : 0), tq->tq_rkey_next);
		if (ret == 0) {
			tq->tq_rmr_bound = B_TRUE;
			rr->rr_new_rkey = tq->tq_rmr->rkey;
			tq->tq_rkey_next = rdk_inc_rkey(tq->tq_rmr->rkey);
		}
		break;
	case RDMAT_OP_LOCAL_INV:
		ret = rdmat_local_inv(tq, deadline);
		break;
	default:
		ret = EINVAL;
		break;
	}

	mutex_enter(&tq->tq_lock);
	rr->rr_done = tq->tq_send_done + tq->tq_recv_done + tq->tq_reg_done;
	rr->rr_bytes = rr->rr_op == RDMAT_OP_WAIT_RECV ? tq->tq_bytes :
	    tq->tq_send_done * rr->rr_size;
	/* An unsignaled stream completes every request it posted. */
	if (ret == 0 && (rr->rr_flags & RDMAT_F_UNSIGNALED) != 0) {
		rr->rr_done = rr->rr_count;
		rr->rr_bytes = (uint64_t)rr->rr_count * rr->rr_size;
	}
	rr->rr_ns = (uint64_t)(MAX(tq->tq_last_ns, start) - start);
	rr->rr_errors = tq->tq_errors;
	rr->rr_status = tq->tq_err_status;
	rr->rr_err_opcode = tq->tq_err_opcode;
	rr->rr_vendor_err = tq->tq_err_vendor;
	rr->rr_last_len = tq->tq_last_len;
	rr->rr_len_mismatch = tq->tq_len_mismatch;
	rr->rr_wc_flags = tq->tq_wc_flags;
	rr->rr_inv_rkey = tq->tq_inv_rkey;
	mutex_exit(&tq->tq_lock);
	if (rdk_query_qp(tq->tq_qp, &qa, RDK_QP_STATE, &qi) == 0)
		rr->rr_qp_state = qa.qp_state;
	return (ret);
}

int
rdmat_buf(rdmat_sess_t *ts, rdmat_buf_t *rb)
{
	rdmat_qp_t *tq;
	hrtime_t t0 = gethrtime();
	uint64_t i, off;
	uint8_t *p;

	if ((tq = rdmat_qp(ts, rb->rb_qp)) == NULL ||
	    rb->rb_offset > tq->tq_len ||
	    rb->rb_len > tq->tq_len - rb->rb_offset)
		return (EINVAL);
	rb->rb_mismatch = -1;
	for (i = 0; i < rb->rb_len; i++) {
		off = rb->rb_offset + i;
		p = (uint8_t *)tq->tq_chunks[off / RDMAT_CHUNK].rdb_va +
		    (off % RDMAT_CHUNK);
		switch (rb->rb_op) {
		case RDMAT_BUF_FILL:
			*p = rdmat_pattern(rb->rb_seed,
			    rb->rb_pattern_base + i);
			break;
		case RDMAT_BUF_ZERO:
			*p = 0;
			break;
		case RDMAT_BUF_VERIFY:
			if (*p != rdmat_pattern(rb->rb_seed,
			    rb->rb_pattern_base + i)) {
				rb->rb_mismatch = (int64_t)off;
				rb->rb_ns = (uint64_t)(gethrtime() - t0);
				return (0);
			}
			break;
		default:
			return (EINVAL);
		}
	}
	rb->rb_ns = (uint64_t)(gethrtime() - t0);
	return (0);
}

int
rdmat_query(rdmat_sess_t *ts, rdmat_query_t *rq)
{
	struct rdk_qp_attr qa;
	struct rdk_qp_init_attr qi;
	rdmat_qp_t *tq;
	int ret;

	if ((tq = rdmat_qp(ts, rq->rq_qp)) == NULL)
		return (EINVAL);
	if ((ret = rdk_query_qp(tq->tq_qp, &qa, RDK_QP_STATE, &qi)) != 0)
		return (ret);
	rq->rq_state = qa.qp_state;
	mutex_enter(&tq->tq_lock);
	rq->rq_events = tq->tq_events;
	rq->rq_last_event = tq->tq_last_event;
	mutex_exit(&tq->tq_lock);
	return (0);
}

/*
 * Destroy the session's objects, in flight or not.  A healthy close drains
 * the QP first; a device being removed is not waited on.
 */
void
rdmat_teardown(rdmat_sess_t *ts, boolean_t removing)
{
	uint_t i;

	for (i = 0; i < ts->ts_nqp; i++) {
		rdmat_qp_t *tq = &ts->ts_qp[i];

		if (tq->tq_qp != NULL) {
			if (!removing && tq->tq_connected)
				rdk_drain_qp(tq->tq_qp);
			rdk_destroy_qp(tq->tq_qp);
			tq->tq_qp = NULL;
		}
		if (tq->tq_ah != NULL) {
			rdk_destroy_ah(tq->tq_ah);
			tq->tq_ah = NULL;
		}
		if (tq->tq_lmr != NULL) {
			(void) rdk_dereg_mr(tq->tq_lmr);
			tq->tq_lmr = NULL;
		}
		if (tq->tq_rmr != NULL) {
			(void) rdk_dereg_mr(tq->tq_rmr);
			tq->tq_rmr = NULL;
		}
		if (tq->tq_scq != NULL) {
			rdk_free_cq(tq->tq_scq);
			tq->tq_scq = NULL;
		}
		if (tq->tq_rcq != NULL) {
			rdk_free_cq(tq->tq_rcq);
			tq->tq_rcq = NULL;
		}
		rdmat_buf_free(ts, tq);
		if (tq->tq_sess != NULL) {
			cv_destroy(&tq->tq_cv);
			mutex_destroy(&tq->tq_lock);
			tq->tq_sess = NULL;
		}
		tq->tq_connected = B_FALSE;
	}
	ts->ts_nqp = 0;
	if (ts->ts_pd != NULL) {
		rdk_dealloc_pd(ts->ts_pd);
		ts->ts_pd = NULL;
	}
}

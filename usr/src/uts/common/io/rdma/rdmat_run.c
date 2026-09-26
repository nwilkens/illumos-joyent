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
#include <sys/proc.h>

#include "rdmat_impl.h"

/* A wait sleeps in slices this long so it notices signals and teardown. */
#define	RDMAT_SLICE_US		(100 * 1000)
/* Empty polls a DIRECT wait spins before it sleeps one tick. */
#define	RDMAT_SPIN		2048
/* Completions a busy poll handles at once. */
#define	RDMAT_SPIN_BUDGET	64

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

/* Arm the CQs a RDMAT_F_ADAPT wait busy polled and give them back. */
void
rdmat_spin_end(rdmat_qp_t *tq)
{
	if (tq->tq_spoll)
		rdk_cq_poll_end(tq->tq_scq);
	if (tq->tq_rpoll)
		rdk_cq_poll_end(tq->tq_rcq);
	tq->tq_spoll = tq->tq_rpoll = B_FALSE;
}

/*
 * Busy poll for up to tq_spin_ns, keeping the CQs between calls while the
 * waits succeed.  Returns -1 once the time is up, with the CQs armed.
 */
static int
rdmat_wait_spin(rdmat_qp_t *tq, uint64_t *ctr, uint64_t want,
    hrtime_t deadline)
{
	rdmat_sess_t *ts = tq->tq_sess;
	hrtime_t end = gethrtime() + tq->tq_spin_ns;
	uint_t spins = 0;
	int ret;

	if (!tq->tq_spoll && !tq->tq_rpoll) {
		tq->tq_spoll = rdk_cq_poll_begin(tq->tq_scq);
		if (tq->tq_rcq != tq->tq_scq)
			tq->tq_rpoll = rdk_cq_poll_begin(tq->tq_rcq);
	}
	for (;;) {
		if (tq->tq_spoll)
			(void) rdk_cq_poll(tq->tq_scq, RDMAT_SPIN_BUDGET);
		if (tq->tq_rpoll)
			(void) rdk_cq_poll(tq->tq_rcq, RDMAT_SPIN_BUDGET);
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
		mutex_exit(&tq->tq_lock);
		if (ret >= 0)
			return (ret);
		if (gethrtime() >= end)
			break;
		if ((++spins & (RDMAT_SPIN - 1)) == 0 &&
		    ISSIG(curthread, JUSTLOOKING))
			return (EINTR);
	}
	rdmat_spin_end(tq);
	return (-1);
}

/*
 * Wait until *ctr reaches want.  Returns EIO once a completion failed,
 * EINTR on a signal, ETIMEDOUT past the deadline and ENXIO when the
 * session is being torn down.  A busy wait never sleeps.
 */
int
rdmat_wait(rdmat_qp_t *tq, uint64_t *ctr, uint64_t want, hrtime_t deadline)
{
	rdmat_sess_t *ts = tq->tq_sess;
	boolean_t direct = ts->ts_poll == RDMAT_POLL_DIRECT;
	uint_t spins = 0;
	int ret = 0;

	if (tq->tq_spin_ns != 0 &&
	    (ret = rdmat_wait_spin(tq, ctr, want, deadline)) >= 0)
		return (ret);
	for (;;) {
		if (direct) {
			(void) rdk_process_cq_direct(tq->tq_scq, -1);
			if (tq->tq_rcq != tq->tq_scq)
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
		if (direct && tq->tq_busy) {
			mutex_exit(&tq->tq_lock);
			if ((++spins & (RDMAT_SPIN - 1)) == 0 &&
			    ISSIG(curthread, JUSTLOOKING))
				return (EINTR);
			continue;
		}
		if (direct && ++spins < RDMAT_SPIN) {
			mutex_exit(&tq->tq_lock);
			continue;
		}
		spins = 0;
		if (cv_reltimedwait_sig(&tq->tq_cv, &tq->tq_lock,
		    direct ? 1 : drv_usectohz(RDMAT_SLICE_US),
		    TR_CLOCK_TICK) == 0) {
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
	if ((ret = rdk_alloc_cq(dev, tq, (int)ts->ts_depth * 2 + 8,
	    (int)ts->ts_comp_vector, pc, &tq->tq_scq)) != 0)
		return (ret);
	if ((ret = rdk_alloc_cq(dev, tq, (int)ts->ts_depth + 8,
	    (int)ts->ts_comp_vector, pc, &tq->tq_rcq)) != 0)
		return (ret);
	if ((ts->ts_mod_count != 0 || ts->ts_mod_us != 0) &&
	    ((ret = rdk_modify_cq(tq->tq_scq, ts->ts_mod_count,
	    ts->ts_mod_us)) != 0 || (ret = rdk_modify_cq(tq->tq_rcq,
	    ts->ts_mod_count, ts->ts_mod_us)) != 0))
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
	init.cap.max_inline_data = ts->ts_inline;
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
	    rs->rs_inline > dev->rd_attr.max_inline_data ||
	    rs->rs_comp_vector >= dev->rd_num_comp_vectors ||
	    ((rs->rs_mod_count != 0 || rs->rs_mod_us != 0) &&
	    rs->rs_poll != RDMAT_POLL_TASKQ) ||
	    rs->rs_buf_len / PAGESIZE > dev->rd_attr.max_fast_reg_page_list_len)
		return (EINVAL);
	if ((ret = rdk_query_port(dev, 1, &pa)) != 0)
		return (ret);

	ts->ts_qpt = rs->rs_qpt;
	ts->ts_poll = rs->rs_poll;
	ts->ts_depth = rs->rs_depth;
	ts->ts_inline = rs->rs_inline;
	ts->ts_comp_vector = rs->rs_comp_vector;
	ts->ts_mod_count = rs->rs_mod_count;
	ts->ts_mod_us = rs->rs_mod_us;
	ts->ts_setup = B_TRUE;

	rdk_gid_from_ipv4(&gid, rs->rs_ipv4);
	if ((ret = rdk_add_gid(dev, 1, &gid, RDK_VLAN_NONE, pa.mac,
	    &ts->ts_gid_index)) != 0)
		goto fail;
	ts->ts_gid_added = B_TRUE;
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
	uint32_t qacc = rc->rc_qp_access;
	int ret, acc;

	if ((tq = rdmat_qp(ts, rc->rc_qp)) == NULL || tq->tq_connected ||
	    rc->rc_rqpn > 0xffffff || rc->rc_retry > 7 ||
	    rc->rc_rnr_retry > 7 ||
	    (qacc != 0 && ((qacc & RDMAT_QPACC_SET) == 0 ||
	    (qacc & ~(RDMAT_QPACC_SET | RDMAT_QPACC_NO_IRD |
	    RDMAT_ACC_LOCAL_WRITE | RDMAT_ACC_REMOTE_WRITE |
	    RDMAT_ACC_REMOTE_READ)) != 0)) ||
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
		if (qacc != 0) {
			a.qp_access_flags =
			    ((qacc & RDMAT_ACC_LOCAL_WRITE) != 0 ?
			    RDK_ACCESS_LOCAL_WRITE : 0) |
			    ((qacc & RDMAT_ACC_REMOTE_WRITE) != 0 ?
			    RDK_ACCESS_REMOTE_WRITE : 0) |
			    ((qacc & RDMAT_ACC_REMOTE_READ) != 0 ?
			    RDK_ACCESS_REMOTE_READ : 0);
		}
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
		a.max_dest_rd_atomic =
		    (qacc & RDMAT_QPACC_NO_IRD) != 0 ? 0 : 16;
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

int
rdmat_run(rdmat_sess_t *ts, rdmat_run_t *rr)
{
	struct rdk_qp_attr qa;
	struct rdk_qp_init_attr qi;
	rdmat_qp_t *tq;
	hrtime_t start, deadline;
	uint64_t posted = 0, len;
	uint32_t timeout, op = rr->rr_op;
	boolean_t rw = op == RDMAT_OP_WRITE || op == RDMAT_OP_READ ||
	    op == RDMAT_OP_WRITE_PING || op == RDMAT_OP_WRITE_PONG;
	int ret;

	if ((tq = rdmat_qp(ts, rr->rr_qp)) == NULL || !tq->tq_connected)
		return (ENXIO);
	timeout = rr->rr_timeout_ms != 0 ? rr->rr_timeout_ms : 10000;
	if (timeout > RDMAT_MAX_TIMEOUT_MS || rr->rr_count > RDMAT_MAX_COUNT ||
	    rr->rr_depth > ts->ts_depth || rr->rr_batch > RDMAT_MAX_BATCH ||
	    rr->rr_signal > RDMAT_MAX_DEPTH || rr->rr_run_ms > timeout ||
	    rr->rr_spin_us > RDMAT_MAX_SPIN_US ||
	    (rr->rr_flags & ~(RDMAT_F_UNSIGNALED | RDMAT_F_DMA_LKEY |
	    RDMAT_F_INLINE | RDMAT_F_BUSY | RDMAT_F_LAT | RDMAT_F_ADAPT)) != 0)
		return (EINVAL);
	if ((rr->rr_flags & RDMAT_F_BUSY) != 0 &&
	    ts->ts_poll != RDMAT_POLL_DIRECT)
		return (EINVAL);
	if ((rr->rr_flags & RDMAT_F_ADAPT) != 0 &&
	    ts->ts_poll != RDMAT_POLL_TASKQ)
		return (EINVAL);
	if (rr->rr_depth == 0)
		rr->rr_depth = 1;
	if (rr->rr_batch == 0)
		rr->rr_batch = 1;
	/* The remote window is walked in rr_size steps. */
	if (rw && rr->rr_size == 0)
		return (EINVAL);
	if ((op == RDMAT_OP_PING || op == RDMAT_OP_PONG ||
	    op == RDMAT_OP_WRITE_PING || op == RDMAT_OP_WRITE_PONG) &&
	    rr->rr_count == 0)
		return (EINVAL);

	/* The run's footprint in the local buffer. */
	len = (uint64_t)rr->rr_size;
	if (ts->ts_qpt == RDMAT_QPT_UD)
		len += RDMAT_GRH_LEN;
	if (op == RDMAT_OP_POST_RECV || op == RDMAT_OP_PING ||
	    op == RDMAT_OP_PONG || op == RDMAT_OP_RECV_STREAM)
		len *= ts->ts_depth;
	if (op == RDMAT_OP_WRITE_PING || op == RDMAT_OP_WRITE_PONG)
		len = 2 * P2ROUNDUP(len, 64);
	if (rr->rr_offset >= tq->tq_len || len > tq->tq_len - rr->rr_offset)
		return (EINVAL);
	if ((op == RDMAT_OP_MR_ALLOC || op == RDMAT_OP_FRWR) &&
	    (rr->rr_size == 0 || (rr->rr_size % PAGESIZE) != 0 ||
	    rr->rr_offset != 0 || rr->rr_count == 0 ||
	    rr->rr_size / PAGESIZE >
	    ts->ts_dev->rd_attr.max_fast_reg_page_list_len))
		return (EINVAL);
	if (ts->ts_qpt == RDMAT_QPT_UD && op != RDMAT_OP_SEND &&
	    op != RDMAT_OP_POST_RECV && op != RDMAT_OP_WAIT_RECV &&
	    op != RDMAT_OP_RECV_STREAM && op != RDMAT_OP_PING &&
	    op != RDMAT_OP_PONG)
		return (ENOTSUP);

	if (op != RDMAT_OP_WAIT_RECV)
		rdmat_reset_counts(tq, op == RDMAT_OP_POST_RECV ||
		    op == RDMAT_OP_RECV_STREAM ? rr->rr_size +
		    (ts->ts_qpt == RDMAT_QPT_UD ? RDMAT_GRH_LEN : 0) : 0);
	tq->tq_busy = (rr->rr_flags & RDMAT_F_BUSY) != 0;
	tq->tq_spin_ns = (rr->rr_flags & RDMAT_F_ADAPT) != 0 ?
	    USEC2NSEC(MAX(rr->rr_spin_us, 1)) : 0;
	tq->tq_posted = tq->tq_post_calls = 0;
	start = gethrtime();
	deadline = start + MSEC2NSEC(timeout);

	switch (op) {
	case RDMAT_OP_WRITE:
	case RDMAT_OP_READ:
		if ((rr->rr_flags & RDMAT_F_LAT) != 0) {
			ret = rdmat_one_lat(ts, tq, rr, deadline);
			break;
		}
		/* FALLTHROUGH */
	case RDMAT_OP_SEND:
	case RDMAT_OP_SEND_INV:
		ret = rdmat_stream(ts, tq, rr, deadline);
		break;
	case RDMAT_OP_RECV_STREAM:
		ret = rdmat_recv_stream(ts, tq, rr, deadline);
		break;
	case RDMAT_OP_WRITE_PING:
	case RDMAT_OP_WRITE_PONG:
		ret = rdmat_write_pingpong(ts, tq, rr, deadline);
		break;
	case RDMAT_OP_MR_ALLOC:
	case RDMAT_OP_FRWR:
		ret = rdmat_mr_cost(ts, tq, rr, deadline);
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

	tq->tq_busy = B_FALSE;
	rdmat_spin_end(tq);
	tq->tq_spin_ns = 0;
	mutex_enter(&tq->tq_lock);
	rr->rr_done = tq->tq_send_done + tq->tq_recv_done + tq->tq_reg_done;
	rr->rr_bytes = op == RDMAT_OP_WAIT_RECV ||
	    op == RDMAT_OP_RECV_STREAM ? tq->tq_bytes :
	    tq->tq_send_done * rr->rr_size;
	rr->rr_posted = tq->tq_posted;
	rr->rr_post_calls = tq->tq_post_calls;
	/* A stream with unsignaled requests completes all it posted. */
	if (ret == 0 && (op == RDMAT_OP_WRITE || op == RDMAT_OP_READ ||
	    op == RDMAT_OP_SEND || op == RDMAT_OP_SEND_INV)) {
		rr->rr_done = tq->tq_posted;
		rr->rr_bytes = tq->tq_posted * rr->rr_size;
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
	if (ts->ts_gid_added) {
		(void) rdk_del_gid(ts->ts_dev, 1, ts->ts_gid_index);
		ts->ts_gid_added = B_FALSE;
	}
}

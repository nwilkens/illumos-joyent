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
 * RDMAT_OP_RW: transfers through rdk_rw, as a storage target makes them.
 * The local memory is a list of cookies laid out by rr_frag and rr_stride
 * in the QP's buffer, cut where a cookie would cross a chunk; the remote
 * window is cut into rr_nsegs segments of the peer's rkey.  Up to rr_depth
 * transfers are in flight, each with its own context and MRs.
 *
 * A run that ends with transfers in flight (an error, a signal, a device
 * being removed) cannot free their contexts and MRs while the QP may still
 * complete them, so it leaves them on the QP for teardown to free after the
 * QP is destroyed, and the QP takes no more runs.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "rdmat_impl.h"

typedef struct rdmat_rwx {
	struct rdk_cqe		rx_cqe;		/* the transfer */
	struct rdk_cqe		rx_scqe;	/* its SEND */
	struct rdmat_rw		*rx_rw;
	rdk_rw_ctx_t		*rx_ctx;
	struct rdk_mr		**rx_mrs;
	uint_t			rx_nmrs;
	uint_t			rx_owed;	/* tq_lock */
	struct rdk_send_wr	rx_send;
	struct rdk_sge		rx_send_sge;
} rdmat_rwx_t;

struct rdmat_rw {
	rdmat_qp_t		*rw_tq;
	uint_t			rw_nslots;
	rdmat_rwx_t		*rw_slots;
	ddi_dma_cookie_t	*rw_ck;
	uint_t			rw_nck;
	struct rdk_rw_seg	rw_segs[RDK_RW_MAX_SEGS];
	uint_t			rw_nsegs;
};

static void
rdmat_rw_err(rdmat_qp_t *tq, const struct rdk_wc *wc)
{
	ASSERT(MUTEX_HELD(&tq->tq_lock));
	if (tq->tq_errors++ == 0) {
		tq->tq_err_status = wc->status;
		tq->tq_err_opcode = wc->opcode;
		tq->tq_err_vendor = wc->vendor_err;
	}
}

/* A transfer is done when its data and its SEND, if any, completed. */
static void
rdmat_rw_complete(rdmat_rwx_t *rx, const struct rdk_wc *wc)
{
	rdmat_qp_t *tq = rx->rx_rw->rw_tq;

	mutex_enter(&tq->tq_lock);
	if (wc->status != RDK_WC_SUCCESS) {
		rdmat_rw_err(tq, wc);
	} else if (rx->rx_owed > 0 && --rx->rx_owed == 0) {
		tq->tq_send_done++;
		tq->tq_last_ns = gethrtime();
	}
	cv_broadcast(&tq->tq_cv);
	mutex_exit(&tq->tq_lock);
}

static void
rdmat_rw_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	_NOTE(ARGUNUSED(cq));
	rdmat_rw_complete((rdmat_rwx_t *)(void *)wc->wr_cqe, wc);
}

static void
rdmat_rw_send_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	rdmat_rwx_t *rx = (rdmat_rwx_t *)(void *)((char *)wc->wr_cqe -
	    offsetof(rdmat_rwx_t, rx_scqe));

	_NOTE(ARGUNUSED(cq));
	rdmat_rw_complete(rx, wc);
}

/* Free a run's state; the QP holds none of its work. */
void
rdmat_rw_free(struct rdmat_rw *rw)
{
	uint_t i, k;

	if (rw == NULL)
		return;
	for (i = 0; i < rw->rw_nslots; i++) {
		rdmat_rwx_t *rx = &rw->rw_slots[i];

		for (k = 0; k < rx->rx_nmrs; k++) {
			if (rx->rx_mrs[k] != NULL)
				(void) rdk_dereg_mr(rx->rx_mrs[k]);
		}
		if (rx->rx_mrs != NULL) {
			kmem_free(rx->rx_mrs,
			    sizeof (struct rdk_mr *) * rx->rx_nmrs);
		}
		if (rx->rx_ctx != NULL)
			rdk_rw_ctx_free(rx->rx_ctx);
	}
	if (rw->rw_slots != NULL)
		kmem_free(rw->rw_slots, sizeof (rdmat_rwx_t) * rw->rw_nslots);
	if (rw->rw_ck != NULL)
		kmem_free(rw->rw_ck, sizeof (ddi_dma_cookie_t) *
		    RDMAT_RW_MAX_CK);
	kmem_free(rw, sizeof (*rw));
}

/*
 * The local cookies of a transfer of len bytes at the run's offset, cut at
 * chunk ends.
 */
static int
rdmat_rw_cookies(rdmat_qp_t *tq, const rdmat_run_t *rr, struct rdmat_rw *rw)
{
	uint32_t frag = rr->rr_frag, stride = rr->rr_stride;
	uint64_t done = 0, off, n, c;

	if (frag == 0) {
		frag = rr->rr_size;
		stride = frag;
	}
	if (stride < frag || howmany((uint64_t)rr->rr_size, frag) >
	    RDMAT_RW_MAX_CK)
		return (EINVAL);
	rw->rw_nck = 0;
	while (done < rr->rr_size) {
		off = rr->rr_offset + (done / frag) * stride + done % frag;
		n = MIN(frag - done % frag, rr->rr_size - done);
		c = off / RDMAT_CHUNK;
		n = MIN(n, RDMAT_CHUNK - off % RDMAT_CHUNK);
		if (off >= tq->tq_len || n > tq->tq_len - off ||
		    rw->rw_nck == RDMAT_RW_MAX_CK)
			return (EINVAL);
		rw->rw_ck[rw->rw_nck].dmac_laddress =
		    tq->tq_chunks[c].rdb_pa + off % RDMAT_CHUNK;
		rw->rw_ck[rw->rw_nck].dmac_size = n;
		rw->rw_nck++;
		done += n;
	}
	return (0);
}

/* The contexts and MRs of rr_depth transfers of the run's shape. */
static int
rdmat_rw_slots(rdmat_sess_t *ts, rdmat_run_t *rr, struct rdmat_rw *rw)
{
	struct rdk_rw_attr a;
	struct rdk_rw_limits l;
	uint_t i, k;
	int ret;

	bzero(&a, sizeof (a));
	a.rwa_flags = (rr->rr_rw_flags & RDMAT_RW_MR) != 0 ? RDK_RW_F_MR : 0;
	a.rwa_max_sge = ts->ts_max_sge;
	a.rwa_mr_pages = rr->rr_mr_pages;
	a.rwa_max_cookies = rw->rw_nck;
	a.rwa_max_segs = rr->rr_nsegs;
	a.rwa_max_len = rr->rr_size;
	if ((ret = rdk_rw_limits(ts->ts_dev, &a, &l)) != 0)
		return (ret);
	rr->rr_rw_limit_wrs = l.rwl_wrs;
	rr->rr_rw_limit_mrs = l.rwl_mrs;
	/* Every transfer in flight, its SEND and a drain fit the queue. */
	if ((uint64_t)rr->rr_depth * (l.rwl_wrs +
	    ((rr->rr_rw_flags & RDMAT_RW_SEND) != 0 ? 1 : 0)) + 1 >
	    ts->ts_sq_depth || l.rwl_mrs > RDMAT_RW_MAX_CK)
		return (ENOSPC);

	rw->rw_nslots = rr->rr_depth;
	rw->rw_slots = kmem_zalloc(sizeof (rdmat_rwx_t) * rw->rw_nslots,
	    KM_SLEEP);
	for (i = 0; i < rw->rw_nslots; i++) {
		rdmat_rwx_t *rx = &rw->rw_slots[i];

		rx->rx_rw = rw;
		rx->rx_cqe.done = rdmat_rw_done;
		rx->rx_scqe.done = rdmat_rw_send_done;
		if ((ret = rdk_rw_ctx_alloc(ts->ts_dev, &a, &rx->rx_ctx)) != 0)
			return (ret);
		rx->rx_nmrs = l.rwl_mrs;
		if (rx->rx_nmrs == 0)
			continue;
		rx->rx_mrs = kmem_zalloc(sizeof (struct rdk_mr *) *
		    rx->rx_nmrs, KM_SLEEP);
		for (k = 0; k < rx->rx_nmrs; k++) {
			if ((ret = rdk_alloc_mr(ts->ts_pd, RDK_MR_TYPE_MEM_REG,
			    rr->rr_mr_pages, &rx->rx_mrs[k])) != 0)
				return (ret);
		}
	}
	return (0);
}

/* Build and post transfer i in slot rx. */
static int
rdmat_rw_post(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    struct rdmat_rw *rw, rdmat_rwx_t *rx, uint64_t i)
{
	enum rdk_rw_dir dir = (rr->rr_rw_flags & RDMAT_RW_READ) != 0 ?
	    RDK_RW_READ : RDK_RW_WRITE;
	boolean_t send = (rr->rr_rw_flags & RDMAT_RW_SEND) != 0;
	uint64_t span = MAX(rr->rr_rlen / rr->rr_size, 1);
	uint64_t raddr = rr->rr_raddr + (i % span) * rr->rr_size;
	uint32_t left = rr->rr_size, n;
	uint_t k;
	int ret;

	for (k = 0; k < rw->rw_nsegs; k++) {
		n = k + 1 == rw->rw_nsegs ? left : rr->rr_size / rw->rw_nsegs;
		rw->rw_segs[k].rs_addr = raddr;
		rw->rw_segs[k].rs_len = n;
		rw->rw_segs[k].rs_key = rr->rr_rkey;
		raddr += n;
		left -= n;
	}
	if ((ret = rdk_rw_init(rx->rx_ctx, tq->tq_qp, dir, rw->rw_ck,
	    rw->rw_nck, 0, rr->rr_size, rw->rw_segs, rw->rw_nsegs, rx->rx_mrs,
	    rx->rx_nmrs)) != 0)
		return (ret);
	rr->rr_rw_wrs = rdk_rw_nwr(rx->rx_ctx);
	rr->rr_rw_mrs = rdk_rw_nmr(rx->rx_ctx);

	if (send) {
		bzero(&rx->rx_send, sizeof (rx->rx_send));
		rx->rx_send.wr_cqe = &rx->rx_scqe;
		rx->rx_send.opcode = RDK_WR_SEND;
		rx->rx_send.send_flags = RDK_SEND_SIGNALED;
		rx->rx_send_sge.addr = tq->tq_chunks[0].rdb_pa;
		rx->rx_send_sge.length = RDMAT_RW_SEND_LEN;
		rx->rx_send_sge.lkey = ts->ts_pd->local_dma_lkey;
		rx->rx_send.sg_list = &rx->rx_send_sge;
		rx->rx_send.num_sge = 1;
		if ((rr->rr_rw_flags & RDMAT_RW_SEND_INV) != 0 &&
		    rdk_rw_send_inv(rx->rx_ctx, &rx->rx_send))
			rr->rr_rw_send_inv = 1;
	}
	mutex_enter(&tq->tq_lock);
	rx->rx_owed = send ? 2 : 1;
	mutex_exit(&tq->tq_lock);
	/* A failed post may have queued part of the chain: count it. */
	tq->tq_posted++;
	tq->tq_post_calls++;
	return (rdk_rw_post(rx->rx_ctx, &rx->rx_cqe,
	    send ? &rx->rx_send : NULL));
}

int
rdmat_rw_run(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	struct rdmat_rw *rw;
	hrtime_t stop = rr->rr_run_ms != 0 ? gethrtime() +
	    MSEC2NSEC(rr->rr_run_ms) : 0;
	uint64_t i;
	uint_t s;
	int ret, ret2;

	if (rr->rr_nsegs == 0)
		rr->rr_nsegs = 1;
	if (rr->rr_size == 0 || rr->rr_size > RDK_RW_MAX_LEN ||
	    rr->rr_count == 0 || rr->rr_nsegs > RDK_RW_MAX_SEGS ||
	    rr->rr_nsegs > rr->rr_size || rr->rr_depth == 0 ||
	    (rr->rr_rw_flags & ~(RDMAT_RW_READ | RDMAT_RW_MR |
	    RDMAT_RW_SEND | RDMAT_RW_SEND_INV)) != 0 ||
	    ((rr->rr_rw_flags & RDMAT_RW_SEND) != 0 &&
	    ((rr->rr_rw_flags & RDMAT_RW_READ) != 0 ||
	    tq->tq_len < RDMAT_RW_SEND_LEN)) ||
	    (rr->rr_rw_flags & (RDMAT_RW_SEND | RDMAT_RW_SEND_INV)) ==
	    RDMAT_RW_SEND_INV)
		return (EINVAL);
	rw = kmem_zalloc(sizeof (*rw), KM_SLEEP);
	rw->rw_tq = tq;
	rw->rw_ck = kmem_zalloc(sizeof (ddi_dma_cookie_t) * RDMAT_RW_MAX_CK,
	    KM_SLEEP);
	rw->rw_nsegs = rr->rr_nsegs;
	if ((ret = rdmat_rw_cookies(tq, rr, rw)) != 0 ||
	    (ret = rdmat_rw_slots(ts, rr, rw)) != 0)
		goto done;

	/* Transfer i takes slot i % depth once transfer i - depth is done. */
	for (i = 0; i < rr->rr_count; i++) {
		if (stop != 0 && gethrtime() >= stop)
			break;
		if (i >= rw->rw_nslots && (ret = rdmat_wait(tq,
		    &tq->tq_send_done, i - rw->rw_nslots + 1, deadline)) != 0)
			goto done;
		s = (uint_t)(i % rw->rw_nslots);
		if ((ret = rdmat_rw_post(ts, tq, rr, rw, &rw->rw_slots[s],
		    i)) != 0)
			goto done;
	}
	ret = rdmat_wait(tq, &tq->tq_send_done, tq->tq_posted, deadline);
done:
	mutex_enter(&tq->tq_lock);
	ret2 = tq->tq_send_done < tq->tq_posted;
	mutex_exit(&tq->tq_lock);
	if (ret2) {
		/* Work of this run may still complete; teardown frees it. */
		tq->tq_rw = rw;
		return (ret != 0 ? ret : EIO);
	}
	rdmat_rw_free(rw);
	return (ret);
}

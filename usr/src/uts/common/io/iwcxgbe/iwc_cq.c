/*
 * Copyright (c) 2009-2010 Chelsio, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Completion queues.  The polling and flush logic is adapted from Linux
 * cxgb4/cq.c under the OpenIB license.  Unlike Linux, every value a CQE
 * carries is checked before it indexes anything: the QP ID must name a QP
 * of this CQ, a send queue index must be inside the send queue, a receive
 * completion needs a posted receive, and a read response needs an
 * outstanding read.  A CQE that fails a check is dropped and counted.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/atomic.h>

#include "iwc.h"
#include "common/t4_regs.h"
#include "common/t4_regs_values.h"

static void
iwc_gts(iwc_cq_t *cq, uint32_t val)
{
	iwc_db_write(cq->cq_iwc, cq->cq_hw.bar2_va, SGE_UDB_GTS,
	    val | V_INGRESSQID(cq->cq_hw.bar2_qid));
}

static void
iwc_hwcq_consume(iwc_cq_t *cq)
{
	t4_cq_t *hw = &cq->cq_hw;

	hw->bits_type_ts = hw->queue[hw->cidx].bits_type_ts;
	if (++hw->cidx_inc == (hw->size >> 4) || hw->cidx_inc == M_CIDXINC) {
		iwc_gts(cq, V_SEINTARM(0) | V_CIDXINC(hw->cidx_inc) |
		    V_TIMERREG(7));
		hw->cidx_inc = 0;
	}
	if (++hw->cidx == hw->size) {
		hw->cidx = 0;
		hw->gen ^= 1;
	}
}

/* The next hardware CQE, if the hardware wrote one; EOVERFLOW on overrun. */
static int
iwc_next_hw_cqe(iwc_cq_t *cq, t4_cqe_t **cqep)
{
	t4_cq_t *hw = &cq->cq_hw;
	const uint16_t prev = hw->cidx == 0 ? hw->size - 1 : hw->cidx - 1;

	if (hw->queue[prev].bits_type_ts != hw->bits_type_ts) {
		hw->error = B_TRUE;
		return (EOVERFLOW);
	}
	if (!t4_valid_cqe(hw, &hw->queue[hw->cidx]))
		return (ENODATA);
	membar_consumer();
	*cqep = &hw->queue[hw->cidx];
	return (0);
}

static int
iwc_next_cqe(iwc_cq_t *cq, t4_cqe_t **cqep)
{
	t4_cq_t *hw = &cq->cq_hw;

	if (hw->error)
		return (ENODATA);
	if (hw->sw_in_use != 0) {
		*cqep = &hw->sw_queue[hw->sw_cidx];
		return (0);
	}
	return (iwc_next_hw_cqe(cq, cqep));
}

static void
iwc_swcq_put(iwc_cq_t *cq, const t4_cqe_t *cqe)
{
	t4_cq_t *hw = &cq->cq_hw;

	hw->sw_queue[hw->sw_pidx] = *cqe;
	hw->sw_queue[hw->sw_pidx].header |= BE_32(V_CQE_SWCQE(1));
	t4_swcq_produce(hw);
}

static void
iwc_insert_recv_cqe(iwc_cq_t *cq, const t4_wq_t *wq)
{
	t4_cqe_t cqe;

	bzero(&cqe, sizeof (cqe));
	cqe.header = BE_32(V_CQE_STATUS(T4_ERR_SWFLUSH) |
	    V_CQE_OPCODE(FW_RI_SEND) | V_CQE_TYPE(0) | V_CQE_SWCQE(1) |
	    V_CQE_QPID(wq->sq.qid));
	cqe.bits_type_ts = BE_64(V_CQE_GENBIT((uint64_t)cq->cq_hw.gen));
	iwc_swcq_put(cq, &cqe);
}

static void
iwc_insert_sq_cqe(iwc_cq_t *cq, const t4_wq_t *wq, const t4_swsqe_t *swsqe)
{
	t4_cqe_t cqe;

	bzero(&cqe, sizeof (cqe));
	cqe.header = BE_32(V_CQE_STATUS(T4_ERR_SWFLUSH) |
	    V_CQE_OPCODE(swsqe->opcode) | V_CQE_TYPE(1) | V_CQE_SWCQE(1) |
	    V_CQE_QPID(wq->sq.qid));
	cqe.u.scqe.cidx = swsqe->idx;
	cqe.bits_type_ts = BE_64(V_CQE_GENBIT((uint64_t)cq->cq_hw.gen));
	iwc_swcq_put(cq, &cqe);
}

/* A drain completion for a work request posted after the flush. */
void
iwc_cq_insert_drain(iwc_cq_t *cq, iwc_qp_t *qp, uint64_t wr_id,
    uint8_t opcode, boolean_t sq)
{
	t4_cqe_t cqe;

	bzero(&cqe, sizeof (cqe));
	cqe.u.drain_cookie = wr_id;
	cqe.header = BE_32(V_CQE_STATUS(T4_ERR_SWFLUSH) |
	    V_CQE_OPCODE(opcode) | V_CQE_TYPE(sq ? 1 : 0) | V_CQE_SWCQE(1) |
	    V_CQE_DRAIN(1) | V_CQE_QPID(qp->qp_wq.sq.qid));
	mutex_enter(&cq->cq_lock);
	cqe.bits_type_ts = BE_64(V_CQE_GENBIT((uint64_t)cq->cq_hw.gen));
	iwc_swcq_put(cq, &cqe);
	mutex_exit(&cq->cq_lock);
	iwc_cq_wake(cq);
}

static void
iwc_advance_oldest_read(t4_wq_t *wq)
{
	uint32_t rptr = (uint32_t)(wq->sq.oldest_read - wq->sq.sw_sq) + 1;

	if (rptr == wq->sq.size)
		rptr = 0;
	while (rptr != wq->sq.pidx) {
		wq->sq.oldest_read = &wq->sq.sw_sq[rptr];
		if (wq->sq.oldest_read->opcode == FW_RI_READ_REQ)
			return;
		if (++rptr == wq->sq.size)
			rptr = 0;
	}
	wq->sq.oldest_read = NULL;
}

static void
iwc_read_req_cqe(const t4_wq_t *wq, const t4_cqe_t *hw, t4_cqe_t *rd)
{
	bzero(rd, sizeof (*rd));
	rd->u.scqe.cidx = wq->sq.oldest_read->idx;
	rd->len = BE_32(wq->sq.oldest_read->read_len);
	rd->header = BE_32(V_CQE_QPID(CQE_QPID(hw)) |
	    V_CQE_SWCQE(CQE_SWCQE(hw)) | V_CQE_OPCODE(FW_RI_READ_REQ) |
	    V_CQE_TYPE(1));
	rd->bits_type_ts = hw->bits_type_ts;
}

/* Move send completions that are now in order into the software CQ. */
static void
iwc_flush_completed_wrs(t4_wq_t *wq, iwc_cq_t *cq)
{
	t4_swsqe_t *swsqe;
	int cidx;

	if (wq->sq.flush_cidx == -1)
		wq->sq.flush_cidx = wq->sq.cidx;
	cidx = wq->sq.flush_cidx;
	while (cidx != wq->sq.pidx) {
		swsqe = &wq->sq.sw_sq[cidx];
		if (!swsqe->signaled) {
			if (++cidx == wq->sq.size)
				cidx = 0;
		} else if (swsqe->complete) {
			iwc_swcq_put(cq, &swsqe->cqe);
			swsqe->flushed = B_TRUE;
			if (++cidx == wq->sq.size)
				cidx = 0;
			wq->sq.flush_cidx = cidx;
		} else {
			break;
		}
	}
}

/* Whether a send queue index names a work request still outstanding. */
static boolean_t
iwc_sq_outstanding(const t4_wq_t *wq, uint16_t idx)
{
	uint32_t d;

	if (idx >= wq->sq.size)
		return (B_FALSE);
	d = (idx + wq->sq.size - wq->sq.cidx) % wq->sq.size;
	return (d < wq->sq.in_use);
}

/* Whether a CQE may be processed for this QP on this CQ. */
static boolean_t
iwc_cqe_owner_ok(iwc_cq_t *cq, iwc_qp_t *qp, const t4_cqe_t *cqe)
{
	const struct rdk_cq *want = CQE_SQ(cqe) ? qp->qp_rdk.send_cq :
	    qp->qp_rdk.recv_cq;

	/* A read response is a receive-type CQE that completes a send. */
	if (!CQE_SQ(cqe) && CQE_OPCODE(cqe) == FW_RI_READ_RESP)
		return (qp->qp_rdk.send_cq == &cq->cq_rdk ||
		    qp->qp_rdk.recv_cq == &cq->cq_rdk);
	return (want == &cq->cq_rdk);
}

/*
 * Move every hardware CQE of a CQ into its software queue, translating as
 * poll would, before a QP of the CQ is flushed.  The CQ's lock and fqp's
 * lock are held.
 */
static void
iwc_flush_hw_cq(iwc_cq_t *cq, iwc_qp_t *fqp)
{
	iwc_t *iwc = cq->cq_iwc;
	t4_cqe_t *hw, rd;
	t4_swsqe_t *swsqe;
	iwc_qp_t *qp;
	t4_wq_t *wq;
	uint16_t idx;

	ASSERT(MUTEX_HELD(&cq->cq_lock));
	while (iwc_next_hw_cqe(cq, &hw) == 0) {
		qp = iwc_qp_get(iwc, CQE_QPID(hw));
		if (qp == NULL || !iwc_cqe_owner_ok(cq, qp, hw)) {
			IWC_STAT(iwc, is_cqe_bad);
			goto next;
		}
		if (qp != fqp)
			mutex_enter(&qp->qp_lock);
		wq = &qp->qp_wq;
		if ((qp != fqp && wq->flushed) ||
		    CQE_OPCODE(hw) == FW_RI_TERMINATE)
			goto unlock;
		if (CQE_OPCODE(hw) == FW_RI_READ_RESP) {
			if (CQE_SQ(hw) || CQE_STAG(hw) == 1)
				goto unlock;
			if (wq->sq.oldest_read == NULL) {
				IWC_STAT(iwc, is_cqe_bad);
				goto unlock;
			}
			if (!wq->sq.oldest_read->signaled) {
				iwc_advance_oldest_read(wq);
				goto unlock;
			}
			iwc_read_req_cqe(wq, hw, &rd);
			hw = &rd;
			iwc_advance_oldest_read(wq);
		}
		if (CQE_SQ(hw)) {
			idx = CQE_SQ_IDX(hw);
			if (!iwc_sq_outstanding(wq, idx)) {
				IWC_STAT(iwc, is_cqe_bad);
				goto unlock;
			}
			swsqe = &wq->sq.sw_sq[idx];
			swsqe->cqe = *hw;
			swsqe->complete = B_TRUE;
			iwc_flush_completed_wrs(wq, cq);
		} else {
			iwc_swcq_put(cq, hw);
		}
unlock:
		if (qp != fqp)
			mutex_exit(&qp->qp_lock);
next:
		iwc_hwcq_consume(cq);
		if (qp != NULL)
			iwc_qp_put(iwc, qp);
	}
}

static boolean_t
iwc_cqe_completes_wr(const t4_cqe_t *cqe, const t4_wq_t *wq)
{
	if (CQE_DRAIN(cqe) || CQE_OPCODE(cqe) == FW_RI_TERMINATE)
		return (B_FALSE);
	if (CQE_OPCODE(cqe) == FW_RI_RDMA_WRITE && !CQE_SQ(cqe))
		return (B_FALSE);
	if (CQE_OPCODE(cqe) == FW_RI_READ_RESP && CQE_SQ(cqe))
		return (B_FALSE);
	if (CQE_SEND_OPCODE(cqe) && !CQE_SQ(cqe) && wq->rq.in_use == 0)
		return (B_FALSE);
	return (B_TRUE);
}

static uint32_t
iwc_count_rcqes(const iwc_cq_t *cq, const t4_wq_t *wq)
{
	const t4_cq_t *hw = &cq->cq_hw;
	uint32_t n = 0, p = hw->sw_cidx;

	while (p != hw->sw_pidx) {
		const t4_cqe_t *c = &hw->sw_queue[p];

		if (!CQE_SQ(c) && CQE_OPCODE(c) != FW_RI_READ_RESP &&
		    CQE_QPID(c) == wq->sq.qid && iwc_cqe_completes_wr(c, wq))
			n++;
		if (++p == hw->size)
			p = 0;
	}
	return (n);
}

static uint32_t
iwc_flush_sq(iwc_qp_t *qp, iwc_cq_t *scq)
{
	t4_wq_t *wq = &qp->qp_wq;
	t4_swsqe_t *swsqe;
	uint32_t flushed = 0;
	int idx;

	if (wq->sq.flush_cidx == -1)
		wq->sq.flush_cidx = wq->sq.cidx;
	idx = wq->sq.flush_cidx;
	while (idx != wq->sq.pidx) {
		swsqe = &wq->sq.sw_sq[idx];
		swsqe->flushed = B_TRUE;
		iwc_insert_sq_cqe(scq, wq, swsqe);
		if (wq->sq.oldest_read == swsqe)
			iwc_advance_oldest_read(wq);
		flushed++;
		if (++idx == wq->sq.size)
			idx = 0;
	}
	wq->sq.flush_cidx += flushed;
	if (wq->sq.flush_cidx >= wq->sq.size)
		wq->sq.flush_cidx -= wq->sq.size;
	return (flushed);
}

/*
 * Complete everything the QP has outstanding with flush errors.  Called
 * once the QP left RDMA mode; later work requests get drain completions.
 */
void
iwc_flush_qp(iwc_qp_t *qp)
{
	iwc_cq_t *rcq = (iwc_cq_t *)qp->qp_rdk.recv_cq;
	iwc_cq_t *scq = (iwc_cq_t *)qp->qp_rdk.send_cq;
	t4_wq_t *wq = &qp->qp_wq;
	uint32_t rq = 0, sq, n, in_use;

	mutex_enter(&rcq->cq_lock);
	if (scq != rcq)
		mutex_enter(&scq->cq_lock);
	mutex_enter(&qp->qp_lock);
	if (wq->flushed) {
		mutex_exit(&qp->qp_lock);
		if (scq != rcq)
			mutex_exit(&scq->cq_lock);
		mutex_exit(&rcq->cq_lock);
		return;
	}
	wq->flushed = B_TRUE;
	t4_set_wq_in_error(wq);

	iwc_flush_hw_cq(rcq, qp);
	n = iwc_count_rcqes(rcq, wq);
	in_use = wq->rq.in_use > n ? wq->rq.in_use - n : 0;
	for (; in_use > 0; in_use--, rq++)
		iwc_insert_recv_cqe(rcq, wq);
	if (scq != rcq)
		iwc_flush_hw_cq(scq, qp);
	sq = iwc_flush_sq(qp, scq);

	mutex_exit(&qp->qp_lock);
	if (scq != rcq)
		mutex_exit(&scq->cq_lock);
	mutex_exit(&rcq->cq_lock);

	if (rq != 0 || (scq == rcq && sq != 0))
		iwc_cq_wake(rcq);
	if (scq != rcq && sq != 0)
		iwc_cq_wake(scq);
}

typedef struct iwc_polled {
	t4_cqe_t	ip_cqe;
	boolean_t	ip_flushed;
	uint64_t	ip_cookie;
} iwc_polled_t;

/*
 * One CQE for a QP whose lock is held (Linux poll_cq()).  0 with *out
 * filled, EAGAIN when the CQE was consumed without a completion.
 */
static int
iwc_poll_one_qp(iwc_cq_t *cq, iwc_qp_t *qp, t4_cqe_t *hw, iwc_polled_t *out)
{
	iwc_t *iwc = cq->cq_iwc;
	t4_wq_t *wq = &qp->qp_wq;
	t4_cq_t *hc = &cq->cq_hw;
	t4_cqe_t rd;
	t4_swsqe_t *swsqe;
	const boolean_t sw = CQE_SWCQE(hw);
	uint16_t idx;
	int ret = 0;

	out->ip_flushed = B_FALSE;
	if (wq->flushed && !sw) {
		ret = EAGAIN;
		goto skip;
	}
	if (CQE_OPCODE(hw) == FW_RI_TERMINATE) {
		ret = EAGAIN;
		goto skip;
	}
	if (CQE_DRAIN(hw)) {
		out->ip_cookie = hw->u.drain_cookie;
		out->ip_cqe = *hw;
		out->ip_flushed = B_TRUE;
		goto skip;
	}
	if (!CQE_SQ(hw) && CQE_OPCODE(hw) == FW_RI_READ_RESP) {
		/* STag 1: the ready-to-receive read of peer-to-peer setup. */
		if (CQE_STAG(hw) == 1) {
			if (CQE_STATUS(hw) != 0)
				t4_set_wq_in_error(wq);
			ret = EAGAIN;
			goto skip;
		}
		if (wq->sq.oldest_read == NULL) {
			IWC_STAT(iwc, is_cqe_bad);
			t4_set_wq_in_error(wq);
			ret = EAGAIN;
			goto skip;
		}
		if (!wq->sq.oldest_read->signaled) {
			iwc_advance_oldest_read(wq);
			ret = EAGAIN;
			goto skip;
		}
		iwc_read_req_cqe(wq, hw, &rd);
		iwc_advance_oldest_read(wq);
		hw = &rd;
	}
	if (CQE_STATUS(hw) != 0 || t4_wq_in_error(wq)) {
		out->ip_flushed = CQE_STATUS(hw) == T4_ERR_SWFLUSH;
		t4_set_wq_in_error(wq);
	}

	if (!CQE_SQ(hw)) {
		if (wq->rq.in_use == 0) {
			IWC_STAT(iwc, is_cqe_bad);
			t4_set_wq_in_error(wq);
			ret = EAGAIN;
			goto skip;
		}
		/* The hardware checks only 4 bits of the MSN. */
		if (CQE_STATUS(hw) == 0 && CQE_MSN(hw) != wq->rq.msn) {
			t4_set_wq_in_error(wq);
			out->ip_cqe = *hw;
			out->ip_cqe.header |= BE_32(V_CQE_STATUS(T4_ERR_MSN));
		} else {
			out->ip_cqe = *hw;
		}
		out->ip_cookie = wq->rq.sw_rq[wq->rq.cidx].wr_id;
		t4_rq_consume(wq);
		wq->rq.msn++;
		goto skip;
	}

	idx = CQE_SQ_IDX(hw);
	if (!iwc_sq_outstanding(wq, idx)) {
		IWC_STAT(iwc, is_cqe_bad);
		t4_set_wq_in_error(wq);
		ret = EAGAIN;
		goto skip;
	}
	if (!sw && idx != wq->sq.cidx) {
		swsqe = &wq->sq.sw_sq[idx];
		swsqe->cqe = *hw;
		swsqe->complete = B_TRUE;
		ret = EAGAIN;
		goto flush;
	}

	out->ip_cqe = *hw;
	/* A signaled completion also completes the unsignaled ones before it. */
	if (idx < wq->sq.cidx)
		wq->sq.in_use -= wq->sq.size + idx - wq->sq.cidx;
	else
		wq->sq.in_use -= idx - wq->sq.cidx;
	wq->sq.cidx = idx;
	out->ip_cookie = wq->sq.sw_sq[wq->sq.cidx].wr_id;
	t4_sq_consume(wq);
flush:
	iwc_flush_completed_wrs(wq, cq);
skip:
	if (sw)
		t4_swcq_consume(hc);
	else
		iwc_hwcq_consume(cq);
	return (ret);
}

static enum rdk_wc_status
iwc_wc_status(uint_t st)
{
	switch (st) {
	case T4_ERR_SUCCESS:
		return (RDK_WC_SUCCESS);
	case T4_ERR_STAG:
	case T4_ERR_QPID:
	case T4_ERR_ACCESS:
		return (RDK_WC_LOC_ACCESS_ERR);
	case T4_ERR_PDID:
		return (RDK_WC_LOC_PROT_ERR);
	case T4_ERR_WRAP:
		return (RDK_WC_GENERAL_ERR);
	case T4_ERR_BOUND:
		return (RDK_WC_LOC_LEN_ERR);
	case T4_ERR_INVALIDATE_SHARED_MR:
	case T4_ERR_INVALIDATE_MR_WITH_MW_BOUND:
		return (RDK_WC_MW_BIND_ERR);
	case T4_ERR_SWFLUSH:
		return (RDK_WC_WR_FLUSH_ERR);
	default:
		return (RDK_WC_FATAL_ERR);
	}
}

/* Fill a work completion; EINVAL for an opcode no work request makes. */
static int
iwc_wc_fill(iwc_qp_t *qp, const iwc_polled_t *p, struct rdk_wc *wc)
{
	const t4_cqe_t *c = &p->ip_cqe;

	bzero(wc, sizeof (*wc));
	wc->wr_id = p->ip_cookie;
	wc->qp = &qp->qp_rdk;
	wc->vendor_err = CQE_STATUS(c);
	wc->port_num = 1;
	if (!CQE_SQ(c)) {
		wc->byte_len = CQE_STATUS(c) == 0 ? CQE_LEN(c) : 0;
		switch (CQE_OPCODE(c)) {
		case FW_RI_SEND:
		case FW_RI_SEND_WITH_SE:
			wc->opcode = RDK_WC_RECV;
			break;
		case FW_RI_SEND_WITH_INV:
		case FW_RI_SEND_WITH_SE_INV:
			wc->opcode = RDK_WC_RECV;
			wc->ex.invalidate_rkey = CQE_STAG(c);
			wc->wc_flags |= RDK_WC_WITH_INVALIDATE;
			break;
		case FW_RI_WRITE_IMMEDIATE:
			wc->opcode = RDK_WC_RECV_RDMA_WITH_IMM;
			wc->ex.imm_data = CQE_IMM_DATA(c);
			wc->wc_flags |= RDK_WC_WITH_IMM;
			break;
		default:
			return (EINVAL);
		}
	} else {
		switch (CQE_OPCODE(c)) {
		case FW_RI_WRITE_IMMEDIATE:
		case FW_RI_RDMA_WRITE:
			wc->opcode = RDK_WC_RDMA_WRITE;
			break;
		case FW_RI_READ_REQ:
			wc->opcode = RDK_WC_RDMA_READ;
			wc->byte_len = CQE_LEN(c);
			break;
		case FW_RI_SEND_WITH_INV:
		case FW_RI_SEND_WITH_SE_INV:
		case FW_RI_SEND:
		case FW_RI_SEND_WITH_SE:
			wc->opcode = RDK_WC_SEND;
			break;
		case FW_RI_LOCAL_INV:
			wc->opcode = RDK_WC_LOCAL_INV;
			break;
		case FW_RI_FAST_REGISTER:
			wc->opcode = RDK_WC_REG_MR;
			break;
		default:
			return (EINVAL);
		}
	}
	wc->status = p->ip_flushed ? RDK_WC_WR_FLUSH_ERR :
	    iwc_wc_status(CQE_STATUS(c));
	return (0);
}

/* One completion; ENODATA when the CQ is empty.  cq_lock is held. */
static int
iwc_poll_one(iwc_cq_t *cq, struct rdk_wc *wc)
{
	iwc_t *iwc = cq->cq_iwc;
	iwc_polled_t p;
	t4_cqe_t *hw;
	iwc_qp_t *qp;
	int ret;

	if ((ret = iwc_next_cqe(cq, &hw)) != 0)
		return (ret);
	qp = iwc_qp_get(iwc, CQE_QPID(hw));
	if (qp == NULL || !iwc_cqe_owner_ok(cq, qp, hw)) {
		if (!CQE_SWCQE(hw))
			IWC_STAT(iwc, is_cqe_bad);
		if (CQE_SWCQE(hw))
			t4_swcq_consume(&cq->cq_hw);
		else
			iwc_hwcq_consume(cq);
		if (qp != NULL)
			iwc_qp_put(iwc, qp);
		return (EAGAIN);
	}
	mutex_enter(&qp->qp_lock);
	ret = iwc_poll_one_qp(cq, qp, hw, &p);
	mutex_exit(&qp->qp_lock);
	if (ret == 0 && iwc_wc_fill(qp, &p, wc) != 0) {
		IWC_STAT(iwc, is_cqe_bad);
		ret = EAGAIN;
	}
	iwc_qp_put(iwc, qp);
	return (ret);
}

int
iwc_poll_cq(struct rdk_cq *rcq, int n, struct rdk_wc *wc)
{
	iwc_cq_t *cq = (iwc_cq_t *)rcq;
	int done = 0, ret;

	mutex_enter(&cq->cq_lock);
	while (done < n) {
		ret = iwc_poll_one(cq, &wc[done]);
		if (ret == EAGAIN)
			continue;
		if (ret != 0)
			break;
		done++;
	}
	mutex_exit(&cq->cq_lock);
	return (done);
}

int
iwc_req_notify_cq(struct rdk_cq *rcq, enum rdk_cq_notify_flags flags)
{
	iwc_cq_t *cq = (iwc_cq_t *)rcq;
	t4_cq_t *hw = &cq->cq_hw;
	const uint32_t se = (flags & RDK_CQ_SOLICITED_MASK) ==
	    RDK_CQ_SOLICITED ? 1 : 0;
	int ret = 0;

	mutex_enter(&cq->cq_lock);
	hw->armed = B_TRUE;
	while (hw->cidx_inc > M_CIDXINC) {
		iwc_gts(cq, V_SEINTARM(0) | V_CIDXINC(M_CIDXINC) |
		    V_TIMERREG(7));
		hw->cidx_inc -= M_CIDXINC;
	}
	iwc_gts(cq, V_SEINTARM(se) | V_CIDXINC(hw->cidx_inc) | V_TIMERREG(6));
	hw->cidx_inc = 0;
	if ((flags & RDK_CQ_REPORT_MISSED_EVENTS) != 0) {
		ret = hw->sw_in_use != 0 ||
		    t4_valid_cqe(hw, &hw->queue[hw->cidx]) ? 1 : 0;
	}
	mutex_exit(&cq->cq_lock);
	return (ret);
}

/* Queue the CQ's completion handler.  iwc_obj_lock is held. */
static void
iwc_cq_schedule(iwc_t *iwc, iwc_cq_t *cq)
{
	ASSERT(MUTEX_HELD(&iwc->iwc_obj_lock));
	if (cq->cq_pending)
		return;
	cq->cq_pending = B_TRUE;
	cq->cq_refs++;
	cq->cq_next = iwc->iwc_cq_pending;
	iwc->iwc_cq_pending = cq;
	if (!iwc->iwc_cq_queued) {
		iwc->iwc_cq_queued = B_TRUE;
		taskq_dispatch_ent(iwc->iwc_cq_tq, iwc_cq_task, iwc, 0,
		    &iwc->iwc_cq_ent);
	}
}

/* Software completions were added; tell an armed CQ's consumer. */
void
iwc_cq_wake(iwc_cq_t *cq)
{
	iwc_t *iwc = cq->cq_iwc;
	boolean_t armed;

	mutex_enter(&cq->cq_lock);
	armed = cq->cq_hw.armed;
	cq->cq_hw.armed = B_FALSE;
	mutex_exit(&cq->cq_lock);
	if (!armed)
		return;
	mutex_enter(&iwc->iwc_obj_lock);
	if (iwc->iwc_cqs[cq->cq_hw.cqid - iwc->iwc_qid_start] == cq)
		iwc_cq_schedule(iwc, cq);
	mutex_exit(&iwc->iwc_obj_lock);
}

/* t4nex: an armed CQ has new entries.  Interrupt context. */
void
iwc_cq_notify(void *arg, uint32_t cqid)
{
	iwc_t *iwc = arg;
	iwc_cq_t *cq;

	if (cqid < iwc->iwc_qid_start || cqid - iwc->iwc_qid_start >=
	    iwc->iwc_qid_n)
		return;
	mutex_enter(&iwc->iwc_obj_lock);
	if ((cq = iwc->iwc_cqs[cqid - iwc->iwc_qid_start]) != NULL)
		iwc_cq_schedule(iwc, cq);
	mutex_exit(&iwc->iwc_obj_lock);
}

void
iwc_cq_task(void *arg)
{
	iwc_t *iwc = arg;
	iwc_cq_t *cq;

	for (;;) {
		mutex_enter(&iwc->iwc_obj_lock);
		if ((cq = iwc->iwc_cq_pending) == NULL) {
			iwc->iwc_cq_queued = B_FALSE;
			mutex_exit(&iwc->iwc_obj_lock);
			return;
		}
		iwc->iwc_cq_pending = cq->cq_next;
		cq->cq_next = NULL;
		cq->cq_pending = B_FALSE;
		mutex_exit(&iwc->iwc_obj_lock);

		mutex_enter(&cq->cq_lock);
		cq->cq_hw.armed = B_FALSE;
		mutex_exit(&cq->cq_lock);
		if (cq->cq_rdk.comp_handler != NULL)
			cq->cq_rdk.comp_handler(&cq->cq_rdk,
			    cq->cq_rdk.cq_context);

		mutex_enter(&iwc->iwc_obj_lock);
		if (--cq->cq_refs == 0)
			cv_broadcast(&iwc->iwc_obj_cv);
		mutex_exit(&iwc->iwc_obj_lock);
	}
}

/* iwc_obj_lock is held. */
static void
iwc_cq_wait_idle(iwc_t *iwc, iwc_cq_t *cq)
{
	ASSERT(MUTEX_HELD(&iwc->iwc_obj_lock));
	while (cq->cq_refs != 0)
		cv_wait(&iwc->iwc_obj_cv, &iwc->iwc_obj_lock);
}

int
iwc_create_cq(struct rdk_cq *rcq, const struct rdk_cq_init_attr *attr)
{
	iwc_cq_t *cq = (iwc_cq_t *)rcq;
	iwc_t *iwc = iwc_of(rcq->device);
	t4_cq_t *hw = &cq->cq_hw;
	t4_rdma_cq_res_t res;
	t4_rdma_db_t db;
	uint32_t entries, hwentries;
	int ret;

	if (attr->cqe == 0 || attr->cqe > IWC_MAX_CQE || attr->flags != 0)
		return (EINVAL);
	if (iwc->iwc_fatal)
		return (EIO);
	/* The status page, one entry to tell full from empty, 16 multiples. */
	entries = roundup(attr->cqe + 2, 16);
	hwentries = MAX(MIN(entries * 2, 65520), 64);

	cq->cq_iwc = iwc;
	mutex_init(&cq->cq_lock, NULL, MUTEX_DRIVER, NULL);
	cq->cq_memlen = (size_t)hwentries * T4_CQE_SIZE;
	if ((ret = iwc->iwc_ops->tro_dma_alloc(iwc->iwc_peer, cq->cq_memlen,
	    4096, &cq->cq_mem)) != 0)
		goto fail;
	hw->sw_queue = kmem_zalloc(cq->cq_memlen, KM_SLEEP);
	if ((ret = iwc_qid_alloc(iwc, &hw->cqid)) != 0)
		goto fail;

	bzero(&res, sizeof (res));
	res.trcq_cqid = hw->cqid;
	res.trcq_size = hwentries;
	res.trcq_mem = cq->cq_mem;
	ret = iwc->iwc_ops->tro_cq_create(iwc->iwc_peer, &res, &db);
	if (ret != 0) {
		if (ret != EFAULT && ret != ENXIO)
			cq->cq_mem = NULL;
		iwc_qid_free(iwc, hw->cqid);
		hw->cqid = 0;
		goto fail;
	}
	cq->cq_live = B_TRUE;
	hw->queue = (t4_cqe_t *)cq->cq_mem->trd_va;
	hw->size = (uint16_t)(hwentries - 1);
	hw->gen = 1;
	hw->bar2_va = iwc->iwc_info.tri_bar2 + db.trdb_off;
	hw->bar2_qid = db.trdb_qid;
	rcq->cqe = (int)(entries - 2);

	mutex_enter(&iwc->iwc_obj_lock);
	iwc->iwc_cqs[hw->cqid - iwc->iwc_qid_start] = cq;
	mutex_exit(&iwc->iwc_obj_lock);
	return (0);
fail:
	if (hw->sw_queue != NULL)
		kmem_free(hw->sw_queue, cq->cq_memlen);
	if (cq->cq_mem != NULL)
		iwc->iwc_ops->tro_dma_free(iwc->iwc_peer, cq->cq_mem, B_TRUE);
	mutex_destroy(&cq->cq_lock);
	return (ret);
}

void
iwc_destroy_cq(struct rdk_cq *rcq)
{
	iwc_cq_t *cq = (iwc_cq_t *)rcq;
	iwc_t *iwc = cq->cq_iwc;
	int ret;

	mutex_enter(&iwc->iwc_obj_lock);
	iwc->iwc_cqs[cq->cq_hw.cqid - iwc->iwc_qid_start] = NULL;
	iwc_cq_wait_idle(iwc, cq);
	mutex_exit(&iwc->iwc_obj_lock);

	/* t4nex frees the ring, or keeps it if the firmware did not answer. */
	ret = iwc->iwc_ops->tro_cq_destroy(iwc->iwc_peer, cq->cq_hw.cqid);
	if (ret == 0)
		iwc_qid_free(iwc, cq->cq_hw.cqid);
	else
		IWC_STAT(iwc, is_quar);
	kmem_free(cq->cq_hw.sw_queue, cq->cq_memlen);
	mutex_destroy(&cq->cq_lock);
}

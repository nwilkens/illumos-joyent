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
 * Kernel QPs: creation, the work request builders, posting and the moves
 * in and out of RDMA mode.  Adapted from Linux cxgb4/qp.c under the OpenIB
 * license.  A QP enters RDMA mode only through the connection manager; the
 * consumer may only move it to the error state.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/atomic.h>

#include "iwc.h"
#include "common/t4_regs.h"
#include "common/t4_regs_values.h"

#define	IWC_QP_WARN_SEC		10

static void
iwc_ring_sq(iwc_qp_t *qp, uint16_t inc)
{
	iwc_db_write(qp->qp_iwc, qp->qp_wq.sq.bar2_va, SGE_UDB_KDOORBELL,
	    V_PIDX_T5(inc) | V_QID(qp->qp_wq.sq.bar2_qid));
}

static void
iwc_ring_rq(iwc_qp_t *qp, uint16_t inc)
{
	iwc_db_write(qp->qp_iwc, qp->qp_wq.rq.bar2_va, SGE_UDB_KDOORBELL,
	    V_PIDX_T5(inc) | V_QID(qp->qp_wq.rq.bar2_qid));
}

static uint32_t
iwc_pow2(uint32_t v)
{
	uint32_t p = 1;

	while (p < v)
		p <<= 1;
	return (p);
}

/*
 * The SQ memory of sqsize slots: the ring and its status page, then with
 * DSGL registration a page list area of T4_MAX_FR_DSGL per slot, at *pbl.
 */
size_t
iwc_sq_bytes(iwc_t *iwc, uint32_t sqsize, size_t *pbl)
{
	size_t b = P2ROUNDUP((size_t)sqsize * T4_SQ_NUM_BYTES +
	    iwc->iwc_info.tri_eq_spg_len * T4_EQ_ENTRY_SIZE + 128, 64);

	*pbl = 0;
	if (iwc->iwc_info.tri_vres.trv_memwrite_dsgl) {
		*pbl = b;
		b += (size_t)sqsize * T4_MAX_FR_DSGL;
	}
	return (P2ROUNDUP(b, PAGESIZE));
}

/* The most work requests a queue takes: its memory must fit one buffer. */
uint32_t
iwc_max_qp_wr(iwc_t *iwc)
{
	uint32_t n = IWC_MAX_QP_WR;
	size_t pbl;

	while (n > 8 && iwc_sq_bytes(iwc, n + 1, &pbl) > T4_RDMA_DMA_MAX_LEN)
		n--;
	return (n);
}

int
iwc_create_qp(struct rdk_qp *rqp, struct rdk_qp_init_attr *init)
{
	iwc_qp_t *qp = (iwc_qp_t *)rqp;
	iwc_t *iwc = iwc_of(rqp->device);
	iwc_pd_t *pd = (iwc_pd_t *)rqp->pd;
	const uint32_t spg = iwc->iwc_info.tri_eq_spg_len;
	t4_wq_t *wq = &qp->qp_wq;
	t4_rdma_qp_res_t res;
	t4_rdma_db_t sdb, rdb;
	uint32_t sqsize, rqsize;
	size_t sqbytes, rqbytes;
	int ret;

	if (init->qp_type != RDK_QPT_RC || init->create_flags != 0 ||
	    init->cap.max_send_wr == 0 || init->cap.max_recv_wr == 0 ||
	    init->cap.max_send_wr > iwc_max_qp_wr(iwc) ||
	    init->cap.max_recv_wr > iwc_max_qp_wr(iwc) ||
	    init->cap.max_send_sge > MIN(T4_MAX_SEND_SGE, T4_MAX_WRITE_SGE) ||
	    init->cap.max_recv_sge > T4_MAX_RECV_SGE ||
	    init->cap.max_inline_data != 0)
		return (EINVAL);
	if (iwc->iwc_fatal)
		return (EIO);

	qp->qp_iwc = iwc;
	mutex_init(&qp->qp_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&qp->qp_cv, NULL, CV_DRIVER, NULL);
	qp->qp_pdid = pd->pd_pdid;
	qp->qp_sig_all = init->sq_sig_type == RDK_SIGNAL_ALL_WR;
	qp->qp_sq_max_sge = MAX(init->cap.max_send_sge, 1);
	qp->qp_rq_max_sge = MAX(init->cap.max_recv_sge, 1);
	sqsize = MAX(init->cap.max_send_wr + 1, 8);
	rqsize = MAX(init->cap.max_recv_wr + 1, 8);
	wq->sq.size = (uint16_t)sqsize;
	wq->rq.size = (uint16_t)rqsize;
	wq->sq.flush_cidx = -1;
	wq->rq.msn = 1;
	sqbytes = iwc_sq_bytes(iwc, sqsize, &qp->qp_pbl_off);
	rqbytes = P2ROUNDUP((size_t)rqsize * T4_RQ_NUM_BYTES +
	    spg * T4_EQ_ENTRY_SIZE, PAGESIZE);

	wq->sq.sw_sq = kmem_zalloc(sqsize * sizeof (t4_swsqe_t), KM_SLEEP);
	wq->rq.sw_rq = kmem_zalloc(rqsize * sizeof (t4_swrqe_t), KM_SLEEP);
	wq->rq.rqt_size = (uint16_t)iwc_pow2(MAX(rqsize, 16));
	if ((wq->rq.rqt_hwaddr = iwc_rqt_alloc(iwc, wq->rq.rqt_size)) == 0) {
		ret = ENOMEM;
		goto fail;
	}
	if ((ret = iwc->iwc_ops->tro_dma_alloc(iwc->iwc_peer, sqbytes,
	    PAGESIZE, &qp->qp_sqmem)) != 0 ||
	    (ret = iwc->iwc_ops->tro_dma_alloc(iwc->iwc_peer, rqbytes,
	    PAGESIZE, &qp->qp_rqmem)) != 0)
		goto fail;
	if ((ret = iwc_qid_alloc(iwc, &wq->sq.qid)) != 0)
		goto fail;
	if ((ret = iwc_qid_alloc(iwc, &wq->rq.qid)) != 0) {
		iwc_qid_free(iwc, wq->sq.qid);
		wq->sq.qid = 0;
		goto fail;
	}

	bzero(&res, sizeof (res));
	res.trqp_sqid = wq->sq.qid;
	res.trqp_rqid = wq->rq.qid;
	res.trqp_scqid = ((iwc_cq_t *)init->send_cq)->cq_hw.cqid;
	res.trqp_rcqid = ((iwc_cq_t *)init->recv_cq)->cq_hw.cqid;
	res.trqp_sq_size = sqsize * T4_SQ_NUM_SLOTS + spg;
	res.trqp_rq_size = rqsize * T4_RQ_NUM_SLOTS + spg;
	res.trqp_sq_mem = qp->qp_sqmem;
	res.trqp_rq_mem = qp->qp_rqmem;
	ret = iwc->iwc_ops->tro_qp_create(iwc->iwc_peer, &res, &sdb, &rdb);
	if (ret != 0) {
		if (ret != EFAULT && ret != ENXIO)
			qp->qp_sqmem = qp->qp_rqmem = NULL;
		iwc_qid_free(iwc, wq->rq.qid);
		iwc_qid_free(iwc, wq->sq.qid);
		wq->sq.qid = wq->rq.qid = 0;
		goto fail;
	}
	qp->qp_live = B_TRUE;
	wq->sq.queue = (union t4_wr *)qp->qp_sqmem->trd_va;
	wq->rq.queue = (union t4_recv_wr *)qp->qp_rqmem->trd_va;
	wq->qp_errp = &wq->rq.queue[wq->rq.size].status.qp_err;
	wq->sq.bar2_va = iwc->iwc_info.tri_bar2 + sdb.trdb_off;
	wq->sq.bar2_qid = sdb.trdb_qid;
	wq->rq.bar2_va = iwc->iwc_info.tri_bar2 + rdb.trdb_off;
	wq->rq.bar2_qid = rdb.trdb_qid;
	qp->qp_state = IWC_QPS_IDLE;
	rqp->qp_num = wq->sq.qid;
	init->cap.max_send_wr = sqsize - 1;
	init->cap.max_recv_wr = rqsize - 1;

	mutex_enter(&iwc->iwc_obj_lock);
	iwc->iwc_qps[wq->sq.qid - iwc->iwc_qid_start] = qp;
	mutex_exit(&iwc->iwc_obj_lock);
	return (0);
fail:
	if (qp->qp_sqmem != NULL)
		iwc->iwc_ops->tro_dma_free(iwc->iwc_peer, qp->qp_sqmem, B_TRUE);
	if (qp->qp_rqmem != NULL)
		iwc->iwc_ops->tro_dma_free(iwc->iwc_peer, qp->qp_rqmem, B_TRUE);
	if (wq->rq.rqt_hwaddr != 0)
		iwc_rqt_free(iwc, wq->rq.rqt_hwaddr, wq->rq.rqt_size);
	kmem_free(wq->sq.sw_sq, sqsize * sizeof (t4_swsqe_t));
	kmem_free(wq->rq.sw_rq, rqsize * sizeof (t4_swrqe_t));
	cv_destroy(&qp->qp_cv);
	mutex_destroy(&qp->qp_lock);
	return (ret);
}

/*
 * Leave RDMA mode for good: flush what is outstanding, forget the
 * connection and wake a destroy waiting for it.  Thread context.
 */
static void
iwc_qp_detach(iwc_qp_t *qp, iwc_qp_state_t next)
{
	iwc_ep_t *ep;

	iwc_flush_qp(qp);
	mutex_enter(&qp->qp_lock);
	qp->qp_state = next;
	ep = qp->qp_ep;
	qp->qp_ep = NULL;
	cv_broadcast(&qp->qp_cv);
	mutex_exit(&qp->qp_lock);
	if (ep != NULL)
		iwc_ep_rele(ep);
}

/* Bind the QP to its connection and enter RDMA mode (FW_RI_WR INIT). */
int
iwc_qp_rts(iwc_qp_t *qp, iwc_ep_t *ep)
{
	iwc_t *iwc = qp->qp_iwc;
	t4_rdma_ri_init_t ri;
	int ret;

	mutex_enter(&qp->qp_lock);
	if (qp->qp_state != IWC_QPS_IDLE || qp->qp_ep != NULL ||
	    qp->qp_wq.flushed) {
		mutex_exit(&qp->qp_lock);
		return (EINVAL);
	}
	iwc_ep_hold(ep);
	qp->qp_ep = ep;
	qp->qp_state = IWC_QPS_RTS;
	qp->qp_ird = ep->ep_ird;
	qp->qp_ord = ep->ep_ord;
	bzero(&ri, sizeof (ri));
	ri.trri_sqid = qp->qp_wq.sq.qid;
	ri.trri_pdid = qp->qp_pdid;
	ri.trri_initiator = ep->ep_attr.ma_initiator;
	ri.trri_p2p_type = ep->ep_attr.ma_p2p_type;
	ri.trri_crc = ep->ep_attr.ma_crc;
	ri.trri_ord = ep->ep_ord;
	ri.trri_ird = ep->ep_ird;
	ri.trri_iss = ep->ep_snd_seq;
	ri.trri_irs = ep->ep_rcv_seq;
	ri.trri_nrqe = qp->qp_wq.rq.in_use;
	ri.trri_rqt_addr = qp->qp_wq.rq.rqt_hwaddr;
	ri.trri_rqt_size = qp->qp_wq.rq.rqt_size;
	mutex_exit(&qp->qp_lock);

	ret = iwc->iwc_ops->tro_ri_init(iwc->iwc_peer, ep->ep_tid, &ri);
	if (ret != 0) {
		iwc_warn(iwc, "QP %u: RDMA init failed: %d",
		    qp->qp_wq.sq.qid, ret);
		iwc_qp_detach(qp, IWC_QPS_ERROR);
	}
	return (ret);
}

/*
 * A graceful close: leave RDMA mode with FW_RI_WR FINI, then flush.  The
 * connection's TCP close follows.
 */
int
iwc_qp_close(iwc_qp_t *qp, iwc_ep_t *ep)
{
	iwc_t *iwc = qp->qp_iwc;
	int ret;

	mutex_enter(&qp->qp_lock);
	if (qp->qp_ep != ep || qp->qp_state != IWC_QPS_RTS) {
		mutex_exit(&qp->qp_lock);
		return (EINVAL);
	}
	qp->qp_state = IWC_QPS_CLOSING;
	t4_set_wq_in_error(&qp->qp_wq);
	mutex_exit(&qp->qp_lock);

	ret = iwc->iwc_ops->tro_ri_fini(iwc->iwc_peer, ep->ep_tid,
	    qp->qp_wq.sq.qid);
	iwc_qp_detach(qp, ret == 0 ? IWC_QPS_IDLE : IWC_QPS_ERROR);
	return (ret);
}

/* An abort or error: the QP leaves RDMA mode with its connection. */
void
iwc_qp_error(iwc_qp_t *qp, iwc_ep_t *ep)
{
	mutex_enter(&qp->qp_lock);
	if (qp->qp_ep != ep) {
		mutex_exit(&qp->qp_lock);
		return;
	}
	t4_set_wq_in_error(&qp->qp_wq);
	mutex_exit(&qp->qp_lock);
	iwc_qp_detach(qp, IWC_QPS_ERROR);
}

int
iwc_modify_qp(struct rdk_qp *rqp, struct rdk_qp_attr *attr, int mask)
{
	iwc_qp_t *qp = (iwc_qp_t *)rqp;
	iwc_ep_t *ep;

	if ((mask & ~(RDK_QP_STATE | RDK_QP_ACCESS_FLAGS)) != 0)
		return (EINVAL);
	if ((mask & RDK_QP_STATE) == 0)
		return (0);
	switch (attr->qp_state) {
	case RDK_QPS_ERR:
		mutex_enter(&qp->qp_lock);
		ep = qp->qp_ep;
		if (ep != NULL)
			iwc_ep_hold(ep);
		mutex_exit(&qp->qp_lock);
		if (ep != NULL) {
			iwc_ep_abort(ep, ECONNABORTED);
			iwc_ep_rele(ep);
		}
		iwc_qp_detach(qp, IWC_QPS_ERROR);
		return (0);
	case RDK_QPS_RESET:
	case RDK_QPS_INIT:
		mutex_enter(&qp->qp_lock);
		ep = qp->qp_ep;
		mutex_exit(&qp->qp_lock);
		return (ep == NULL ? 0 : EINVAL);
	default:
		return (EINVAL);
	}
}

int
iwc_query_qp(struct rdk_qp *rqp, struct rdk_qp_attr *attr, int mask,
    struct rdk_qp_init_attr *init)
{
	iwc_qp_t *qp = (iwc_qp_t *)rqp;

	_NOTE(ARGUNUSED(mask));
	mutex_enter(&qp->qp_lock);
	switch (qp->qp_state) {
	case IWC_QPS_IDLE:
		attr->qp_state = qp->qp_wq.flushed ? RDK_QPS_ERR :
		    RDK_QPS_INIT;
		break;
	case IWC_QPS_RTS:
		attr->qp_state = RDK_QPS_RTS;
		break;
	case IWC_QPS_CLOSING:
		attr->qp_state = RDK_QPS_SQD;
		break;
	default:
		attr->qp_state = RDK_QPS_ERR;
		break;
	}
	attr->cur_qp_state = attr->qp_state;
	attr->cap.max_send_wr = qp->qp_wq.sq.size - 1;
	attr->cap.max_recv_wr = qp->qp_wq.rq.size - 1;
	attr->cap.max_send_sge = qp->qp_sq_max_sge;
	attr->cap.max_recv_sge = qp->qp_rq_max_sge;
	attr->max_rd_atomic = (uint8_t)MIN(qp->qp_ord, UINT8_MAX);
	attr->max_dest_rd_atomic = (uint8_t)MIN(qp->qp_ird, UINT8_MAX);
	init->cap = attr->cap;
	init->qp_type = RDK_QPT_RC;
	init->sq_sig_type = qp->qp_sig_all ? RDK_SIGNAL_ALL_WR :
	    RDK_SIGNAL_REQ_WR;
	mutex_exit(&qp->qp_lock);
	return (0);
}

void
iwc_destroy_qp(struct rdk_qp *rqp)
{
	iwc_qp_t *qp = (iwc_qp_t *)rqp;
	iwc_t *iwc = qp->qp_iwc;
	t4_wq_t *wq = &qp->qp_wq;
	iwc_ep_t *ep;
	int ret;

	mutex_enter(&qp->qp_lock);
	ep = qp->qp_ep;
	if (ep != NULL)
		iwc_ep_hold(ep);
	mutex_exit(&qp->qp_lock);
	if (ep != NULL) {
		iwc_ep_abort(ep, ECONNABORTED);
		iwc_ep_rele(ep);
	}
	mutex_enter(&qp->qp_lock);
	while (qp->qp_ep != NULL) {
		if (cv_reltimedwait(&qp->qp_cv, &qp->qp_lock,
		    SEC_TO_TICK(IWC_QP_WARN_SEC), TR_SEC) == -1 &&
		    qp->qp_ep != NULL) {
			iwc_warn(iwc, "QP %u: waiting for its connection to "
			    "end", wq->sq.qid);
		}
	}
	mutex_exit(&qp->qp_lock);
	iwc_flush_qp(qp);

	mutex_enter(&iwc->iwc_obj_lock);
	iwc->iwc_qps[wq->sq.qid - iwc->iwc_qid_start] = NULL;
	while (qp->qp_refs != 0)
		cv_wait(&iwc->iwc_obj_cv, &iwc->iwc_obj_lock);
	mutex_exit(&iwc->iwc_obj_lock);

	ret = iwc->iwc_ops->tro_qp_destroy(iwc->iwc_peer, wq->sq.qid);
	if (ret == 0) {
		iwc_qid_free(iwc, wq->rq.qid);
		iwc_qid_free(iwc, wq->sq.qid);
		iwc_rqt_free(iwc, wq->rq.rqt_hwaddr, wq->rq.rqt_size);
	} else {
		/* The firmware may still own the queues and the RQT. */
		IWC_STAT(iwc, is_quar);
		iwc_taint(iwc);
	}
	kmem_free(wq->sq.sw_sq, wq->sq.size * sizeof (t4_swsqe_t));
	kmem_free(wq->rq.sw_rq, wq->rq.size * sizeof (t4_swrqe_t));
	cv_destroy(&qp->qp_cv);
	mutex_destroy(&qp->qp_lock);
}

/*
 * Write scatter entries into the ring, wrapping at its end.  EMSGSIZE if
 * the lengths overflow.
 */
static int
iwc_build_isgl(uint64_t *qstart, uint64_t *qend, struct fw_ri_isgl *isglp,
    const struct rdk_sge *sg, int nsge, uint32_t *plenp)
{
	uint64_t *flit;
	uint32_t plen = 0;
	int i;

	if ((uint64_t *)isglp == qend)
		isglp = (struct fw_ri_isgl *)qstart;
	flit = (uint64_t *)isglp->sge;
	for (i = 0; i < nsge; i++) {
		if (plen + sg[i].length < plen)
			return (EMSGSIZE);
		plen += sg[i].length;
		*flit = BE_64(((uint64_t)sg[i].lkey << 32) | sg[i].length);
		if (++flit == qend)
			flit = qstart;
		*flit = BE_64(sg[i].addr);
		if (++flit == qend)
			flit = qstart;
	}
	*flit = 0;
	isglp->op = FW_RI_DATA_ISGL;
	isglp->r1 = 0;
	isglp->nsge = BE_16((uint16_t)nsge);
	isglp->r2 = 0;
	if (plenp != NULL)
		*plenp = plen;
	return (0);
}

static void
iwc_zero_immd(struct fw_ri_immd *immd)
{
	immd->op = FW_RI_DATA_IMMD;
	immd->r1 = 0;
	immd->r2 = 0;
	immd->immdlen = 0;
}

static int
iwc_build_send(t4_sq_t *sq, union t4_wr *wqe, const struct rdk_send_wr *wr,
    uint8_t *len16)
{
	uint64_t *qs = (uint64_t *)sq->queue;
	uint64_t *qe = (uint64_t *)&sq->queue[sq->size];
	const boolean_t se = (wr->send_flags & RDK_SEND_SOLICITED) != 0;
	uint32_t plen = 0;
	size_t size;
	int ret;

	if (wr->num_sge < 0 || wr->num_sge > (int)T4_MAX_SEND_SGE)
		return (EINVAL);
	if (wr->opcode == RDK_WR_SEND) {
		wqe->send.sendop_pkd = BE_32(V_FW_RI_SEND_WR_SENDOP(se ?
		    FW_RI_SEND_WITH_SE : FW_RI_SEND));
		wqe->send.stag_inv = 0;
	} else {
		wqe->send.sendop_pkd = BE_32(V_FW_RI_SEND_WR_SENDOP(se ?
		    FW_RI_SEND_WITH_SE_INV : FW_RI_SEND_WITH_INV));
		wqe->send.stag_inv = BE_32(wr->ex.invalidate_rkey);
	}
	wqe->send.r3 = 0;
	wqe->send.r4 = 0;
	if (wr->num_sge != 0) {
		if ((ret = iwc_build_isgl(qs, qe, wqe->send.u.isgl_src,
		    wr->sg_list, wr->num_sge, &plen)) != 0)
			return (ret);
		size = sizeof (wqe->send) + sizeof (struct fw_ri_isgl) +
		    wr->num_sge * sizeof (struct fw_ri_sge);
	} else {
		iwc_zero_immd(wqe->send.u.immd_src);
		size = sizeof (wqe->send) + sizeof (struct fw_ri_immd);
	}
	*len16 = (uint8_t)howmany(size, 16);
	wqe->send.plen = BE_32(plen);
	return (0);
}

static int
iwc_build_write(t4_sq_t *sq, union t4_wr *wqe, const struct rdk_send_wr *wr,
    uint8_t *len16)
{
	const struct rdk_rdma_wr *rw = RDK_RDMA_WR(wr);
	uint64_t *qs = (uint64_t *)sq->queue;
	uint64_t *qe = (uint64_t *)&sq->queue[sq->size];
	uint32_t plen = 0;
	size_t size;
	int ret;

	if (wr->num_sge < 0 || wr->num_sge > (int)T4_MAX_WRITE_SGE)
		return (EINVAL);
	wqe->write.immd_data = 0;
	if (wr->opcode == RDK_WR_RDMA_WRITE_WITH_IMM)
		bcopy(&wr->ex.imm_data, &wqe->write.immd_data,
		    sizeof (wr->ex.imm_data));
	wqe->write.stag_sink = BE_32(rw->rkey);
	wqe->write.to_sink = BE_64(rw->remote_addr);
	if (wr->num_sge != 0) {
		if ((ret = iwc_build_isgl(qs, qe, wqe->write.u.isgl_src,
		    wr->sg_list, wr->num_sge, &plen)) != 0)
			return (ret);
		size = sizeof (wqe->write) + sizeof (struct fw_ri_isgl) +
		    wr->num_sge * sizeof (struct fw_ri_sge);
	} else {
		iwc_zero_immd(wqe->write.u.immd_src);
		size = sizeof (wqe->write) + sizeof (struct fw_ri_immd);
	}
	*len16 = (uint8_t)howmany(size, 16);
	wqe->write.plen = BE_32(plen);
	return (0);
}

static int
iwc_build_read(union t4_wr *wqe, const struct rdk_send_wr *wr, uint8_t *len16)
{
	const struct rdk_rdma_wr *rw = RDK_RDMA_WR(wr);

	if (wr->num_sge < 0 || wr->num_sge > 1)
		return (EINVAL);
	if (wr->num_sge == 1 && wr->sg_list[0].length != 0) {
		wqe->read.stag_src = BE_32(rw->rkey);
		wqe->read.to_src_hi = BE_32((uint32_t)(rw->remote_addr >> 32));
		wqe->read.to_src_lo = BE_32((uint32_t)rw->remote_addr);
		wqe->read.stag_sink = BE_32(wr->sg_list[0].lkey);
		wqe->read.plen = BE_32(wr->sg_list[0].length);
		wqe->read.to_sink_hi = BE_32((uint32_t)(wr->sg_list[0].addr >>
		    32));
		wqe->read.to_sink_lo = BE_32((uint32_t)wr->sg_list[0].addr);
	} else {
		/* A zero-length read, which the firmware wants as STag 2. */
		wqe->read.stag_src = BE_32(2);
		wqe->read.to_src_hi = 0;
		wqe->read.to_src_lo = 0;
		wqe->read.stag_sink = BE_32(2);
		wqe->read.plen = 0;
		wqe->read.to_sink_hi = 0;
		wqe->read.to_sink_lo = 0;
	}
	wqe->read.r2 = 0;
	wqe->read.r5 = 0;
	*len16 = (uint8_t)howmany(sizeof (wqe->read), 16);
	return (0);
}

/* A fast registration with the page list inline in the work request. */
static int
iwc_build_memreg(iwc_qp_t *qp, union t4_wr *wqe, const struct rdk_send_wr *wr,
    uint8_t *len16)
{
	const struct rdk_reg_wr *rw = RDK_REG_WR(wr);
	t4_sq_t *sq = &qp->qp_wq.sq;
	iwc_mr_t *mr = (iwc_mr_t *)rw->mr;
	struct fw_ri_immd *imdp;
	struct fw_ri_dsgl *sglp;
	uint64_t *p, *qe = (uint64_t *)&sq->queue[sq->size];
	uint32_t pbllen, i;
	int rem;

	if (mr == NULL || mr->mr_npages == 0 || mr->mr_npages > mr->mr_max ||
	    mr->mr_npages > IWC_FR_DEPTH(qp->qp_iwc) ||
	    !ISP2(mr->mr_rdk.page_size) || mr->mr_rdk.page_size < 4096 ||
	    (rw->key >> 8) != (mr->mr_stag >> 8))
		return (EINVAL);
	pbllen = roundup(mr->mr_npages * sizeof (uint64_t), 32);
	wqe->fr.qpbinde_to_dcacpu = 0;
	wqe->fr.pgsz_shift = (uint8_t)(highbit(mr->mr_rdk.page_size) - 1 - 12);
	wqe->fr.addr_type = FW_RI_VA_BASED_TO;
	wqe->fr.mem_perms = (uint8_t)iwc_tpt_perms(rw->access);
	wqe->fr.len_hi = BE_32((uint32_t)(mr->mr_rdk.length >> 32));
	wqe->fr.len_lo = BE_32((uint32_t)mr->mr_rdk.length);
	wqe->fr.stag = BE_32(rw->key);
	wqe->fr.va_hi = BE_32((uint32_t)(mr->mr_rdk.iova >> 32));
	wqe->fr.va_lo_fbo = BE_32((uint32_t)mr->mr_rdk.iova);

	if (pbllen > T4_MAX_FR_IMMD) {
		/* The chip reads the list from this slot's area. */
		const size_t off = qp->qp_pbl_off +
		    (size_t)sq->pidx * T4_MAX_FR_DSGL;

		if (qp->qp_pbl_off == 0 || pbllen > T4_MAX_FR_DSGL)
			return (EINVAL);
		p = (uint64_t *)(qp->qp_sqmem->trd_va + off);
		for (i = 0; i < mr->mr_npages; i++)
			p[i] = BE_64(mr->mr_pages[i]);
		for (; i < pbllen / sizeof (uint64_t); i++)
			p[i] = 0;
		sglp = (struct fw_ri_dsgl *)(&wqe->fr + 1);
		sglp->op = FW_RI_DATA_DSGL;
		sglp->r1 = 0;
		sglp->nsge = BE_16(1);
		sglp->addr0 = BE_64(qp->qp_sqmem->trd_pa + off);
		sglp->len0 = BE_32(pbllen);
		*len16 = (uint8_t)howmany(sizeof (wqe->fr) + sizeof (*sglp),
		    16);
		return (0);
	}

	imdp = (struct fw_ri_immd *)(&wqe->fr + 1);
	imdp->op = FW_RI_DATA_IMMD;
	imdp->r1 = 0;
	imdp->r2 = 0;
	imdp->immdlen = BE_32(pbllen);
	p = (uint64_t *)(imdp + 1);
	rem = (int)pbllen;
	for (i = 0; i < mr->mr_npages; i++) {
		*p = BE_64(mr->mr_pages[i]);
		rem -= sizeof (*p);
		if (++p == qe)
			p = (uint64_t *)sq->queue;
	}
	while (rem > 0) {
		*p = 0;
		rem -= sizeof (*p);
		if (++p == qe)
			p = (uint64_t *)sq->queue;
	}
	*len16 = (uint8_t)howmany(sizeof (wqe->fr) + sizeof (*imdp) + pbllen,
	    16);
	return (0);
}

static void
iwc_init_wr_hdr(union t4_wr *wqe, uint16_t wrid, uint8_t opcode, uint8_t flags,
    uint8_t len16)
{
	wqe->send.opcode = opcode;
	wqe->send.flags = flags;
	wqe->send.wrid = wrid;
	wqe->send.r1[0] = 0;
	wqe->send.r1[1] = 0;
	wqe->send.r1[2] = 0;
	wqe->send.len16 = len16;
}

static int
iwc_fw_opcode(enum rdk_wr_opcode op)
{
	switch (op) {
	case RDK_WR_SEND:
		return (FW_RI_SEND);
	case RDK_WR_SEND_WITH_INV:
		return (FW_RI_SEND_WITH_INV);
	case RDK_WR_RDMA_WRITE:
		return (FW_RI_RDMA_WRITE);
	case RDK_WR_RDMA_WRITE_WITH_IMM:
		return (FW_RI_WRITE_IMMEDIATE);
	case RDK_WR_RDMA_READ:
		return (FW_RI_READ_REQ);
	case RDK_WR_REG_MR:
		return (FW_RI_FAST_REGISTER);
	case RDK_WR_LOCAL_INV:
		return (FW_RI_LOCAL_INV);
	default:
		return (-1);
	}
}

int
iwc_post_send(struct rdk_qp *rqp, const struct rdk_send_wr *wr,
    const struct rdk_send_wr **bad)
{
	iwc_qp_t *qp = (iwc_qp_t *)rqp;
	iwc_t *iwc = qp->qp_iwc;
	t4_wq_t *wq = &qp->qp_wq;
	iwc_cq_t *scq = (iwc_cq_t *)rqp->send_cq;
	t4_swsqe_t *swsqe;
	union t4_wr *wqe;
	uint16_t idx = 0;
	uint8_t len16, fwop, flags;
	uint32_t avail;
	int op, ret = 0;

	mutex_enter(&qp->qp_lock);
	if (wq->flushed) {
		/* Each request completes at once as a flushed drain. */
		mutex_exit(&qp->qp_lock);
		for (; wr != NULL; wr = wr->next) {
			if ((op = iwc_fw_opcode(wr->opcode)) < 0) {
				*bad = wr;
				return (EINVAL);
			}
			iwc_cq_insert_drain(scq, qp, wr->wr_id, (uint8_t)op,
			    B_TRUE);
		}
		return (0);
	}
	if (qp->qp_state != IWC_QPS_RTS || iwc->iwc_fatal) {
		mutex_exit(&qp->qp_lock);
		*bad = wr;
		return (EINVAL);
	}
	avail = t4_sq_avail(wq);
	for (; wr != NULL; wr = wr->next) {
		if (avail == 0) {
			ret = ENOMEM;
			break;
		}
		if (wr->num_sge > (int)qp->qp_sq_max_sge) {
			ret = EINVAL;
			break;
		}
		wqe = (union t4_wr *)((caddr_t)wq->sq.queue +
		    (size_t)wq->sq.wq_pidx * T4_EQ_ENTRY_SIZE);
		swsqe = &wq->sq.sw_sq[wq->sq.pidx];
		flags = 0;
		if ((wr->send_flags & RDK_SEND_SOLICITED) != 0)
			flags |= FW_RI_SOLICITED_EVENT_FLAG;
		if ((wr->send_flags & RDK_SEND_SIGNALED) != 0 || qp->qp_sig_all)
			flags |= FW_RI_COMPLETION_FLAG;
		bzero(swsqe, sizeof (*swsqe));
		switch (wr->opcode) {
		case RDK_WR_SEND:
		case RDK_WR_SEND_WITH_INV:
			if ((wr->send_flags & RDK_SEND_FENCE) != 0)
				flags |= FW_RI_READ_FENCE_FLAG;
			fwop = FW_RI_SEND_WR;
			swsqe->opcode = wr->opcode == RDK_WR_SEND ?
			    FW_RI_SEND : FW_RI_SEND_WITH_INV;
			ret = iwc_build_send(&wq->sq, wqe, wr, &len16);
			break;
		case RDK_WR_RDMA_WRITE_WITH_IMM:
			if (!iwc->iwc_info.tri_vres.trv_write_w_imm) {
				ret = EINVAL;
				break;
			}
			flags |= FW_RI_RDMA_WRITE_WITH_IMMEDIATE;
			/* FALLTHROUGH */
		case RDK_WR_RDMA_WRITE:
			fwop = FW_RI_RDMA_WRITE_WR;
			swsqe->opcode = FW_RI_RDMA_WRITE;
			ret = iwc_build_write(&wq->sq, wqe, wr, &len16);
			break;
		case RDK_WR_RDMA_READ:
			fwop = FW_RI_RDMA_READ_WR;
			swsqe->opcode = FW_RI_READ_REQ;
			/*
			 * The read response completes a read.  A hardware
			 * completion would come when the request is sent,
			 * before the data is placed.
			 */
			flags = 0;
			ret = iwc_build_read(wqe, wr, &len16);
			if (ret != 0)
				break;
			swsqe->read_len = wr->num_sge == 1 ?
			    wr->sg_list[0].length : 0;
			if (wq->sq.oldest_read == NULL)
				wq->sq.oldest_read = swsqe;
			break;
		case RDK_WR_REG_MR:
			fwop = FW_RI_FR_NSMR_WR;
			swsqe->opcode = FW_RI_FAST_REGISTER;
			ret = iwc_build_memreg(qp, wqe, wr, &len16);
			break;
		case RDK_WR_LOCAL_INV:
			if ((wr->send_flags & RDK_SEND_FENCE) != 0)
				flags |= FW_RI_LOCAL_FENCE_FLAG;
			fwop = FW_RI_INV_LSTAG_WR;
			swsqe->opcode = FW_RI_LOCAL_INV;
			wqe->inv.stag_inv = BE_32(wr->ex.invalidate_rkey);
			wqe->inv.r2 = 0;
			len16 = (uint8_t)howmany(sizeof (wqe->inv), 16);
			break;
		default:
			ret = EINVAL;
			break;
		}
		if (ret != 0)
			break;
		swsqe->idx = wq->sq.pidx;
		swsqe->signaled = (wr->send_flags & RDK_SEND_SIGNALED) != 0 ||
		    qp->qp_sig_all;
		swsqe->wr_id = wr->wr_id;
		iwc_init_wr_hdr(wqe, wq->sq.pidx, fwop, flags, len16);
		t4_sq_produce(wq, len16);
		idx += howmany(len16 * 16, T4_EQ_ENTRY_SIZE);
		avail--;
	}
	if (idx != 0) {
		iwc_ring_sq(qp, idx);
		atomic_inc_64(&qp->qp_iwc->iwc_vecs[((iwc_cq_t *)
		    qp->qp_rdk.send_cq)->cq_vec].iv_sq_db);
	}
	mutex_exit(&qp->qp_lock);
	if (ret != 0)
		*bad = wr;
	return (ret);
}

int
iwc_post_recv(struct rdk_qp *rqp, const struct rdk_recv_wr *wr,
    const struct rdk_recv_wr **bad)
{
	iwc_qp_t *qp = (iwc_qp_t *)rqp;
	t4_wq_t *wq = &qp->qp_wq;
	iwc_cq_t *rcq = (iwc_cq_t *)rqp->recv_cq;
	union t4_recv_wr *wqe;
	uint16_t idx = 0;
	uint8_t len16;
	uint32_t avail;
	int ret = 0;

	mutex_enter(&qp->qp_lock);
	if (wq->flushed) {
		mutex_exit(&qp->qp_lock);
		for (; wr != NULL; wr = wr->next)
			iwc_cq_insert_drain(rcq, qp, wr->wr_id, FW_RI_SEND,
			    B_FALSE);
		return (0);
	}
	if (qp->qp_state != IWC_QPS_IDLE && qp->qp_state != IWC_QPS_RTS) {
		mutex_exit(&qp->qp_lock);
		*bad = wr;
		return (EINVAL);
	}
	avail = t4_rq_avail(wq);
	for (; wr != NULL; wr = wr->next) {
		if (avail == 0) {
			ret = ENOMEM;
			break;
		}
		if (wr->num_sge < 0 || wr->num_sge > (int)qp->qp_rq_max_sge) {
			ret = EINVAL;
			break;
		}
		wqe = (union t4_recv_wr *)((caddr_t)wq->rq.queue +
		    (size_t)wq->rq.wq_pidx * T4_EQ_ENTRY_SIZE);
		if ((ret = iwc_build_isgl((uint64_t *)wq->rq.queue,
		    (uint64_t *)&wq->rq.queue[wq->rq.size], &wqe->recv.isgl,
		    wr->sg_list, wr->num_sge, NULL)) != 0)
			break;
		len16 = (uint8_t)howmany(sizeof (wqe->recv) +
		    wr->num_sge * sizeof (struct fw_ri_sge), 16);
		wq->rq.sw_rq[wq->rq.pidx].wr_id = wr->wr_id;
		wqe->recv.opcode = FW_RI_RECV_WR;
		wqe->recv.r1 = 0;
		wqe->recv.wrid = wq->rq.pidx;
		wqe->recv.r2[0] = 0;
		wqe->recv.r2[1] = 0;
		wqe->recv.r2[2] = 0;
		wqe->recv.len16 = len16;
		t4_rq_produce(wq, len16);
		idx += howmany(len16 * 16, T4_EQ_ENTRY_SIZE);
		avail--;
	}
	if (idx != 0)
		iwc_ring_rq(qp, idx);
	mutex_exit(&qp->qp_lock);
	if (ret != 0)
		*bad = wr;
	return (ret);
}

/* RFC 5040 TERMINATE layers and error types, and the codes used here. */
#define	TERM_RDMAP		0x00
#define	TERM_DDP		0x10
#define	TERM_MPA		0x20
#define	TERM_LOCAL_CATA		0x00
#define	TERM_REMOTE_PROT	0x01	/* RDMAP */
#define	TERM_REMOTE_OP		0x02	/* RDMAP */
#define	TERM_DDP_TAGGED		0x01
#define	TERM_DDP_UNTAGGED	0x02
#define	TERM_DDP_LLP		0x03

/*
 * The TERMINATE for an asynchronous error CQE, as Linux build_term_codes()
 * maps it.
 */
void
iwc_term_codes(const t4_cqe_t *cqe, uint8_t *layer, uint8_t *ecode)
{
	const uint_t op = CQE_OPCODE(cqe);
	const boolean_t send_inv = op == FW_RI_SEND_WITH_INV ||
	    op == FW_RI_SEND_WITH_SE_INV;
	const boolean_t tagged = op == FW_RI_RDMA_WRITE ||
	    (!CQE_SQ(cqe) && op == FW_RI_READ_RESP);

	*layer = TERM_RDMAP | TERM_LOCAL_CATA;
	*ecode = 0;
	switch (CQE_STATUS(cqe)) {
	case T4_ERR_STAG:
		*layer = TERM_RDMAP | (send_inv ? TERM_REMOTE_OP :
		    TERM_REMOTE_PROT);
		*ecode = send_inv ? 0x09 : 0x00;
		break;
	case T4_ERR_PDID:
		*layer = TERM_RDMAP | TERM_REMOTE_PROT;
		*ecode = send_inv ? 0x09 : 0x03;
		break;
	case T4_ERR_QPID:
		*layer = TERM_RDMAP | TERM_REMOTE_PROT;
		*ecode = 0x03;
		break;
	case T4_ERR_ACCESS:
		*layer = TERM_RDMAP | TERM_REMOTE_PROT;
		*ecode = 0x02;
		break;
	case T4_ERR_WRAP:
		*layer = TERM_RDMAP | TERM_REMOTE_PROT;
		*ecode = 0x04;
		break;
	case T4_ERR_BOUND:
		*layer = tagged ? TERM_DDP | TERM_DDP_TAGGED :
		    TERM_RDMAP | TERM_REMOTE_PROT;
		*ecode = 0x01;
		break;
	case T4_ERR_INVALIDATE_SHARED_MR:
	case T4_ERR_INVALIDATE_MR_WITH_MW_BOUND:
		*layer = TERM_RDMAP | TERM_REMOTE_OP;
		*ecode = 0x09;
		break;
	case T4_ERR_OUT_OF_RQE:
		*layer = TERM_DDP | TERM_DDP_UNTAGGED;
		*ecode = 0x02;
		break;
	case T4_ERR_PBL_ADDR_BOUND:
		*layer = TERM_DDP | TERM_DDP_TAGGED;
		*ecode = 0x01;
		break;
	case T4_ERR_CRC:
		*layer = TERM_MPA | TERM_DDP_LLP;
		*ecode = 0x02;
		break;
	case T4_ERR_MARKER:
		*layer = TERM_MPA | TERM_DDP_LLP;
		*ecode = 0x03;
		break;
	case T4_ERR_PDU_LEN_ERR:
		*layer = TERM_DDP | TERM_DDP_UNTAGGED;
		*ecode = 0x05;
		break;
	case T4_ERR_DDP_VERSION:
		*layer = TERM_DDP | (tagged ? TERM_DDP_TAGGED :
		    TERM_DDP_UNTAGGED);
		*ecode = tagged ? 0x04 : 0x06;
		break;
	case T4_ERR_RDMA_VERSION:
		*layer = TERM_RDMAP | TERM_REMOTE_OP;
		*ecode = 0x05;
		break;
	case T4_ERR_OPCODE:
		*layer = TERM_RDMAP | TERM_REMOTE_OP;
		*ecode = 0x06;
		break;
	case T4_ERR_DDP_QUEUE_NUM:
		*layer = TERM_DDP | TERM_DDP_UNTAGGED;
		*ecode = 0x01;
		break;
	case T4_ERR_MSN:
	case T4_ERR_MSN_GAP:
	case T4_ERR_MSN_RANGE:
	case T4_ERR_IRD_OVERFLOW:
		*layer = TERM_DDP | TERM_DDP_UNTAGGED;
		*ecode = 0x03;
		break;
	case T4_ERR_TBIT:
		*layer = TERM_DDP | TERM_LOCAL_CATA;
		break;
	case T4_ERR_MO:
		*layer = TERM_DDP | TERM_DDP_UNTAGGED;
		*ecode = 0x04;
		break;
	default:
		break;
	}
}

/*
 * An asynchronous error the firmware reported for a QP (Linux ev.c): tell
 * the consumer and end the connection.  CM taskq.
 */
void
iwc_qp_async(iwc_t *iwc, const t4_cqe_t *cqe)
{
	struct rdk_event ev;
	uint8_t layer, ecode;
	iwc_qp_t *qp;
	iwc_ep_t *ep;

	IWC_STAT(iwc, is_async_err);
	if ((qp = iwc_qp_get(iwc, CQE_QPID(cqe))) == NULL)
		return;
	bzero(&ev, sizeof (ev));
	ev.device = qp->qp_rdk.device;
	ev.element.qp = &qp->qp_rdk;
	switch (CQE_STATUS(cqe)) {
	case T4_ERR_STAG:
	case T4_ERR_PDID:
	case T4_ERR_QPID:
	case T4_ERR_ACCESS:
	case T4_ERR_WRAP:
	case T4_ERR_BOUND:
	case T4_ERR_INVALIDATE_SHARED_MR:
	case T4_ERR_INVALIDATE_MR_WITH_MW_BOUND:
		ev.event = RDK_EVENT_QP_ACCESS_ERR;
		break;
	case T4_ERR_CRC:
	case T4_ERR_MARKER:
	case T4_ERR_PDU_LEN_ERR:
	case T4_ERR_DDP_VERSION:
	case T4_ERR_RDMA_VERSION:
	case T4_ERR_OPCODE:
	case T4_ERR_DDP_QUEUE_NUM:
	case T4_ERR_MSN:
	case T4_ERR_TBIT:
	case T4_ERR_MO:
	case T4_ERR_MSN_GAP:
	case T4_ERR_MSN_RANGE:
	case T4_ERR_RQE_ADDR_BOUND:
	case T4_ERR_IRD_OVERFLOW:
		ev.event = RDK_EVENT_QP_REQ_ERR;
		break;
	default:
		ev.event = RDK_EVENT_QP_FATAL;
		break;
	}
	if (qp->qp_rdk.event_handler != NULL)
		qp->qp_rdk.event_handler(&ev, qp->qp_rdk.qp_context);

	mutex_enter(&qp->qp_lock);
	ep = qp->qp_ep;
	if (ep != NULL)
		iwc_ep_hold(ep);
	mutex_exit(&qp->qp_lock);
	if (ep != NULL) {
		iwc_term_codes(cqe, &layer, &ecode);
		iwc_ep_terminate(ep, layer, ecode);
		iwc_ep_rele(ep);
	}
	iwc_qp_put(iwc, qp);
}

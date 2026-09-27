/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2015 - 2021 Intel Corporation */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Queue pairs: create, modify, query, destroy, flush and error handling.
 * The logic follows irdma_create_qp(), irdma_modify_qp_roce(),
 * irdma_destroy_qp(), irdma_flush_wqes() and
 * irdma_generate_flush_completions() of Linux irdma (see README.illumos).
 *
 * A QP is in the QP table from create until destroy has told the device to
 * drop it.  The table holds one reference; the AEQ and the deferred work
 * take more while they use the QP.  Work that may wait for the device (the
 * move to the error state after an asynchronous error, and the flush
 * completions the driver generates) runs on irdma_wq, never in the
 * interrupt task.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>

#include "irdma_verbs.h"

static void irdma_qp_err_task(void *);
static void irdma_qp_flush_task(void *);

irdma_qp_t *
irdma_qp_get(irdma_t *irdma, uint32_t qpn)
{
	irdma_qp_t *iqp;

	if (!irdma->irdma_verbs_live || qpn >= irdma->irdma_max_qp)
		return (NULL);
	mutex_enter(&irdma->irdma_qptable_lock);
	if ((iqp = irdma->irdma_qp_table[qpn]) != NULL)
		iqp->iqp_refs++;
	mutex_exit(&irdma->irdma_qptable_lock);
	return (iqp);
}

void
irdma_qp_rele(irdma_qp_t *iqp)
{
	irdma_t *irdma = iqp->iqp_irdma;

	mutex_enter(&irdma->irdma_qptable_lock);
	VERIFY3U(iqp->iqp_refs, >, 0);
	if (--iqp->iqp_refs == 0)
		cv_broadcast(&iqp->iqp_ref_cv);
	mutex_exit(&irdma->irdma_qptable_lock);
}

void
irdma_qp_event(irdma_qp_t *iqp, enum irdma_qp_event_type type)
{
	struct rdk_qp *rqp = &iqp->iqp_rdk;
	struct rdk_event ev;

	if (rqp->event_handler == NULL)
		return;
	bzero(&ev, sizeof (ev));
	switch (type) {
	case IRDMA_QP_EVENT_ACCESS_ERR:
		ev.event = RDK_EVENT_QP_ACCESS_ERR;
		break;
	case IRDMA_QP_EVENT_REQ_ERR:
		ev.event = RDK_EVENT_QP_REQ_ERR;
		break;
	case IRDMA_QP_EVENT_CATASTROPHIC:
	default:
		ev.event = RDK_EVENT_QP_FATAL;
		break;
	}
	ev.device = rqp->device;
	ev.element.qp = rqp;
	rqp->event_handler(&ev, rqp->qp_context);
}

/*
 * Queue deferred work.  The caller holds iqp_lock; a QP being destroyed
 * takes no more work.
 */
static boolean_t
irdma_qp_queue(irdma_qp_t *iqp, void (*fn)(void *))
{
	irdma_t *irdma = iqp->iqp_irdma;

	ASSERT(MUTEX_HELD(&iqp->iqp_lock));
	if (iqp->iqp_destroying)
		return (B_FALSE);
	iqp->iqp_work++;
	if (ddi_taskq_dispatch(irdma->irdma_wq, fn, iqp, DDI_NOSLEEP) !=
	    DDI_SUCCESS) {
		iqp->iqp_work--;
		return (B_FALSE);
	}
	return (B_TRUE);
}

static void
irdma_qp_work_done(irdma_qp_t *iqp)
{
	mutex_enter(&iqp->iqp_lock);
	if (--iqp->iqp_work == 0)
		cv_broadcast(&iqp->iqp_cv);
	mutex_exit(&iqp->iqp_lock);
}

/*
 * An asynchronous event moved the QP to an error: take it to the error
 * state, which flushes it, and tell the consumer.  From the AEQ task.
 */
void
irdma_qp_to_error(irdma_qp_t *iqp)
{
	mutex_enter(&iqp->iqp_lock);
	if (!iqp->iqp_err_queued && !iqp->iqp_flush_issued &&
	    irdma_qp_queue(iqp, irdma_qp_err_task))
		iqp->iqp_err_queued = B_TRUE;
	mutex_exit(&iqp->iqp_lock);
}

static void
irdma_qp_err_task(void *arg)
{
	irdma_qp_t *iqp = arg;
	struct rdk_qp_attr attr;

	bzero(&attr, sizeof (attr));
	attr.qp_state = RDK_QPS_ERR;
	(void) irdma_modify_qp(&iqp->iqp_rdk, &attr, RDK_QP_STATE);
	atomic_inc_64(&iqp->iqp_irdma->irdma_qp_errors);
	irdma_qp_event(iqp, iqp->iqp_sc.event_type);
	mutex_enter(&iqp->iqp_lock);
	iqp->iqp_err_queued = B_FALSE;
	mutex_exit(&iqp->iqp_lock);
	irdma_qp_work_done(iqp);
}

static void
irdma_qp_flush_timeout(void *arg)
{
	irdma_qp_t *iqp = arg;

	mutex_enter(&iqp->iqp_lock);
	iqp->iqp_flush_tid = 0;
	if (!iqp->iqp_flush_queued &&
	    irdma_qp_queue(iqp, irdma_qp_flush_task))
		iqp->iqp_flush_queued = B_TRUE;
	mutex_exit(&iqp->iqp_lock);
}

/*
 * The device flushes only the work queued when the flush was issued; later
 * work requests get completions made by the driver, after a delay that lets
 * the device's own flushed entries arrive first.
 */
void
irdma_flush_later(irdma_qp_t *iqp)
{
	mutex_enter(&iqp->iqp_lock);
	if (!iqp->iqp_destroying && iqp->iqp_flush_tid == 0 &&
	    !iqp->iqp_flush_queued) {
		iqp->iqp_flush_tid = timeout(irdma_qp_flush_timeout, iqp,
		    drv_usectohz(IRDMA_FLUSH_DELAY_MS * MILLISEC));
	}
	mutex_exit(&iqp->iqp_lock);
}

static void
irdma_qp_flush_task(void *arg)
{
	irdma_qp_t *iqp = arg;
	boolean_t later;

	later = irdma_generate_flush_completions(iqp);
	mutex_enter(&iqp->iqp_lock);
	iqp->iqp_flush_queued = B_FALSE;
	mutex_exit(&iqp->iqp_lock);
	if (later)
		irdma_flush_later(iqp);
	irdma_qp_work_done(iqp);
}

static void
irdma_set_cpi(struct irdma_cq_poll_info *cpi, irdma_qp_t *iqp)
{
	bzero(cpi, sizeof (*cpi));
	cpi->comp_status = IRDMA_COMPL_STATUS_FLUSHED;
	cpi->error = true;
	cpi->major_err = IRDMA_FLUSH_MAJOR_ERR;
	cpi->minor_err = FLUSH_GENERAL_ERR;
	cpi->qp_handle = (irdma_qp_handle)&iqp->iqp_sc;
	cpi->qp_id = iqp->iqp_sc.qp_uk.qp_id;
}

/*
 * Complete, with a flush error, the work requests the device did not
 * flush.  Returns whether to try again later: the device's own entries are
 * still in a CQ, or memory ran out.
 */
boolean_t
irdma_generate_flush_completions(irdma_qp_t *iqp)
{
	struct irdma_qp_uk *qp = &iqp->iqp_sc.qp_uk;
	struct irdma_ring *sq = &qp->sq_ring, *rq = &qp->rq_ring;
	irdma_cq_t *cqs[2] = { iqp->iqp_scq, iqp->iqp_rcq };
	boolean_t made[2] = { B_FALSE, B_FALSE };
	boolean_t later = B_FALSE;
	irdma_cmpl_gen_t *g = NULL;
	uint32_t idx, n;
	uint_t i;
	u64 qword;

	for (i = 0; i < 2; i++) {
		irdma_cq_t *icq = cqs[i];

		mutex_enter(&icq->icq_lock);
		if (!irdma_cq_empty(icq)) {
			mutex_exit(&icq->icq_lock);
			later = B_TRUE;
			continue;
		}
		mutex_enter(&iqp->iqp_lock);
		for (n = 0; i == 0 && IRDMA_RING_MORE_WORK(*sq) &&
		    n < sq->size; n++) {
			if (g == NULL)
				g = kmem_zalloc(sizeof (*g), KM_NOSLEEP);
			if (g == NULL) {
				later = B_TRUE;
				break;
			}
			idx = sq->tail;
			get_64bit_val(qp->sq_base[idx].elem, 24, &qword);
			IRDMA_RING_SET_TAIL(*sq, idx +
			    MAX(qp->sq_wrtrk_array[idx].quanta, 1));
			if (FIELD_GET(IRDMAQPSQ_OPCODE, qword) ==
			    IRDMAQP_OP_NOP)
				continue;
			irdma_set_cpi(&g->icg_cpi, iqp);
			g->icg_cpi.wr_id = qp->sq_wrtrk_array[idx].wrid;
			g->icg_cpi.op_type = (u8)FIELD_GET(IRDMAQPSQ_OPCODE,
			    qword);
			g->icg_cpi.q_type = IRDMA_CQE_QTYPE_SQ;
			list_insert_tail(&icq->icq_gen, g);
			g = NULL;
			made[i] = B_TRUE;
		}
		for (n = 0; i == 1 && IRDMA_RING_MORE_WORK(*rq) &&
		    n < rq->size; n++) {
			if (g == NULL)
				g = kmem_zalloc(sizeof (*g), KM_NOSLEEP);
			if (g == NULL) {
				later = B_TRUE;
				break;
			}
			idx = rq->tail;
			irdma_set_cpi(&g->icg_cpi, iqp);
			g->icg_cpi.wr_id = qp->rq_wrid_array[idx];
			g->icg_cpi.op_type = IRDMA_OP_TYPE_REC;
			g->icg_cpi.q_type = IRDMA_CQE_QTYPE_RQ;
			IRDMA_RING_SET_TAIL(*rq, idx + 1);
			list_insert_tail(&icq->icq_gen, g);
			g = NULL;
			made[i] = B_TRUE;
		}
		mutex_exit(&iqp->iqp_lock);
		mutex_exit(&icq->icq_lock);
	}
	if (g != NULL)
		kmem_free(g, sizeof (*g));

	for (i = 0; i < 2; i++) {
		if (made[i])
			irdma_comp_handler(cqs[i]);
	}
	return (later);
}

/* A CQP QP_MODIFY with info; the caller holds iqp_mod_lock. */
static int
irdma_hw_modify_qp(irdma_qp_t *iqp, struct irdma_modify_qp_info *info)
{
	irdma_t *irdma = iqp->iqp_irdma;
	irdma_cqp_req_t *req;

	ASSERT(MUTEX_HELD(&iqp->iqp_mod_lock));
	if ((req = irdma_vreq(irdma, IRDMA_OP_QP_MODIFY)) == NULL)
		return (EIO);
	req->icr_cmd.in.u.qp_modify.info = *info;
	req->icr_cmd.in.u.qp_modify.qp = &iqp->iqp_sc;
	req->icr_cmd.in.u.qp_modify.scratch = irdma_req_scratch(irdma, req);
	return (irdma_cqp_exec(irdma, req, NULL));
}

/*
 * Ask the device to flush the queues.  A later work request is completed
 * by the driver (irdma_flush_later()).
 */
void
irdma_flush_wqes(irdma_qp_t *iqp, uint32_t mask)
{
	irdma_t *irdma = iqp->iqp_irdma;
	struct irdma_sc_qp *sc = &iqp->iqp_sc;
	struct irdma_qp_flush_info *fi;
	struct irdma_ccq_cqe_info cqe;
	irdma_cqp_req_t *req;
	int ret;

	if ((mask & (IRDMA_FLUSH_SQ | IRDMA_FLUSH_RQ)) == 0)
		return;
	if ((req = irdma_vreq(irdma, IRDMA_OP_QP_FLUSH_WQES)) == NULL) {
		sc->qp_uk.sq_flush_complete = true;
		sc->qp_uk.rq_flush_complete = true;
		irdma_flush_later(iqp);
		return;
	}
	fi = &req->icr_cmd.in.u.qp_flush_wqes.info;
	fi->sq = (mask & IRDMA_FLUSH_SQ) != 0;
	fi->rq = (mask & IRDMA_FLUSH_RQ) != 0;
	fi->sq_major_code = IRDMA_FLUSH_MAJOR_ERR;
	fi->sq_minor_code = FLUSH_GENERAL_ERR;
	fi->rq_major_code = IRDMA_FLUSH_MAJOR_ERR;
	fi->rq_minor_code = FLUSH_GENERAL_ERR;
	fi->userflushcode = true;
	if ((mask & IRDMA_REFLUSH) != 0) {
		if (fi->sq)
			sc->flush_sq = false;
		if (fi->rq)
			sc->flush_rq = false;
	} else if (sc->flush_code != 0) {
		if (fi->sq && sc->sq_flush_code)
			fi->sq_minor_code = sc->flush_code;
		if (fi->rq && sc->rq_flush_code)
			fi->rq_minor_code = sc->flush_code;
	}
	req->icr_cmd.in.u.qp_flush_wqes.qp = sc;
	req->icr_cmd.in.u.qp_flush_wqes.scratch = irdma_req_scratch(irdma, req);
	bzero(&cqe, sizeof (cqe));
	ret = irdma_cqp_exec(irdma, req, &cqe);
	atomic_inc_64(&irdma->irdma_flushes);

	if (ret != 0)
		irdma_verbs_uncertain(irdma, "failed to flush a QP");
	mutex_enter(&iqp->iqp_lock);
	iqp->iqp_flush_issued = B_TRUE;
	if (ret != 0) {
		sc->qp_uk.sq_flush_complete = true;
		sc->qp_uk.rq_flush_complete = true;
	} else {
		if (fi->rq && (cqe.min_err_code ==
		    IRDMA_CQP_COMPL_SQ_WQE_FLUSHED || cqe.min_err_code == 0))
			sc->qp_uk.rq_flush_complete = true;
		if (fi->sq && (cqe.min_err_code ==
		    IRDMA_CQP_COMPL_RQ_WQE_FLUSHED || cqe.min_err_code == 0))
			sc->qp_uk.sq_flush_complete = true;
	}
	mutex_exit(&iqp->iqp_lock);
	irdma_flush_later(iqp);
}

static void
irdma_roce_ctx(irdma_qp_t *iqp)
{
	irdma_t *irdma = iqp->iqp_irdma;
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_udp_offload_info *udp = &iqp->iqp_udp;
	struct irdma_roce_offload_info *roce = &iqp->iqp_roce;

	udp->snd_mss = (u32)rdk_mtu_enum_to_int(
	    MAX(rdk_roce_mtu((int)irdma->irdma_vsi.mtu), RDK_MTU_256));
	udp->cwnd = IRDMA_ROCE_CWND_DEFAULT;
	udp->rexmit_thresh = 2;
	udp->rnr_nak_thresh = 2;
	udp->src_port = 0xc000;
	udp->dst_port = IRDMA_ROCE_UDP_DPORT;
	bcopy(irdma->irdma_info.iri_mac, roce->mac_addr, ETHERADDRL);
	/* An RC QP gets remote access only from RDK_QP_ACCESS_FLAGS. */
	roce->rd_en = roce->wr_rdresp_en = iqp->iqp_rdk.qp_type != RDK_QPT_RC;
	roce->dcqcn_en = false;
	roce->rtomin = 5;
	roce->ack_credits = IRDMA_ROCE_ACKCREDS_DEFAULT;
	roce->ird_size = dev->hw_attrs.max_hw_ird;
	roce->ord_size = dev->hw_attrs.max_hw_ord;
	roce->priv_mode_en = true;
	roce->fast_reg_en = true;
	roce->udprivcq_en = true;
	roce->roce_tver = 0;
	iqp->iqp_ctx.roce_info = roce;
	iqp->iqp_ctx.udp_info = udp;
	irdma_sc_qp_setctx_roce(&iqp->iqp_sc, iqp->iqp_sc.hw_host_ctx,
	    &iqp->iqp_ctx);
}

static void
irdma_qp_free_mem(irdma_qp_t *iqp)
{
	irdma_t *irdma = iqp->iqp_irdma;

	dma_free_coherent(&irdma->irdma_osdev, iqp->iqp_q2ctx.size,
	    iqp->iqp_q2ctx.va, iqp->iqp_q2ctx.pa);
	iqp->iqp_q2ctx.va = NULL;
	dma_free_coherent(&irdma->irdma_osdev, iqp->iqp_ring.size,
	    iqp->iqp_ring.va, iqp->iqp_ring.pa);
	iqp->iqp_ring.va = NULL;
	if (iqp->iqp_sq_wrid != NULL) {
		kmem_free(iqp->iqp_sq_wrid,
		    sizeof (*iqp->iqp_sq_wrid) * iqp->iqp_sq_depth);
		iqp->iqp_sq_wrid = NULL;
	}
	if (iqp->iqp_rq_wrid != NULL) {
		kmem_free(iqp->iqp_rq_wrid,
		    sizeof (*iqp->iqp_rq_wrid) * iqp->iqp_rq_depth);
		iqp->iqp_rq_wrid = NULL;
	}
	if (iqp->iqp_rq_slots != NULL) {
		kmem_free(iqp->iqp_rq_slots,
		    sizeof (*iqp->iqp_rq_slots) * iqp->iqp_rq_depth);
		iqp->iqp_rq_slots = NULL;
	}
}

static void
irdma_qp_free_num(irdma_qp_t *iqp, uint32_t num)
{
	irdma_t *irdma = iqp->iqp_irdma;

	if (num == IRDMA_GSI_QPN) {
		mutex_enter(&irdma->irdma_rsrc_lock);
		irdma->irdma_gsi_used = B_FALSE;
		mutex_exit(&irdma->irdma_rsrc_lock);
	} else {
		irdma_free_rsrc(irdma, irdma->irdma_qp_map, num);
	}
}

static int
irdma_qp_kmode(irdma_qp_t *iqp, struct irdma_qp_init_info *info,
    struct rdk_qp_init_attr *init)
{
	irdma_t *irdma = iqp->iqp_irdma;
	struct irdma_qp_uk_init_info *uk = &info->qp_uk_init_info;
	struct irdma_dma_mem *mem = &iqp->iqp_ring;
	size_t size;

	if (irdma_uk_calc_depth_shift_sq(uk, &uk->sq_depth, &uk->sq_shift) !=
	    0 || irdma_uk_calc_depth_shift_rq(uk, &uk->rq_depth,
	    &uk->rq_shift) != 0)
		return (EINVAL);

	iqp->iqp_sq_depth = uk->sq_depth;
	iqp->iqp_rq_depth = uk->rq_depth;
	iqp->iqp_sq_wrid = kmem_zalloc(sizeof (*iqp->iqp_sq_wrid) *
	    uk->sq_depth, KM_SLEEP);
	iqp->iqp_rq_wrid = kmem_zalloc(sizeof (*iqp->iqp_rq_wrid) *
	    uk->rq_depth, KM_SLEEP);
	iqp->iqp_rq_slots = kmem_zalloc(sizeof (*iqp->iqp_rq_slots) *
	    uk->rq_depth, KM_SLEEP);
	uk->sq_wrtrk_array = iqp->iqp_sq_wrid;
	uk->rq_wrid_array = iqp->iqp_rq_wrid;

	size = ((size_t)uk->sq_depth + uk->rq_depth) * IRDMA_QP_WQE_MIN_SIZE +
	    (IRDMA_SHADOW_AREA_SIZE << 3);
	if (size > IRDMA_DMA_BUF_MAX)
		return (EINVAL);
	mem->size = ALIGN(size, 256);
	mem->va = dma_alloc_coherent(&irdma->irdma_osdev, mem->size, &mem->pa,
	    GFP_KERNEL);
	if (mem->va == NULL)
		return (ENOMEM);

	uk->sq = mem->va;
	info->sq_pa = mem->pa;
	uk->rq = &uk->sq[uk->sq_depth];
	info->rq_pa = info->sq_pa + (uint64_t)uk->sq_depth *
	    IRDMA_QP_WQE_MIN_SIZE;
	uk->shadow_area = uk->rq[uk->rq_depth].elem;
	info->shadow_area_pa = info->rq_pa + (uint64_t)uk->rq_depth *
	    IRDMA_QP_WQE_MIN_SIZE;
	uk->sq_size = uk->sq_depth >> uk->sq_shift;
	uk->rq_size = uk->rq_depth >> uk->rq_shift;

	iqp->iqp_max_send_wr = (uk->sq_depth - IRDMA_SQ_RSVD) >> uk->sq_shift;
	iqp->iqp_max_recv_wr = (uk->rq_depth - IRDMA_RQ_RSVD) >> uk->rq_shift;
	init->cap.max_send_wr = iqp->iqp_max_send_wr;
	init->cap.max_recv_wr = iqp->iqp_max_recv_wr;
	return (0);
}

int
irdma_create_qp(struct rdk_qp *rqp, struct rdk_qp_init_attr *init)
{
	irdma_t *irdma = IRDMA_DEV(rqp->device);
	irdma_qp_t *iqp = IRDMA_QP(rqp);
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_uk_attrs *uka = &dev->hw_attrs.uk_attrs;
	struct irdma_qp_init_info info;
	struct irdma_create_qp_info *ci;
	irdma_cqp_req_t *req;
	uint32_t num;
	int ret;

	if (init->cap.max_inline_data > uka->max_hw_inline ||
	    init->cap.max_send_sge > uka->max_hw_wq_frags ||
	    init->cap.max_recv_sge > uka->max_hw_wq_frags ||
	    init->cap.max_send_wr == 0 || init->cap.max_recv_wr == 0 ||
	    init->cap.max_send_wr > uka->max_hw_wq_quanta ||
	    init->cap.max_recv_wr > uka->max_hw_rq_quanta)
		return (EINVAL);

	iqp->iqp_irdma = irdma;
	iqp->iqp_pd = IRDMA_PD(rqp->pd);
	iqp->iqp_scq = IRDMA_CQ(init->send_cq);
	iqp->iqp_rcq = IRDMA_CQ(init->recv_cq);
	mutex_init(&iqp->iqp_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&iqp->iqp_mod_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&iqp->iqp_cv, NULL, CV_DRIVER, NULL);
	cv_init(&iqp->iqp_ref_cv, NULL, CV_DRIVER, NULL);

	if (init->qp_type == RDK_QPT_GSI) {
		mutex_enter(&irdma->irdma_rsrc_lock);
		ret = irdma->irdma_gsi_used ? EBUSY : 0;
		irdma->irdma_gsi_used = B_TRUE;
		mutex_exit(&irdma->irdma_rsrc_lock);
		num = IRDMA_GSI_QPN;
	} else {
		ret = irdma_alloc_rsrc(irdma, irdma->irdma_qp_map,
		    irdma->irdma_max_qp, &num, &irdma->irdma_next_qp);
	}
	if (ret != 0)
		goto fail_locks;

	iqp->iqp_q2ctx.size = ALIGN(IRDMA_Q2_BUF_SIZE + IRDMA_QP_CTX_SIZE, 256);
	iqp->iqp_q2ctx.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    iqp->iqp_q2ctx.size, &iqp->iqp_q2ctx.pa, GFP_KERNEL);
	if (iqp->iqp_q2ctx.va == NULL) {
		ret = ENOMEM;
		goto fail;
	}

	bzero(&info, sizeof (info));
	info.vsi = &irdma->irdma_vsi;
	info.pd = &iqp->iqp_pd->ipd_sc;
	info.q2 = iqp->iqp_q2ctx.va;
	info.q2_pa = iqp->iqp_q2ctx.pa;
	info.host_ctx = (__le64 *)(void *)(info.q2 + IRDMA_Q2_BUF_SIZE);
	info.host_ctx_pa = info.q2_pa + IRDMA_Q2_BUF_SIZE;
	info.qp_uk_init_info.uk_attrs = uka;
	info.qp_uk_init_info.sq_size = init->cap.max_send_wr;
	info.qp_uk_init_info.rq_size = init->cap.max_recv_wr;
	info.qp_uk_init_info.max_sq_frag_cnt = MAX(init->cap.max_send_sge, 1);
	info.qp_uk_init_info.max_rq_frag_cnt = MAX(init->cap.max_recv_sge, 1);
	info.qp_uk_init_info.max_inline_data = init->cap.max_inline_data;
	info.qp_uk_init_info.qp_id = num;
	info.qp_uk_init_info.abi_ver = IRDMA_ABI_VER;
	if ((ret = irdma_qp_kmode(iqp, &info, init)) != 0)
		goto fail;
	if (init->qp_type == RDK_QPT_RC) {
		info.qp_uk_init_info.type = IRDMA_QP_TYPE_ROCE_RC;
		info.qp_uk_init_info.qp_caps = IRDMA_SEND_WITH_IMM |
		    IRDMA_WRITE_WITH_IMM | IRDMA_ROCE;
	} else {
		info.qp_uk_init_info.type = IRDMA_QP_TYPE_ROCE_UD;
		info.qp_uk_init_info.qp_caps = IRDMA_SEND_WITH_IMM |
		    IRDMA_ROCE;
	}

	iqp->iqp_sc.qp_uk.back_qp = iqp;
	iqp->iqp_sc.push_idx = IRDMA_INVALID_PUSH_PAGE_INDEX;
	iqp->iqp_sc.user_pri = 0;
	if (irdma_sc_qp_init(&iqp->iqp_sc, &info) != 0) {
		ret = EINVAL;
		goto fail;
	}
	iqp->iqp_ctx.qp_compl_ctx = (uintptr_t)&iqp->iqp_sc;
	iqp->iqp_ctx.send_cq_num = iqp->iqp_scq->icq_num;
	iqp->iqp_ctx.rcv_cq_num = iqp->iqp_rcq->icq_num;
	iqp->iqp_ctx.user_pri = 0;
	if (dev->ws_add(&irdma->irdma_vsi, 0) != 0) {
		ret = ENOMEM;
		goto fail;
	}
	irdma_qp_add_qos(&iqp->iqp_sc);
	irdma_roce_ctx(iqp);

	if ((req = irdma_vreq(irdma, IRDMA_OP_QP_CREATE)) == NULL) {
		ret = EIO;
		goto fail_qos;
	}
	ci = &req->icr_cmd.in.u.qp_create.info;
	ci->mac_valid = true;
	ci->cq_num_valid = true;
	ci->next_iwarp_state = IRDMA_QP_STATE_IDLE;
	req->icr_cmd.in.u.qp_create.qp = &iqp->iqp_sc;
	req->icr_cmd.in.u.qp_create.scratch = irdma_req_scratch(irdma, req);
	if ((ret = irdma_cqp_exec(irdma, req, NULL)) != 0) {
		irdma_verbs_uncertain(irdma, "failed to create a QP");
		goto fail_qos;
	}

	iqp->iqp_sig_all = init->sq_sig_type == RDK_SIGNAL_ALL_WR;
	iqp->iqp_state = RDK_QPS_RESET;
	iqp->iqp_hw_state = IRDMA_QP_STATE_INVALID;
	rqp->qp_num = num;
	mutex_enter(&irdma->irdma_qptable_lock);
	irdma->irdma_qp_table[num] = iqp;
	iqp->iqp_refs = 1;
	iqp->iqp_in_table = B_TRUE;
	mutex_exit(&irdma->irdma_qptable_lock);
	irdma_cq_add_qp(iqp->iqp_scq, num);
	if (iqp->iqp_rcq != iqp->iqp_scq)
		irdma_cq_add_qp(iqp->iqp_rcq, num);
	atomic_inc_32(&irdma->irdma_nqps);
	return (0);

fail_qos:
	irdma_qp_rem_qos(&iqp->iqp_sc);
	dev->ws_remove(&irdma->irdma_vsi, 0);
fail:
	irdma_qp_free_mem(iqp);
	irdma_qp_free_num(iqp, num);
fail_locks:
	cv_destroy(&iqp->iqp_ref_cv);
	cv_destroy(&iqp->iqp_cv);
	mutex_destroy(&iqp->iqp_mod_lock);
	mutex_destroy(&iqp->iqp_lock);
	return (ret);
}

/*
 * The address vector: source from the GID the framework resolved, the
 * destination's ARP entry from the MAC the consumer gave.
 */
static int
irdma_qp_av(irdma_qp_t *iqp, struct rdk_qp_attr *attr, uint32_t dest_qp)
{
	irdma_t *irdma = iqp->iqp_irdma;
	struct irdma_udp_offload_info *udp = &iqp->iqp_udp;
	const struct rdk_ah_attr *ah = &attr->ah_attr;
	const struct rdk_gid_attr *sgid = ah->grh.sgid_attr;
	uint32_t sip[4] = { 0 }, dip[4] = { 0 };
	ipaddr_t s4, d4;
	boolean_t v4;
	uint_t i;
	int arp;

	v4 = rdk_gid_to_ipv4(&sgid->gid, &s4);
	if (v4 != rdk_gid_to_ipv4(&ah->grh.dgid, &d4))
		return (EINVAL);
	if (v4) {
		sip[3] = ntohl(s4);
		dip[3] = ntohl(d4);
		if ((dip[3] >> 28) == 0xe || dip[3] == 0 ||
		    dip[3] == UINT32_MAX)
			return (EINVAL);
	} else {
		if (ah->grh.dgid.raw[0] == 0xff)
			return (EINVAL);
		for (i = 0; i < 4; i++) {
			uint32_t a, b;

			bcopy(&sgid->gid.raw[i * 4], &a, sizeof (a));
			bcopy(&ah->grh.dgid.raw[i * 4], &b, sizeof (b));
			sip[i] = ntohl(a);
			dip[i] = ntohl(b);
		}
	}

	udp->ttl = ah->grh.hop_limit;
	udp->flow_label = ah->grh.flow_label;
	udp->tos = ah->grh.traffic_class;
	udp->src_port = rdk_get_udp_sport(udp->flow_label,
	    iqp->iqp_rdk.qp_num, dest_qp);
	udp->insert_vlan_tag = false;
	udp->ipv4 = v4;
	bcopy(dip, udp->dest_ip_addr, sizeof (dip));
	bcopy(sip, udp->local_ipaddr, sizeof (sip));
	bcopy(sgid->mac, iqp->iqp_roce.mac_addr, ETHERADDRL);

	arp = irdma_add_arp(irdma, v4 ? &dip[3] : dip, v4, ah->roce.dmac);
	if (arp < 0)
		return (EIO);
	udp->arp_idx = (u16)arp;
	/* E810 takes a frame to its own MAC off the wire as malicious. */
	iqp->iqp_lpbk = bcmp(sip, dip, sizeof (sip)) == 0 ||
	    bcmp(ah->roce.dmac, irdma->irdma_info.iri_mac, ETHERADDRL) == 0;
	return (0);
}

int
irdma_modify_qp(struct rdk_qp *rqp, struct rdk_qp_attr *attr, int mask)
{
	irdma_qp_t *iqp = IRDMA_QP(rqp);
	irdma_t *irdma = iqp->iqp_irdma;
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_qp_host_ctx_info *ctx = &iqp->iqp_ctx;
	struct irdma_roce_offload_info *roce = &iqp->iqp_roce;
	struct irdma_udp_offload_info *udp = &iqp->iqp_udp;
	struct irdma_modify_qp_info info;
	struct irdma_udp_offload_info udp_old;
	struct irdma_roce_offload_info roce_old;
	boolean_t issue = B_FALSE, flush = B_FALSE;
	uint32_t dest_qp, new_arp = 0, old_arp;
	int access_old;
	boolean_t ird_zero_old;
	int ret = 0;

	if ((mask & ~RDK_QP_ATTR_STANDARD_BITS) != 0)
		return (EOPNOTSUPP);
	if ((mask & RDK_QP_DEST_QPN) != 0 && attr->dest_qp_num > 0xffffff)
		return (EINVAL);
	if ((mask & RDK_QP_PKEY_INDEX) != 0 &&
	    attr->pkey_index >= IRDMA_PKEY_TBL_SZ)
		return (EINVAL);
	if (((mask & RDK_QP_RETRY_CNT) != 0 && attr->retry_cnt > 7) ||
	    ((mask & RDK_QP_RNR_RETRY) != 0 && attr->rnr_retry > 7))
		return (EINVAL);
	if ((mask & RDK_QP_PATH_MTU) != 0 &&
	    attr->path_mtu > rdk_roce_mtu((int)irdma->irdma_mtu))
		return (EINVAL);
	if ((mask & RDK_QP_MAX_QP_RD_ATOMIC) != 0 &&
	    attr->max_rd_atomic > dev->hw_attrs.max_hw_ord)
		return (EINVAL);
	if ((mask & RDK_QP_MAX_DEST_RD_ATOMIC) != 0 &&
	    attr->max_dest_rd_atomic > dev->hw_attrs.max_hw_ird)
		return (EINVAL);

	mutex_enter(&iqp->iqp_mod_lock);
	mutex_enter(&iqp->iqp_lock);
	udp_old = *udp;
	roce_old = *roce;
	access_old = iqp->iqp_access;
	ird_zero_old = iqp->iqp_ird_zero;
	mutex_exit(&iqp->iqp_lock);
	dest_qp = (mask & RDK_QP_DEST_QPN) != 0 ? attr->dest_qp_num :
	    roce->dest_qp;
	if ((mask & RDK_QP_AV) != 0) {
		if ((ret = irdma_qp_av(iqp, attr, dest_qp)) != 0)
			goto out;
		new_arp = udp->arp_idx;
	}

	bzero(&info, sizeof (info));
	mutex_enter(&iqp->iqp_lock);
	if ((mask & RDK_QP_STATE) != 0) {
		if (!rdk_modify_qp_is_ok(iqp->iqp_state, attr->qp_state,
		    iqp->iqp_rdk.qp_type, mask)) {
			mutex_exit(&iqp->iqp_lock);
			ret = EINVAL;
			goto out;
		}
		info.curr_iwarp_state = iqp->iqp_hw_state;
		switch (attr->qp_state) {
		case RDK_QPS_INIT:
			if (iqp->iqp_hw_state > IRDMA_QP_STATE_IDLE) {
				ret = EINVAL;
				break;
			}
			if (iqp->iqp_hw_state == IRDMA_QP_STATE_INVALID) {
				info.next_iwarp_state = IRDMA_QP_STATE_IDLE;
				issue = B_TRUE;
			}
			break;
		case RDK_QPS_RTR:
			if (iqp->iqp_hw_state > IRDMA_QP_STATE_IDLE) {
				ret = EINVAL;
				break;
			}
			info.arp_cache_idx_valid = true;
			info.cq_num_valid = true;
			info.next_iwarp_state = IRDMA_QP_STATE_RTR;
			issue = B_TRUE;
			break;
		case RDK_QPS_RTS:
			if (iqp->iqp_state < RDK_QPS_RTR ||
			    iqp->iqp_state == RDK_QPS_ERR) {
				ret = EINVAL;
				break;
			}
			info.arp_cache_idx_valid = true;
			info.cq_num_valid = true;
			info.ord_valid = true;
			info.next_iwarp_state = IRDMA_QP_STATE_RTS;
			issue = B_TRUE;
			break;
		case RDK_QPS_ERR:
			if (iqp->iqp_hw_state == IRDMA_QP_STATE_ERROR) {
				iqp->iqp_state = attr->qp_state;
				mutex_exit(&iqp->iqp_lock);
				goto out;
			}
			info.next_iwarp_state = IRDMA_QP_STATE_ERROR;
			issue = B_TRUE;
			break;
		case RDK_QPS_RESET:
			/* The device cannot take a created QP back to reset. */
			if (iqp->iqp_hw_state != IRDMA_QP_STATE_INVALID)
				ret = ENOTSUP;
			break;
		case RDK_QPS_SQE:
		case RDK_QPS_SQD:
		default:
			ret = ENOTSUP;
			break;
		}
		if (ret != 0) {
			mutex_exit(&iqp->iqp_lock);
			goto out;
		}
	}
	/* The device reads these only when the QP changes state. */
	if (!issue && iqp->iqp_hw_state > IRDMA_QP_STATE_IDLE &&
	    (mask & (RDK_QP_ACCESS_FLAGS | RDK_QP_MAX_DEST_RD_ATOMIC)) != 0) {
		mutex_exit(&iqp->iqp_lock);
		ret = ENOTSUP;
		goto out;
	}

	if ((mask & RDK_QP_DEST_QPN) != 0)
		roce->dest_qp = attr->dest_qp_num;
	if ((mask & RDK_QP_PKEY_INDEX) != 0)
		roce->p_key = IRDMA_DEFAULT_PKEY;
	if ((mask & RDK_QP_QKEY) != 0)
		roce->qkey = attr->qkey;
	if ((mask & RDK_QP_PATH_MTU) != 0)
		udp->snd_mss = (u32)rdk_mtu_enum_to_int(attr->path_mtu);
	if ((mask & RDK_QP_SQ_PSN) != 0) {
		udp->psn_nxt = attr->sq_psn & 0xffffff;
		udp->lsn = 0xffff;
		udp->psn_una = attr->sq_psn & 0xffffff;
		udp->psn_max = attr->sq_psn & 0xffffff;
	}
	if ((mask & RDK_QP_RQ_PSN) != 0)
		udp->epsn = attr->rq_psn & 0xffffff;
	if ((mask & RDK_QP_RNR_RETRY) != 0)
		udp->rnr_nak_thresh = attr->rnr_retry;
	if ((mask & RDK_QP_RETRY_CNT) != 0)
		udp->rexmit_thresh = attr->retry_cnt;
	if ((mask & RDK_QP_MAX_QP_RD_ATOMIC) != 0 && attr->max_rd_atomic != 0)
		roce->ord_size = attr->max_rd_atomic;
	if ((mask & RDK_QP_MAX_DEST_RD_ATOMIC) != 0) {
		iqp->iqp_ird_zero = attr->max_dest_rd_atomic == 0;
		if (attr->max_dest_rd_atomic != 0)
			roce->ird_size = attr->max_dest_rd_atomic;
	}
	if ((mask & RDK_QP_ACCESS_FLAGS) != 0)
		iqp->iqp_access = attr->qp_access_flags;
	if (iqp->iqp_rdk.qp_type == RDK_QPT_RC) {
		roce->wr_rdresp_en = (iqp->iqp_access &
		    (RDK_ACCESS_LOCAL_WRITE | RDK_ACCESS_REMOTE_WRITE)) != 0;
		roce->rd_en = (iqp->iqp_access & RDK_ACCESS_REMOTE_READ) != 0 &&
		    !iqp->iqp_ird_zero;
	}
	roce->pd_id = iqp->iqp_pd->ipd_sc.pd_id;
	ctx->send_cq_num = iqp->iqp_scq->icq_num;
	ctx->rcv_cq_num = iqp->iqp_rcq->icq_num;
	irdma_sc_qp_setctx_roce(&iqp->iqp_sc, iqp->iqp_sc.hw_host_ctx, ctx);
	if ((mask & RDK_QP_STATE) != 0 && !issue)
		iqp->iqp_state = attr->qp_state;
	mutex_exit(&iqp->iqp_lock);

	if (!issue)
		goto out;

	ctx->rem_endpoint_idx = udp->arp_idx;
	info.force_lpb = iqp->iqp_lpbk;
	if ((ret = irdma_hw_modify_qp(iqp, &info)) != 0) {
		/* The device may have applied it, with its ARP index. */
		irdma_verbs_uncertain(irdma, "failed to modify a QP");
		/* An error transition still stops posting and flushes. */
		if (info.next_iwarp_state != IRDMA_QP_STATE_ERROR) {
			ret = EIO;
			goto out;
		}
		ret = 0;
	}
	mutex_enter(&iqp->iqp_lock);
	if (iqp->iqp_hw_state == info.curr_iwarp_state) {
		iqp->iqp_hw_state = info.next_iwarp_state;
		iqp->iqp_state = attr->qp_state;
	}
	flush = iqp->iqp_state > RDK_QPS_RTS && !iqp->iqp_flush_issued;
	mutex_exit(&iqp->iqp_lock);
	if (flush)
		irdma_flush_wqes(iqp, IRDMA_FLUSH_SQ | IRDMA_FLUSH_RQ |
		    IRDMA_FLUSH_WAIT);
out:
	if (ret != 0) {
		/* A failed modify leaves the QP as it was. */
		mutex_enter(&iqp->iqp_lock);
		*udp = udp_old;
		*roce = roce_old;
		iqp->iqp_access = access_old;
		iqp->iqp_ird_zero = ird_zero_old;
		mutex_exit(&iqp->iqp_lock);
		if (new_arp != 0)
			irdma_arp_rele(irdma, new_arp);
	} else if (new_arp != 0) {
		old_arp = iqp->iqp_arp_idx;
		iqp->iqp_arp_idx = new_arp;
		if (old_arp != 0)
			irdma_arp_rele(irdma, old_arp);
	}
	mutex_exit(&iqp->iqp_mod_lock);
	return (ret);
}

int
irdma_query_qp(struct rdk_qp *rqp, struct rdk_qp_attr *attr, int mask,
    struct rdk_qp_init_attr *init)
{
	irdma_qp_t *iqp = IRDMA_QP(rqp);
	struct irdma_qp_uk *uk = &iqp->iqp_sc.qp_uk;

	_NOTE(ARGUNUSED(mask));
	mutex_enter(&iqp->iqp_lock);
	attr->qp_state = attr->cur_qp_state = iqp->iqp_state;
	attr->cap.max_send_wr = iqp->iqp_max_send_wr;
	attr->cap.max_recv_wr = iqp->iqp_max_recv_wr;
	attr->cap.max_inline_data = uk->max_inline_data;
	attr->cap.max_send_sge = uk->max_sq_frag_cnt;
	attr->cap.max_recv_sge = uk->max_rq_frag_cnt;
	attr->qp_access_flags = iqp->iqp_access;
	attr->port_num = 1;
	attr->path_mtu = rdk_mtu_int_to_enum((int)iqp->iqp_udp.snd_mss);
	attr->qkey = iqp->iqp_roce.qkey;
	attr->rq_psn = iqp->iqp_udp.epsn;
	attr->sq_psn = iqp->iqp_udp.psn_nxt;
	attr->dest_qp_num = iqp->iqp_roce.dest_qp;
	attr->retry_cnt = iqp->iqp_udp.rexmit_thresh;
	attr->rnr_retry = iqp->iqp_udp.rnr_nak_thresh;
	attr->max_rd_atomic = (uint8_t)iqp->iqp_roce.ord_size;
	attr->max_dest_rd_atomic = iqp->iqp_ird_zero ? 0 :
	    (uint8_t)iqp->iqp_roce.ird_size;
	mutex_exit(&iqp->iqp_lock);

	init->event_handler = rqp->event_handler;
	init->qp_context = rqp->qp_context;
	init->send_cq = rqp->send_cq;
	init->recv_cq = rqp->recv_cq;
	init->cap = attr->cap;
	init->qp_type = rqp->qp_type;
	init->sq_sig_type = iqp->iqp_sig_all ? RDK_SIGNAL_ALL_WR :
	    RDK_SIGNAL_REQ_WR;
	return (0);
}

/*
 * Stop the QP in the device, then drop it from the table and wait for the
 * AEQ and the deferred work to let go of it, then clear its completions.
 */
void
irdma_destroy_qp(struct rdk_qp *rqp)
{
	irdma_qp_t *iqp = IRDMA_QP(rqp);
	irdma_t *irdma = iqp->iqp_irdma;
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	irdma_cqp_req_t *req;
	struct rdk_qp_attr attr;
	timeout_id_t tid;
	boolean_t live;

	mutex_enter(&iqp->iqp_lock);
	live = iqp->iqp_hw_state >= IRDMA_QP_STATE_IDLE &&
	    iqp->iqp_hw_state != IRDMA_QP_STATE_ERROR;
	mutex_exit(&iqp->iqp_lock);
	if (live) {
		bzero(&attr, sizeof (attr));
		attr.qp_state = RDK_QPS_ERR;
		(void) irdma_modify_qp(rqp, &attr, RDK_QP_STATE);
	}

	mutex_enter(&iqp->iqp_lock);
	iqp->iqp_destroying = B_TRUE;
	iqp->iqp_sc.qp_uk.destroy_pending = true;
	tid = iqp->iqp_flush_tid;
	iqp->iqp_flush_tid = 0;
	mutex_exit(&iqp->iqp_lock);
	if (tid != 0)
		(void) untimeout(tid);
	mutex_enter(&iqp->iqp_lock);
	while (iqp->iqp_work != 0)
		cv_wait(&iqp->iqp_cv, &iqp->iqp_lock);
	mutex_exit(&iqp->iqp_lock);

	mutex_enter(&iqp->iqp_mod_lock);
	if ((req = irdma_vreq(irdma, IRDMA_OP_QP_DESTROY)) == NULL) {
		irdma_taint(irdma);
	} else {
		req->icr_cmd.in.u.qp_destroy.qp = &iqp->iqp_sc;
		req->icr_cmd.in.u.qp_destroy.remove_hash_idx = true;
		req->icr_cmd.in.u.qp_destroy.scratch =
		    irdma_req_scratch(irdma, req);
		if (irdma_cqp_exec(irdma, req, NULL) != 0)
			irdma_verbs_uncertain(irdma, "failed to destroy a QP");
	}
	irdma_arp_rele(irdma, iqp->iqp_arp_idx);
	iqp->iqp_arp_idx = 0;
	mutex_exit(&iqp->iqp_mod_lock);

	mutex_enter(&irdma->irdma_qptable_lock);
	if (iqp->iqp_in_table) {
		irdma->irdma_qp_table[rqp->qp_num] = NULL;
		iqp->iqp_in_table = B_FALSE;
		iqp->iqp_refs--;
	}
	while (iqp->iqp_refs != 0)
		cv_wait(&iqp->iqp_ref_cv, &irdma->irdma_qptable_lock);
	mutex_exit(&irdma->irdma_qptable_lock);

	irdma_cq_purge_qp(iqp->iqp_scq, iqp);
	if (iqp->iqp_rcq != iqp->iqp_scq)
		irdma_cq_purge_qp(iqp->iqp_rcq, iqp);

	irdma_qp_rem_qos(&iqp->iqp_sc);
	dev->ws_remove(&irdma->irdma_vsi, iqp->iqp_sc.user_pri);
	irdma_qp_free_mem(iqp);
	irdma_qp_free_num(iqp, rqp->qp_num);
	cv_destroy(&iqp->iqp_ref_cv);
	cv_destroy(&iqp->iqp_cv);
	mutex_destroy(&iqp->iqp_mod_lock);
	mutex_destroy(&iqp->iqp_lock);
	atomic_dec_32(&irdma->irdma_nqps);
}

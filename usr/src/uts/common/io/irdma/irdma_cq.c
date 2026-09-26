/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2015 - 2021 Intel Corporation */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Completion queues.  A CQ is registered on the CEQ of its comp_vector,
 * whose vector thread calls the CQ's completion handler (irdma_intr.c).
 * The logic follows the Linux irdma verbs.c and utils.c (see
 * README.illumos).
 *
 * A CQE names its QP with a pointer and a QP number, both written by the
 * device.  irdma_osdep_cqe_qp() accepts the entry only if the QP table
 * holds a QP at that number, at that address, that uses this CQ.  QP
 * destroy removes the QP from the table and then takes each of its CQ
 * locks, so no poller is still using the QP when it is freed.
 *
 * A receive posted by the driver carries the index of its slot as its work
 * request id; the slot holds the consumer's id and the posted length, and
 * a completion that claims more bytes than were posted is an error.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>

#include "irdma_verbs.h"

static enum rdk_wc_status
irdma_flush_err_to_wc_status(uint16_t code)
{
	switch (code) {
	case FLUSH_PROT_ERR:
		return (RDK_WC_LOC_PROT_ERR);
	case FLUSH_REM_ACCESS_ERR:
		return (RDK_WC_REM_ACCESS_ERR);
	case FLUSH_LOC_QP_OP_ERR:
		return (RDK_WC_LOC_QP_OP_ERR);
	case FLUSH_REM_OP_ERR:
		return (RDK_WC_REM_OP_ERR);
	case FLUSH_LOC_LEN_ERR:
		return (RDK_WC_LOC_LEN_ERR);
	case FLUSH_GENERAL_ERR:
		return (RDK_WC_WR_FLUSH_ERR);
	case FLUSH_RETRY_EXC_ERR:
		return (RDK_WC_RETRY_EXC_ERR);
	case FLUSH_MW_BIND_ERR:
		return (RDK_WC_MW_BIND_ERR);
	case FLUSH_REM_INV_REQ_ERR:
		return (RDK_WC_REM_INV_REQ_ERR);
	case FLUSH_RNR_RETRY_EXC_ERR:
		return (RDK_WC_RNR_RETRY_EXC_ERR);
	case FLUSH_FATAL_ERR:
	default:
		return (RDK_WC_FATAL_ERR);
	}
}

static enum rdk_wc_opcode
irdma_wc_op_sq(uint8_t op, enum rdk_wc_status *status)
{
	switch (op) {
	case IRDMA_OP_TYPE_RDMA_WRITE:
	case IRDMA_OP_TYPE_RDMA_WRITE_SOL:
		return (RDK_WC_RDMA_WRITE);
	case IRDMA_OP_TYPE_RDMA_READ_INV_STAG:
	case IRDMA_OP_TYPE_RDMA_READ:
		return (RDK_WC_RDMA_READ);
	case IRDMA_OP_TYPE_SEND_SOL:
	case IRDMA_OP_TYPE_SEND_SOL_INV:
	case IRDMA_OP_TYPE_SEND_INV:
	case IRDMA_OP_TYPE_SEND:
		return (RDK_WC_SEND);
	case IRDMA_OP_TYPE_FAST_REG_NSMR:
		return (RDK_WC_REG_MR);
	case IRDMA_OP_TYPE_INV_STAG:
		return (RDK_WC_LOCAL_INV);
	default:
		*status = RDK_WC_GENERAL_ERR;
		return (RDK_WC_SEND);
	}
}

/*
 * Validate the QP a CQE names; see the block comment.  The caller holds
 * the CQ's icq_lock.  A zero context is an entry the driver cleaned for a
 * destroyed QP.
 */
struct irdma_qp_uk *
irdma_osdep_cqe_qp(struct irdma_cq_uk *ukcq, u64 comp_ctx, u32 qp_id)
{
	irdma_cq_t *icq = container_of(ukcq, irdma_cq_t, icq_sc.cq_uk);
	irdma_t *irdma = icq->icq_irdma;
	irdma_qp_t *iqp = NULL;

	ASSERT(MUTEX_HELD(&icq->icq_lock));
	if (comp_ctx == 0)
		return (NULL);
	if (qp_id < irdma->irdma_max_qp) {
		mutex_enter(&irdma->irdma_qptable_lock);
		iqp = irdma->irdma_qp_table[qp_id];
		if (iqp != NULL && ((uintptr_t)&iqp->iqp_sc != comp_ctx ||
		    (iqp->iqp_scq != icq && iqp->iqp_rcq != icq)))
			iqp = NULL;
		mutex_exit(&irdma->irdma_qptable_lock);
	}
	if (iqp == NULL) {
		icq->icq_bad_cqes++;
		atomic_inc_64(&irdma->irdma_bad_cqes);
		return (NULL);
	}
	return (&iqp->iqp_sc.qp_uk);
}

int
irdma_create_cq(struct rdk_cq *rcq, const struct rdk_cq_init_attr *attr)
{
	irdma_t *irdma = IRDMA_DEV(rcq->device);
	irdma_cq_t *icq = IRDMA_CQ(rcq);
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_cq_init_info info;
	struct irdma_cq_uk_init_info *uk = &info.cq_uk_init_info;
	irdma_cqp_req_t *req;
	uint32_t num, entries;
	int ret;

	if (attr->cqe == 0 || attr->cqe > IRDMA_MAX_KCQE ||
	    attr->cqe > dev->hw_attrs.uk_attrs.max_hw_cq_size ||
	    attr->comp_vector >= irdma->irdma_nceqs)
		return (EINVAL);
	if ((dev->hw_attrs.uk_attrs.feature_flags &
	    IRDMA_FEATURE_64_BYTE_CQE) != 0)
		return (ENOTSUP);

	ret = irdma_alloc_rsrc(irdma, irdma->irdma_cq_map, irdma->irdma_max_cq,
	    &num, &irdma->irdma_next_cq);
	if (ret != 0)
		return (ret);

	icq->icq_irdma = irdma;
	icq->icq_ceq = &irdma->irdma_ceqs[attr->comp_vector];
	icq->icq_num = num;
	mutex_init(&icq->icq_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&icq->icq_cv, NULL, CV_DRIVER, NULL);
	list_create(&icq->icq_gen, sizeof (irdma_cmpl_gen_t),
	    offsetof(irdma_cmpl_gen_t, icg_node));

	/* GEN2 uses two entries per completion without 64 byte CQEs. */
	entries = (attr->cqe + 1) * 2;
	icq->icq_mem.size = ALIGN(entries * sizeof (struct irdma_cqe), 256);
	icq->icq_mem.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    icq->icq_mem.size, &icq->icq_mem.pa, GFP_KERNEL);
	icq->icq_shadow.size = ALIGN(IRDMA_SHADOW_AREA_SIZE << 3, 64);
	icq->icq_shadow.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    icq->icq_shadow.size, &icq->icq_shadow.pa, GFP_KERNEL);
	if (icq->icq_mem.va == NULL || icq->icq_shadow.va == NULL) {
		ret = ENOMEM;
		goto fail;
	}

	bzero(&info, sizeof (info));
	info.dev = dev;
	uk->cq_size = entries;
	uk->cq_id = num;
	uk->cq_base = icq->icq_mem.va;
	uk->shadow_area = icq->icq_shadow.va;
	uk->avoid_mem_cflct = false;
	info.cq_base_pa = icq->icq_mem.pa;
	info.shadow_area_pa = icq->icq_shadow.pa;
	info.ceq_id = icq->icq_ceq->ic_id;
	info.ceq_id_valid = true;
	info.ceqe_mask = 1;
	info.type = IRDMA_CQ_TYPE_IWARP;
	info.vsi = &irdma->irdma_vsi;
	info.shadow_read_threshold = MIN(entries / 2, IRDMA_MAX_CQ_READ_THRESH);
	if (irdma_sc_cq_init(&icq->icq_sc, &info) != 0) {
		ret = EINVAL;
		goto fail;
	}
	icq->icq_sc.back_cq = icq;

	if ((req = irdma_vreq(irdma, IRDMA_OP_CQ_CREATE)) == NULL) {
		ret = EIO;
		goto fail;
	}
	req->icr_cmd.in.u.cq_create.cq = &icq->icq_sc;
	req->icr_cmd.in.u.cq_create.check_overflow = true;
	req->icr_cmd.in.u.cq_create.scratch = irdma_req_scratch(irdma, req);
	if ((ret = irdma_cqp_exec(irdma, req, NULL)) != 0) {
		/* The command may have registered the CQ and reached it. */
		irdma_verbs_uncertain(irdma, "failed to create a CQ");
		mutex_enter(&icq->icq_ceq->ic_lock);
		icq->icq_dying = B_TRUE;
		irdma_sc_remove_cq_ctx(&icq->icq_ceq->ic_sc, &icq->icq_sc);
		irdma_sc_cleanup_ceqes(&icq->icq_sc, &icq->icq_ceq->ic_sc);
		mutex_exit(&icq->icq_ceq->ic_lock);
		goto fail;
	}

	mutex_enter(&irdma->irdma_cqtable_lock);
	irdma->irdma_cq_table[num] = icq;
	mutex_exit(&irdma->irdma_cqtable_lock);
	mutex_enter(&icq->icq_ceq->ic_lock);
	icq->icq_live = B_TRUE;
	mutex_exit(&icq->icq_ceq->ic_lock);
	atomic_inc_32(&irdma->irdma_ncqs);
	return (0);

fail:
	dma_free_coherent(&irdma->irdma_osdev, icq->icq_shadow.size,
	    icq->icq_shadow.va, icq->icq_shadow.pa);
	dma_free_coherent(&irdma->irdma_osdev, icq->icq_mem.size,
	    icq->icq_mem.va, icq->icq_mem.pa);
	list_destroy(&icq->icq_gen);
	cv_destroy(&icq->icq_cv);
	mutex_destroy(&icq->icq_lock);
	irdma_free_rsrc(irdma, irdma->irdma_cq_map, num);
	return (ret);
}

/*
 * The framework destroys a CQ only once no QP uses it.  After the CQ is off
 * its CEQ and no handler holds it, the destroy command stops the device.
 */
void
irdma_destroy_cq(struct rdk_cq *rcq)
{
	irdma_t *irdma = IRDMA_DEV(rcq->device);
	irdma_cq_t *icq = IRDMA_CQ(rcq);
	irdma_ceq_t *ic = icq->icq_ceq;
	irdma_cmpl_gen_t *g;
	irdma_cqp_req_t *req;

	mutex_enter(&irdma->irdma_cqtable_lock);
	irdma->irdma_cq_table[icq->icq_num] = NULL;
	mutex_exit(&irdma->irdma_cqtable_lock);

	mutex_enter(&ic->ic_lock);
	icq->icq_dying = B_TRUE;
	irdma_sc_remove_cq_ctx(&ic->ic_sc, &icq->icq_sc);
	irdma_sc_cleanup_ceqes(&icq->icq_sc, &ic->ic_sc);
	while (icq->icq_refs != 0)
		cv_wait(&icq->icq_cv, &ic->ic_lock);
	mutex_exit(&ic->ic_lock);

	if ((req = irdma_vreq(irdma, IRDMA_OP_CQ_DESTROY)) == NULL) {
		irdma_taint(irdma);
	} else {
		req->icr_cmd.in.u.cq_destroy.cq = &icq->icq_sc;
		req->icr_cmd.in.u.cq_destroy.scratch =
		    irdma_req_scratch(irdma, req);
		if (irdma_cqp_exec(irdma, req, NULL) != 0)
			irdma_verbs_uncertain(irdma, "failed to destroy a CQ");
	}
	/* Entries the device queued before it dropped the CQ. */
	mutex_enter(&ic->ic_lock);
	irdma_sc_cleanup_ceqes(&icq->icq_sc, &ic->ic_sc);
	mutex_exit(&ic->ic_lock);

	mutex_enter(&icq->icq_lock);
	while ((g = list_remove_head(&icq->icq_gen)) != NULL)
		kmem_free(g, sizeof (*g));
	mutex_exit(&icq->icq_lock);

	dma_free_coherent(&irdma->irdma_osdev, icq->icq_shadow.size,
	    icq->icq_shadow.va, icq->icq_shadow.pa);
	dma_free_coherent(&irdma->irdma_osdev, icq->icq_mem.size,
	    icq->icq_mem.va, icq->icq_mem.pa);
	list_destroy(&icq->icq_gen);
	cv_destroy(&icq->icq_cv);
	mutex_destroy(&icq->icq_lock);
	irdma_free_rsrc(irdma, irdma->irdma_cq_map, icq->icq_num);
	atomic_dec_32(&irdma->irdma_ncqs);
}

/*
 * The CEQ named sc_cq, one of the CQs registered on it.  The caller holds
 * the CEQ's ic_lock, under which destroy takes a CQ off the CEQ.
 */
irdma_cq_t *
irdma_cq_ceq_hold(irdma_ceq_t *ic, struct irdma_sc_cq *sc_cq)
{
	irdma_cq_t *icq = sc_cq->back_cq;

	ASSERT(MUTEX_HELD(&ic->ic_lock));
	if (icq == NULL || icq->icq_ceq != ic || !icq->icq_live ||
	    icq->icq_dying)
		return (NULL);
	icq->icq_refs++;
	return (icq);
}

static void
irdma_cq_rele(irdma_cq_t *icq)
{
	irdma_ceq_t *ic = icq->icq_ceq;

	mutex_enter(&ic->ic_lock);
	if (--icq->icq_refs == 0 && icq->icq_dying)
		cv_broadcast(&icq->icq_cv);
	mutex_exit(&ic->ic_lock);
}

/*
 * The provider's cq_resched: call the handler again from the CQ's vector
 * thread, after the handlers already waiting there.
 */
void
irdma_cq_resched(struct rdk_cq *rcq)
{
	irdma_cq_t *icq = IRDMA_CQ(rcq);
	irdma_ceq_t *ic = icq->icq_ceq;
	boolean_t kick = B_FALSE;

	mutex_enter(&ic->ic_lock);
	if (icq->icq_live && !icq->icq_dying && !icq->icq_resched) {
		icq->icq_resched = B_TRUE;
		icq->icq_refs++;
		list_insert_tail(&ic->ic_resched, icq);
		kick = B_TRUE;
	}
	mutex_exit(&ic->ic_lock);
	if (kick)
		irdma_ceq_kick(ic);
}

/* In the CQ's vector thread, with no driver lock held. */
void
irdma_cq_ceq_dispatch(irdma_cq_t *icq)
{
	struct rdk_cq *rcq = &icq->icq_rdk;

	mutex_enter(&icq->icq_lock);
	icq->icq_armed = B_FALSE;
	mutex_exit(&icq->icq_lock);
	if (rcq->comp_handler != NULL)
		rcq->comp_handler(rcq, rcq->cq_context);
	irdma_cq_rele(icq);
}

/* Completions the driver generated; the CQ may be armed. */
void
irdma_comp_handler(irdma_cq_t *icq)
{
	struct rdk_cq *rcq = &icq->icq_rdk;
	boolean_t armed;

	mutex_enter(&icq->icq_lock);
	armed = icq->icq_armed;
	icq->icq_armed = B_FALSE;
	mutex_exit(&icq->icq_lock);
	if (armed && rcq->comp_handler != NULL)
		rcq->comp_handler(rcq, rcq->cq_context);
}

/* An asynchronous CQ error from the AEQ. */
void
irdma_cq_error(irdma_t *irdma, uint32_t cq_id)
{
	struct rdk_event ev;
	irdma_cq_t *icq = NULL;

	if (cq_id >= irdma->irdma_max_cq)
		return;
	mutex_enter(&irdma->irdma_cqtable_lock);
	if ((icq = irdma->irdma_cq_table[cq_id]) != NULL) {
		irdma_ceq_t *ic = icq->icq_ceq;

		mutex_enter(&ic->ic_lock);
		if (icq->icq_dying)
			icq = NULL;
		else
			icq->icq_refs++;
		mutex_exit(&ic->ic_lock);
	}
	mutex_exit(&irdma->irdma_cqtable_lock);
	if (icq == NULL)
		return;

	irdma_error(irdma, "CQ %u error", cq_id);
	if (icq->icq_rdk.event_handler != NULL) {
		bzero(&ev, sizeof (ev));
		ev.device = icq->icq_rdk.device;
		ev.event = RDK_EVENT_CQ_ERR;
		ev.element.cq = &icq->icq_rdk;
		icq->icq_rdk.event_handler(&ev, icq->icq_rdk.cq_context);
	}
	irdma_cq_rele(icq);
}

boolean_t
irdma_cq_empty(irdma_cq_t *icq)
{
	struct irdma_cq_uk *uk = &icq->icq_sc.cq_uk;
	__le64 *cqe;
	u64 qword3;

	ASSERT(MUTEX_HELD(&icq->icq_lock));
	cqe = IRDMA_GET_CURRENT_CQ_ELEM(uk);
	get_64bit_val(cqe, 24, &qword3);
	return ((u8)FIELD_GET(IRDMA_CQ_VALID, qword3) != uk->polarity);
}

/*
 * Forget a QP that is going away: clear its entries in the ring and drop
 * the completions the driver made for it.
 */
void
irdma_cq_purge_qp(irdma_cq_t *icq, irdma_qp_t *iqp)
{
	irdma_cmpl_gen_t *g, *next;

	mutex_enter(&icq->icq_lock);
	irdma_uk_clean_cq(&iqp->iqp_sc.qp_uk, &icq->icq_sc.cq_uk);
	for (g = list_head(&icq->icq_gen); g != NULL; g = next) {
		next = list_next(&icq->icq_gen, g);
		if (g->icg_cpi.qp_handle == (irdma_qp_handle)&iqp->iqp_sc) {
			list_remove(&icq->icq_gen, g);
			kmem_free(g, sizeof (*g));
		}
	}
	mutex_exit(&icq->icq_lock);
}

/*
 * Fill in a work completion.  The caller holds icq_lock, so the QP the
 * entry names is live.  Returns B_FALSE for an entry to drop.
 */
static boolean_t
irdma_process_cqe(irdma_cq_t *icq, struct irdma_cq_poll_info *cpi,
    struct rdk_wc *wc)
{
	struct irdma_sc_qp *sc = (struct irdma_sc_qp *)cpi->qp_handle;
	irdma_qp_t *iqp = sc->qp_uk.back_qp;
	irdma_rq_slot_t *slot;

	bzero(wc, sizeof (*wc));
	wc->qp = &iqp->iqp_rdk;
	wc->wr_id = cpi->wr_id;
	wc->byte_len = cpi->bytes_xfered;
	wc->port_num = 1;

	if (cpi->error) {
		wc->status = cpi->comp_status == IRDMA_COMPL_STATUS_FLUSHED ?
		    irdma_flush_err_to_wc_status(cpi->minor_err) :
		    RDK_WC_GENERAL_ERR;
		wc->vendor_err = ((uint32_t)cpi->major_err << 16) |
		    cpi->minor_err;
	} else {
		wc->status = RDK_WC_SUCCESS;
		if (cpi->imm_valid) {
			wc->ex.imm_data = htonl(cpi->imm_data);
			wc->wc_flags |= RDK_WC_WITH_IMM;
		}
		if (cpi->ud_smac_valid) {
			bcopy(cpi->ud_smac, wc->smac, ETHERADDRL);
			wc->wc_flags |= RDK_WC_WITH_SMAC;
		}
		if (cpi->ud_vlan_valid) {
			wc->sl = cpi->ud_vlan >> 13;
			if ((cpi->ud_vlan & 0xfff) != 0) {
				wc->vlan_id = cpi->ud_vlan & 0xfff;
				wc->wc_flags |= RDK_WC_WITH_VLAN;
			}
		}
	}

	if (cpi->q_type == IRDMA_CQE_QTYPE_SQ) {
		wc->opcode = irdma_wc_op_sq(cpi->op_type, &wc->status);
	} else {
		/* The id is the driver's slot index; see irdma_post_recv(). */
		if (cpi->wr_id >= iqp->iqp_sc.qp_uk.rq_size) {
			icq->icq_bad_cqes++;
			atomic_inc_64(&icq->icq_irdma->irdma_bad_cqes);
			return (B_FALSE);
		}
		slot = &iqp->iqp_rq_slots[cpi->wr_id];
		wc->wr_id = slot->irs_wr_id;
		if (wc->status == RDK_WC_SUCCESS &&
		    cpi->bytes_xfered > slot->irs_len) {
			wc->status = RDK_WC_LOC_LEN_ERR;
			wc->byte_len = slot->irs_len;
			icq->icq_bad_cqes++;
		}
		wc->opcode = RDK_WC_RECV;
		if (cpi->op_type == IRDMA_RC_WRITE_ONLY_IMM ||
		    cpi->op_type == IRDMA_RC_WRITE_LAST_IMM)
			wc->opcode = RDK_WC_RECV_RDMA_WITH_IMM;
		if (sc->qp_uk.qp_type != IRDMA_QP_TYPE_ROCE_UD &&
		    cpi->stag_invalid_set) {
			wc->ex.invalidate_rkey = cpi->inv_stag;
			wc->wc_flags |= RDK_WC_WITH_INVALIDATE;
		}
	}

	if (sc->qp_uk.qp_type == IRDMA_QP_TYPE_ROCE_UD) {
		wc->src_qp = cpi->ud_src_qpn;
		wc->wc_flags |= RDK_WC_GRH | RDK_WC_WITH_NETWORK_HDR_TYPE;
		wc->network_hdr_type = cpi->ipv4 ? RDK_NETWORK_IPV4 :
		    RDK_NETWORK_IPV6;
	} else {
		wc->src_qp = cpi->qp_id;
	}
	return (B_TRUE);
}

int
irdma_poll_cq(struct rdk_cq *rcq, int n, struct rdk_wc *wc)
{
	irdma_cq_t *icq = IRDMA_CQ(rcq);
	struct irdma_cq_uk *uk = &icq->icq_sc.cq_uk;
	struct irdma_cq_poll_info *cpi = &icq->icq_cur;
	irdma_cmpl_gen_t *g;
	uint32_t tries;
	int got = 0, ret;

	if (n <= 0)
		return (0);

	mutex_enter(&icq->icq_lock);
	/* The device can keep entries valid; look at most a ring's worth. */
	for (tries = 0; got < n && tries < (uint32_t)n + uk->cq_size;
	    tries++) {
		bzero(cpi, sizeof (*cpi));
		ret = irdma_uk_cq_poll_cmpl(uk, cpi);
		if (ret == -ENOENT) {
			if ((g = list_remove_head(&icq->icq_gen)) == NULL)
				break;
			*cpi = g->icg_cpi;
			kmem_free(g, sizeof (*g));
			ret = 0;
		}
		if (ret != 0)
			continue;
		if (irdma_process_cqe(icq, cpi, &wc[got]))
			got++;
	}
	mutex_exit(&icq->icq_lock);
	return (got);
}

int
irdma_req_notify_cq(struct rdk_cq *rcq, enum rdk_cq_notify_flags flags)
{
	irdma_cq_t *icq = IRDMA_CQ(rcq);
	enum irdma_cmpl_notify notify;
	boolean_t promote;
	int ret = 0;

	notify = (flags & RDK_CQ_SOLICITED_MASK) == RDK_CQ_SOLICITED ?
	    IRDMA_CQ_COMPL_SOLICITED : IRDMA_CQ_COMPL_EVENT;

	mutex_enter(&icq->icq_lock);
	promote = icq->icq_last_notify == IRDMA_CQ_COMPL_SOLICITED &&
	    notify != IRDMA_CQ_COMPL_SOLICITED;
	if (!icq->icq_armed || promote) {
		icq->icq_armed = B_TRUE;
		icq->icq_last_notify = notify;
		irdma_uk_cq_request_notification(&icq->icq_sc.cq_uk, notify);
	}
	if ((flags & RDK_CQ_REPORT_MISSED_EVENTS) != 0 &&
	    (!irdma_cq_empty(icq) || !list_is_empty(&icq->icq_gen)))
		ret = 1;
	mutex_exit(&icq->icq_lock);
	return (ret);
}

/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2015 - 2021 Intel Corporation */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Asynchronous events, after irdma_process_aeq() of Linux irdma for RoCE
 * (see README.illumos).  Runs in the interrupt task.  An error on a QP
 * records the flush codes and queues the move to the error state on
 * irdma_wq; an error on a CQ goes to the CQ's event handler.
 *
 * Every field comes from the device: the QP or CQ number is looked up in
 * a bounded table, the WQE index is checked against the RQ before it goes
 * into the QP context, and completion contexts are never dereferenced.
 */

#include <sys/types.h>

#include "irdma_verbs.h"

static void
irdma_aeq_qp(irdma_t *irdma, const struct irdma_aeqe_info *info)
{
	struct qp_err_code err;
	struct irdma_sc_qp *sc;
	irdma_qp_t *iqp;

	if ((iqp = irdma_qp_get(irdma, info->qp_cq_id)) == NULL) {
		irdma->irdma_bad_entries++;
		return;
	}
	sc = &iqp->iqp_sc;

	mutex_enter(&iqp->iqp_lock);
	iqp->iqp_hw_ae_state = info->iwarp_state;
	if (info->ae_id != IRDMA_AE_QP_SUSPEND_COMPLETE)
		iqp->iqp_last_ae = info->ae_id;
	mutex_exit(&iqp->iqp_lock);

	switch (info->ae_id) {
	case IRDMA_AE_QP_SUSPEND_COMPLETE:
	case IRDMA_AE_LLP_CONNECTION_ESTABLISHED:
	case IRDMA_AE_RESET_NOT_SENT:
	case IRDMA_AE_LLP_DOUBT_REACHABILITY:
	case IRDMA_AE_RESOURCE_EXHAUSTION:
		break;
	default:
		irdma_error(irdma, "QP %u: async event 0x%x source 0x%x",
		    info->qp_cq_id, info->ae_id, info->ae_src);
		err = irdma_ae_to_qp_err_code(info->ae_id);
		mutex_enter(&iqp->iqp_lock);
		sc->sq_flush_code = info->sq;
		sc->rq_flush_code = info->rq;
		sc->flush_code = err.flush_code;
		sc->event_type = err.event_type;
		if (info->err_rq_idx_valid && info->wqe_idx <
		    sc->qp_uk.rq_size) {
			iqp->iqp_roce.err_rq_idx = info->wqe_idx;
			iqp->iqp_roce.err_rq_idx_valid = true;
			irdma_sc_qp_setctx_roce(sc, sc->hw_host_ctx,
			    &iqp->iqp_ctx);
		}
		mutex_exit(&iqp->iqp_lock);
		irdma_qp_to_error(iqp);
		break;
	}
	irdma_qp_rele(iqp);
}

void
irdma_aeq_process(irdma_t *irdma)
{
	struct irdma_aeqe_info info;
	uint32_t n = 0;

	while (n < irdma->irdma_aeq.elem_cnt) {
		bzero(&info, sizeof (info));
		if (irdma_sc_get_next_aeqe(&irdma->irdma_aeq, &info) != 0)
			break;
		n++;
		irdma->irdma_aeqes++;
		if (info.aeqe_overflow) {
			irdma_sc_repost_aeq_entries(&irdma->irdma_sc, n);
			irdma_fatal(irdma, "the AEQ overflowed");
			return;
		}
		if (info.qp && irdma->irdma_verbs_live) {
			irdma_aeq_qp(irdma, &info);
		} else if (info.cq && irdma->irdma_verbs_live) {
			irdma_cq_error(irdma, info.qp_cq_id);
		} else {
			irdma->irdma_bad_entries++;
			irdma_error(irdma, "async event 0x%x source 0x%x "
			    "id %u", info.ae_id, info.ae_src, info.qp_cq_id);
		}
	}
	if (n != 0)
		irdma_sc_repost_aeq_entries(&irdma->irdma_sc, n);
}

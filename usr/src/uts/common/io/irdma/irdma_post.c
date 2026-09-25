/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2015 - 2021 Intel Corporation */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Posting work requests, after irdma_post_send() and irdma_post_recv() of
 * Linux irdma (see README.illumos).  Kernel consumers are trusted to name
 * their own memory, but every count and length is still checked before it
 * sizes a WQE: struct rdk_sge has the layout of the device's fragment, and
 * the core code copies num_sge of them.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>

#include "irdma_verbs.h"

CTASSERT(sizeof (struct rdk_sge) == sizeof (struct ib_sge));
CTASSERT(offsetof(struct rdk_sge, addr) == offsetof(struct ib_sge, addr));
CTASSERT(offsetof(struct rdk_sge, length) ==
    offsetof(struct ib_sge, length));
CTASSERT(offsetof(struct rdk_sge, lkey) == offsetof(struct ib_sge, lkey));

uint16_t
irdma_get_mr_access(int access)
{
	uint16_t hw = IRDMA_ACCESS_FLAGS_LOCALREAD;

	if ((access & RDK_ACCESS_LOCAL_WRITE) != 0)
		hw |= IRDMA_ACCESS_FLAGS_LOCALWRITE;
	if ((access & RDK_ACCESS_REMOTE_WRITE) != 0)
		hw |= IRDMA_ACCESS_FLAGS_REMOTEWRITE;
	if ((access & RDK_ACCESS_REMOTE_READ) != 0)
		hw |= IRDMA_ACCESS_FLAGS_REMOTEREAD;
	if ((access & RDK_ACCESS_ZERO_BASED) != 0)
		hw |= IRDMA_ACCESS_FLAGS_ZERO_BASED;
	return (hw);
}

static int
irdma_post_reg(irdma_qp_t *iqp, const struct rdk_reg_wr *rw,
    const struct irdma_post_sq_info *pi)
{
	irdma_mr_t *mr = IRDMA_MR(rw->mr);
	struct irdma_fast_reg_stag_info si;

	if (rw->mr == NULL || rw->mr->device != iqp->iqp_rdk.device ||
	    rw->mr->pd != iqp->iqp_rdk.pd || !mr->imr_pble_live ||
	    mr->imr_npages == 0 || mr->imr_npages > mr->imr_page_cnt ||
	    (rw->key >> 8) != (mr->imr_stag >> 8) ||
	    (rw->access & ~(RDK_ACCESS_LOCAL_WRITE | RDK_ACCESS_REMOTE_WRITE |
	    RDK_ACCESS_REMOTE_READ)) != 0 ||
	    rw->mr->length > (uint64_t)mr->imr_npages * rw->mr->page_size)
		return (EINVAL);

	bzero(&si, sizeof (si));
	si.signaled = pi->signaled;
	si.read_fence = pi->read_fence;
	si.local_fence = pi->read_fence;
	si.access_rights = irdma_get_mr_access(rw->access);
	si.stag_key = rw->key & 0xff;
	si.stag_idx = rw->key >> 8;
	si.page_size = rw->mr->page_size;
	si.wr_id = pi->wr_id;
	si.addr_type = IRDMA_ADDR_TYPE_VA_BASED;
	si.va = (void *)(uintptr_t)rw->mr->iova;
	si.total_len = rw->mr->length;
	si.reg_addr_pa = *mr->imr_pble.level1.addr;
	si.first_pm_pbl_index = mr->imr_pble.level1.idx;
	if (mr->imr_npages > IRDMA_MIN_PAGES_PER_FMR)
		si.chunk_size = 1;
	return (irdma_sc_mr_fast_register(&iqp->iqp_sc, &si, false) == 0 ?
	    0 : ENOMEM);
}

int
irdma_post_send(struct rdk_qp *rqp, const struct rdk_send_wr *wr,
    const struct rdk_send_wr **bad)
{
	irdma_qp_t *iqp = IRDMA_QP(rqp);
	struct irdma_qp_uk *uk = &iqp->iqp_sc.qp_uk;
	struct irdma_uk_attrs *uka = uk->uk_attrs;
	struct irdma_post_sq_info pi;
	boolean_t flushed;
	int err = 0;

	mutex_enter(&iqp->iqp_lock);
	for (; wr != NULL; wr = wr->next) {
		bzero(&pi, sizeof (pi));
		pi.wr_id = wr->wr_id;
		pi.signaled = (wr->send_flags & RDK_SEND_SIGNALED) != 0 ||
		    iqp->iqp_sig_all;
		pi.read_fence = (wr->send_flags & RDK_SEND_FENCE) != 0;
		if (wr->num_sge < 0 || (uint32_t)wr->num_sge >
		    uk->max_sq_frag_cnt || (wr->num_sge > 0 &&
		    wr->sg_list == NULL)) {
			err = EINVAL;
			break;
		}

		switch (wr->opcode) {
		case RDK_WR_SEND_WITH_IMM:
			pi.imm_data_valid = true;
			pi.imm_data = ntohl(wr->ex.imm_data);
			/* FALLTHROUGH */
		case RDK_WR_SEND:
		case RDK_WR_SEND_WITH_INV:
			if (wr->opcode == RDK_WR_SEND_WITH_INV) {
				pi.op_type = (wr->send_flags &
				    RDK_SEND_SOLICITED) != 0 ?
				    IRDMA_OP_TYPE_SEND_SOL_INV :
				    IRDMA_OP_TYPE_SEND_INV;
				pi.stag_to_inv = wr->ex.invalidate_rkey;
			} else {
				pi.op_type = (wr->send_flags &
				    RDK_SEND_SOLICITED) != 0 ?
				    IRDMA_OP_TYPE_SEND_SOL :
				    IRDMA_OP_TYPE_SEND;
			}
			pi.op.send.num_sges = (u32)wr->num_sge;
			pi.op.send.sg_list = (struct ib_sge *)wr->sg_list;
			if (rqp->qp_type == RDK_QPT_UD ||
			    rqp->qp_type == RDK_QPT_GSI) {
				const struct rdk_ud_wr *ud = RDK_UD_WR(wr);

				if (ud->ah == NULL ||
				    ud->ah->device != rqp->device ||
				    wr->opcode == RDK_WR_SEND_WITH_INV ||
				    ud->remote_qpn > 0xffffff) {
					err = EINVAL;
					break;
				}
				pi.op.send.ah_id =
				    IRDMA_AH(ud->ah)->iah_sc.ah_info.ah_idx;
				pi.op.send.qkey = ud->remote_qkey;
				pi.op.send.dest_qp = ud->remote_qpn;
			}
			if ((wr->send_flags & RDK_SEND_INLINE) != 0)
				err = irdma_uk_inline_send(uk, &pi, false);
			else
				err = irdma_uk_send(uk, &pi, false);
			err = err == 0 ? 0 : (err == -ENOMEM ? ENOMEM : EINVAL);
			break;
		case RDK_WR_RDMA_WRITE_WITH_IMM:
			pi.imm_data_valid = true;
			pi.imm_data = ntohl(wr->ex.imm_data);
			/* FALLTHROUGH */
		case RDK_WR_RDMA_WRITE:
			if (rqp->qp_type != RDK_QPT_RC) {
				err = EINVAL;
				break;
			}
			pi.op_type = IRDMA_OP_TYPE_RDMA_WRITE;
			if ((wr->send_flags & RDK_SEND_SOLICITED) != 0)
				pi.op_type = IRDMA_OP_TYPE_RDMA_WRITE_SOL;
			pi.op.rdma_write.num_lo_sges = (u32)wr->num_sge;
			pi.op.rdma_write.lo_sg_list =
			    (struct ib_sge *)wr->sg_list;
			pi.op.rdma_write.rem_addr.addr =
			    RDK_RDMA_WR(wr)->remote_addr;
			pi.op.rdma_write.rem_addr.lkey = RDK_RDMA_WR(wr)->rkey;
			if ((wr->send_flags & RDK_SEND_INLINE) != 0) {
				err = irdma_uk_inline_rdma_write(uk, &pi,
				    false);
			} else {
				err = irdma_uk_rdma_write(uk, &pi, false);
			}
			err = err == 0 ? 0 : (err == -ENOMEM ? ENOMEM : EINVAL);
			break;
		case RDK_WR_RDMA_READ_WITH_INV:
		case RDK_WR_RDMA_READ:
			if (rqp->qp_type != RDK_QPT_RC ||
			    (uint32_t)wr->num_sge > uka->max_hw_read_sges) {
				err = EINVAL;
				break;
			}
			pi.op_type = IRDMA_OP_TYPE_RDMA_READ;
			pi.op.rdma_read.rem_addr.addr =
			    RDK_RDMA_WR(wr)->remote_addr;
			pi.op.rdma_read.rem_addr.lkey = RDK_RDMA_WR(wr)->rkey;
			pi.op.rdma_read.lo_sg_list =
			    (struct ib_sge *)wr->sg_list;
			pi.op.rdma_read.num_lo_sges = (u32)wr->num_sge;
			err = irdma_uk_rdma_read(uk, &pi,
			    wr->opcode == RDK_WR_RDMA_READ_WITH_INV, false);
			err = err == 0 ? 0 : (err == -ENOMEM ? ENOMEM : EINVAL);
			break;
		case RDK_WR_LOCAL_INV:
			if (rqp->qp_type != RDK_QPT_RC) {
				err = EINVAL;
				break;
			}
			pi.op_type = IRDMA_OP_TYPE_INV_STAG;
			pi.local_fence = pi.read_fence;
			pi.op.inv_local_stag.target_stag =
			    wr->ex.invalidate_rkey;
			err = irdma_uk_stag_local_invalidate(uk, &pi, false);
			err = err == 0 ? 0 : (err == -ENOMEM ? ENOMEM : EINVAL);
			break;
		case RDK_WR_REG_MR:
			if (rqp->qp_type != RDK_QPT_RC) {
				err = EINVAL;
				break;
			}
			err = irdma_post_reg(iqp, RDK_REG_WR(wr), &pi);
			break;
		default:
			err = EINVAL;
			break;
		}
		if (err != 0)
			break;
	}

	flushed = iqp->iqp_flush_issued;
	if (!flushed && iqp->iqp_hw_ae_state <= IRDMA_QP_STATE_RTS)
		irdma_uk_qp_post_wr(uk);
	mutex_exit(&iqp->iqp_lock);
	if (flushed)
		irdma_flush_later(iqp);

	if (err != 0)
		*bad = wr;
	return (err);
}

int
irdma_post_recv(struct rdk_qp *rqp, const struct rdk_recv_wr *wr,
    const struct rdk_recv_wr **bad)
{
	irdma_qp_t *iqp = IRDMA_QP(rqp);
	struct irdma_qp_uk *uk = &iqp->iqp_sc.qp_uk;
	struct irdma_post_rq_info pi;
	uint64_t total;
	uint32_t slot;
	boolean_t flushed;
	int i, err = 0;

	mutex_enter(&iqp->iqp_lock);
	for (; wr != NULL; wr = wr->next) {
		if (wr->num_sge < 0 ||
		    (uint32_t)wr->num_sge > uk->max_rq_frag_cnt ||
		    (wr->num_sge > 0 && wr->sg_list == NULL)) {
			err = EINVAL;
			break;
		}
		for (total = 0, i = 0; i < wr->num_sge; i++)
			total += wr->sg_list[i].length;
		/* The device reports at most 32 bits of payload. */
		if (total > UINT32_MAX) {
			err = EINVAL;
			break;
		}

		/*
		 * uk uses the free slot at the head for this receive.  Fill it
		 * before the WQE turns valid: a poller does not take iqp_lock.
		 */
		slot = IRDMA_RING_CURRENT_HEAD(uk->rq_ring);
		iqp->iqp_rq_slots[slot].irs_wr_id = wr->wr_id;
		iqp->iqp_rq_slots[slot].irs_len = (uint32_t)total;
		bzero(&pi, sizeof (pi));
		pi.wr_id = slot;
		pi.num_sges = (u32)wr->num_sge;
		pi.sg_list = (struct ib_sge *)wr->sg_list;
		if (irdma_uk_post_receive(uk, &pi) != 0) {
			err = ENOMEM;
			break;
		}
	}
	flushed = iqp->iqp_flush_issued;
	mutex_exit(&iqp->iqp_lock);
	if (flushed)
		irdma_flush_later(iqp);

	if (err != 0)
		*bad = wr;
	return (err);
}

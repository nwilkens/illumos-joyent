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
 * The offload queues.  Each port gets a control queue and an offload Tx queue.
 * The adapter gets one connection queue, with a free list for CPLs and their
 * payload, and one CQ event queue for RDMA completion notifications; each
 * takes an MSI-X vector from the block t4_cfg_intrs_queues() reserved after
 * the LAN vectors.
 *
 * The egress queues report progress only through their status page; a queue
 * that fills returns EAGAIN rather than waiting.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/strsun.h>
#include <sys/atomic.h>

#include "common/common.h"
#include "common/t4_regs.h"
#include "common/t4_regs_values.h"
#include "t4_ofld.h"

/* The handler argument: T4_OFLD_Q_RX, or T4_OFLD_Q_CIQ plus the CIQ. */
#define	T4_OFLD_Q_RX	1
#define	T4_OFLD_Q_CIQ	2

static void
t4_ofld_eq_cmd_common(struct adapter *sc, t4_sge_eq_t *eq, uint32_t *fetch,
    uint32_t *dca)
{
	const uint_t fbmin = t4_cver_ge(sc, CHELSIO_T6) ?
	    X_FETCHBURSTMIN_64B_T6 : X_FETCHBURSTMIN_64B;

	/* The ctrl and ofld commands share these field layouts. */
	*fetch = V_FW_EQ_CTRL_CMD_HOSTFCMODE(X_HOSTFCMODE_STATUS_PAGE) |
	    V_FW_EQ_CTRL_CMD_PCIECHN(eq->tse_tx_chan) |
	    F_FW_EQ_CTRL_CMD_FETCHRO | V_FW_EQ_CTRL_CMD_IQID(eq->tse_iqid);
	*dca = V_FW_EQ_CTRL_CMD_FBMIN(fbmin) |
	    V_FW_EQ_CTRL_CMD_FBMAX(X_FETCHBURSTMAX_512B) |
	    V_FW_EQ_CTRL_CMD_CIDXFTHRESH(X_CIDXFLUSHTHRESH_32) |
	    F_FW_EQ_CTRL_CMD_CIDXFTHRESHO |
	    V_FW_EQ_CTRL_CMD_EQSIZE(eq->tse_qsize_spg);
}

CTASSERT(S_FW_EQ_CTRL_CMD_HOSTFCMODE == S_FW_EQ_OFLD_CMD_HOSTFCMODE);
CTASSERT(S_FW_EQ_CTRL_CMD_PCIECHN == S_FW_EQ_OFLD_CMD_PCIECHN);
CTASSERT(S_FW_EQ_CTRL_CMD_IQID == S_FW_EQ_OFLD_CMD_IQID);
CTASSERT(S_FW_EQ_CTRL_CMD_FBMIN == S_FW_EQ_OFLD_CMD_FBMIN);
CTASSERT(S_FW_EQ_CTRL_CMD_FBMAX == S_FW_EQ_OFLD_CMD_FBMAX);
CTASSERT(S_FW_EQ_CTRL_CMD_CIDXFTHRESH == S_FW_EQ_OFLD_CMD_CIDXFTHRESH);
CTASSERT(S_FW_EQ_CTRL_CMD_CIDXFTHRESHO == S_FW_EQ_OFLD_CMD_CIDXFTHRESHO);
CTASSERT(S_FW_EQ_CTRL_CMD_EQSIZE == S_FW_EQ_OFLD_CMD_EQSIZE);
CTASSERT(S_FW_EQ_CTRL_CMD_FETCHRO == S_FW_EQ_OFLD_CMD_FETCHRO);

static int
t4_ofld_alloc_eq(t4_ofld_port_t *op, t4_sge_eq_t *eq, t4_eq_type_t type,
    uint16_t qsize)
{
	struct port_info *pi = op->op_pi;
	struct adapter *sc = pi->adapter;
	uint32_t fetch, dca, id;
	t4_sge_eq_t **slot;
	int rc;

	bzero(eq, sizeof (*eq));
	eq->tse_type = type;
	eq->tse_qsize = qsize;
	eq->tse_tx_chan = pi->tx_chan;
	eq->tse_iqid = op->op_ofld->of_rxq.iq.tsi_cntxt_id;
	if ((rc = t4_alloc_eq_base(pi, eq)) != 0)
		return (rc);
	t4_ofld_eq_cmd_common(sc, eq, &fetch, &dca);

	if (type == TEQT_CTRL) {
		struct fw_eq_ctrl_cmd c;

		bzero(&c, sizeof (c));
		c.op_to_vfn = BE_32(V_FW_CMD_OP(FW_EQ_CTRL_CMD) |
		    F_FW_CMD_REQUEST | F_FW_CMD_WRITE | F_FW_CMD_EXEC |
		    V_FW_EQ_CTRL_CMD_PFN(sc->pf) | V_FW_EQ_CTRL_CMD_VFN(0));
		c.alloc_to_len16 = BE_32(F_FW_EQ_CTRL_CMD_ALLOC |
		    F_FW_EQ_CTRL_CMD_EQSTART | FW_LEN16(c));
		c.cmpliqid_eqid = BE_32(V_FW_EQ_CTRL_CMD_CMPLIQID(
		    eq->tse_iqid));
		c.fetchszm_to_iqid = BE_32(fetch);
		c.dcaen_to_eqsize = BE_32(dca);
		c.eqaddr = BE_64(eq->tse_ring_ba);
		rc = -t4_wr_mbox(sc, sc->mbox, &c, sizeof (c), &c);
		id = G_FW_EQ_CTRL_CMD_EQID(BE_32(c.cmpliqid_eqid));
	} else {
		struct fw_eq_ofld_cmd c;

		bzero(&c, sizeof (c));
		c.op_to_vfn = BE_32(V_FW_CMD_OP(FW_EQ_OFLD_CMD) |
		    F_FW_CMD_REQUEST | F_FW_CMD_WRITE | F_FW_CMD_EXEC |
		    V_FW_EQ_OFLD_CMD_PFN(sc->pf) | V_FW_EQ_OFLD_CMD_VFN(0));
		c.alloc_to_len16 = BE_32(F_FW_EQ_OFLD_CMD_ALLOC |
		    F_FW_EQ_OFLD_CMD_EQSTART | FW_LEN16(c));
		c.fetchszm_to_iqid = BE_32(fetch);
		c.dcaen_to_eqsize = BE_32(dca);
		c.eqaddr = BE_64(eq->tse_ring_ba);
		rc = -t4_wr_mbox(sc, sc->mbox, &c, sizeof (c), &c);
		id = G_FW_EQ_OFLD_CMD_EQID(BE_32(c.eqid_pkd));
	}
	if (rc != 0) {
		cxgb_printf(sc->dip, CE_WARN, "failed to create offload egress "
		    "queue: %d", rc);
		t4_free_eq(pi, eq);
		return (rc);
	}
	eq->tse_cntxt_id = id;
	eq->tse_flags |= EQ_ALLOC_DEV;
	if ((slot = t4_eqmap_ent(sc, id)) == NULL) {
		cxgb_printf(sc->dip, CE_WARN, "firmware returned an egress "
		    "queue ID outside this PF's range: %u", id);
		t4_free_eq(pi, eq);
		return (EIO);
	}
	*slot = eq;
	t4_alloc_eq_post(pi, eq);

	EQ_LOCK(eq);
	eq->tse_flags |= EQ_ENABLED;
	EQ_UNLOCK(eq);
	return (0);
}

static void
t4_ofld_iq_enable(t4_sge_iq_t *iq)
{
	IQ_LOCK(iq);
	iq->tsi_flags |= IQ_ENABLED;
	t4_iq_gts_update(iq, iq->tsi_gts_rearm, 0);
	IQ_UNLOCK(iq);
}

static void
t4_ofld_iq_disable(t4_sge_iq_t *iq)
{
	if ((iq->tsi_flags & IQ_ALLOC_HOST) == 0)
		return;
	IQ_LOCK(iq);
	iq->tsi_flags &= ~IQ_ENABLED;
	IQ_UNLOCK(iq);
}

static int
t4_ofld_port_queues_init(t4_ofld_port_t *op)
{
	int rc;

	if ((rc = t4_ofld_alloc_eq(op, &op->op_ctrlq, TEQT_CTRL,
	    T4_OFLD_CTRLQ_QSIZE)) != 0)
		return (rc);
	if ((rc = t4_ofld_alloc_eq(op, &op->op_txq, TEQT_OFLD,
	    T4_OFLD_TXQ_QSIZE)) != 0)
		return (rc);
	op->op_mtu = op->op_pi->mtu;
	return (0);
}

static int
t4_ofld_iqs_init(t4_ofld_t *of)
{
	struct port_info *pi = of->of_port[0].op_pi;
	struct adapter *sc = of->of_sc;
	int rc;

	of->of_rxq.port = pi;
	const t4_iq_params_t rxp = {
		.tip_iq_type	= TIQT_OFLD_RX,
		.tip_tmr_idx	= sc->props.ethq_tmr_idx,
		.tip_pktc_idx	= sc->props.ethq_pktc_idx,
		.tip_qsize	= T4_OFLD_RXQ_QSIZE,
		.tip_esize	= RX_IQ_ESIZE,
		.tip_fl_qsize	= T4_OFLD_FL_QSIZE,
		.tip_cong_chan	= -1,
		.tip_intr_evtq	= NULL,
		.tip_intr_idx	= of->of_rxq_vec,
	};
	if ((rc = t4_alloc_iq(pi, &rxp, &of->of_rxq.iq, &of->of_rxq.fl)) != 0)
		return (rc);

	for (uint_t i = 0; i < of->of_nciq; i++) {
		const t4_iq_params_t ciqp = {
			.tip_iq_type	= TIQT_OFLD_CIQ,
			.tip_tmr_idx	= sc->props.ethq_tmr_idx,
			.tip_pktc_idx	= sc->props.ethq_pktc_idx,
			.tip_qsize	= T4_OFLD_CIQ_QSIZE,
			.tip_esize	= RX_IQ_ESIZE,
			.tip_cong_chan	= -1,
			.tip_intr_evtq	= NULL,
			.tip_intr_idx	= of->of_ciq_vec + i,
		};
		if ((rc = t4_alloc_iq(pi, &ciqp, &of->of_ciq[i], NULL)) != 0)
			return (rc);
	}

	/* CPLs name the connection queue in a 10 bit field. */
	if (of->of_rxq.iq.tsi_abs_id > M_TID_QID) {
		cxgb_printf(sc->dip, CE_WARN, "offload queue ID %u does not "
		    "fit in a CPL", of->of_rxq.iq.tsi_abs_id);
		return (ERANGE);
	}
	return (0);
}

/* Keep the starving list from reaching a free list about to go. */
static void
t4_ofld_fl_doom(t4_ofld_t *of)
{
	struct adapter *sc = of->of_sc;
	struct sge_fl *fl = &of->of_rxq.fl;

	if ((fl->eq.tse_flags & EQ_ALLOC_HOST) != 0) {
		mutex_enter(&sc->sfl_lock);
		FL_LOCK(fl);
		fl->sfl_flags |= SFL_DOOMED;
		if ((fl->sfl_flags & SFL_STARVING) != 0) {
			list_remove(&sc->sfl_list, fl);
			fl->sfl_flags &= ~SFL_STARVING;
		}
		FL_UNLOCK(fl);
		mutex_exit(&sc->sfl_lock);
	}
}

static void
t4_ofld_iqs_fini(t4_ofld_t *of)
{
	struct port_info *pi = of->of_port[0].op_pi;

	for (uint_t i = 0; i < of->of_nciq; i++) {
		if ((of->of_ciq[i].tsi_flags & IQ_ALLOC_HOST) != 0)
			t4_free_iq(pi, &of->of_ciq[i]);
	}
	if ((of->of_rxq.iq.tsi_flags & IQ_ALLOC_HOST) != 0)
		t4_free_iq(pi, &of->of_rxq.iq);
	bzero(&of->of_rxq, sizeof (of->of_rxq));
}

/*
 * Free the queues in the firmware first.  If the firmware keeps any of them,
 * the chip can still write to the host memory of all of them, so that memory
 * is leaked on purpose.
 */
static boolean_t
t4_ofld_queues_free_dev(t4_ofld_t *of)
{
	boolean_t ok = B_TRUE;

	for (uint_t i = 0; i < of->of_nports; i++) {
		t4_ofld_port_t *op = &of->of_port[i];

		if (t4_free_eq_dev(of->of_sc, &op->op_ctrlq) != 0)
			ok = B_FALSE;
		if (t4_free_eq_dev(of->of_sc, &op->op_txq) != 0)
			ok = B_FALSE;
	}
	for (uint_t i = 0; i < of->of_nciq; i++) {
		if (t4_free_iq_dev(&of->of_ciq[i]) != 0)
			ok = B_FALSE;
	}
	if (t4_free_iq_dev(&of->of_rxq.iq) != 0)
		ok = B_FALSE;
	if (!ok) {
		cxgb_printf(of->of_sc->dip, CE_WARN, "offload queues not "
		    "freed by the firmware; their host memory is leaked");
		for (uint_t i = 0; i < of->of_nports; i++) {
			bzero(&of->of_port[i].op_ctrlq,
			    sizeof (of->of_port[i].op_ctrlq));
			bzero(&of->of_port[i].op_txq,
			    sizeof (of->of_port[i].op_txq));
		}
		bzero(of->of_ciq, sizeof (of->of_ciq));
		bzero(&of->of_rxq, sizeof (of->of_rxq));
	}
	return (ok);
}

int
t4_ofld_queues_init(t4_ofld_t *of)
{
	int rc;

	if ((rc = t4_ofld_iqs_init(of)) == 0) {
		for (uint_t i = 0; i < of->of_nports; i++) {
			if ((rc = t4_ofld_port_queues_init(&of->of_port[i])) !=
			    0)
				break;
		}
	}
	if (rc != 0) {
		t4_ofld_queues_fini(of);
		return (rc);
	}
	t4_ofld_iq_enable(&of->of_rxq.iq);
	for (uint_t i = 0; i < of->of_nciq; i++)
		t4_ofld_iq_enable(&of->of_ciq[i]);
	return (0);
}

void
t4_ofld_queues_fini(t4_ofld_t *of)
{
	mutex_enter(&of->of_lock);
	of->of_queues_up = B_FALSE;
	mutex_exit(&of->of_lock);

	t4_ofld_iq_disable(&of->of_rxq.iq);
	for (uint_t i = 0; i < of->of_nciq; i++)
		t4_ofld_iq_disable(&of->of_ciq[i]);
	t4_ofld_fl_doom(of);
	if (!t4_ofld_queues_free_dev(of))
		return;
	for (uint_t i = 0; i < of->of_nports; i++) {
		t4_ofld_port_t *op = &of->of_port[i];

		if ((op->op_ctrlq.tse_flags & EQ_ALLOC_HOST) != 0)
			t4_free_eq(op->op_pi, &op->op_ctrlq);
		if ((op->op_txq.tse_flags & EQ_ALLOC_HOST) != 0)
			t4_free_eq(op->op_pi, &op->op_txq);
	}
	t4_ofld_iqs_fini(of);
}

/*
 * Add the handlers for the offload vector block.  Called by t4_setup_intrs();
 * handlers counts the ones added so that a failure can remove them.
 */
int
t4_ofld_intr_handlers(t4_ofld_t *of, int *handlers)
{
	struct adapter *sc = of->of_sc;
	const struct t4_intrs_queues *iaq = &sc->intr_queue_cfg;
	int rc;

	of->of_rxq_vec = iaq->intr_rdma_first;
	of->of_ciq_vec = iaq->intr_rdma_first + 1;
	VERIFY3U(iaq->intr_rdma, ==, 1 + of->of_nciq);
	VERIFY3U(of->of_ciq_vec + of->of_nciq, <=, iaq->intr_count);

	rc = ddi_intr_add_handler(sc->intr_handle[of->of_rxq_vec],
	    t4_intr_ofld, (caddr_t)of, (caddr_t)T4_OFLD_Q_RX);
	if (rc != DDI_SUCCESS)
		return (rc);
	*handlers += 1;
	for (uint_t i = 0; i < of->of_nciq; i++) {
		rc = ddi_intr_add_handler(sc->intr_handle[of->of_ciq_vec + i],
		    t4_intr_ofld, (caddr_t)of,
		    (caddr_t)(uintptr_t)(T4_OFLD_Q_CIQ + i));
		if (rc != DDI_SUCCESS)
			return (rc);
		*handlers += 1;
	}
	return (DDI_SUCCESS);
}

/*
 * Service an offload ingress queue.  The CPLs are collected under the queue
 * lock and handed out after it is dropped.
 */
static void
t4_ofld_iq_service(t4_ofld_t *of, t4_sge_iq_t *iq, t4_rdma_queue_t q,
    uint_t vec)
{
	struct adapter *sc = iq->tsi_adapter;
	struct sge_fl *fl = iq->tsi_fl;
	const size_t inl = iq->tsi_esize_bytes - sizeof (struct rsp_ctrl) -
	    sizeof (struct rss_header);
	mblk_t *head = NULL, **tailp = &head, *mp;
	uint32_t cqs[T4_OFLD_CIQ_QSIZE / 8];
	uint_t cidx_incr = 0, budget, ncq = 0;
	t4_gts_config_t rearm;
	struct rsp_ctrl ctrl;
	boolean_t broken = B_FALSE;

	IQ_LOCK(iq);
	if ((iq->tsi_flags & IQ_ENABLED) == 0) {
		IQ_UNLOCK(iq);
		return;
	}
	budget = MIN(iq->tsi_qsize / 8, ARRAY_SIZE(cqs));
	rearm = iq->tsi_gts_rearm;

	while (cidx_incr < budget && t4_iq_next_rsp(iq, &ctrl)) {
		const struct rss_header *rss = iq->tsi_cdesc;
		const uint8_t type = G_RSPD_TYPE(ctrl.u.type_gen);
		const uint8_t opcode = rss->opcode;

		if ((ctrl.u.type_gen & F_RSPD_QOVFL) != 0)
			iq->tsi_stats.sis_overflow++;

		mp = NULL;
		if (type == X_RSPD_TYPE_CPL) {
			if ((mp = allocb(inl, BPRI_HI)) == NULL) {
				T4_OFLD_STAT(of, os_cpl_nomem);
				rearm = TGC_TIMER5;
				break;
			}
			bcopy(&rss[1], mp->b_wptr, inl);
			mp->b_wptr += inl;
		} else if (type == X_RSPD_TYPE_FLBUF && fl != NULL) {
			const uint32_t dlen_nb = BE_32(ctrl.pldbuflen_qid);
			const uint32_t len = G_RSPD_LEN(dlen_nb);
			int rc;

			if (len == 0 || len > T4_OFLD_MAX_PAYLOAD) {
				T4_OFLD_STAT(of, os_fl_badlen);
				broken = B_TRUE;
				break;
			}
			mp = t4_fl_payload(fl, len,
			    (dlen_nb & F_RSPD_NEWBUF) != 0, &rc);
			if (mp == NULL) {
				if (rc == ERANGE) {
					T4_OFLD_STAT(of, os_fl_badlen);
					broken = B_TRUE;
				} else {
					T4_OFLD_STAT(of, os_cpl_nomem);
					rearm = TGC_TIMER5;
				}
				break;
			}
		} else if (type == X_RSPD_TYPE_INTR && q == T4_RDMA_Q_CIQ) {
			/* A CQ with new entries, named by its context ID. */
			cqs[ncq++] = BE_32(ctrl.pldbuflen_qid);
		} else {
			iq->tsi_stats.sis_bad_cpl++;
		}

		if (mp != NULL) {
			mp->b_band = opcode;
			*tailp = mp;
			tailp = &mp->b_next;
		}
		t4_iq_advance(iq);
		cidx_incr++;
		iq->tsi_stats.sis_processed++;
	}

	/*
	 * The free list position can no longer be trusted once the device
	 * reports a payload it could not have placed; stop the queue.
	 */
	if (broken)
		iq->tsi_flags &= ~IQ_ENABLED;
	else
		t4_iq_gts_update(iq, rearm, cidx_incr);
	IQ_UNLOCK(iq);

	if (fl != NULL && !broken)
		t4_fl_replenish(sc, fl);
	if (broken) {
		cxgb_printf(sc->dip, CE_WARN, "offload queue %u reported a "
		    "bad payload length; queue stopped", iq->tsi_cntxt_id);
		t4_ofld_fatal(sc);
	}

	if (ncq != 0)
		t4_ofld_cq_notify(of, vec, cqs, ncq);
	while ((mp = head) != NULL) {
		const uint8_t opcode = mp->b_band;

		head = mp->b_next;
		mp->b_next = NULL;
		mp->b_band = 0;
		T4_OFLD_STAT(of, os_cpl_rx);
		t4_ofld_cpl_dispatch(of, q, opcode, mp);
	}
}

uint_t
t4_intr_ofld(caddr_t arg1, caddr_t arg2)
{
	t4_ofld_t *of = (t4_ofld_t *)arg1;
	const uintptr_t q = (uintptr_t)arg2;

	if (q >= T4_OFLD_Q_CIQ && q - T4_OFLD_Q_CIQ < of->of_nciq) {
		t4_ofld_iq_service(of, &of->of_ciq[q - T4_OFLD_Q_CIQ],
		    T4_RDMA_Q_CIQ, (uint_t)(q - T4_OFLD_Q_CIQ));
	} else {
		t4_ofld_iq_service(of, &of->of_rxq.iq, T4_RDMA_Q_RX, 0);
	}
	return (DDI_INTR_CLAIMED);
}

/*
 * Take back the credits the device consumed, from the status page.  A cidx
 * the device could not have reached stops the queue.  The caller holds the EQ
 * lock.
 */
static boolean_t
t4_ofld_eq_reclaim(t4_ofld_t *of, t4_sge_eq_t *eq)
{
	uint16_t hw_cidx, done, inflight;

	EQ_LOCK_ASSERT_OWNED(eq);
	(void) ddi_dma_sync(eq->tse_ring_dhdl,
	    (off_t)eq->tse_qsize * EQ_HC_SIZE, sizeof (struct sge_qstat),
	    DDI_DMA_SYNC_FORKERNEL);
	hw_cidx = BE_16(eq->tse_spg->cidx);
	inflight = eq->tse_qsize - 1 - eq->tse_avail;
	done = hw_cidx >= eq->tse_cidx ? hw_cidx - eq->tse_cidx :
	    hw_cidx + eq->tse_qsize - eq->tse_cidx;
	if (hw_cidx >= eq->tse_qsize || done > inflight) {
		T4_OFLD_STAT(of, os_eq_bad_cidx);
		eq->tse_flags &= ~EQ_ENABLED;
		return (B_FALSE);
	}
	eq->tse_cidx = hw_cidx;
	eq->tse_avail += done;
	return (B_TRUE);
}

/*
 * Copy one work request into an offload egress queue and ring its doorbell.
 * len is a multiple of 16 bytes and at most SGE_MAX_WR_LEN.  Any context.
 */
int
t4_ofld_wr_send(t4_ofld_t *of, t4_sge_eq_t *eq, const void *wr, size_t len)
{
	const uint16_t ncred = howmany(len, EQ_HC_SIZE);
	const uint8_t *src = wr;
	uint8_t *ring;
	size_t first;

	if (len < 16 || len > SGE_MAX_WR_LEN || (len & 15) != 0)
		return (EINVAL);

	EQ_LOCK(eq);
	if ((eq->tse_flags & EQ_ENABLED) == 0 ||
	    !t4_ofld_eq_reclaim(of, eq)) {
		EQ_UNLOCK(eq);
		return (EIO);
	}
	if (eq->tse_avail < ncred) {
		EQ_UNLOCK(eq);
		T4_OFLD_STAT(of, os_wr_full);
		return (EAGAIN);
	}

	ring = eq->tse_ring;
	first = MIN(len, (size_t)(eq->tse_qsize - eq->tse_pidx) * EQ_HC_SIZE);
	bcopy(src, ring + (size_t)eq->tse_pidx * EQ_HC_SIZE, first);
	if (first < len)
		bcopy(src + first, ring, len - first);

	eq->tse_pidx += ncred;
	if (eq->tse_pidx >= eq->tse_qsize)
		eq->tse_pidx -= eq->tse_qsize;
	eq->tse_avail -= ncred;
	eq->tse_pending += ncred;
	t4_eq_ring_db(of->of_sc, eq);
	EQ_UNLOCK(eq);

	T4_OFLD_STAT(of, os_wr_sent);
	return (0);
}

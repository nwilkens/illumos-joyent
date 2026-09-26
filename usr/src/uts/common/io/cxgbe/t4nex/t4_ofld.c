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
 * The t4nex offload core: the capabilities and resources the firmware gives
 * the TOE and RDMA functions, the lifecycle of the "iwcxgbe" child node, and
 * the events t4nex sends it.  t4_rdma.h describes the contract with the child.
 *
 * Offload is used only when the rdma-enable property is set, the chip is a T5
 * or later, and the firmware grants both TOE and RDMA capabilities with
 * resources inside the bounds below.  Otherwise t4nex behaves as it does
 * without the property.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/atomic.h>
#include <sys/sysmacros.h>

#include "common/common.h"
#include "common/t4_regs.h"
#include "t4_ofld.h"

#define	FW_PARAM_DEV(param) \
	(V_FW_PARAMS_MNEM(FW_PARAMS_MNEM_DEV) | \
	    V_FW_PARAMS_PARAM_X(FW_PARAMS_PARAM_DEV_##param))
#define	FW_PARAM_PFVF(param) \
	(V_FW_PARAMS_MNEM(FW_PARAMS_MNEM_PFVF) | \
	    V_FW_PARAMS_PARAM_X(FW_PARAMS_PARAM_PFVF_##param))

/* Older firmware counts high priority filter TIDs in NTID. */
#define	T4_OFLD_MIN_FW	(V_FW_HDR_FW_VER_MAJOR(1) | \
	V_FW_HDR_FW_VER_MINOR(20) | V_FW_HDR_FW_VER_MICRO(5))

boolean_t
t4_ofld_requested(struct adapter *sc)
{
	return (sc->props.rdma_enable && t4_cver_ge(sc, CHELSIO_T5));
}

/*
 * Choose the offload capabilities to keep in the FW_CAPS_CONFIG write.  TOE
 * and the hash filter are mutually exclusive, so the hash filter goes when
 * TOE stays.
 */
void
t4_ofld_caps(struct adapter *sc, struct fw_caps_config_cmd *caps)
{
	const uint16_t toe = BE_16(caps->toecaps) & FW_CAPS_CONFIG_TOE;
	const uint16_t rdma = BE_16(caps->rdmacaps) &
	    (FW_CAPS_CONFIG_RDMA_RDDP | FW_CAPS_CONFIG_RDMA_RDMAC);

	if (!t4_ofld_requested(sc) || toe == 0 ||
	    rdma != (FW_CAPS_CONFIG_RDMA_RDDP | FW_CAPS_CONFIG_RDMA_RDMAC)) {
		if (t4_ofld_requested(sc)) {
			cxgb_printf(sc->dip, CE_NOTE, "!firmware offers no "
			    "TOE and RDMA (toe 0x%x, rdma 0x%x); rdma-enable "
			    "ignored", toe, rdma);
		}
		caps->toecaps = 0;
		caps->rdmacaps = 0;
		return;
	}

	caps->toecaps = BE_16(toe);
	caps->rdmacaps = BE_16(rdma);
	caps->niccaps &= BE_16(~FW_CAPS_CONFIG_NIC_HASHFILTER);
}

static int
t4_ofld_query(struct adapter *sc, uint_t n, const uint32_t *param,
    uint32_t *val)
{
	uint32_t p[7];

	ASSERT3U(n, <=, ARRAY_SIZE(p));
	bcopy(param, p, n * sizeof (uint32_t));
	bzero(val, n * sizeof (uint32_t));
	return (-t4_query_params(sc, sc->mbox, sc->pf, 0, n, p, val));
}

/*
 * Turn a firmware [start, end] pair into a range.  The range must be
 * non-empty, must end at or below limit, must have a size that fits in 32
 * bits, and its start must be aligned to align (a power of two).
 */
static boolean_t
t4_ofld_range(uint32_t start, uint32_t end, uint64_t limit, uint32_t align,
    t4_rdma_range_t *r)
{
	if (end < start || (uint64_t)end + 1 > limit ||
	    (uint64_t)end - start + 1 > UINT32_MAX ||
	    (start & (align - 1)) != 0)
		return (B_FALSE);
	r->trr_start = start;
	r->trr_size = end - start + 1;
	return (B_TRUE);
}

/*
 * Firmware reports a region it does not have as end < start, or as 0 to
 * 0xffffffff (a size that wraps to zero; a T62100 does this for OCQ).
 */
static boolean_t
t4_ofld_optional(uint32_t start, uint32_t end)
{
	return (end < start || (start == 0 && end == UINT32_MAX));
}

static int
t4_ofld_bad(struct adapter *sc, const char *what, uint32_t a, uint32_t b)
{
	cxgb_printf(sc->dip, CE_NOTE, "!offload disabled: firmware %s out of "
	    "bounds: 0x%x 0x%x", what, a, b);
	return (ERANGE);
}

static boolean_t
t4_ofld_overlap(const t4_rdma_range_t *a, const t4_rdma_range_t *b)
{
	return ((uint64_t)a->trr_start < (uint64_t)b->trr_start + b->trr_size &&
	    (uint64_t)b->trr_start < (uint64_t)a->trr_start + a->trr_size);
}

/*
 * Read back the capabilities the firmware kept, then the TID and RDMA
 * resources.  Every value is bounded before use; a bad one disables offload.
 */
static int
t4_ofld_params(struct adapter *sc, t4_ofld_t *of)
{
	struct fw_caps_config_cmd caps;
	t4_rdma_vres_t *vr = &of->of_vres;
	t4_rdma_range_t r;
	uint32_t p[7], v[7];
	int rc;

	bzero(&caps, sizeof (caps));
	caps.op_to_write = BE_32(V_FW_CMD_OP(FW_CAPS_CONFIG_CMD) |
	    F_FW_CMD_REQUEST | F_FW_CMD_READ);
	caps.cfvalid_to_len16 = BE_32(FW_LEN16(caps));
	if ((rc = -t4_wr_mbox(sc, sc->mbox, &caps, sizeof (caps), &caps)) != 0)
		return (rc);
	of->of_toecaps = BE_16(caps.toecaps);
	of->of_rdmacaps = BE_16(caps.rdmacaps);
	if ((of->of_toecaps & FW_CAPS_CONFIG_TOE) == 0 ||
	    (of->of_rdmacaps & FW_CAPS_CONFIG_RDMA_RDMAC) == 0 ||
	    (BE_16(caps.niccaps) & FW_CAPS_CONFIG_NIC_HASHFILTER) != 0) {
		cxgb_printf(sc->dip, CE_NOTE, "!firmware kept no TOE and RDMA "
		    "(toe 0x%x rdma 0x%x nic 0x%x)", of->of_toecaps,
		    of->of_rdmacaps, BE_16(caps.niccaps));
		return (ENOTSUP);
	}
	if (sc->params.fw_vers < T4_OFLD_MIN_FW)
		return (ENOTSUP);

	p[0] = FW_PARAM_DEV(NTID);
	p[1] = FW_PARAM_PFVF(SERVER_START);
	p[2] = FW_PARAM_PFVF(SERVER_END);
	p[3] = FW_PARAM_DEV(FLOWC_BUFFIFO_SZ);
	p[4] = FW_PARAM_PFVF(L2T_START);
	p[5] = FW_PARAM_PFVF(L2T_END);
	if ((rc = t4_ofld_query(sc, 6, p, v)) != 0)
		return (rc);
	if (v[0] == 0 || v[0] > T4_OFLD_MAX_NTIDS)
		return (t4_ofld_bad(sc, "ntids", v[0], 0));
	of->of_ntids = v[0];
	of->of_natids = MIN(of->of_ntids / 2, T4_OFLD_MAX_NATIDS);
	if (!t4_ofld_range(v[1], v[2], T4_OFLD_M_TID + 1ULL, 1, &r) ||
	    r.trr_size > T4_OFLD_MAX_NSTIDS)
		return (t4_ofld_bad(sc, "server range", v[1], v[2]));
	of->of_stid_base = r.trr_start;
	of->of_nstids = r.trr_size;
	if (v[3] == 0 || v[3] >= T4_OFLD_MAX_WR_CRED)
		return (t4_ofld_bad(sc, "ofld wr credits", v[3], 0));
	vr->trv_ofldq_wr_cred = v[3];
	/* The last index shares a TID field with the SYNC_WR flag. */
	if (!t4_ofld_range(v[4], v[5], T4_OFLD_MAX_L2T, 1, &r))
		return (t4_ofld_bad(sc, "l2t range", v[4], v[5]));
	of->of_l2t_start = r.trr_start;
	of->of_l2t_size = r.trr_size;

	if (t4_cver_ge(sc, CHELSIO_T6)) {
		of->of_tid_base = t4_read_reg(sc,
		    A_LE_DB_ACTIVE_TABLE_START_INDEX);
	} else {
		of->of_tid_base = 0;
	}
	/* Server and connection TIDs share the LE index space. */
	if ((uint64_t)of->of_tid_base + of->of_ntids > T4_OFLD_M_TID + 1ULL)
		return (t4_ofld_bad(sc, "tid base", of->of_tid_base,
		    of->of_ntids));

	p[0] = FW_PARAM_PFVF(STAG_START);
	p[1] = FW_PARAM_PFVF(STAG_END);
	p[2] = FW_PARAM_PFVF(PBL_START);
	p[3] = FW_PARAM_PFVF(PBL_END);
	if ((rc = t4_ofld_query(sc, 4, p, v)) != 0)
		return (rc);
	if (!t4_ofld_range(v[0], v[1], 1ULL << 32, T4_TPT_UNIT, &vr->trv_stag))
		return (t4_ofld_bad(sc, "stag range", v[0], v[1]));
	if (!t4_ofld_range(v[2], v[3], 1ULL << 32, T4_TPT_UNIT, &vr->trv_pbl) ||
	    t4_ofld_overlap(&vr->trv_stag, &vr->trv_pbl))
		return (t4_ofld_bad(sc, "pbl range", v[2], v[3]));

	p[0] = FW_PARAM_PFVF(RQ_START);
	p[1] = FW_PARAM_PFVF(RQ_END);
	p[2] = FW_PARAM_PFVF(SQRQ_START);
	p[3] = FW_PARAM_PFVF(SQRQ_END);
	p[4] = FW_PARAM_PFVF(CQ_START);
	p[5] = FW_PARAM_PFVF(CQ_END);
	if ((rc = t4_ofld_query(sc, 6, p, v)) != 0)
		return (rc);
	if (!t4_ofld_range(v[0], v[1], 1ULL << 32, 1, &vr->trv_rq) ||
	    t4_ofld_overlap(&vr->trv_rq, &vr->trv_stag) ||
	    t4_ofld_overlap(&vr->trv_rq, &vr->trv_pbl))
		return (t4_ofld_bad(sc, "rq range", v[0], v[1]));
	if (!t4_ofld_range(v[2], v[3], UINT16_MAX + 1ULL, 1, &vr->trv_qp))
		return (t4_ofld_bad(sc, "qp range", v[2], v[3]));
	if (!t4_ofld_range(v[4], v[5], UINT16_MAX + 1ULL, 1, &vr->trv_cq))
		return (t4_ofld_bad(sc, "cq range", v[4], v[5]));

	p[0] = FW_PARAM_PFVF(OCQ_START);
	p[1] = FW_PARAM_PFVF(OCQ_END);
	p[2] = FW_PARAM_PFVF(SRQ_START);
	p[3] = FW_PARAM_PFVF(SRQ_END);
	p[4] = FW_PARAM_DEV(MAXORDIRD_QP);
	p[5] = FW_PARAM_DEV(MAXIRD_ADAPTER);
	if ((rc = t4_ofld_query(sc, 6, p, v)) != 0)
		return (rc);
	if (!t4_ofld_optional(v[0], v[1]) &&
	    !t4_ofld_range(v[0], v[1], 1ULL << 32, 1, &vr->trv_ocq))
		return (t4_ofld_bad(sc, "ocq range", v[0], v[1]));
	if (!t4_ofld_optional(v[2], v[3]) &&
	    !t4_ofld_range(v[2], v[3], 1ULL << 32, 1, &vr->trv_srq))
		return (t4_ofld_bad(sc, "srq range", v[2], v[3]));
	if (v[4] == 0 || v[4] > T4_OFLD_MAX_ORDIRD ||
	    v[5] == 0 || v[5] > T4_OFLD_MAX_IRD_ADAPTER)
		return (t4_ofld_bad(sc, "ord/ird", v[4], v[5]));
	vr->trv_max_ordird_qp = v[4];
	vr->trv_max_ird_adapter = v[5];

	p[0] = FW_PARAM_DEV(RDMA_WRITE_WITH_IMM);
	if (t4_ofld_query(sc, 1, p, v) == 0 && v[0] != 0)
		vr->trv_write_w_imm = B_TRUE;
	p[0] = FW_PARAM_DEV(RI_WRITE_CMPL_WR);
	if (t4_ofld_query(sc, 1, p, v) == 0 && v[0] != 0)
		vr->trv_write_cmpl = B_TRUE;

	return (0);
}

/*
 * Set up the offload state once the firmware is initialized and the ports
 * exist.  On any failure offload stays off and the NIC is unaffected.
 */
int
t4_ofld_init(struct adapter *sc)
{
	t4_ofld_t *of;
	int rc;

	ASSERT3P(sc->ofld, ==, NULL);
	if (!t4_ofld_requested(sc))
		return (0);
	if (sc->params.nports > T4_RDMA_MAX_PORTS)
		return (0);

	of = kmem_zalloc(sizeof (*of), KM_SLEEP);
	of->of_sc = sc;
	of->of_nports = sc->params.nports;
	if ((rc = t4_ofld_params(sc, of)) != 0) {
		cxgb_printf(sc->dip, CE_NOTE, "!offload disabled: bad or "
		    "missing firmware resources: %d", rc);
		kmem_free(of, sizeof (*of));
		return (0);
	}
	(void) t4_init_tp_params(sc, true);

	of->of_peer.trp_hdr.trp_version = T4_RDMA_VERSION;
	of->of_peer.trp_hdr.trp_ops = &t4_rdma_ops;
	of->of_peer.trp_ofld = of;
	of->of_gen = 1;
	for (uint_t i = 0; i < of->of_nports; i++) {
		of->of_port[i].op_ofld = of;
		of->of_port[i].op_pi = sc->port[i];
		of->of_port[i].op_idx = i;
		of->of_port[i].op_link = LINK_STATE_UNKNOWN;
	}
	sc->ofld = of;
	return (0);
}

/*
 * Whether the adapter can give offload its vector block.  Called by
 * t4_cfg_intrs_queues() before the interrupt allocation.
 */
boolean_t
t4_ofld_vectors(struct adapter *sc, uint_t avail)
{
	t4_ofld_t *of = sc->ofld;

	if (of == NULL)
		return (B_FALSE);
	return (avail >= 2 + of->of_nports + T4_OFLD_VECS);
}

static int
t4_ofld_kstat_update(kstat_t *ksp, int rw)
{
	t4_ofld_t *of = ksp->ks_private;
	t4_ofld_kstats_t *k = &of->of_kstats;
	t4_ofld_stats_t *s = &of->of_stats;

	if (rw == KSTAT_WRITE)
		return (EACCES);

	mutex_enter(&of->of_lock);
	k->ok_state.value.ui32 = of->of_fatal ? 3 : (of->of_client != NULL ?
	    (of->of_client_test ? 2 : 1) : 0);
	k->ok_generation.value.ui32 = of->of_gen;
	mutex_exit(&of->of_lock);

	mutex_enter(&of->of_tids.td_lock);
	k->ok_tids_inuse.value.ui32 = of->of_tids.td_hw.tt_inuse;
	k->ok_atids_inuse.value.ui32 = of->of_tids.td_atid.tt_inuse;
	k->ok_stids_inuse.value.ui32 = of->of_tids.td_stid.tt_inuse;
	mutex_exit(&of->of_tids.td_lock);

	k->ok_ntids.value.ui32 = of->of_ntids;
	k->ok_natids.value.ui32 = of->of_natids;
	k->ok_nstids.value.ui32 = of->of_nstids;
	k->ok_l2t_size.value.ui32 = of->of_l2t_size;
	k->ok_stag_size.value.ui32 = of->of_vres.trv_stag.trr_size;
	k->ok_pbl_size.value.ui32 = of->of_vres.trv_pbl.trr_size;
	k->ok_cpl_rx.value.ui64 = s->os_cpl_rx;
	k->ok_cpl_unknown.value.ui64 = s->os_cpl_unknown;
	k->ok_cpl_short.value.ui64 = s->os_cpl_short;
	k->ok_cpl_badid.value.ui64 = s->os_cpl_badid;
	k->ok_cpl_stale.value.ui64 = s->os_cpl_stale;
	k->ok_cpl_wrongq.value.ui64 = s->os_cpl_wrongq;
	k->ok_cpl_mismatch.value.ui64 = s->os_cpl_mismatch;
	k->ok_cpl_nomem.value.ui64 = s->os_cpl_nomem;
	k->ok_cq_notify.value.ui64 = s->os_cq_notify;
	k->ok_cq_badid.value.ui64 = s->os_cq_badid;
	k->ok_fl_badlen.value.ui64 = s->os_fl_badlen;
	k->ok_orphan_release.value.ui64 = s->os_orphan_release;
	k->ok_orphan_abort.value.ui64 = s->os_orphan_abort;
	k->ok_orphan_retry.value.ui64 = s->os_orphan_retry;
	k->ok_wr_sent.value.ui64 = s->os_wr_sent;
	k->ok_wr_full.value.ui64 = s->os_wr_full;
	k->ok_wr_badcookie.value.ui64 = s->os_wr_badcookie;
	k->ok_l2t_write.value.ui64 = s->os_l2t_write;
	k->ok_l2t_fail.value.ui64 = s->os_l2t_fail;
	k->ok_tpt_write.value.ui64 = s->os_tpt_write;
	k->ok_events.value.ui64 = s->os_events;
	k->ok_eq_bad_cidx.value.ui64 = s->os_eq_bad_cidx;
	k->ok_syn_refused.value.ui64 = s->os_syn_refused;

	mutex_enter(&of->of_dma_lock);
	k->ok_dma_bytes.value.ui64 = of->of_dma_bytes;
	k->ok_quar_bytes.value.ui64 = of->of_quar_bytes;
	mutex_exit(&of->of_dma_lock);
	return (0);
}

#define	OK_U32(f, n)	kstat_named_init(&k->f, n, KSTAT_DATA_UINT32)
#define	OK_U64(f, n)	kstat_named_init(&k->f, n, KSTAT_DATA_UINT64)

static void
t4_ofld_kstat_init(t4_ofld_t *of)
{
	struct adapter *sc = of->of_sc;
	t4_ofld_kstats_t *k = &of->of_kstats;
	kstat_t *ksp;

	ksp = kstat_create(T4_NEXUS_NAME, ddi_get_instance(sc->dip), "ofld",
	    "net", KSTAT_TYPE_NAMED, sizeof (*k) / sizeof (kstat_named_t), 0);
	if (ksp == NULL)
		return;
	ksp->ks_data = k;
	ksp->ks_private = of;
	ksp->ks_update = t4_ofld_kstat_update;

	OK_U32(ok_state, "state");
	OK_U32(ok_generation, "generation");
	OK_U32(ok_ntids, "ntids");
	OK_U32(ok_natids, "natids");
	OK_U32(ok_nstids, "nstids");
	OK_U32(ok_tids_inuse, "tids_inuse");
	OK_U32(ok_atids_inuse, "atids_inuse");
	OK_U32(ok_stids_inuse, "stids_inuse");
	OK_U32(ok_l2t_size, "l2t_size");
	OK_U32(ok_stag_size, "stag_size");
	OK_U32(ok_pbl_size, "pbl_size");
	OK_U64(ok_cpl_rx, "cpl_rx");
	OK_U64(ok_cpl_unknown, "cpl_unknown");
	OK_U64(ok_cpl_short, "cpl_short");
	OK_U64(ok_cpl_badid, "cpl_badid");
	OK_U64(ok_cpl_stale, "cpl_stale");
	OK_U64(ok_cpl_wrongq, "cpl_wrongq");
	OK_U64(ok_cpl_mismatch, "cpl_mismatch");
	OK_U64(ok_cpl_nomem, "cpl_nomem");
	OK_U64(ok_cq_notify, "cq_notify");
	OK_U64(ok_cq_badid, "cq_badid");
	OK_U64(ok_fl_badlen, "fl_badlen");
	OK_U64(ok_orphan_release, "orphan_release");
	OK_U64(ok_orphan_abort, "orphan_abort");
	OK_U64(ok_orphan_retry, "orphan_retry");
	OK_U64(ok_wr_sent, "wr_sent");
	OK_U64(ok_wr_full, "wr_full");
	OK_U64(ok_wr_badcookie, "wr_badcookie");
	OK_U64(ok_l2t_write, "l2t_write");
	OK_U64(ok_l2t_fail, "l2t_fail");
	OK_U64(ok_tpt_write, "tpt_write");
	OK_U64(ok_events, "events");
	OK_U64(ok_eq_bad_cidx, "eq_bad_cidx");
	OK_U64(ok_syn_refused, "syn_refused");
	OK_U64(ok_dma_bytes, "dma_bytes");
	OK_U64(ok_quar_bytes, "quarantine_bytes");

	kstat_install(ksp);
	of->of_ksp = ksp;
}

static void
t4_ofld_online(t4_ofld_t *of)
{
	struct adapter *sc = of->of_sc;
	dev_info_t *cdip;
	boolean_t stop;

	ASSERT(MUTEX_HELD(&of->of_cfg_lock));

	mutex_enter(&of->of_lock);
	stop = of->of_stopping || of->of_fatal || !of->of_queues_up;
	mutex_exit(&of->of_lock);
	if (stop)
		return;

	if ((cdip = of->of_cdip) == NULL) {
		if (ndi_devi_alloc(sc->dip, T4_RDMA_NODE_NAME,
		    (pnode_t)DEVI_SID_NODEID, &cdip) != NDI_SUCCESS) {
			cxgb_printf(sc->dip, CE_WARN,
			    "failed to allocate the RDMA node");
			return;
		}
		ddi_set_parent_data(cdip, &of->of_peer);
		of->of_cdip = cdip;
	}

	if (ndi_devi_online(cdip, 0) != NDI_SUCCESS) {
		cxgb_printf(sc->dip, CE_NOTE, "!RDMA function not attached; "
		    "is the iwcxgbe driver installed?");
	}
}

static void
t4_ofld_online_task(void *arg)
{
	t4_ofld_t *of = arg;

	mutex_enter(&of->of_cfg_lock);
	t4_ofld_online(of);
	mutex_exit(&of->of_cfg_lock);
}

/*
 * Bring up the offload queues and the child at the end of attach.  The
 * vector handlers already point into the port structures, so a failure here
 * leaves the state in place, idle, until detach.
 */
void
t4_ofld_start(struct adapter *sc)
{
	t4_ofld_t *of = sc->ofld;
	int rc;

	if (of == NULL)
		return;

	/* The interrupt priority is known only now. */
	mutex_init(&of->of_cfg_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&of->of_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(sc->intr_pri));
	cv_init(&of->of_cv, NULL, CV_DRIVER, NULL);
	mutex_init(&of->of_wlock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(sc->intr_pri));
	cv_init(&of->of_wcv, NULL, CV_DRIVER, NULL);
	mutex_init(&of->of_dma_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&of->of_bufs, sizeof (t4_ofld_buf_t),
	    offsetof(t4_ofld_buf_t, ob_node));
	list_create(&of->of_quar, sizeof (t4_ofld_buf_t),
	    offsetof(t4_ofld_buf_t, ob_node));
	t4_clip_init(of);
	membar_producer();
	of->of_ready = B_TRUE;

	if (t4_tids_init(of) != 0 || t4_l2t_init(of) != 0) {
		cxgb_printf(sc->dip, CE_WARN, "offload disabled: cannot "
		    "allocate the TID or L2T tables");
		return;
	}

	if ((rc = t4_ofld_queues_init(of)) != 0) {
		cxgb_printf(sc->dip, CE_WARN, "offload disabled: cannot "
		    "create the offload queues: %d", rc);
		return;
	}

	of->of_tq = ddi_taskq_create(sc->dip, "t4_ofld", 1, TASKQ_DEFAULTPRI,
	    0);
	if (of->of_tq == NULL) {
		t4_ofld_queues_fini(of);
		cxgb_printf(sc->dip, CE_WARN, "offload disabled: cannot "
		    "create the event taskq");
		return;
	}
	t4_ofld_kstat_init(of);

	mutex_enter(&of->of_lock);
	of->of_queues_up = B_TRUE;
	mutex_exit(&of->of_lock);

	cxgb_printf(sc->dip, CE_NOTE, "!offload enabled: %u tids from %u, "
	    "%u stids from %u, %u atids, l2t %u", of->of_ntids,
	    of->of_tid_base, of->of_nstids, of->of_stid_base, of->of_natids,
	    of->of_l2t_size);

	if (ddi_taskq_dispatch(of->of_tq, t4_ofld_online_task, of,
	    DDI_SLEEP) != DDI_SUCCESS)
		cxgb_printf(sc->dip, CE_WARN, "failed to online the RDMA node");
}

/*
 * Detach, before anything else: remove the child.  Fails if the child cannot
 * be detached.
 */
boolean_t
t4_ofld_detach(struct adapter *sc)
{
	t4_ofld_t *of = sc->ofld;
	boolean_t ok = B_TRUE;

	if (of == NULL || !of->of_ready)
		return (B_TRUE);

	mutex_enter(&of->of_lock);
	of->of_stopping = B_TRUE;
	mutex_exit(&of->of_lock);

	if (of->of_tq != NULL)
		ddi_taskq_wait(of->of_tq);

	mutex_enter(&of->of_cfg_lock);
	if (of->of_cdip != NULL) {
		if (ndi_devi_offline(of->of_cdip, NDI_DEVI_REMOVE) ==
		    NDI_SUCCESS) {
			of->of_cdip = NULL;
		} else {
			cxgb_printf(sc->dip, CE_WARN,
			    "the RDMA function did not detach");
			ok = B_FALSE;
		}
	}
	mutex_exit(&of->of_cfg_lock);

	if (ok) {
		mutex_enter(&of->of_cfg_lock);
		t4_ofld_test_fini(of);
		mutex_exit(&of->of_cfg_lock);
	}
	if (ok) {
		mutex_enter(&of->of_lock);
		ok = of->of_client == NULL && !of->of_closing;
		mutex_exit(&of->of_lock);
	}

	if (!ok) {
		mutex_enter(&of->of_lock);
		of->of_stopping = B_FALSE;
		mutex_exit(&of->of_lock);
	}
	return (ok);
}

/*
 * Release the offload state.  The caller has disabled the interrupts and
 * removed the child.  Buffers the device may still reach are freed only after
 * the SGE is stopped.
 */
void
t4_ofld_fini(struct adapter *sc)
{
	t4_ofld_t *of = sc->ofld;

	if (of == NULL)
		return;
	if (!of->of_ready) {
		kmem_free(of, sizeof (*of));
		sc->ofld = NULL;
		return;
	}

	VERIFY3P(of->of_cdip, ==, NULL);
	VERIFY3P(of->of_client, ==, NULL);

	t4_ofld_retry_stop(of);
	if (of->of_tq != NULL) {
		ddi_taskq_destroy(of->of_tq);
		of->of_tq = NULL;
	}
	if (of->of_ksp != NULL)
		kstat_delete(of->of_ksp);

	t4_ofld_queues_fini(of);

	mutex_enter(&of->of_dma_lock);
	const boolean_t quar = !list_is_empty(&of->of_quar);
	mutex_exit(&of->of_dma_lock);
	if (quar)
		t4_set_reg_field(sc, A_SGE_CONTROL, F_GLOBALENABLE, 0);
	t4_ofld_dma_fini(of, (t4_read_reg(sc, A_SGE_CONTROL) &
	    F_GLOBALENABLE) == 0);

	t4_clip_fini(of);
	t4_l2t_fini(of);
	t4_tids_fini(of);

	list_destroy(&of->of_bufs);
	list_destroy(&of->of_quar);
	mutex_destroy(&of->of_dma_lock);
	cv_destroy(&of->of_wcv);
	mutex_destroy(&of->of_wlock);
	cv_destroy(&of->of_cv);
	mutex_destroy(&of->of_lock);
	mutex_destroy(&of->of_cfg_lock);
	kmem_free(of, sizeof (*of));
	sc->ofld = NULL;
}

/* Whether a bus_config name ("iwcxgbe@0") names the RDMA node. */
boolean_t
t4_ofld_named(struct adapter *sc, const char *devname)
{
	const size_t len = sizeof (T4_RDMA_NODE_NAME) - 1;

	return (sc != NULL && sc->ofld != NULL && devname != NULL &&
	    strncmp(devname, T4_RDMA_NODE_NAME, len) == 0 &&
	    (devname[len] == '@' || devname[len] == '\0'));
}

boolean_t
t4_ofld_is_child(struct adapter *sc, dev_info_t *child)
{
	return (sc->ofld != NULL && child != NULL &&
	    strcmp(ddi_node_name(child), T4_RDMA_NODE_NAME) == 0 &&
	    ddi_get_parent_data(child) == &sc->ofld->of_peer);
}

/* Whether the client may use the offload core now; of_lock is held. */
boolean_t
t4_ofld_client_ok(t4_ofld_t *of)
{
	ASSERT(MUTEX_HELD(&of->of_lock));
	return (of->of_client != NULL && of->of_client_gen == of->of_gen &&
	    !of->of_stopping && !of->of_fatal);
}

void
t4_ofld_info(t4_ofld_t *of, t4_rdma_info_t *info)
{
	struct adapter *sc = of->of_sc;

	bzero(info, sizeof (*info));
	info->tri_chip = CHELSIO_CHIP_VERSION(sc->params.chip);
	info->tri_pf = sc->pf;
	info->tri_nports = of->of_nports;
	info->tri_vres = of->of_vres;
	for (uint_t i = 0; i < T4_RDMA_NMTUS && i < NMTUS; i++)
		info->tri_mtus[i] = sc->params.mtus[i];
	info->tri_bar2 = sc->bar2_ptr;
	info->tri_bar2_handle = sc->bar2_hdl;
	info->tri_sge_page_shift = sc->params.sge.hps + 10;
	info->tri_eq_qpp_shift = sc->params.sge.eq_qpp;
	info->tri_iq_qpp_shift = sc->params.sge.iq_qpp;
	info->tri_write_combine = (sc->doorbells & DOORBELL_WCWR) != 0;
	info->tri_rxq_id = of->of_rxq.iq.tsi_abs_id;
	info->tri_ciq_id = of->of_ciq.tsi_abs_id;
	info->tri_ciq_cntxt = of->of_ciq.tsi_cntxt_id;

	for (uint_t i = 0; i < of->of_nports; i++) {
		const t4_ofld_port_t *op = &of->of_port[i];
		const struct port_info *pi = op->op_pi;
		t4_rdma_port_t *p = &info->tri_port[i];

		p->trpo_tx_chan = pi->tx_chan;
		p->trpo_lport = pi->lport;
		p->trpo_viid = pi->viid;
		bcopy(pi->hw_addr, p->trpo_mac, ETHERADDRL);
		p->trpo_mtu = op->op_mtu;
		p->trpo_link = op->op_link;
		p->trpo_speed = op->op_speed;
	}
}

int
t4_ofld_client_open(t4_ofld_t *of, const t4_rdma_client_t *client, void *arg,
    boolean_t test)
{
	int rc = 0;

	if (client == NULL || client->trcl_event == NULL ||
	    client->trcl_cpl == NULL || client->trcl_cq == NULL)
		return (EINVAL);

	mutex_enter(&of->of_lock);
	if (!of->of_queues_up) {
		rc = ENOTSUP;
	} else if (of->of_fatal) {
		rc = EIO;
	} else if (of->of_stopping) {
		rc = EAGAIN;
	} else if (of->of_client != NULL || of->of_closing) {
		rc = EBUSY;
	} else {
		of->of_client = client;
		of->of_client_arg = arg;
		of->of_client_gen = of->of_gen;
		of->of_client_test = test;
		of->of_ev_pending &= T4_OFLD_EVP_FATAL;
	}
	mutex_exit(&of->of_lock);
	return (rc);
}

/*
 * The client is going away.  Wait out its callbacks, make every ID it owned
 * stale, and let t4nex tear those down.
 */
void
t4_ofld_client_close(t4_ofld_t *of)
{
	uint32_t gen;

	mutex_enter(&of->of_lock);
	if (of->of_client == NULL) {
		mutex_exit(&of->of_lock);
		return;
	}
	gen = of->of_client_gen;
	of->of_closing = B_TRUE;
	of->of_client = NULL;
	of->of_client_arg = NULL;
	of->of_client_test = B_FALSE;
	if (of->of_gen == gen)
		of->of_gen++;
	while (of->of_cb_busy != 0 || of->of_op_busy != 0)
		cv_wait(&of->of_cv, &of->of_lock);
	mutex_exit(&of->of_lock);

	t4_ofld_orphan_sweep(of, gen);
	t4_ofld_dma_close(of);
	t4_l2t_reset(of);
	t4_clip_reset(of);

	mutex_enter(&of->of_lock);
	of->of_closing = B_FALSE;
	mutex_exit(&of->of_lock);
}

static void
t4_ofld_deliver(t4_ofld_t *of, const t4_rdma_event_t *ev)
{
	const t4_rdma_client_t *client;
	void *arg;

	mutex_enter(&of->of_lock);
	if ((client = of->of_client) == NULL) {
		mutex_exit(&of->of_lock);
		return;
	}
	arg = of->of_client_arg;
	of->of_cb_busy++;
	of->of_ev_thread = curthread;
	mutex_exit(&of->of_lock);

	T4_OFLD_STAT(of, os_events);
	client->trcl_event(arg, ev);

	mutex_enter(&of->of_lock);
	of->of_ev_thread = NULL;
	if (--of->of_cb_busy == 0)
		cv_broadcast(&of->of_cv);
	mutex_exit(&of->of_lock);
}

static void
t4_ofld_event_task(void *arg)
{
	t4_ofld_t *of = arg;
	t4_rdma_event_t ev;
	uint32_t pending;

	mutex_enter(&of->of_lock);
	pending = of->of_ev_pending;
	of->of_ev_pending = 0;
	of->of_ev_queued = B_FALSE;
	mutex_exit(&of->of_lock);

	if ((pending & T4_OFLD_EVP_FATAL) != 0) {
		bzero(&ev, sizeof (ev));
		ev.tre_type = T4_RDMA_EV_FATAL;
		t4_ofld_deliver(of, &ev);
	}
	for (uint_t i = 0; i < of->of_nports; i++) {
		t4_ofld_port_t *op = &of->of_port[i];

		if ((pending & T4_OFLD_EVP_LINK(i)) != 0) {
			bzero(&ev, sizeof (ev));
			ev.tre_type = T4_RDMA_EV_LINK;
			ev.tre_port = i;
			mutex_enter(&of->of_lock);
			ev.tre_link = op->op_link;
			ev.tre_speed = op->op_speed;
			mutex_exit(&of->of_lock);
			t4_ofld_deliver(of, &ev);
		}
		if ((pending & T4_OFLD_EVP_MTU(i)) != 0) {
			bzero(&ev, sizeof (ev));
			ev.tre_type = T4_RDMA_EV_MTU;
			ev.tre_port = i;
			mutex_enter(&of->of_lock);
			ev.tre_mtu = op->op_mtu;
			mutex_exit(&of->of_lock);
			t4_ofld_deliver(of, &ev);
		}
	}
}

/* Queue coalesced events; of_lock is held.  Any context. */
static void
t4_ofld_event_post(t4_ofld_t *of, uint32_t what)
{
	ASSERT(MUTEX_HELD(&of->of_lock));

	of->of_ev_pending |= what;
	if (of->of_client == NULL || of->of_tq == NULL || of->of_ev_queued)
		return;
	if (ddi_taskq_dispatch(of->of_tq, t4_ofld_event_task, of,
	    DDI_NOSLEEP) == DDI_SUCCESS)
		of->of_ev_queued = B_TRUE;
}

/* Called from the firmware event queue's interrupt. */
void
t4_ofld_link_notify(struct adapter *sc, int idx)
{
	t4_ofld_t *of = sc->ofld;
	const struct port_info *pi;
	link_state_t link;
	uint64_t speed;

	if (of == NULL || !of->of_ready || idx < 0 ||
	    (uint_t)idx >= of->of_nports)
		return;
	pi = sc->port[idx];
	link = pi->link_cfg.link_ok ? LINK_STATE_UP : LINK_STATE_DOWN;
	speed = (uint64_t)t4_link_fwcap_to_speed(pi->link_cfg.link_caps) *
	    1000000;

	mutex_enter(&of->of_lock);
	if (of->of_port[idx].op_link != link ||
	    of->of_port[idx].op_speed != speed) {
		of->of_port[idx].op_link = link;
		of->of_port[idx].op_speed = speed;
		t4_ofld_event_post(of, T4_OFLD_EVP_LINK(idx));
	}
	mutex_exit(&of->of_lock);
}

void
t4_ofld_mtu_notify(struct port_info *pi)
{
	t4_ofld_t *of = pi->adapter->ofld;

	if (of == NULL || !of->of_ready || pi->port_id >= of->of_nports)
		return;
	mutex_enter(&of->of_lock);
	if (of->of_port[pi->port_id].op_mtu != (uint32_t)pi->mtu) {
		of->of_port[pi->port_id].op_mtu = pi->mtu;
		t4_ofld_event_post(of, T4_OFLD_EVP_MTU(pi->port_id));
	}
	mutex_exit(&of->of_lock);
}

/*
 * The adapter stopped (t4_fatal_err()).  Every client ID is stale from now
 * on; the SGE no longer reaches host memory, so the quarantine may go.
 */
void
t4_ofld_fatal(struct adapter *sc)
{
	t4_ofld_t *of = sc->ofld;

	if (of == NULL || !of->of_ready)
		return;
	mutex_enter(&of->of_lock);
	if (!of->of_fatal) {
		of->of_fatal = B_TRUE;
		of->of_gen++;
		t4_ofld_event_post(of, T4_OFLD_EVP_FATAL);
	}
	mutex_exit(&of->of_lock);
}

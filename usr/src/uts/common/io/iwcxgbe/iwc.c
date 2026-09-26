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
 * iwcxgbe: module linkage, attach and detach, the adapter resources and the
 * rdmak device of each port.
 */

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/bitmap.h>
#include <sys/varargs.h>
#include <sys/disp.h>

#include "iwc.h"

static void *iwc_state;

void
iwc_warn(iwc_t *iwc, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);
	dev_err(iwc->iwc_dip, CE_WARN, "!%s", buf);
}

/*
 * Queue IDs: QPs and CQs share the vres QP range.  Each QP takes two, its
 * SQ ID being the QP number.
 */
int
iwc_qid_alloc(iwc_t *iwc, uint32_t *qidp)
{
	uint32_t i, idx;

	mutex_enter(&iwc->iwc_res_lock);
	for (i = 0; i < iwc->iwc_qid_n; i++) {
		idx = (iwc->iwc_qid_rotor + i) % iwc->iwc_qid_n;
		if (!BT_TEST(iwc->iwc_qid_map, idx))
			break;
	}
	if (i == iwc->iwc_qid_n) {
		mutex_exit(&iwc->iwc_res_lock);
		return (ENOSPC);
	}
	BT_SET(iwc->iwc_qid_map, idx);
	iwc->iwc_qid_rotor = (idx + 1) % iwc->iwc_qid_n;
	mutex_exit(&iwc->iwc_res_lock);
	*qidp = iwc->iwc_qid_start + idx;
	return (0);
}

void
iwc_qid_free(iwc_t *iwc, uint32_t qid)
{
	const uint32_t idx = qid - iwc->iwc_qid_start;

	VERIFY3U(idx, <, iwc->iwc_qid_n);
	mutex_enter(&iwc->iwc_res_lock);
	VERIFY(BT_TEST(iwc->iwc_qid_map, idx));
	BT_CLEAR(iwc->iwc_qid_map, idx);
	mutex_exit(&iwc->iwc_res_lock);
}

/*
 * The QP with a queue ID taken from the hardware, held; NULL for an ID
 * outside the range or with no QP.
 */
iwc_qp_t *
iwc_qp_get(iwc_t *iwc, uint32_t qid)
{
	iwc_qp_t *qp = NULL;

	if (qid < iwc->iwc_qid_start || qid - iwc->iwc_qid_start >=
	    iwc->iwc_qid_n)
		return (NULL);
	mutex_enter(&iwc->iwc_obj_lock);
	if ((qp = iwc->iwc_qps[qid - iwc->iwc_qid_start]) != NULL)
		qp->qp_refs++;
	mutex_exit(&iwc->iwc_obj_lock);
	return (qp);
}

void
iwc_qp_put(iwc_t *iwc, iwc_qp_t *qp)
{
	mutex_enter(&iwc->iwc_obj_lock);
	VERIFY3U(qp->qp_refs, >, 0);
	if (--qp->qp_refs == 0)
		cv_broadcast(&iwc->iwc_obj_cv);
	mutex_exit(&iwc->iwc_obj_lock);
}

/* A write to a queue's BAR2 register block. */
void
iwc_db_write(iwc_t *iwc, caddr_t reg, uint32_t off, uint32_t val)
{
	membar_producer();
	ddi_put32(iwc->iwc_info.tri_bar2_handle, (uint32_t *)(reg + off), val);
}

static int
iwc_query_device(struct rdk_device *rdev, struct rdk_device_attr *a)
{
	iwc_t *iwc = iwc_of(rdev);
	const t4_rdma_vres_t *vr = &iwc->iwc_info.tri_vres;

	a->hw_ver = iwc->iwc_info.tri_chip;
	a->vendor_id = 0x1425;
	a->max_mr_size = UINT32_MAX;
	a->page_size_cap = T4_PAGESIZE_MASK;
	a->max_qp = (int)(iwc->iwc_qid_n / 2);
	a->max_qp_wr = IWC_MAX_QP_WR;
	a->device_cap_flags = RDK_DEVICE_MEM_MGT_EXTENSIONS;
	a->kernel_cap_flags = RDK_KCAP_LOCAL_DMA_LKEY;
	a->local_dma_lkey = 0;
	a->max_send_sge = (int)MIN(T4_MAX_SEND_SGE, T4_MAX_WRITE_SGE);
	a->max_recv_sge = T4_MAX_RECV_SGE;
	a->max_sge_rd = 1;
	a->max_cq = (int)iwc->iwc_qid_n;
	a->max_cqe = IWC_MAX_CQE;
	a->max_mr = (int)iwc->iwc_nstag;
	a->max_pd = IWC_MAX_PD - 1;
	a->max_qp_rd_atom = (int)MIN(vr->trv_max_ordird_qp, IWC_MAX_ORDIRD);
	a->max_qp_init_rd_atom = a->max_qp_rd_atom;
	a->max_fast_reg_page_list_len = T4_MAX_FR_IMMD_DEPTH;
	return (0);
}

static int
iwc_query_port(struct rdk_device *rdev, uint32_t port, struct rdk_port_attr *a)
{
	iwc_dev_t *d = iwc_dev(rdev);
	const t4_rdma_port_t *p = &d->d_iwc->iwc_info.tri_port[d->d_port];

	_NOTE(ARGUNUSED(port));
	a->state = p->trpo_link == LINK_STATE_UP ? RDK_PORT_ACTIVE :
	    RDK_PORT_DOWN;
	a->max_mtu = RDK_MTU_4096;
	a->phys_mtu = p->trpo_mtu;
	a->active_mtu = rdk_mtu_int_to_enum((int)p->trpo_mtu);
	a->max_msg_sz = UINT32_MAX;
	a->speed = p->trpo_speed;
	bcopy(p->trpo_mac, a->mac, ETHERADDRL);
	return (0);
}

/* iWARP has no GID table; the entries rdmak keeps are not used. */
static int
iwc_add_gid(const struct rdk_gid_attr *attr)
{
	_NOTE(ARGUNUSED(attr));
	return (0);
}

static void
iwc_del_gid(const struct rdk_gid_attr *attr)
{
	_NOTE(ARGUNUSED(attr));
}

static int
iwc_create_ah(struct rdk_ah *ah, struct rdk_ah_attr *attr)
{
	_NOTE(ARGUNUSED(ah, attr));
	return (ENOTSUP);
}

static void
iwc_destroy_ah(struct rdk_ah *ah)
{
	_NOTE(ARGUNUSED(ah));
}

const struct rdk_device_ops iwc_rdk_ops = {
	.version = RDK_ABI_VERSION,
	.query_device = iwc_query_device,
	.query_port = iwc_query_port,
	.add_gid = iwc_add_gid,
	.del_gid = iwc_del_gid,
	.alloc_pd = iwc_alloc_pd,
	.dealloc_pd = iwc_dealloc_pd,
	.create_cq = iwc_create_cq,
	.destroy_cq = iwc_destroy_cq,
	.poll_cq = iwc_poll_cq,
	.req_notify_cq = iwc_req_notify_cq,
	.create_qp = iwc_create_qp,
	.modify_qp = iwc_modify_qp,
	.query_qp = iwc_query_qp,
	.destroy_qp = iwc_destroy_qp,
	.post_send = iwc_post_send,
	.post_recv = iwc_post_recv,
	.alloc_mr = iwc_alloc_mr,
	.map_mr_sg = iwc_map_mr_sg,
	.dereg_mr = iwc_dereg_mr,
	.create_ah = iwc_create_ah,
	.destroy_ah = iwc_destroy_ah,
	.dma_alloc = iwc_dma_alloc,
	.dma_free = iwc_dma_free,
	.cq_resched = iwc_cq_resched,
	.size_pd = sizeof (iwc_pd_t),
	.size_cq = sizeof (iwc_cq_t),
	.size_qp = sizeof (iwc_qp_t),
	.size_ah = sizeof (struct rdk_ah)
};

/* t4nex events, in its taskq. */
static void
iwc_event(void *arg, const t4_rdma_event_t *ev)
{
	iwc_t *iwc = arg;
	struct rdk_event rev;
	t4_rdma_port_t *p;

	switch (ev->tre_type) {
	case T4_RDMA_EV_LINK:
	case T4_RDMA_EV_MTU:
		if (ev->tre_port >= iwc->iwc_ndev)
			return;
		p = &iwc->iwc_info.tri_port[ev->tre_port];
		if (ev->tre_type == T4_RDMA_EV_LINK) {
			p->trpo_link = ev->tre_link;
			p->trpo_speed = ev->tre_speed;
		} else {
			p->trpo_mtu = ev->tre_mtu;
		}
		if (iwc->iwc_dev[ev->tre_port].d_registered) {
			bzero(&rev, sizeof (rev));
			rev.device = &iwc->iwc_dev[ev->tre_port].d_rdk;
			rev.element.port_num = 1;
			rev.event = p->trpo_link == LINK_STATE_UP ?
			    RDK_EVENT_PORT_ACTIVE : RDK_EVENT_PORT_ERR;
			rdk_dispatch_event(&rev);
		}
		break;
	case T4_RDMA_EV_FATAL:
		iwc->iwc_fatal = B_TRUE;
		iwc_warn(iwc, "the adapter stopped; RDMA is down");
		for (uint32_t i = 0; i < iwc->iwc_ndev; i++) {
			if (!iwc->iwc_dev[i].d_registered)
				continue;
			bzero(&rev, sizeof (rev));
			rev.device = &iwc->iwc_dev[i].d_rdk;
			rev.event = RDK_EVENT_DEVICE_FATAL;
			rdk_dispatch_event(&rev);
		}
		break;
	default:
		break;
	}
}

static const t4_rdma_client_t iwc_client = {
	.trcl_event = iwc_event,
	.trcl_cpl = iwc_cpl,
	.trcl_cq = iwc_cq_notify
};

static int
iwc_info_ok(iwc_t *iwc)
{
	const t4_rdma_info_t *in = &iwc->iwc_info;
	const t4_rdma_vres_t *vr = &in->tri_vres;

	if (in->tri_chip < CHELSIO_T5 || in->tri_nports == 0 ||
	    in->tri_nports > T4_RDMA_MAX_PORTS || in->tri_bar2 == NULL ||
	    in->tri_eq_spg_len == 0 || in->tri_eq_spg_len > 2 ||
	    in->tri_nciq == 0 || in->tri_nciq > T4_RDMA_MAX_CIQ) {
		iwc_warn(iwc, "unsupported adapter (chip %u, %u ports)",
		    in->tri_chip, in->tri_nports);
		return (ENOTSUP);
	}
	/* The queue IDs QPs and CQs take come from one range. */
	if (vr->trv_qp.trr_start != vr->trv_cq.trr_start ||
	    vr->trv_qp.trr_size != vr->trv_cq.trr_size ||
	    vr->trv_qp.trr_size < 4 || vr->trv_qp.trr_size > (1U << 20) ||
	    vr->trv_stag.trr_size < 64 || vr->trv_pbl.trr_size < 4096 ||
	    vr->trv_rq.trr_size < 4096) {
		iwc_warn(iwc, "unusable RDMA resources: qp %u/%u cq %u/%u",
		    vr->trv_qp.trr_start, vr->trv_qp.trr_size,
		    vr->trv_cq.trr_start, vr->trv_cq.trr_size);
		return (ENOTSUP);
	}
	return (0);
}

static void
iwc_teardown(iwc_t *iwc)
{
	iwc_mem_fini(iwc);
	if (iwc->iwc_qps != NULL) {
		kmem_free(iwc->iwc_qps, iwc->iwc_qid_n * sizeof (iwc_qp_t *));
		iwc->iwc_qps = NULL;
	}
	if (iwc->iwc_cqs != NULL) {
		kmem_free(iwc->iwc_cqs, iwc->iwc_qid_n * sizeof (iwc_cq_t *));
		iwc->iwc_cqs = NULL;
	}
	if (iwc->iwc_qid_map != NULL) {
		kmem_free(iwc->iwc_qid_map, BT_SIZEOFMAP(iwc->iwc_qid_n));
		iwc->iwc_qid_map = NULL;
	}
}

static int
iwc_setup(iwc_t *iwc)
{
	const t4_rdma_vres_t *vr = &iwc->iwc_info.tri_vres;

	iwc->iwc_qid_start = vr->trv_qp.trr_start;
	iwc->iwc_qid_n = vr->trv_qp.trr_size;
	iwc->iwc_qid_map = kmem_zalloc(BT_SIZEOFMAP(iwc->iwc_qid_n),
	    KM_SLEEP);
	iwc->iwc_cqs = kmem_zalloc(iwc->iwc_qid_n * sizeof (iwc_cq_t *),
	    KM_SLEEP);
	iwc->iwc_qps = kmem_zalloc(iwc->iwc_qid_n * sizeof (iwc_qp_t *),
	    KM_SLEEP);
	return (iwc_mem_init(iwc));
}

static void
iwc_unregister(iwc_t *iwc)
{
	for (uint32_t i = 0; i < iwc->iwc_ndev; i++) {
		iwc_dev_t *d = &iwc->iwc_dev[i];

		if (d->d_registered) {
			rdk_unregister_device(&d->d_rdk);
			d->d_registered = B_FALSE;
		}
		if (d->d_iw_attached) {
			rdk_iw_cm_detach(&d->d_rdk);
			d->d_iw_attached = B_FALSE;
		}
	}
}

static int
iwc_register(iwc_t *iwc)
{
	int ret;

	for (uint32_t i = 0; i < iwc->iwc_ndev; i++) {
		iwc_dev_t *d = &iwc->iwc_dev[i];

		d->d_iwc = iwc;
		d->d_port = (uint8_t)i;
		(void) snprintf(d->d_rdk.rd_name, sizeof (d->d_rdk.rd_name),
		    "%s%dp%u", IWC_NAME, ddi_get_instance(iwc->iwc_dip), i);
		d->d_rdk.rd_dip = iwc->iwc_dip;
		d->d_rdk.rd_ops = &iwc_rdk_ops;
		d->d_rdk.rd_phys_port_cnt = 1;
		d->d_rdk.rd_num_comp_vectors = iwc->iwc_nvec;
		if ((ret = rdk_iw_cm_attach(&d->d_rdk, &iwc_iw_ops)) != 0)
			return (ret);
		d->d_iw_attached = B_TRUE;
		if ((ret = rdk_register_device(&d->d_rdk)) != 0)
			return (ret);
		d->d_registered = B_TRUE;
	}
	return (0);
}

static void
iwc_locks_init(iwc_t *iwc, uint_t pri)
{
	mutex_init(&iwc->iwc_res_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&iwc->iwc_obj_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(pri));
	cv_init(&iwc->iwc_obj_cv, NULL, CV_DRIVER, NULL);
	mutex_init(&iwc->iwc_cm_qlock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(pri));
	mutex_init(&iwc->iwc_ep_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&iwc->iwc_ep_cv, NULL, CV_DRIVER, NULL);
	list_create(&iwc->iwc_eps, sizeof (iwc_ep_t),
	    offsetof(iwc_ep_t, ep_node));
}

static void
iwc_locks_fini(iwc_t *iwc)
{
	list_destroy(&iwc->iwc_eps);
	cv_destroy(&iwc->iwc_ep_cv);
	mutex_destroy(&iwc->iwc_ep_lock);
	mutex_destroy(&iwc->iwc_cm_qlock);
	cv_destroy(&iwc->iwc_obj_cv);
	mutex_destroy(&iwc->iwc_obj_lock);
	mutex_destroy(&iwc->iwc_res_lock);
}

static const char *const iwc_stat_names[] = {
	"cpl_drop", "cpl_lost", "cqe_bad", "mpa_bad", "syn_refused",
	"conn_est", "conn_abort", "async_err", "quarantine"
};

/*
 * After the counters: the vectors, with the names irdma uses so that one
 * tool reads both, and a ceqN_intrs for each vector N from 1.
 */
static const char *const iwc_vec_names[] = {
	"comp_vectors", "ceq_intrs", "ceq_busy_ns", "ceq_runs", "cq_arms",
	"sq_doorbells"
};
#define	IWC_KS_FIXED	(ARRAY_SIZE(iwc_stat_names) + ARRAY_SIZE(iwc_vec_names))

static int
iwc_kstat_update(kstat_t *ksp, int rw)
{
	iwc_t *iwc = ksp->ks_private;
	const uint64_t *v = (const uint64_t *)&iwc->iwc_stats;
	kstat_named_t *kn = ksp->ks_data;
	uint64_t intrs = 0, busy = 0, runs = 0, arms = 0, db = 0;
	uint_t i, n = ARRAY_SIZE(iwc_stat_names);

	if (rw == KSTAT_WRITE)
		return (EACCES);
	for (i = 0; i < n; i++)
		kn[i].value.ui64 = v[i];
	for (i = 0; i < iwc->iwc_nvec; i++) {
		iwc_vec_t *iv = &iwc->iwc_vecs[i];

		mutex_enter(&iv->iv_lock);
		kn[IWC_KS_FIXED + i].value.ui64 = iv->iv_intrs;
		intrs += iv->iv_intrs;
		busy += iv->iv_busy_ns;
		runs += iv->iv_runs;
		mutex_exit(&iv->iv_lock);
		arms += iv->iv_arms;
		db += iv->iv_sq_db;
	}
	kn[n++].value.ui64 = iwc->iwc_nvec;
	kn[n++].value.ui64 = intrs;
	kn[n++].value.ui64 = busy;
	kn[n++].value.ui64 = runs;
	kn[n++].value.ui64 = arms;
	kn[n++].value.ui64 = db;
	return (0);
}

static void
iwc_kstat_init(iwc_t *iwc)
{
	kstat_named_t *kn;
	kstat_t *ksp;
	char name[KSTAT_STRLEN];
	uint_t i, n;

	CTASSERT(sizeof (iwc->iwc_stats) ==
	    ARRAY_SIZE(iwc_stat_names) * sizeof (uint64_t));
	ksp = kstat_create(IWC_NAME, ddi_get_instance(iwc->iwc_dip), "stats",
	    "misc", KSTAT_TYPE_NAMED, IWC_KS_FIXED + iwc->iwc_nvec, 0);
	if (ksp == NULL)
		return;
	kn = ksp->ks_data;
	for (i = 0; i < ARRAY_SIZE(iwc_stat_names); i++)
		kstat_named_init(&kn[i], iwc_stat_names[i], KSTAT_DATA_UINT64);
	for (n = 0; n < ARRAY_SIZE(iwc_vec_names); n++, i++)
		kstat_named_init(&kn[i], iwc_vec_names[n], KSTAT_DATA_UINT64);
	for (n = 0; n < iwc->iwc_nvec; n++, i++) {
		(void) snprintf(name, sizeof (name), "ceq%u_intrs", n + 1);
		kstat_named_init(&kn[i], name, KSTAT_DATA_UINT64);
	}
	ksp->ks_private = iwc;
	ksp->ks_update = iwc_kstat_update;
	kstat_install(ksp);
	iwc->iwc_ksp = ksp;
}

static int
iwc_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	const t4_rdma_peer_hdr_t *hdr;
	iwc_t *iwc;
	int instance, ret;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);
	hdr = ddi_get_parent_data(dip);
	if (hdr == NULL || hdr->trp_version != T4_RDMA_VERSION ||
	    hdr->trp_ops == NULL) {
		dev_err(dip, CE_WARN, "!not a t4nex RDMA function of version "
		    "%u", T4_RDMA_VERSION);
		return (DDI_FAILURE);
	}
	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(iwc_state, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	iwc = ddi_get_soft_state(iwc_state, instance);
	iwc->iwc_dip = dip;
	iwc->iwc_peer = (t4_rdma_peer_t *)hdr;
	iwc->iwc_ops = hdr->trp_ops;

	iwc_locks_init(iwc, hdr->trp_intr_pri);
	iwc->iwc_cm_tq = taskq_create("iwc_cm", 1, minclsyspri, 1, 1,
	    TASKQ_PREPOPULATE);

	if ((ret = iwc->iwc_ops->tro_open(iwc->iwc_peer, &iwc_client, iwc,
	    &iwc->iwc_info)) != 0) {
		dev_err(dip, CE_WARN, "!cannot open the RDMA function: %d",
		    ret);
		goto fail;
	}
	iwc->iwc_open = B_TRUE;
	iwc_vecs_init(iwc, hdr->trp_intr_pri);
	iwc->iwc_ndev = iwc->iwc_info.tri_nports;
	if ((ret = iwc_info_ok(iwc)) != 0 || (ret = iwc_setup(iwc)) != 0)
		goto fail;
	iwc->iwc_tick = timeout(iwc_cm_tick, iwc, drv_usectohz(MICROSEC / 2));
	if ((ret = iwc_register(iwc)) != 0) {
		iwc_warn(iwc, "cannot register the RDMA devices: %d", ret);
		goto fail;
	}
	iwc_kstat_init(iwc);
	ddi_report_dev(dip);
	return (DDI_SUCCESS);
fail:
	iwc_unregister(iwc);
	iwc_cm_fini(iwc);
	if (iwc->iwc_open)
		(void) iwc->iwc_ops->tro_close(iwc->iwc_peer);
	iwc_teardown(iwc);
	iwc_vecs_fini(iwc);
	taskq_destroy(iwc->iwc_cm_tq);
	iwc_locks_fini(iwc);
	ddi_soft_state_free(iwc_state, instance);
	return (DDI_FAILURE);
}

static int
iwc_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	iwc_t *iwc;
	int instance;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	instance = ddi_get_instance(dip);
	if ((iwc = ddi_get_soft_state(iwc_state, instance)) == NULL)
		return (DDI_FAILURE);

	if (iwc->iwc_ksp != NULL)
		kstat_delete(iwc->iwc_ksp);
	iwc_unregister(iwc);
	iwc_cm_fini(iwc);
	(void) iwc->iwc_ops->tro_close(iwc->iwc_peer);
	iwc->iwc_open = B_FALSE;
	iwc_teardown(iwc);
	iwc_vecs_fini(iwc);
	taskq_destroy(iwc->iwc_cm_tq);
	iwc_locks_fini(iwc);
	ddi_soft_state_free(iwc_state, instance);
	return (DDI_SUCCESS);
}

static struct cb_ops iwc_cb_ops = {
	.cb_open = nulldev,
	.cb_close = nulldev,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nodev,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_MP,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops iwc_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = nodev,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = iwc_attach,
	.devo_detach = iwc_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &iwc_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed
};

static struct modldrv iwc_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "Chelsio iWARP RDMA",
	.drv_dev_ops = &iwc_dev_ops
};

static struct modlinkage iwc_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &iwc_modldrv, NULL }
};

int
_init(void)
{
	int ret;

	if ((ret = ddi_soft_state_init(&iwc_state, sizeof (iwc_t), 1)) != 0)
		return (ret);
	if ((ret = mod_install(&iwc_modlinkage)) != 0)
		ddi_soft_state_fini(&iwc_state);
	return (ret);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&iwc_modlinkage, mi));
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&iwc_modlinkage)) == 0)
		ddi_soft_state_fini(&iwc_state);
	return (ret);
}

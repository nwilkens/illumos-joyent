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
 * irdma - Intel Ethernet 800 series RDMA function
 *
 * The RDMA protocol engine of an E810 port is a function of the ice(4D) PF.
 * ice creates the child node irdma@0 when RDMA is enabled and hands it the
 * peer interface in ice_rdma.h: BAR0, a block of MSI-X vectors, DMA memory,
 * qsets in the transmit scheduler, the PE filter, and resets.  This driver
 * brings up the control plane of the engine (irdma_ctl.c) on top of the
 * Intel shared code in core/, which is imported from Linux; core/README.illumos
 * lists its provenance and local changes.  There are no verbs yet.
 *
 * Lifecycle: attach opens the peer and runs the bring-up steps of
 * irdma_ctl.c; any failure undoes the completed steps.  Detach runs them in
 * reverse and always succeeds.  When ice resets the PF, it takes this node
 * offline first and attaches it again after the rebuild.  If the device is
 * resetting or a CQP command failed, teardown issues no commands and every
 * DMA buffer goes back to ice as possibly still in use, so ice frees it only
 * after the next reset.
 *
 * Firmware and the device are not trusted: FPM data are bounded before the
 * core code sizes anything (irdma_osdep.c), completions are matched to
 * requests by slot and generation, and CEQ entries resolve only to CQs
 * registered on the CEQ.
 */

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/cred.h>
#include <sys/policy.h>
#include <sys/zone.h>
#include <sys/varargs.h>

#include "irdma_impl.h"

#define	IRDMA_DEF_QP_LIMIT	1024
#define	IRDMA_MIN_QP_LIMIT	128
#define	IRDMA_MAX_QP_LIMIT	65536
#define	IRDMA_DEF_CQP_TIMEOUT	5000
#define	IRDMA_MIN_CQP_TIMEOUT	100
#define	IRDMA_MAX_CQP_TIMEOUT	60000

static void *irdma_state;

static void irdma_event(void *, const ice_rdma_event_t *);

static const ice_rdma_client_t irdma_client = {
	.irc_event = irdma_event
};

void
irdma_error(irdma_t *irdma, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);
	dev_err(irdma->irdma_dip, CE_WARN, "!%s", buf);
}

void
irdma_fm_report(irdma_t *irdma, const char *detail, int impact)
{
	irdma->irdma_ops->iro_fm_report(irdma->irdma_peer, detail, impact);
}

/*
 * The device or firmware misbehaved.  Stop issuing commands, keep every
 * buffer until a reset, and ask ice for one.  This never waits: a CQP waiter
 * can be the caller.
 */
void
irdma_fatal(irdma_t *irdma, const char *why)
{
	uint_t i;

	irdma_error(irdma, "%s; requesting a PF reset", why);
	irdma_taint(irdma);
	mutex_enter(&irdma->irdma_req_lock);
	atomic_or_32(&irdma->irdma_flags, IRDMA_F_CQP_DEAD);
	for (i = 0; i < IRDMA_CQP_NREQS; i++)
		cv_broadcast(&irdma->irdma_reqs[i].icr_cv);
	cv_broadcast(&irdma->irdma_req_cv);
	mutex_exit(&irdma->irdma_req_lock);
	irdma_fm_report(irdma, DDI_FM_DEVICE_INVAL_STATE, DDI_SERVICE_DEGRADED);
	(void) irdma->irdma_ops->iro_reset(irdma->irdma_peer,
	    ICE_RDMA_RESET_PF);
}

/*
 * Events from ice, on its event taskq or its reset worker, with no ice lock
 * held.  RESET_PREP arrives only when this node could not be taken offline:
 * the device is about to reset under us, so stop touching it.
 */
static void
irdma_event(void *arg, const ice_rdma_event_t *ev)
{
	irdma_t *irdma = arg;

	irdma->irdma_events++;
	switch (ev->ire_type) {
	case ICE_RDMA_EV_LINK:
		irdma->irdma_link = ev->ire_link;
		break;
	case ICE_RDMA_EV_MTU:
		irdma->irdma_mtu = ev->ire_mtu;
		break;
	case ICE_RDMA_EV_CRIT_ERR:
		irdma->irdma_crit_errors++;
		irdma_error(irdma, "critical error from ice (OICR 0x%x)",
		    ev->ire_oicr);
		irdma_taint(irdma);
		break;
	case ICE_RDMA_EV_RESET_PREP:
		irdma_taint(irdma);
		mutex_enter(&irdma->irdma_intr_lock);
		irdma->irdma_intr_off = B_TRUE;
		mutex_exit(&irdma->irdma_intr_lock);
		irdma_cqp_fail_all(irdma);
		break;
	case ICE_RDMA_EV_RESET_DONE:
		irdma_error(irdma, "the PF was reset under this instance; "
		    "detach and attach it again");
		break;
	case ICE_RDMA_EV_TC:
	default:
		break;
	}
}

static int
irdma_kstat_update(kstat_t *ksp, int rw)
{
	irdma_t *irdma = ksp->ks_private;
	irdma_kstats_t *k = &irdma->irdma_kstats;
	struct irdma_hmc_info *hmc = irdma->irdma_sc.hmc_info;

	if (rw == KSTAT_WRITE)
		return (EACCES);

	k->ik_progress.value.ui32 = irdma->irdma_progress;
	k->ik_flags.value.ui32 = irdma->irdma_flags;
	k->ik_cqp_submitted.value.ui64 = irdma->irdma_cqp_submitted;
	k->ik_cqp_completed.value.ui64 = irdma->irdma_cqp_completed;
	k->ik_cqp_timeouts.value.ui64 = irdma->irdma_cqp_timeouts;
	k->ik_cqp_errors.value.ui64 = irdma->irdma_cqp_errors;
	k->ik_ceq_intrs.value.ui64 = irdma->irdma_ceq_intrs;
	k->ik_aeq_intrs.value.ui64 = irdma->irdma_aeq_intrs;
	k->ik_aeqes.value.ui64 = irdma->irdma_aeqes;
	k->ik_bad_entries.value.ui64 = irdma->irdma_bad_entries;
	k->ik_events.value.ui64 = irdma->irdma_events;
	k->ik_crit_errors.value.ui64 = irdma->irdma_crit_errors;
	k->ik_dma_bufs.value.ui32 = irdma->irdma_osdev.od_nbufs;
	k->ik_link.value.ui32 = irdma->irdma_link;
	k->ik_mtu.value.ui32 = irdma->irdma_mtu;
	if (hmc != NULL && hmc->hmc_obj != NULL) {
		k->ik_hmc_sds.value.ui32 = hmc->sd_table.sd_cnt;
		k->ik_qp_cnt.value.ui32 = hmc->hmc_obj[IRDMA_HMC_IW_QP].cnt;
		k->ik_cq_cnt.value.ui32 = hmc->hmc_obj[IRDMA_HMC_IW_CQ].cnt;
		k->ik_mr_cnt.value.ui32 = hmc->hmc_obj[IRDMA_HMC_IW_MR].cnt;
		k->ik_pble_cnt.value.ui32 =
		    hmc->hmc_obj[IRDMA_HMC_IW_PBLE].cnt;
	}
	return (0);
}

static void
irdma_kstat_init(irdma_t *irdma)
{
	irdma_kstats_t *k = &irdma->irdma_kstats;
	kstat_t *ksp;

	ksp = kstat_create(IRDMA_MODULE_NAME, irdma->irdma_instance, "ctl",
	    "net", KSTAT_TYPE_NAMED, sizeof (*k) / sizeof (kstat_named_t), 0);
	if (ksp == NULL) {
		irdma_error(irdma, "failed to create the kstat");
		return;
	}
	ksp->ks_data = k;
	ksp->ks_private = irdma;
	ksp->ks_update = irdma_kstat_update;

	kstat_named_init(&k->ik_progress, "progress", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_flags, "flags", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_cqp_submitted, "cqp_submitted",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_cqp_completed, "cqp_completed",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_cqp_timeouts, "cqp_timeouts",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_cqp_errors, "cqp_errors", KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_ceq_intrs, "ceq_intrs", KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_aeq_intrs, "aeq_intrs", KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_aeqes, "aeq_entries", KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_bad_entries, "bad_entries", KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_events, "events", KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_crit_errors, "crit_errors", KSTAT_DATA_UINT64);
	kstat_named_init(&k->ik_dma_bufs, "dma_bufs", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_hmc_sds, "hmc_sds", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_qp_cnt, "qp_count", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_cq_cnt, "cq_count", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_mr_cnt, "mr_count", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_pble_cnt, "pble_count", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_link, "link_state", KSTAT_DATA_UINT32);
	kstat_named_init(&k->ik_mtu, "mtu", KSTAT_DATA_UINT32);

	kstat_install(ksp);
	irdma->irdma_kstat = ksp;
}

static uint32_t
irdma_prop(irdma_t *irdma, const char *name, uint32_t def, uint32_t lo,
    uint32_t hi)
{
	int v;

	v = ddi_prop_get_int(DDI_DEV_T_ANY, irdma->irdma_dip, DDI_PROP_DONTPASS,
	    (char *)name, (int)def);
	if (v < (int)lo || v > (int)hi) {
		irdma_error(irdma, "%s %d is outside %u to %u; using %u", name,
		    v, lo, hi, def);
		return (def);
	}
	return ((uint32_t)v);
}

static void
irdma_locks_init(irdma_t *irdma)
{
	uint_t i;

	(mutex_init)(&irdma->irdma_cfg_lock, NULL, MUTEX_DRIVER, NULL);
	(mutex_init)(&irdma->irdma_req_lock, NULL, MUTEX_DRIVER, NULL);
	(mutex_init)(&irdma->irdma_ccq_lock, NULL, MUTEX_DRIVER, NULL);
	(mutex_init)(&irdma->irdma_ws_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&irdma->irdma_req_cv, NULL, CV_DRIVER, NULL);
	for (i = 0; i < IRDMA_CQP_NREQS; i++)
		cv_init(&irdma->irdma_reqs[i].icr_cv, NULL, CV_DRIVER, NULL);
}

static void
irdma_locks_fini(irdma_t *irdma)
{
	uint_t i;

	for (i = 0; i < IRDMA_CQP_NREQS; i++)
		cv_destroy(&irdma->irdma_reqs[i].icr_cv);
	cv_destroy(&irdma->irdma_req_cv);
	mutex_destroy(&irdma->irdma_ws_lock);
	mutex_destroy(&irdma->irdma_ccq_lock);
	mutex_destroy(&irdma->irdma_req_lock);
	mutex_destroy(&irdma->irdma_cfg_lock);
}

/*
 * Release what attach set up around the control plane, in reverse.  The
 * caller has stopped the control plane.
 */
static void
irdma_unsetup(irdma_t *irdma)
{
	if (irdma->irdma_kstat != NULL) {
		kstat_delete(irdma->irdma_kstat);
		irdma->irdma_kstat = NULL;
	}
	ddi_remove_minor_node(irdma->irdma_dip, NULL);
	if (irdma->irdma_test_taskq != NULL) {
		ddi_taskq_destroy(irdma->irdma_test_taskq);
		irdma->irdma_test_taskq = NULL;
	}
	if (irdma->irdma_taskq != NULL) {
		ddi_taskq_destroy(irdma->irdma_taskq);
		irdma->irdma_taskq = NULL;
		mutex_destroy(&irdma->irdma_intr_lock);
	}
	if (irdma->irdma_info.iri_bar0 != NULL)
		irdma_osdep_regs_remove(irdma->irdma_info.iri_bar0);
	if ((irdma->irdma_progress & BIT(IRDMA_STEP_OPEN)) != 0) {
		irdma_osdep_fini(irdma);
		irdma->irdma_ops->iro_close(irdma->irdma_peer);
		irdma->irdma_progress &= ~BIT(IRDMA_STEP_OPEN);
	}
}

static int
irdma_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	const ice_rdma_peer_hdr_t *hdr;
	irdma_t *irdma;
	int instance, ret;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	hdr = ddi_get_parent_data(dip);
	if (hdr == NULL || hdr->irp_version != ICE_RDMA_VERSION ||
	    hdr->irp_ops == NULL) {
		dev_err(dip, CE_WARN, "!not an ice RDMA function of version "
		    "%u", ICE_RDMA_VERSION);
		return (DDI_FAILURE);
	}

	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(irdma_state, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	irdma = ddi_get_soft_state(irdma_state, instance);
	irdma->irdma_dip = dip;
	irdma->irdma_instance = instance;
	irdma->irdma_peer = (ice_rdma_peer_t *)hdr;
	irdma->irdma_ops = hdr->irp_ops;
	irdma->irdma_link = LINK_STATE_UNKNOWN;
	irdma_locks_init(irdma);

	irdma->irdma_qp_limit = irdma_prop(irdma, "qp_limit",
	    IRDMA_DEF_QP_LIMIT, IRDMA_MIN_QP_LIMIT, IRDMA_MAX_QP_LIMIT);
	irdma->irdma_cqp_timeout_ms = irdma_prop(irdma, "cqp_timeout_ms",
	    IRDMA_DEF_CQP_TIMEOUT, IRDMA_MIN_CQP_TIMEOUT,
	    IRDMA_MAX_CQP_TIMEOUT);
#ifdef DEBUG
	irdma->irdma_fail_step = irdma_prop(irdma, "fail_step", 0, 0,
	    IRDMA_STEP_MAX);
	if (irdma->irdma_fail_step == IRDMA_STEP_OPEN + 1) {
		irdma_error(irdma, "injected failure at step open");
		goto fail;
	}
#endif

	ret = irdma->irdma_ops->iro_open(irdma->irdma_peer, &irdma_client,
	    irdma, &irdma->irdma_info);
	if (ret != 0) {
		irdma_error(irdma, "ice refused the RDMA function: %d", ret);
		goto fail;
	}
	irdma->irdma_progress |= BIT(IRDMA_STEP_OPEN);
	irdma->irdma_link = irdma->irdma_info.iri_link;
	irdma->irdma_mtu = irdma->irdma_info.iri_mtu;
	irdma_osdep_init(irdma);

	ret = irdma->irdma_ops->iro_intr_get(irdma->irdma_peer,
	    &irdma->irdma_intr);
	if (ret != 0 || irdma->irdma_intr.irin_count == 0) {
		irdma_error(irdma, "no RDMA vectors: %d", ret);
		goto fail;
	}
	(mutex_init)(&irdma->irdma_intr_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(irdma->irdma_intr.irin_pri));
	irdma->irdma_taskq = ddi_taskq_create(dip, "irdma_intr", 1,
	    TASKQ_DEFAULTPRI, 0);
	irdma->irdma_test_taskq = ddi_taskq_create(dip, "irdma_test", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (irdma->irdma_taskq == NULL || irdma->irdma_test_taskq == NULL) {
		irdma_error(irdma, "failed to create the taskqs");
		goto fail;
	}
	if (!irdma_osdep_regs_add(irdma->irdma_info.iri_bar0,
	    irdma->irdma_info.iri_bar0_size,
	    irdma->irdma_info.iri_bar0_handle)) {
		irdma_error(irdma, "too many RDMA functions");
		irdma->irdma_info.iri_bar0 = NULL;
		goto fail;
	}

	mutex_enter(&irdma->irdma_cfg_lock);
	ret = irdma_ctl_start(irdma);
	mutex_exit(&irdma->irdma_cfg_lock);
	if (ret != 0)
		goto fail;

	if (ddi_create_minor_node(dip, IRDMA_MODULE_NAME, S_IFCHR, instance,
	    DDI_PSEUDO, 0) != DDI_SUCCESS) {
		irdma_error(irdma, "failed to create the minor node");
		mutex_enter(&irdma->irdma_cfg_lock);
		irdma_ctl_stop(irdma);
		mutex_exit(&irdma->irdma_cfg_lock);
		goto fail;
	}
	irdma_kstat_init(irdma);

	dev_err(dip, CE_NOTE, "!RDMA control plane up: PF %u, VSI %u, "
	    "%u vectors, %u QPs, %u CQs, %u SDs",
	    irdma->irdma_info.iri_pf_id, irdma->irdma_info.iri_vsi_num,
	    irdma->irdma_intr.irin_count,
	    irdma->irdma_sc.hmc_info->hmc_obj[IRDMA_HMC_IW_QP].cnt,
	    irdma->irdma_sc.hmc_info->hmc_obj[IRDMA_HMC_IW_CQ].cnt,
	    irdma->irdma_sc.hmc_info->sd_table.sd_cnt);
	return (DDI_SUCCESS);

fail:
	irdma_unsetup(irdma);
	irdma_locks_fini(irdma);
	ddi_soft_state_free(irdma_state, instance);
	return (DDI_FAILURE);
}

/*
 * Detach always completes.  Test work is released and drained first so
 * that nothing waits on the CQP while it goes away.
 */
static int
irdma_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	irdma_t *irdma;
	int instance;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	instance = ddi_get_instance(dip);
	if ((irdma = ddi_get_soft_state(irdma_state, instance)) == NULL)
		return (DDI_FAILURE);

	atomic_or_32(&irdma->irdma_flags, IRDMA_F_STOPPING);
	if (irdma->irdma_ops->iro_resetting(irdma->irdma_peer))
		irdma_cqp_fail_all(irdma);
	irdma_ctl_hold_release(irdma);
	ddi_taskq_wait(irdma->irdma_test_taskq);

	mutex_enter(&irdma->irdma_cfg_lock);
	irdma_ctl_stop(irdma);
	mutex_exit(&irdma->irdma_cfg_lock);

	irdma_unsetup(irdma);
	irdma_locks_fini(irdma);
	ddi_soft_state_free(irdma_state, instance);
	return (DDI_SUCCESS);
}

static int
irdma_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	irdma_t *irdma;
	minor_t m;

	_NOTE(ARGUNUSED(dip));
	m = getminor((dev_t)arg);
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		if ((irdma = ddi_get_soft_state(irdma_state, m)) == NULL)
			return (DDI_FAILURE);
		*result = irdma->irdma_dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)(uintptr_t)m;
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}

static int
irdma_open(dev_t *devp, int flag, int otyp, cred_t *cr)
{
	_NOTE(ARGUNUSED(flag));
	if (otyp != OTYP_CHR)
		return (EINVAL);
	if (crgetzoneid(cr) != GLOBAL_ZONEID ||
	    secpolicy_sys_config(cr, B_FALSE) != 0)
		return (EPERM);
	if (ddi_get_soft_state(irdma_state, getminor(*devp)) == NULL)
		return (ENXIO);
	return (0);
}

static int
irdma_close(dev_t dev, int flag, int otyp, cred_t *cr)
{
	_NOTE(ARGUNUSED(dev, flag, otyp, cr));
	return (0);
}

#ifdef DEBUG
static void
irdma_test_job(void *arg)
{
	irdma_t *irdma = arg;
	int ret;

	ret = irdma_cqp_probe(irdma);
	mutex_enter(&irdma->irdma_cfg_lock);
	irdma->irdma_test_result.irs_test_error = ret;
	irdma->irdma_test_result.irs_test_runs++;
	irdma->irdma_test_result.irs_test_busy = 0;
	mutex_exit(&irdma->irdma_cfg_lock);
}

static int
irdma_ioctl_test(irdma_t *irdma, intptr_t arg, int mode)
{
	irdma_ioc_test_t t;
	int ret = 0;

	if (ddi_copyin((void *)arg, &t, sizeof (t), mode) != 0)
		return (EFAULT);

	switch (t.irt_action) {
	case IRDMA_TEST_CQP_NOP:
		mutex_enter(&irdma->irdma_cfg_lock);
		if (irdma->irdma_test_result.irs_test_busy != 0) {
			ret = EBUSY;
		} else if (ddi_taskq_dispatch(irdma->irdma_test_taskq,
		    irdma_test_job, irdma, DDI_NOSLEEP) != DDI_SUCCESS) {
			ret = EAGAIN;
		} else {
			irdma->irdma_test_result.irs_test_busy = 1;
		}
		mutex_exit(&irdma->irdma_cfg_lock);
		break;
	case IRDMA_TEST_HOLD_CQES:
		mutex_enter(&irdma->irdma_ccq_lock);
		irdma->irdma_hold_cqes = B_TRUE;
		mutex_exit(&irdma->irdma_ccq_lock);
		break;
	case IRDMA_TEST_RELEASE_CQES:
		irdma_ctl_hold_release(irdma);
		break;
	case IRDMA_TEST_RESET:
		ret = irdma->irdma_ops->iro_reset(irdma->irdma_peer,
		    ICE_RDMA_RESET_PF);
		break;
	case IRDMA_TEST_IRM_REMOVE:
		ret = irdma->irdma_ops->iro_test(irdma->irdma_peer,
		    ICE_RDMA_TEST_IRM_REMOVE, t.irt_arg);
		break;
	case IRDMA_TEST_IRM_ADD:
		ret = irdma->irdma_ops->iro_test(irdma->irdma_peer,
		    ICE_RDMA_TEST_IRM_ADD, t.irt_arg);
		break;
	default:
		ret = EINVAL;
		break;
	}
	return (ret);
}
#endif

static int
irdma_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *cr,
    int *rvalp)
{
	irdma_ioc_status_t st;
	irdma_t *irdma;
	struct irdma_hmc_info *hmc;

	_NOTE(ARGUNUSED(rvalp));
	if (crgetzoneid(cr) != GLOBAL_ZONEID ||
	    secpolicy_sys_config(cr, B_FALSE) != 0)
		return (EPERM);
	if ((irdma = ddi_get_soft_state(irdma_state, getminor(dev))) == NULL)
		return (ENXIO);

	switch (cmd) {
	case IRDMA_IOC_STATUS:
		bzero(&st, sizeof (st));
		mutex_enter(&irdma->irdma_cfg_lock);
		st.irs_test_busy = irdma->irdma_test_result.irs_test_busy;
		st.irs_test_error = irdma->irdma_test_result.irs_test_error;
		st.irs_test_runs = irdma->irdma_test_result.irs_test_runs;
		st.irs_progress = irdma->irdma_progress;
		mutex_exit(&irdma->irdma_cfg_lock);
		st.irs_flags = irdma->irdma_flags;
		st.irs_generation = irdma->irdma_info.iri_generation;
		st.irs_vectors = irdma->irdma_intr.irin_count;
		st.irs_cqp_submitted = irdma->irdma_cqp_submitted;
		st.irs_cqp_completed = irdma->irdma_cqp_completed;
		st.irs_cqp_timeouts = irdma->irdma_cqp_timeouts;
		hmc = irdma->irdma_sc.hmc_info;
		if (hmc != NULL && hmc->hmc_obj != NULL) {
			st.irs_hmc_sds = hmc->sd_table.sd_cnt;
			st.irs_qp_cnt = hmc->hmc_obj[IRDMA_HMC_IW_QP].cnt;
			st.irs_pble_cnt = hmc->hmc_obj[IRDMA_HMC_IW_PBLE].cnt;
		}
		if (ddi_copyout(&st, (void *)arg, sizeof (st), mode) != 0)
			return (EFAULT);
		return (0);
	case IRDMA_IOC_TEST:
#ifdef DEBUG
		return (irdma_ioctl_test(irdma, arg, mode));
#else
		return (ENOTSUP);
#endif
	default:
		return (ENOTTY);
	}
}

static struct cb_ops irdma_cb_ops = {
	.cb_open = irdma_open,
	.cb_close = irdma_close,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = irdma_ioctl,
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

static struct dev_ops irdma_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = irdma_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = irdma_attach,
	.devo_detach = irdma_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &irdma_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed
};

static struct modldrv irdma_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "Intel E800 Series RDMA",
	.drv_dev_ops = &irdma_dev_ops
};

static struct modlinkage irdma_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &irdma_modldrv, NULL }
};

int
_init(void)
{
	int ret;

	ret = ddi_soft_state_init(&irdma_state, sizeof (irdma_t), 1);
	if (ret != 0)
		return (ret);
	irdma_osdep_regs_init();
	if ((ret = mod_install(&irdma_modlinkage)) != 0) {
		irdma_osdep_regs_fini();
		ddi_soft_state_fini(&irdma_state);
	}
	return (ret);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&irdma_modlinkage, mi));
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&irdma_modlinkage)) == 0) {
		irdma_osdep_regs_fini();
		ddi_soft_state_fini(&irdma_state);
	}
	return (ret);
}

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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/host/nvmf.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * DDI entry points of the NVMe over Fabrics host (initiator).  The single
 * instance carries one association, which userland establishes and hands
 * over with NVMF_HANDOFF_HOST.  Each namespace of that association appears
 * as a blkdev child of the instance.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/autoconf.h>
#include <sys/mkdev.h>
#include <sys/modctl.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/errno.h>
#include <sys/cred.h>
#include <sys/policy.h>
#include <sys/zone.h>
#include <sys/stat.h>
#include <sys/open.h>
#include <sys/blkdev.h>

#include "nvmf_var.h"

void *nvmf_state;

static int
nvmf_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	nvmf_softc_t *sc;
	int instance;

	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(nvmf_state, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	sc = ddi_get_soft_state(nvmf_state, instance);

	sc->dip = dip;
	sc->state = NVMF_SC_IDLE;
	mutex_init(&sc->lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&sc->cv, NULL, CV_DRIVER, NULL);
	cv_init(&sc->qp_cv, NULL, CV_DRIVER, NULL);
	sc->cdata = kmem_zalloc(sizeof (*sc->cdata), KM_SLEEP);
	gethrestime(&sc->last_disconnect);
	nvmf_bd_init(sc);
	nvmf_ns_init(sc);
	nvmf_mpath_init(sc);
	sc->conn_tq = taskq_create_instance("nvmf_conn", instance, 1,
	    minclsyspri, 1, 1, 0);
	sc->ns_tq = taskq_create_instance("nvmf_ns", instance, 1,
	    minclsyspri, 1, 1, 0);

	if (ddi_create_minor_node(dip, "nvmf", S_IFCHR, instance,
	    DDI_PSEUDO, 0) != DDI_SUCCESS) {
		taskq_destroy(sc->ns_tq);
		taskq_destroy(sc->conn_tq);
		nvmf_mpath_fini(sc);
		nvmf_ns_fini(sc);
		nvmf_bd_fini(sc);
		kmem_free(sc->cdata, sizeof (*sc->cdata));
		cv_destroy(&sc->qp_cv);
		cv_destroy(&sc->cv);
		mutex_destroy(&sc->lock);
		ddi_soft_state_free(nvmf_state, instance);
		return (DDI_FAILURE);
	}

	ddi_report_dev(dip);
	return (DDI_SUCCESS);
}

/*
 * The instance detaches only with no association and no disk left.  A disk
 * that was open when its association went away keeps the instance until it
 * is closed.
 */
static int
nvmf_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	nvmf_softc_t *sc;
	int instance;

	switch (cmd) {
	case DDI_DETACH:
		break;
	case DDI_SUSPEND:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	instance = ddi_get_instance(dip);
	sc = ddi_get_soft_state(nvmf_state, instance);
	if (sc == NULL)
		return (DDI_FAILURE);

	mutex_enter(&sc->lock);
	if (sc->state != NVMF_SC_IDLE) {
		mutex_exit(&sc->lock);
		return (DDI_FAILURE);
	}
	sc->state = NVMF_SC_DETACHING;
	mutex_exit(&sc->lock);

	/* The connection worker can still post to the namespace worker. */
	taskq_wait(sc->conn_tq);
	taskq_wait(sc->ns_tq);
	nvmf_mpath_reap(sc);
	if (nvmf_mpath_busy(sc)) {
		mutex_enter(&sc->lock);
		sc->state = NVMF_SC_IDLE;
		mutex_exit(&sc->lock);
		return (DDI_FAILURE);
	}

	ddi_remove_minor_node(dip, NULL);
	taskq_destroy(sc->conn_tq);
	taskq_destroy(sc->ns_tq);
	nvmf_mpath_fini(sc);
	nvmf_ns_fini(sc);
	nvmf_bd_fini(sc);
	kmem_free(sc->cdata, sizeof (*sc->cdata));
	cv_destroy(&sc->qp_cv);
	cv_destroy(&sc->cv);
	mutex_destroy(&sc->lock);
	ddi_soft_state_free(nvmf_state, instance);
	return (DDI_SUCCESS);
}

static int
nvmf_getinfo(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	nvmf_softc_t *sc;
	int instance;

	_NOTE(ARGUNUSED(dip));

	instance = getminor((dev_t)(uintptr_t)arg);
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		sc = ddi_get_soft_state(nvmf_state, instance);
		if (sc == NULL)
			return (DDI_FAILURE);
		*result = sc->dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)(uintptr_t)instance;
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}

static int
nvmf_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
	_NOTE(ARGUNUSED(flag));

	if (otyp != OTYP_CHR)
		return (EINVAL);
	if (secpolicy_sys_config(credp, B_FALSE) != 0 ||
	    crgetzoneid(credp) != GLOBAL_ZONEID)
		return (EPERM);
	if (ddi_get_soft_state(nvmf_state, getminor(*devp)) == NULL)
		return (ENXIO);
	return (0);
}

static int
nvmf_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
	_NOTE(ARGUNUSED(dev, flag, otyp, credp));
	return (0);
}

static struct cb_ops nvmf_cb_ops = {
	.cb_open = nvmf_open,
	.cb_close = nvmf_close,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nvmf_ioctl,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_NEW | D_MP,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops nvmf_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = nvmf_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = nvmf_attach,
	.devo_detach = nvmf_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &nvmf_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed
};

/*
 * nvmf_host is a pseudo device, and e_ddi_assign_instance() does not assign
 * instances to the children of a pseudo nexus.  This wraps blkdev's bus_ctl
 * so that INITCHILD gives each blkdev child an instance, as pseudonex does for
 * its own children.
 */
static struct bus_ops nvmf_bus_ops;
static int (*nvmf_bd_bus_ctl)(dev_info_t *, dev_info_t *, ddi_ctl_enum_t,
    void *, void *);

/* blkdev packs 64 partitions per instance into a 32-bit capable minor. */
#define	NVMF_BD_MAX_INSTANCE	(MAXMIN32 / 64)

/*
 * Give the child the highest blkdev instance no node has.  The instance tree,
 * which numbers every other blkdev child, does not know these numbers and
 * hands its own out from the bottom, so the two are kept apart by starting
 * at the top.
 */
static int
nvmf_blkdev_assign_instance(dev_info_t *child)
{
	major_t		maj = ddi_driver_major(child);
	struct devnames	*dnp = &devnamesp[maj];
	dev_info_t	*tdip;
	int		inst;

	LOCK_DEV_OPS(&dnp->dn_lock);
	for (inst = NVMF_BD_MAX_INSTANCE; inst >= 0; inst--) {
		for (tdip = dnp->dn_head; tdip != NULL;
		    tdip = ddi_get_next(tdip)) {
			if (tdip != child && ddi_get_instance(tdip) == inst)
				break;
		}
		if (tdip == NULL) {
			DEVI(child)->devi_instance = inst;
			break;
		}
	}
	UNLOCK_DEV_OPS(&dnp->dn_lock);
	return (inst);
}

static int
nvmf_bus_ctl(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t ctlop,
    void *arg, void *result)
{
	int rv = nvmf_bd_bus_ctl(dip, rdip, ctlop, arg, result);

	if (ctlop == DDI_CTLOPS_INITCHILD && rv == DDI_SUCCESS) {
		dev_info_t *child = (dev_info_t *)arg;

		if (ddi_get_instance(child) == -1 &&
		    nvmf_blkdev_assign_instance(child) < 0) {
			(void) nvmf_bd_bus_ctl(dip, rdip,
			    DDI_CTLOPS_UNINITCHILD, arg, result);
			return (DDI_FAILURE);
		}
	}
	return (rv);
}

static struct modldrv nvmf_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "NVMe over Fabrics host",
	.drv_dev_ops = &nvmf_dev_ops
};

static struct modlinkage nvmf_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &nvmf_modldrv, NULL }
};

int
_init(void)
{
	int error;

	error = ddi_soft_state_init(&nvmf_state, sizeof (nvmf_softc_t), 0);
	if (error != 0)
		return (error);

	nvmf_cmd_init();

	bd_mod_init(&nvmf_dev_ops);
	nvmf_bus_ops = *nvmf_dev_ops.devo_bus_ops;
	nvmf_bd_bus_ctl = nvmf_bus_ops.bus_ctl;
	nvmf_bus_ops.bus_ctl = nvmf_bus_ctl;
	nvmf_dev_ops.devo_bus_ops = &nvmf_bus_ops;

	error = mod_install(&nvmf_modlinkage);
	if (error != 0) {
		bd_mod_fini(&nvmf_dev_ops);
		nvmf_cmd_fini();
		ddi_soft_state_fini(&nvmf_state);
	}
	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&nvmf_modlinkage, modinfop));
}

int
_fini(void)
{
	int error;

	error = mod_remove(&nvmf_modlinkage);
	if (error != 0)
		return (error);

	bd_mod_fini(&nvmf_dev_ops);
	nvmf_cmd_fini();
	ddi_soft_state_fini(&nvmf_state);
	return (0);
}

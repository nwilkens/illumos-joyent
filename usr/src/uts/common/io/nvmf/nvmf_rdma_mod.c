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
 * nvmf_rdma: the module and its control node.  See nvmf_rdma.c.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/disp.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/cred.h>
#include <sys/zone.h>
#include <sys/modctl.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/policy.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "nvmf_rdma_impl.h"

uint32_t nvmf_rdma_io_entries = 128;
uint32_t nvmf_rdma_max_qid = 64;
uint32_t nvmf_rdma_icd = 8192;

static dev_info_t *nvmf_rdma_dip;

static int
nvmf_rdma_open(dev_t *devp, int flag, int otyp, cred_t *cr)
{
	_NOTE(ARGUNUSED(flag));
	if (otyp != OTYP_CHR || getminor(*devp) != 0)
		return (ENXIO);
	if (crgetzoneid(cr) != GLOBAL_ZONEID || drv_priv(cr) != 0)
		return (EPERM);
	return (0);
}

/* ARGSUSED */
static int
nvmf_rdma_close(dev_t dev, int flag, int otyp, cred_t *cr)
{
	return (0);
}

static void
nvmf_rdma_sin(struct sockaddr_in *sin, uint32_t addr, uint16_t port)
{
	bzero(sin, sizeof (*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
	sin->sin_port = htons(port);
}

static int
nvmf_rdma_ioc_listen(intptr_t arg, int mode, cred_t *cr)
{
	nvmf_rdma_listen_t *l;
	struct sockaddr_in addr, *peers;
	nvmf_rdma_limits_t lim;
	uint32_t icd, id;
	uint_t i;
	int ret;

	l = kmem_zalloc(sizeof (*l), KM_SLEEP);
	if (ddi_copyin((void *)arg, l, sizeof (*l), mode) != 0) {
		kmem_free(l, sizeof (*l));
		return (EFAULT);
	}
	if (l->nrl_npeers == 0 || l->nrl_npeers > NVMF_RDMA_MAX_PEERS) {
		kmem_free(l, sizeof (*l));
		return (EINVAL);
	}
	nvmf_rdma_sin(&addr, l->nrl_addr, l->nrl_port != 0 ? l->nrl_port :
	    NVMF_RDMA_PORT);
	peers = kmem_zalloc(sizeof (*peers) * l->nrl_npeers, KM_SLEEP);
	for (i = 0; i < l->nrl_npeers; i++)
		nvmf_rdma_sin(&peers[i], l->nrl_peers[i], 0);
	lim.nrl_admin_entries = NVMF_RDMA_ADMIN_ENTRIES;
	lim.nrl_io_entries = l->nrl_io_entries != 0 ? l->nrl_io_entries :
	    nvmf_rdma_io_entries;
	lim.nrl_max_qid = (uint16_t)MIN(l->nrl_max_qid != 0 ? l->nrl_max_qid :
	    nvmf_rdma_max_qid, UINT16_MAX);
	icd = l->nrl_icd != NVMF_RDMA_ICD_DEFAULT ? l->nrl_icd : nvmf_rdma_icd;

	ret = nr_listen(cr, &addr, peers, l->nrl_npeers, &lim, icd, &id);
	if (ret == 0) {
		l->nrl_id = id;
		if (ddi_copyout(l, (void *)arg, sizeof (*l), mode) != 0) {
			(void) nr_unlisten(id);
			ret = EFAULT;
		}
	}
	kmem_free(peers, sizeof (*peers) * l->nrl_npeers);
	kmem_free(l, sizeof (*l));
	return (ret);
}

/* ARGSUSED */
static int
nvmf_rdma_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *cr,
    int *rvalp)
{
	nvmf_rdma_unlisten_t u;

	if (crgetzoneid(cr) != GLOBAL_ZONEID || drv_priv(cr) != 0)
		return (EPERM);
	if ((mode & FWRITE) == 0)
		return (EBADF);
	switch (cmd) {
	case NVMF_RDMA_IOC_LISTEN:
		return (nvmf_rdma_ioc_listen(arg, mode, cr));
	case NVMF_RDMA_IOC_UNLISTEN:
		if (ddi_copyin((void *)arg, &u, sizeof (u), mode) != 0)
			return (EFAULT);
		return (nr_unlisten(u.nru_id));
	default:
		return (ENOTTY);
	}
}

static int
nvmf_rdma_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);
	if (ddi_get_instance(dip) != 0 || nvmf_rdma_dip != NULL)
		return (DDI_FAILURE);
	if (ddi_create_minor_node(dip, "nvmf_rdma", S_IFCHR, 0, DDI_PSEUDO,
	    0) != DDI_SUCCESS)
		return (DDI_FAILURE);
	nvmf_rdma_dip = dip;
	return (DDI_SUCCESS);
}

static int
nvmf_rdma_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	if (nr_cm_busy())
		return (DDI_FAILURE);
	ddi_remove_minor_node(dip, NULL);
	nvmf_rdma_dip = NULL;
	return (DDI_SUCCESS);
}

/* ARGSUSED */
static int
nvmf_rdma_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg,
    void **resultp)
{
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*resultp = nvmf_rdma_dip;
		return (nvmf_rdma_dip != NULL ? DDI_SUCCESS : DDI_FAILURE);
	case DDI_INFO_DEVT2INSTANCE:
		*resultp = (void *)0;
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}

static struct cb_ops nvmf_rdma_cb_ops = {
	.cb_open = nvmf_rdma_open,
	.cb_close = nvmf_rdma_close,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nvmf_rdma_ioctl,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_MP | D_NEW,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops nvmf_rdma_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = nvmf_rdma_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = nvmf_rdma_attach,
	.devo_detach = nvmf_rdma_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &nvmf_rdma_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed
};

static struct modldrv nvmf_rdma_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "NVMe/RDMA transport",
	.drv_dev_ops = &nvmf_rdma_dev_ops
};

static struct modlinkage nvmf_rdma_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &nvmf_rdma_modldrv, NULL }
};

int
_init(void)
{
	int ret;

	nvmf_rdma_taskq = taskq_create("nvmf_rdma", 4, minclsyspri, 4,
	    INT_MAX, TASKQ_PREPOPULATE);
	if (nvmf_rdma_taskq == NULL)
		return (ENOMEM);
	if ((ret = nvmf_transport_register(&nvmf_rdma_ops)) != 0)
		goto fail;
	if ((ret = nr_cm_init()) != 0) {
		(void) nvmf_transport_unregister(&nvmf_rdma_ops);
		goto fail;
	}
	if ((ret = mod_install(&nvmf_rdma_modlinkage)) != 0) {
		nr_cm_fini();
		(void) nvmf_transport_unregister(&nvmf_rdma_ops);
		goto fail;
	}
	return (0);
fail:
	taskq_destroy(nvmf_rdma_taskq);
	nvmf_rdma_taskq = NULL;
	return (ret);
}

int
_fini(void)
{
	int ret;

	if (nr_cm_busy())
		return (EBUSY);
	/* Data buffers STMF still holds keep the transport registered. */
	if ((ret = nvmf_transport_unregister(&nvmf_rdma_ops)) != 0)
		return (ret);
	if ((ret = mod_remove(&nvmf_rdma_modlinkage)) != 0) {
		(void) nvmf_transport_register(&nvmf_rdma_ops);
		return (ret);
	}
	nr_cm_fini();
	taskq_destroy(nvmf_rdma_taskq);
	nvmf_rdma_taskq = NULL;
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&nvmf_rdma_modlinkage, modinfop));
}

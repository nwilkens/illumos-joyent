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
 * Module entry points, the STMF port provider, and the ioctls of
 * /dev/nvmft/admin that create and delete subsystems and hand queues from
 * nvmfd to the kernel.  Each subsystem is an STMF local port; see
 * nvmft_port.c.
 */

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/stat.h>		/* S_IFCHR */
#include <sys/file.h>		/* FMODELS */
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/list.h>
#include <sys/taskq.h>
#include <sys/disp.h>		/* minclsyspri */
#include <sys/cmn_err.h>
#include <sys/byteorder.h>	/* LE_16 */
#include <sys/zone.h>		/* zone_get_hostid() */
#include <sys/cred.h>
#include <sys/systm.h>		/* delay_sig() */

#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>
#include <sys/nvme/nvmf_ioctl.h>
#include <sys/nvpair.h>
#include <sys/sysmacros.h>

/* Fabrics controller defaults (NVMF_MAX_IO_ENTRIES, NVMF_IOCCSZ, ...). */
#include "../../../nvmf/nvmf_core.h"

#include <sys/time.h>		/* hrtime_t (sys/stmf.h needs it) */
#include <sys/stmf.h>
#include <sys/stmf_ioctl.h>
#include <sys/portif.h>

#include "nvmft_var.h"

/* Global module context. */
nvmft_softc_t	*nvmft_global = NULL;
uint_t		nvmft_errlevel = NVMFT_LOG_L1;

static char nvmft_pp_name[] = "nvmft";

#define	NVMFT_NAME_VERSION	"NVMe-oF Target Port Provider"

static int nvmft_drv_attach(dev_info_t *, ddi_attach_cmd_t);
static int nvmft_drv_detach(dev_info_t *, ddi_detach_cmd_t);
static int nvmft_drv_getinfo(dev_info_t *, ddi_info_cmd_t, void *, void **);
static int nvmft_drv_open(dev_t *, int, int, cred_t *);
static int nvmft_drv_close(dev_t, int, int, cred_t *);
static int nvmft_drv_ioctl(dev_t, int, intptr_t, int, cred_t *, int *);
static void nvmft_pp_cb(stmf_port_provider_t *, int, void *, uint32_t);

extern struct mod_ops mod_driverops;

static struct cb_ops nvmft_cb_ops = {
	nvmft_drv_open,		/* cb_open */
	nvmft_drv_close,	/* cb_close */
	nodev,			/* cb_strategy */
	nodev,			/* cb_print */
	nodev,			/* cb_dump */
	nodev,			/* cb_read */
	nodev,			/* cb_write */
	nvmft_drv_ioctl,	/* cb_ioctl */
	nodev,			/* cb_devmap */
	nodev,			/* cb_mmap */
	nodev,			/* cb_segmap */
	nochpoll,		/* cb_chpoll */
	ddi_prop_op,		/* cb_prop_op */
	NULL,			/* cb_streamtab */
	D_MP,			/* cb_flag */
	CB_REV,			/* cb_rev */
	nodev,			/* cb_aread */
	nodev,			/* cb_awrite */
};

static struct dev_ops nvmft_dev_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* devo_refcnt */
	nvmft_drv_getinfo,	/* devo_getinfo */
	nulldev,		/* devo_identify */
	nulldev,		/* devo_probe */
	nvmft_drv_attach,	/* devo_attach */
	nvmft_drv_detach,	/* devo_detach */
	nodev,			/* devo_reset */
	&nvmft_cb_ops,		/* devo_cb_ops */
	NULL,			/* devo_bus_ops */
	NULL,			/* devo_power */
	ddi_quiesce_not_needed,	/* devo_quiesce */
};

static struct modldrv modldrv = {
	&mod_driverops,
	NVMFT_NAME_VERSION,
	&nvmft_dev_ops,
};

static struct modlinkage nvmft_modlinkage = {
	MODREV_1,
	&modldrv,
	NULL,
};

int
_init(void)
{
	int status;

	nvmft_global = kmem_zalloc(sizeof (nvmft_softc_t), KM_SLEEP);
	mutex_init(&nvmft_global->ns_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&nvmft_global->ns_config_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&nvmft_global->ns_ports, sizeof (nvmft_port_t),
	    offsetof(nvmft_port_t, np_link));

	/*
	 * ns_taskq has one thread, which orders shutdown, terminate and the
	 * Connect of kernel-born queues.  taskq_dispatch_ent() needs taskqs
	 * that are not TASKQ_DYNAMIC.
	 */
	nvmft_global->ns_taskq = taskq_create("nvmft", 1, minclsyspri, 1,
	    INT_MAX, 0);
	nvmft_global->ns_admin_taskq = taskq_create("nvmft_admin", 4,
	    minclsyspri, 4, INT_MAX, 0);
	nvmft_global->ns_io_taskq = taskq_create("nvmft_io", 4, minclsyspri,
	    4, INT_MAX, 0);

	status = mod_install(&nvmft_modlinkage);
	if (status != DDI_SUCCESS) {
		taskq_destroy(nvmft_global->ns_io_taskq);
		taskq_destroy(nvmft_global->ns_admin_taskq);
		taskq_destroy(nvmft_global->ns_taskq);
		list_destroy(&nvmft_global->ns_ports);
		mutex_destroy(&nvmft_global->ns_config_lock);
		mutex_destroy(&nvmft_global->ns_lock);
		kmem_free(nvmft_global, sizeof (nvmft_softc_t));
		nvmft_global = NULL;
	}

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&nvmft_modlinkage, modinfop));
}

int
_fini(void)
{
	int status;

	mutex_enter(&nvmft_global->ns_lock);
	if (!list_is_empty(&nvmft_global->ns_ports) ||
	    nvmft_global->ns_kernel_qpairs != 0) {
		mutex_exit(&nvmft_global->ns_lock);
		return (EBUSY);
	}
	nvmft_global->ns_closing = B_TRUE;
	mutex_exit(&nvmft_global->ns_lock);

	status = mod_remove(&nvmft_modlinkage);
	if (status != DDI_SUCCESS) {
		mutex_enter(&nvmft_global->ns_lock);
		nvmft_global->ns_closing = B_FALSE;
		mutex_exit(&nvmft_global->ns_lock);
		return (status);
	}

	taskq_destroy(nvmft_global->ns_io_taskq);
	taskq_destroy(nvmft_global->ns_admin_taskq);
	taskq_destroy(nvmft_global->ns_taskq);
	list_destroy(&nvmft_global->ns_ports);
	mutex_destroy(&nvmft_global->ns_config_lock);
	mutex_destroy(&nvmft_global->ns_lock);
	kmem_free(nvmft_global, sizeof (nvmft_softc_t));
	nvmft_global = NULL;

	return (status);
}

/* ARGSUSED */
static int
nvmft_drv_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = nvmft_global != NULL ? nvmft_global->ns_dip : NULL;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = NULL;
		return (DDI_SUCCESS);
	default:
		break;
	}
	return (DDI_FAILURE);
}

static int
nvmft_drv_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	stmf_status_t status;

	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	if (ddi_get_instance(dip) != 0)
		return (DDI_FAILURE);

	if (ddi_create_minor_node(dip, "admin", S_IFCHR, 0, DDI_PSEUDO, 0) !=
	    DDI_SUCCESS)
		return (DDI_FAILURE);

	nvmft_global->ns_dip = dip;

	/*
	 * Register the STMF port provider.  Per-subsystem local ports are
	 * created later via nvmft_port_alloc() in response to the provider
	 * configuration callback.
	 */
	nvmft_global->ns_pp = stmf_alloc(STMF_STRUCT_PORT_PROVIDER, 0, 0);
	if (nvmft_global->ns_pp == NULL) {
		ddi_remove_minor_node(dip, NULL);
		return (DDI_FAILURE);
	}
	nvmft_global->ns_pp->pp_portif_rev = PORTIF_REV_1;
	nvmft_global->ns_pp->pp_name = nvmft_pp_name;
	nvmft_global->ns_pp->pp_cb = nvmft_pp_cb;

	status = stmf_register_port_provider(nvmft_global->ns_pp);
	if (status != STMF_SUCCESS) {
		stmf_free(nvmft_global->ns_pp);
		nvmft_global->ns_pp = NULL;
		ddi_remove_minor_node(dip, NULL);
		return (DDI_FAILURE);
	}

	ddi_report_dev(dip);
	return (DDI_SUCCESS);
}

static int
nvmft_drv_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_DETACH:
		break;
	case DDI_SUSPEND:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	mutex_enter(&nvmft_global->ns_lock);
	if (!list_is_empty(&nvmft_global->ns_ports) ||
	    nvmft_global->ns_kernel_qpairs != 0) {
		mutex_exit(&nvmft_global->ns_lock);
		return (DDI_FAILURE);
	}
	mutex_exit(&nvmft_global->ns_lock);

	if (nvmft_global->ns_pp != NULL) {
		if (stmf_deregister_port_provider(nvmft_global->ns_pp) !=
		    STMF_SUCCESS)
			return (DDI_FAILURE);
		stmf_free(nvmft_global->ns_pp);
		nvmft_global->ns_pp = NULL;
	}

	ddi_remove_minor_node(dip, NULL);
	nvmft_global->ns_dip = NULL;
	return (DDI_SUCCESS);
}

/* ARGSUSED */
static int
nvmft_drv_open(dev_t *devp, int flag, int otyp, cred_t *cred)
{
	if (otyp != OTYP_CHR)
		return (EINVAL);
	if (drv_priv(cred) != 0 || crgetzoneid(cred) != GLOBAL_ZONEID)
		return (EPERM);
	return (0);
}

/* ARGSUSED */
static int
nvmft_drv_close(dev_t dev, int flag, int otyp, cred_t *cred)
{
	return (0);
}

/*
 * Look up a subsystem by SubNQN.  Returns the port with a reference held, or
 * NULL.  Called with ns_lock held.
 */
static nvmft_port_t *
nvmft_port_lookup(const char *subnqn)
{
	nvmft_port_t *np;
	size_t len = strlen(subnqn);

	ASSERT(MUTEX_HELD(&nvmft_global->ns_lock));

	for (np = list_head(&nvmft_global->ns_ports); np != NULL;
	    np = list_next(&nvmft_global->ns_ports, np)) {
		scsi_devid_desc_t *id = np->np_devid;

		if (id->ident_length == len &&
		    memcmp(id->ident, subnqn, len) == 0) {
			nvmft_port_ref(np);
			return (np);
		}
	}
	return (NULL);
}

/* The subsystem for a new queue: one that is not being deleted. */
nvmft_port_t *
nvmft_port_find(const char *subnqn)
{
	nvmft_port_t *np;

	mutex_enter(&nvmft_global->ns_lock);
	np = nvmft_port_lookup(subnqn);
	if (np != NULL && np->np_deleting) {
		mutex_exit(&nvmft_global->ns_lock);
		nvmft_port_rele(np);
		return (NULL);
	}
	mutex_exit(&nvmft_global->ns_lock);
	return (np);
}

/*
 * The limits on the parameters of a subsystem.  A capsule size is a multiple
 * of 16 bytes that holds at least the queue entry, and an NSID is an STMF LUN
 * plus one.
 */
static boolean_t
nvmft_port_params_valid(uint64_t portid, uint64_t max_io_qsize,
    uint64_t ioccsz, uint64_t iorcsz, uint64_t nn)
{
	if (portid > UINT16_MAX)
		return (B_FALSE);
	if (max_io_qsize < NVME_MIN_IO_ENTRIES ||
	    max_io_qsize > NVME_MAX_IO_ENTRIES)
		return (B_FALSE);
	if (ioccsz < sizeof (nvme_sqe_t) || ioccsz > UINT32_MAX ||
	    ioccsz % 16 != 0)
		return (B_FALSE);
	if (iorcsz < sizeof (nvme_cqe_t) || iorcsz > UINT32_MAX ||
	    iorcsz % 16 != 0)
		return (B_FALSE);
	if (nn < 1 || nn > NVMFT_MAX_NSID)
		return (B_FALSE);
	return (B_TRUE);
}

/*
 * NVMFT_IOC_SUBSYS_CREATE: create and register one subsystem stmf_local_port_t.
 * Only "subnqn" is required; everything else defaults to the Fabrics values.
 * STMF must still be told to online the port (stmfadm online-target).
 */
static int
nvmft_ioc_subsys_create(intptr_t arg, int mode)
{
	struct nvmf_ioc_nv ionv;
	nvlist_t *req = NULL;
	nvmft_port_t *np;
	char *subnqn = NULL, *serial = NULL;
	char serbuf[NVME_SERIAL_SZ];
	uint64_t portid, max_io_qsize, ioccsz, iorcsz, nn;
	int error;

	if (ddi_copyin((void *)arg, &ionv, sizeof (ionv), mode) != 0)
		return (EFAULT);

	if ((error = nvmf_unpack_ioc_nvlist(&ionv, &req)) != 0)
		return (error);

	if (nvlist_lookup_string(req, NVMFT_NV_SUBNQN, &subnqn) != 0 ||
	    !nvmf_nqn_valid(subnqn)) {
		nvlist_free(req);
		return (EINVAL);
	}

	if (nvlist_lookup_string(req, NVMFT_NV_SERIAL, &serial) != 0) {
		nvmf_controller_serial(serbuf, sizeof (serbuf),
		    zone_get_hostid(NULL));
		serial = serbuf;
	}
	if (nvlist_lookup_uint64(req, NVMFT_NV_PORTID, &portid) != 0)
		portid = 1;
	if (nvlist_lookup_uint64(req, NVMFT_NV_MAX_IO_QSIZE,
	    &max_io_qsize) != 0)
		max_io_qsize = NVMF_MAX_IO_ENTRIES;
	if (nvlist_lookup_uint64(req, NVMFT_NV_IOCCSZ, &ioccsz) != 0)
		ioccsz = NVMF_IOCCSZ;
	if (nvlist_lookup_uint64(req, NVMFT_NV_IORCSZ, &iorcsz) != 0)
		iorcsz = NVMF_IORCSZ;
	if (nvlist_lookup_uint64(req, NVMFT_NV_NN, &nn) != 0)
		nn = NVMF_NN;

	if (!nvmft_port_params_valid(portid, max_io_qsize, ioccsz, iorcsz,
	    nn)) {
		nvlist_free(req);
		return (EINVAL);
	}

	/* ns_config_lock keeps a second create of the SubNQN out. */
	mutex_enter(&nvmft_global->ns_config_lock);
	mutex_enter(&nvmft_global->ns_lock);
	np = nvmft_port_lookup(subnqn);
	mutex_exit(&nvmft_global->ns_lock);
	if (np != NULL) {
		nvmft_port_rele(np);
		error = EEXIST;
	} else {
		/* The port keeps the reference it is returned with. */
		np = nvmft_port_alloc(subnqn, (uint16_t)portid, serial,
		    (uint32_t)max_io_qsize, NVMF_CC_EN_TIMEOUT * 500,
		    (uint32_t)ioccsz, (uint32_t)iorcsz, (uint32_t)nn);
		error = (np == NULL) ? EIO : 0;
	}
	mutex_exit(&nvmft_global->ns_config_lock);
	nvlist_free(req);
	return (error);
}

/*
 * Take the port offline, wait until STMF has seen it offline, and deregister
 * it.  The wait can be interrupted, which leaves the port registered.
 */
static int
nvmft_port_unregister(nvmft_port_t *np)
{
	stmf_state_change_info_t ssci;
	stmf_status_t st;

	ssci.st_rflags = 0;
	ssci.st_additional_info = NULL;
	for (;;) {
		st = stmf_ctl(STMF_CMD_LPORT_OFFLINE, np->np_lport, &ssci);
		/* STMF_BUSY: an online request is still on its way. */
		if (st != STMF_BUSY)
			break;
		if (delay_sig(drv_usectohz(10000)) != 0)
			return (EINTR);
	}
	if (st != STMF_SUCCESS && st != STMF_ALREADY)
		return (EIO);

	/*
	 * A session is not registered while the port is deregistered, so
	 * this waits for an admin queue that is being accepted as well.
	 */
	mutex_enter(&np->np_lock);
	while (!np->np_offline || np->np_accepting != 0) {
		if (cv_wait_sig(&np->np_cv, &np->np_lock) == 0) {
			mutex_exit(&np->np_lock);
			return (EINTR);
		}
	}
	mutex_exit(&np->np_lock);

	for (;;) {
		st = stmf_deregister_local_port(np->np_lport);
		if (st != STMF_BUSY)
			break;
		if (delay_sig(drv_usectohz(100000)) != 0)
			return (EINTR);
	}
	return (st == STMF_SUCCESS ? 0 : EIO);
}

/*
 * NVMFT_IOC_SUBSYS_DELETE: deregister a subsystem by SubNQN.  Refuses while
 * any host is connected (np_controllers non-empty).
 */
static int
nvmft_ioc_subsys_delete(intptr_t arg, int mode)
{
	struct nvmf_ioc_nv ionv;
	nvlist_t *req = NULL;
	nvmft_port_t *np;
	char *subnqn = NULL;
	int error;

	if (ddi_copyin((void *)arg, &ionv, sizeof (ionv), mode) != 0)
		return (EFAULT);

	if ((error = nvmf_unpack_ioc_nvlist(&ionv, &req)) != 0)
		return (error);

	if (nvlist_lookup_string(req, NVMFT_NV_SUBNQN, &subnqn) != 0) {
		nvlist_free(req);
		return (EINVAL);
	}

	/* From here no queue finds the port, and no one else deletes it. */
	mutex_enter(&nvmft_global->ns_config_lock);
	mutex_enter(&nvmft_global->ns_lock);
	np = nvmft_port_lookup(subnqn);
	if (np != NULL)
		np->np_deleting = B_TRUE;
	mutex_exit(&nvmft_global->ns_lock);
	nvlist_free(req);
	if (np == NULL) {
		mutex_exit(&nvmft_global->ns_config_lock);
		return (ENOENT);
	}

	mutex_enter(&np->np_lock);
	error = list_is_empty(&np->np_controllers) ? 0 : EBUSY;
	mutex_exit(&np->np_lock);
	if (error == 0)
		error = nvmft_port_unregister(np);

	mutex_enter(&nvmft_global->ns_lock);
	if (error == 0)
		list_remove(&nvmft_global->ns_ports, np);
	else
		np->np_deleting = B_FALSE;
	mutex_exit(&nvmft_global->ns_lock);
	mutex_exit(&nvmft_global->ns_config_lock);

	nvmft_port_rele(np);
	if (error == 0)
		nvmft_port_rele(np);	/* the port's own reference */
	return (error);
}

/*
 * NVMFT_IOC_SUBSYS_LIST: copy out the list of registered SubNQNs as a packed
 * nvlist with a single "subnqns" string array.
 */
static int
nvmft_ioc_subsys_list(intptr_t arg, int mode)
{
	struct nvmf_ioc_nv ionv;
	nvlist_t *reply;
	nvmft_port_t *np;
	char **nqns;
	uint_t n, i;
	int error;

	if (ddi_copyin((void *)arg, &ionv, sizeof (ionv), mode) != 0)
		return (EFAULT);

	reply = fnvlist_alloc();

	mutex_enter(&nvmft_global->ns_lock);
	n = 0;
	for (np = list_head(&nvmft_global->ns_ports); np != NULL;
	    np = list_next(&nvmft_global->ns_ports, np))
		n++;

	nqns = kmem_zalloc(MAX(n, 1) * sizeof (char *), KM_SLEEP);
	i = 0;
	for (np = list_head(&nvmft_global->ns_ports); np != NULL;
	    np = list_next(&nvmft_global->ns_ports, np))
		nqns[i++] = (char *)np->np_devid->ident;
	fnvlist_add_string_array(reply, NVMFT_NV_SUBNQNS, nqns, n);
	mutex_exit(&nvmft_global->ns_lock);

	kmem_free(nqns, MAX(n, 1) * sizeof (char *));

	error = nvmf_pack_ioc_nvlist(reply, &ionv);
	nvlist_free(reply);
	if (error != 0)
		return (error);

	if (ddi_copyout(&ionv, (void *)arg, sizeof (ionv), mode) != 0)
		return (EFAULT);

	return (0);
}

/*
 * What the kernel requires of a handoff besides the shape of its nvlist.
 * nvmfd made these checks when it accepted the Connect, but nvmfd is not
 * trusted to have: the fields size kernel objects and select the queue type.
 */
static boolean_t
nvmft_handoff_valid(uint64_t trtype, boolean_t admin, uint64_t qsize,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data)
{
	nvmft_connect_status_t st;
	uint16_t qid = LE_16(cmd->nfcc_qid);

	if (trtype > NVMF_TRTYPE_INTRA_HOST)
		return (B_FALSE);
	if (admin != (qid == 0))
		return (B_FALSE);
	if (qsize == 0 || qsize > NVME_MAX_IO_ENTRIES)
		return (B_FALSE);
	if (!nvmft_connect_cmd_valid(cmd, sizeof (*data), qid,
	    (uint16_t)(qsize - 1), NVME_MAX_ADMIN_ENTRIES, NVME_MAX_IO_ENTRIES,
	    &st))
		return (B_FALSE);
	return (nvmft_connect_data_valid(cmd, data, &st));
}

/*
 * NVMFT_IOC_HANDOFF: adopt a queue pair whose Connect nvmfd accepted.  The
 * nvlist is { trtype, params, cmd, data } as libnvmf's
 * nvmf_handoff_controller_qpair() packs it: the transport parameters with
 * the socket, and the Connect command and data.  The transport looks up the
 * socket in the calling process, so this runs on the ioctl thread.
 */
static int
nvmft_ioc_handoff(intptr_t arg, int mode)
{
	struct nvmf_ioc_nv ionv;
	nvlist_t *req = NULL;
	nvlist_t *params = NULL;
	nvmft_port_t *np;
	nvmf_fabric_connect_cmd_t cmd;
	nvmf_fabric_connect_data_t data;
	uchar_t *cmdb = NULL, *datab = NULL;
	char subnqn[NVMF_NQN_FIELD_SIZE + 1];
	uint64_t trtype, qsize = 0;
	boolean_t admin = B_FALSE;
	uint_t cmdlen, datalen;
	int error;

	if (ddi_copyin((void *)arg, &ionv, sizeof (ionv), mode) != 0)
		return (EFAULT);

	if ((error = nvmf_unpack_ioc_nvlist(&ionv, &req)) != 0)
		return (error);

	if (nvlist_lookup_uint64(req, NVMFT_NV_TRTYPE, &trtype) != 0 ||
	    nvlist_lookup_nvlist(req, NVMFT_NV_PARAMS, &params) != 0 ||
	    nvlist_lookup_byte_array(req, NVMFT_NV_CMD, &cmdb, &cmdlen) != 0 ||
	    nvlist_lookup_byte_array(req, NVMFT_NV_DATA, &datab,
	    &datalen) != 0 || cmdlen != sizeof (cmd) ||
	    datalen != sizeof (data) ||
	    !nvmf_validate_qpair_nvlist(params, B_TRUE)) {
		nvlist_free(req);
		return (EINVAL);
	}

	/* The byte arrays of an nvlist are not aligned. */
	(void) memcpy(&cmd, cmdb, sizeof (cmd));
	(void) memcpy(&data, datab, sizeof (data));

	(void) nvlist_lookup_boolean_value(params, "admin", &admin);
	(void) nvlist_lookup_uint64(params, "qsize", &qsize);
	if (!nvmft_handoff_valid(trtype, admin, qsize, &cmd, &data)) {
		nvlist_free(req);
		return (EINVAL);
	}

	/* The field may have no terminator. */
	(void) memcpy(subnqn, data.nfcd_subnqn, sizeof (data.nfcd_subnqn));
	subnqn[sizeof (data.nfcd_subnqn)] = '\0';
	np = nvmft_port_find(subnqn);
	if (np == NULL) {
		nvlist_free(req);
		return (ENOENT);
	}

	/* On failure these destroy the qpair, which closes the socket. */
	if (admin)
		error = nvmft_handoff_admin_queue(np, (nvmf_trtype_t)trtype,
		    params, &cmd, &data);
	else
		error = nvmft_handoff_io_queue(np, (nvmf_trtype_t)trtype,
		    params, &cmd, &data);

	nvmft_port_rele(np);
	nvlist_free(req);
	return (error);
}

/* ARGSUSED */
static int
nvmft_drv_ioctl(dev_t dev, int cmd, intptr_t data, int mode, cred_t *cred,
    int *rval)
{
	if (drv_priv(cred) != 0 || crgetzoneid(cred) != GLOBAL_ZONEID)
		return (EPERM);

	/*
	 * struct nvmf_ioc_nv has a pointer and two size_t fields and is
	 * copied in at its native size.  nvmfd(8) and nvmfadm(8) are 64-bit.
	 */
	if (ddi_model_convert_from(mode & FMODELS) != DDI_MODEL_NONE)
		return (ENOTSUP);

	switch (cmd) {
	case NVMFT_IOC_SUBSYS_CREATE:
		return (nvmft_ioc_subsys_create(data, mode));
	case NVMFT_IOC_SUBSYS_DELETE:
		return (nvmft_ioc_subsys_delete(data, mode));
	case NVMFT_IOC_SUBSYS_LIST:
		return (nvmft_ioc_subsys_list(data, mode));
	case NVMFT_IOC_HANDOFF:
		return (nvmft_ioc_handoff(data, mode));
	default:
		break;
	}

	return (ENOTTY);
}

/*
 * Subsystems are configured with the ioctls above, not with STMF provider
 * data, so there is nothing to do here.
 */
/* ARGSUSED */
static void
nvmft_pp_cb(stmf_port_provider_t *pp, int cmd, void *arg, uint32_t flags)
{
}

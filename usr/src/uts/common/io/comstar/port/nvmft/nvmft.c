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
 * NEW file.  Module entry points and STMF port-provider registration for the
 * NVMe-over-Fabrics COMSTAR controller (target).
 *
 * This is modeled on the SRP target port provider's srpt_mod.c.  Where FreeBSD
 * registered an NVMe target frontend with CTL via CTL_FRONTEND_DECLARE() in
 * ctl_frontend_nvmf.c, illumos registers an stmf_port_provider_t with STMF here.
 * The per-subsystem stmf_local_port_t registrations happen in nvmft_stmf.c,
 * driven by the provider configuration callback (pp_cb).
 *
 * Layering: this module depends on the transport core module (nvmf) for the
 * Fabrics capsule/queue-pair API (<sys/nvme/nvmf_transport.h>) and on stmf for
 * the port-provider API (<sys/portif.h>).  The build glue must add
 * "-N misc/nvmf -N drv/stmf" to this module's LDFLAGS (NVMEOF.md 7.5).
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

/*
 * Worker thread count for the datamove taskq.  FreeBSD started mp_ncpus threads
 * on its single nvmft_taskq; a small fixed pool is sufficient here because the
 * datamove taskq carries only deferred per-qpair transfers, and must allow at
 * least one transfer to run while a shutdown worker drains the queue.
 */
#define	NVMFT_DATAMOVE_NTHREADS	4

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
	list_create(&nvmft_global->ns_ports, sizeof (nvmft_port_t),
	    offsetof(nvmft_port_t, np_link));

	/*
	 * Two worker taskqs split the roles that FreeBSD's single nvmft_taskq
	 * served (created in nvmft_init()):
	 *
	 *   ns_taskq          controller shutdown / terminate work.
	 *   ns_datamove_taskq per-qpair datamove work.
	 *
	 * They MUST be distinct.  The shutdown worker (running on ns_taskq) drains
	 * the qpair datamove work via taskq_wait()/nvmft_drain_task(); illumos
	 * forbids waiting on, or dispatching to, the taskq you are executing on
	 * (taskq.c: ASSERT(tq != curthread->t_taskq) in both taskq_wait() and
	 * taskq_dispatch_ent()).  A single shared taskq would self-wait and
	 * deadlock every controller shutdown.  This mirrors FreeBSD draining only
	 * the specific qp->datamove_task rather than the whole queue.
	 *
	 * Neither taskq may be TASKQ_DYNAMIC: every consumer uses the preallocated
	 * taskq_dispatch_ent() path, which asserts !TASKQ_DYNAMIC.  The control
	 * taskq is single-threaded to keep shutdown/terminate ordering serialized;
	 * the datamove taskq is multi-threaded so transfers can proceed while a
	 * shutdown worker drains them.
	 */
	nvmft_global->ns_taskq = taskq_create("nvmft", 1, minclsyspri, 1,
	    INT_MAX, 0);
	if (nvmft_global->ns_taskq == NULL) {
		list_destroy(&nvmft_global->ns_ports);
		mutex_destroy(&nvmft_global->ns_lock);
		kmem_free(nvmft_global, sizeof (nvmft_softc_t));
		nvmft_global = NULL;
		return (ENOMEM);
	}

	nvmft_global->ns_datamove_taskq = taskq_create("nvmft_datamove",
	    NVMFT_DATAMOVE_NTHREADS, minclsyspri, 1, INT_MAX, 0);
	if (nvmft_global->ns_datamove_taskq == NULL) {
		taskq_destroy(nvmft_global->ns_taskq);
		list_destroy(&nvmft_global->ns_ports);
		mutex_destroy(&nvmft_global->ns_lock);
		kmem_free(nvmft_global, sizeof (nvmft_softc_t));
		nvmft_global = NULL;
		return (ENOMEM);
	}

	status = mod_install(&nvmft_modlinkage);
	if (status != DDI_SUCCESS) {
		taskq_destroy(nvmft_global->ns_datamove_taskq);
		taskq_destroy(nvmft_global->ns_taskq);
		list_destroy(&nvmft_global->ns_ports);
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
	if (!list_is_empty(&nvmft_global->ns_ports)) {
		mutex_exit(&nvmft_global->ns_lock);
		return (EBUSY);
	}
	mutex_exit(&nvmft_global->ns_lock);

	status = mod_remove(&nvmft_modlinkage);
	if (status != DDI_SUCCESS)
		return (status);

	taskq_destroy(nvmft_global->ns_datamove_taskq);
	taskq_destroy(nvmft_global->ns_taskq);
	list_destroy(&nvmft_global->ns_ports);
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
	if (!list_is_empty(&nvmft_global->ns_ports)) {
		mutex_exit(&nvmft_global->ns_lock);
		return (DDI_FAILURE);
	}
	mutex_exit(&nvmft_global->ns_lock);

	if (nvmft_global->ns_pp != NULL) {
		(void) stmf_deregister_port_provider(nvmft_global->ns_pp);
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
	if (drv_priv(cred) != 0)
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
 * Look up a registered subsystem port by SubNQN under ns_lock.  Returns the
 * port with a reference held (caller must nvmft_port_rele()), or NULL.
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

/*
 * NVMFT_IOC_SUBSYS_CREATE: create and register one subsystem stmf_local_port_t.
 * Only "subnqn" is required; everything else defaults to the Fabrics values.
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

	/* Optional parameters fall back to the Fabrics defaults. */
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

	if (portid > UINT16_MAX) {
		nvlist_free(req);
		return (EINVAL);
	}

	/* Reject a duplicate SubNQN. */
	mutex_enter(&nvmft_global->ns_lock);
	np = nvmft_port_lookup(subnqn);
	mutex_exit(&nvmft_global->ns_lock);
	if (np != NULL) {
		nvmft_port_rele(np);
		nvlist_free(req);
		return (EEXIST);
	}

	np = nvmft_port_alloc(subnqn, (uint16_t)portid, serial,
	    (uint32_t)max_io_qsize, NVMF_CC_EN_TIMEOUT * 500, (uint32_t)ioccsz,
	    (uint32_t)iorcsz, (uint32_t)nn);
	nvlist_free(req);
	if (np == NULL)
		return (EIO);

	/*
	 * nvmft_port_alloc() returns the port with the initial reference that
	 * keeps it registered; do not release it here.  STMF must still be told
	 * to online the port (stmfadm online-target).
	 */
	return (0);
}

/*
 * NVMFT_IOC_SUBSYS_DELETE: deregister a subsystem by SubNQN.  Refuses while
 * any host is connected (np_controllers non-empty).
 */
static int
nvmft_ioc_subsys_delete(intptr_t arg, int mode)
{
	struct nvmf_ioc_nv ionv;
	stmf_change_status_t cstatus;
	stmf_status_t st;
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

	mutex_enter(&nvmft_global->ns_lock);
	np = nvmft_port_lookup(subnqn);
	if (np == NULL) {
		mutex_exit(&nvmft_global->ns_lock);
		nvlist_free(req);
		return (ENOENT);
	}
	mutex_exit(&nvmft_global->ns_lock);

	mutex_enter(&np->np_lock);
	if (!list_is_empty(&np->np_controllers)) {
		mutex_exit(&np->np_lock);
		nvmft_port_rele(np);
		nvlist_free(req);
		return (EBUSY);
	}
	mutex_exit(&np->np_lock);

	/*
	 * Offline the port (drains any controllers, synchronously here since no
	 * hosts are connected), then deregister.  STMF returns STMF_BUSY until
	 * in-flight tasks drain; retry as srpt_stp_destroy_port() does.  Remove
	 * the port from ns_ports and drop the registration reference only after
	 * STMF accepts the deregistration.
	 */
	cstatus.st_completion_status = STMF_SUCCESS;
	cstatus.st_additional_info = NULL;
	(void) stmf_ctl(STMF_CMD_LPORT_OFFLINE, np->np_lport, &cstatus);

	for (;;) {
		st = stmf_deregister_local_port(np->np_lport);
		if (st != STMF_BUSY)
			break;
		delay(drv_usectohz(1000000));
	}

	if (st != STMF_SUCCESS) {
		/* Re-online so the port remains usable. */
		(void) stmf_ctl(STMF_CMD_LPORT_ONLINE, np->np_lport, &cstatus);
		nvmft_port_rele(np);
		nvlist_free(req);
		return (EIO);
	}

	mutex_enter(&nvmft_global->ns_lock);
	list_remove(&nvmft_global->ns_ports, np);
	mutex_exit(&nvmft_global->ns_lock);

	nvmft_port_rele(np);	/* lookup reference */
	nvmft_port_rele(np);	/* registration ref; frees the port */
	nvlist_free(req);
	return (0);
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
 * NVMFT_IOC_HANDOFF: adopt a CONNECT-negotiated queue pair from the userland
 * Fabrics daemon and start the matching in-kernel controller queue.
 *
 * This is the target-side equivalent of the host NVMF_HANDOFF_HOST: the daemon
 * has run the Fabrics CONNECT exchange in userland and packed the qpair (the
 * negotiated transport "params" carrying the socket fd, plus the raw CONNECT
 * "cmd" and "data") into the handoff nvlist.  We unpack it, find the target
 * subsystem from the CONNECT data's SubNQN, and dispatch on the CONNECT command
 * queue id: queue id 0 is the admin queue (creates the association), a nonzero
 * queue id attaches a new I/O queue to an existing one.  The transport resolves
 * the socket fd to a ksocket_t in this (the ioctl caller's) process context, so
 * this must run synchronously on the ioctl thread.  (FreeBSD
 * ctl_frontend_nvmf.c:nvmft_handoff via CTL_NVMF_HANDOFF.)
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
	uint64_t v64;
	uint_t cmdlen, datalen;
	nvmf_trtype_t trtype;
	int error;

	if (ddi_copyin((void *)arg, &ionv, sizeof (ionv), mode) != 0)
		return (EFAULT);

	if ((error = nvmf_unpack_ioc_nvlist(&ionv, &req)) != 0)
		return (error);

	/*
	 * The handoff nvlist shape is { trtype, params, cmd, data }, matching
	 * libnvmf's nvmf_handoff_controller_qpair() (see <sys/nvme/nvmf_ioctl.h>).
	 * Validate every key's presence, type, and (for the binary CONNECT
	 * structures) exact size before dereferencing them.
	 */
	if (nvlist_lookup_uint64(req, NVMFT_NV_TRTYPE, &v64) != 0 ||
	    nvlist_lookup_nvlist(req, NVMFT_NV_PARAMS, &params) != 0 ||
	    nvlist_lookup_byte_array(req, NVMFT_NV_CMD, &cmdb, &cmdlen) != 0 ||
	    nvlist_lookup_byte_array(req, NVMFT_NV_DATA, &datab, &datalen) != 0) {
		nvlist_free(req);
		return (EINVAL);
	}
	if (cmdlen != sizeof (nvmf_fabric_connect_cmd_t) ||
	    datalen != sizeof (nvmf_fabric_connect_data_t)) {
		nvlist_free(req);
		return (EINVAL);
	}
	if (!nvmf_validate_qpair_nvlist(params, B_TRUE)) {
		nvlist_free(req);
		return (EINVAL);
	}

	trtype = (nvmf_trtype_t)v64;

	/*
	 * Copy the CONNECT command and data out of the (byte-aligned) nvlist
	 * byte arrays into properly aligned locals before dereferencing their
	 * multi-byte wire fields.  The sizes were verified exactly above.
	 */
	(void) memcpy(&cmd, cmdb, sizeof (cmd));
	(void) memcpy(&data, datab, sizeof (data));

	/*
	 * The SubNQN that selects the target subsystem comes from the CONNECT
	 * data, not a separate nvlist key.  nfcd_subnqn is a fixed 256-byte
	 * on-wire field that is not guaranteed NUL-terminated; copy exactly the
	 * field and terminate it explicitly before validation and lookup.
	 */
	(void) memcpy(subnqn, data.nfcd_subnqn, sizeof (data.nfcd_subnqn));
	subnqn[sizeof (data.nfcd_subnqn)] = '\0';
	if (!nvmf_nqn_valid(subnqn)) {
		nvlist_free(req);
		return (EINVAL);
	}

	mutex_enter(&nvmft_global->ns_lock);
	np = nvmft_port_lookup(subnqn);
	mutex_exit(&nvmft_global->ns_lock);
	if (np == NULL) {
		nvlist_free(req);
		return (ENOENT);
	}

	/*
	 * Dispatch on the CONNECT queue id.  nfcc_qid is a little-endian wire
	 * field; the handoff helpers re-check it, but select the entry point
	 * here.  Both helpers own qpair teardown on failure (they destroy the
	 * qpair they built, which ksocket_close()s the adopted socket), so on
	 * error there is nothing further to clean up beyond the port reference.
	 */
	if (cmd.nfcc_qid == LE_16(0))
		error = nvmft_handoff_admin_queue(np, trtype, params, &cmd,
		    &data);
	else
		error = nvmft_handoff_io_queue(np, trtype, params, &cmd, &data);

	nvmft_port_rele(np);
	nvlist_free(req);
	return (error);
}

/* ARGSUSED */
static int
nvmft_drv_ioctl(dev_t dev, int cmd, intptr_t data, int mode, cred_t *cred,
    int *rval)
{
	if (drv_priv(cred) != 0)
		return (EPERM);

	/*
	 * struct nvmf_ioc_nv embeds a pointer and two size_t fields, so its
	 * layout differs between ILP32 and LP64.  Every handler below copies it
	 * in at the native (LP64) size; a 32-bit caller's fields would land
	 * misaligned, letting a garbage nv->size drive a huge KM_SLEEP
	 * allocation in nvmf_unpack_ioc_nvlist().  nvmfd(8)/nvmfadm(8) are 64-bit
	 * only, so reject ILP32 callers outright rather than carry a 32-bit
	 * shadow struct.
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

	/*
	 * PORT-TODO (FreeBSD ctl_frontend_nvmf.c:nvmft_ioctl): list/terminate
	 * associations and ANA set-group-state (nvmft_ana_ioctl, see NVMEOF.md)
	 * are wired here as additional NVMFT_IOC_* commands.
	 */
	return (ENOTTY);
}

/*
 * STMF port-provider configuration callback.  STMF invokes this with
 * provider-specific configuration data (delivered by libstmf/stmfadm) as part
 * of, and after, registration.  Modeled on srpt_pp_cb().
 *
 * PORT-TODO: parse the STMF_PROVIDER_DATA_UPDATED nvlist into the per-subsystem
 * parameters (subnqn, portid, serial, max_io_qsize, ioccsz, iorcsz, nn) and
 * call nvmft_port_alloc()/nvmft_port_rele() to create/destroy the exported
 * subsystem's stmf_local_port_t.  This replaces FreeBSD's nvmft_port_create()
 * being driven directly by a ctl_req ioctl.
 */
/* ARGSUSED */
static void
nvmft_pp_cb(stmf_port_provider_t *pp, int cmd, void *arg, uint32_t flags)
{
	NVMFT_DPRINTF_L2("nvmft_pp_cb, invoked (%d)", cmd);
}

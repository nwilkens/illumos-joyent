/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * The STMF local port of a subsystem, the STMF session of an association, and
 * the port's online and offline transitions.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/kmem.h>
#include <sys/list.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/id_space.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include <sys/time.h>
#include <sys/stmf.h>
#include <sys/lpif.h>
#include <sys/stmf_ioctl.h>
#include <sys/portif.h>

#include "nvmft_io.h"

static scsi_devid_desc_t *nvmft_alloc_scsi_devid_desc(const char *nqn);
static void nvmft_free_scsi_devid_desc(scsi_devid_desc_t *sdd);
static void nvmft_lport_ctl(stmf_local_port_t *lport, int cmd, void *arg);
static stmf_status_t nvmft_lport_info(uint32_t cmd, stmf_local_port_t *lport,
    void *arg, uint8_t *buf, uint32_t *bufsizep);
static void nvmft_lport_event_handler(stmf_local_port_t *lport, int eventid,
    void *arg, uint32_t flags);

/*
 * Allocate and register an stmf_local_port_t for a subsystem (SubNQN).  The
 * caller has checked the parameters and that the SubNQN is not in use.
 */
nvmft_port_t *
nvmft_port_alloc(const char *subnqn, uint16_t portid, const char *serial,
    uint32_t max_io_qsize, uint32_t enable_timeout, uint32_t ioccsz,
    uint32_t iorcsz, uint32_t nn)
{
	stmf_local_port_t *lport;
	nvmft_port_t *np;

	lport = stmf_alloc(STMF_STRUCT_STMF_LOCAL_PORT, sizeof (*np), 0);
	if (lport == NULL)
		return (NULL);

	np = lport->lport_port_private;
	np->np_lport = lport;
	np->np_portid = portid;
	np->np_max_io_qsize = max_io_qsize;
	np->np_cap = _nvmf_controller_cap(max_io_qsize, enable_timeout / 500);
	np->np_offline = B_TRUE;
	np->np_refs = 1;

	mutex_init(&np->np_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&np->np_cv, NULL, CV_DRIVER, NULL);
	np->np_ids = id_space_create("nvmft_cntlid", 0,
	    NVMF_CNTLID_STATIC_MAX);
	list_create(&np->np_controllers, sizeof (nvmft_controller_t),
	    offsetof(nvmft_controller_t, ctrlr_link));

	/* The cntlid is set per-controller; pass 0 for the template. */
	_nvmf_init_io_controller_data(0, max_io_qsize, serial, utsname.sysname,
	    utsname.version, subnqn, nn, ioccsz, iorcsz, &np->np_cdata);

	np->np_devid = nvmft_alloc_scsi_devid_desc(subnqn);
	lport->lport_ds = nvmft_dbuf_store_create();
	if (np->np_devid == NULL || lport->lport_ds == NULL) {
		nvmft_port_free(np);
		return (NULL);
	}
	lport->lport_id = np->np_devid;
	lport->lport_pp = nvmft_global->ns_pp;
	lport->lport_xfer_data = nvmft_lport_xfer_data;
	lport->lport_send_status = nvmft_lport_send_status;
	lport->lport_task_free = nvmft_lport_task_free;
	lport->lport_abort = nvmft_lport_abort;
	lport->lport_abort_timeout = 300;	/* 5 minutes */
	lport->lport_task_poll = nvmft_lport_task_poll;
	lport->lport_ctl = nvmft_lport_ctl;
	lport->lport_info = nvmft_lport_info;
	lport->lport_event_handler = nvmft_lport_event_handler;

	if (stmf_register_local_port(lport) != STMF_SUCCESS) {
		nvmft_port_free(np);
		return (NULL);
	}

	mutex_enter(&nvmft_global->ns_lock);
	list_insert_tail(&nvmft_global->ns_ports, np);
	nvmft_global->ns_generation++;
	mutex_exit(&nvmft_global->ns_lock);

	return (np);
}

void
nvmft_port_free(nvmft_port_t *np)
{
	ASSERT(list_is_empty(&np->np_controllers));

	if (np->np_lport->lport_ds != NULL) {
		nvmft_dbuf_store_destroy(np->np_lport->lport_ds);
		np->np_lport->lport_ds = NULL;
	}
	if (np->np_devid != NULL)
		nvmft_free_scsi_devid_desc(np->np_devid);
	id_space_destroy(np->np_ids);
	list_destroy(&np->np_controllers);
	cv_destroy(&np->np_cv);
	mutex_destroy(&np->np_lock);
	/* stmf_free() releases the lport and the embedded nvmft_port. */
	stmf_free(np->np_lport);
}

/*
 * Register the STMF session of an association.  Every task needs one, and
 * sbd keys persistent reservations on its remote port, which is the host NQN.
 * This fails unless the local port is online.
 */
int
nvmft_session_register(nvmft_controller_t *ctrlr)
{
	nvmft_port_t *np = ctrlr->ctrlr_np;
	stmf_scsi_session_t *ss;
	scsi_devid_desc_t *rport_id;
	char hostnqn[NVMF_NQN_FIELD_SIZE + 1];

	/* The field may have no terminator. */
	(void) memcpy(hostnqn, ctrlr->ctrlr_hostnqn, NVMF_NQN_FIELD_SIZE);
	hostnqn[NVMF_NQN_FIELD_SIZE] = '\0';

	rport_id = nvmft_alloc_scsi_devid_desc(hostnqn);
	if (rport_id == NULL)
		return (EINVAL);

	ss = stmf_alloc(STMF_STRUCT_SCSI_SESSION, 0, 0);
	if (ss == NULL) {
		nvmft_free_scsi_devid_desc(rport_id);
		return (ENOMEM);
	}

	/* With no ss_rport, STMF builds the transport ID from ss_rport_id. */
	ss->ss_rport_id = rport_id;
	ss->ss_lport = np->np_lport;

	if (stmf_register_scsi_session(np->np_lport, ss) != STMF_SUCCESS) {
		nvmft_free_scsi_devid_desc(rport_id);
		stmf_free(ss);
		return (EIO);
	}

	ctrlr->ctrlr_session = ss;
	return (0);
}

/* The association has no tasks left. */
void
nvmft_session_deregister(nvmft_controller_t *ctrlr)
{
	stmf_scsi_session_t *ss = ctrlr->ctrlr_session;

	if (ss == NULL)
		return;

	stmf_deregister_scsi_session(ctrlr->ctrlr_np->np_lport, ss);
	nvmft_free_scsi_devid_desc(ss->ss_rport_id);
	stmf_free(ss);
	ctrlr->ctrlr_session = NULL;
}

/* Tell STMF that the port is offline: its last controller is gone. */
void
nvmft_port_offline_complete(nvmft_port_t *np)
{
	stmf_change_status_t cstatus;

	cstatus.st_completion_status = STMF_SUCCESS;
	cstatus.st_additional_info = NULL;
	if (stmf_ctl(STMF_CMD_LPORT_OFFLINE_COMPLETE, np->np_lport,
	    &cstatus) != STMF_SUCCESS) {
		/* No acknowledgement follows; do not leave a waiter. */
		mutex_enter(&np->np_lock);
		np->np_offline = B_TRUE;
		cv_broadcast(&np->np_cv);
		mutex_exit(&np->np_lock);
	}
}

/*
 * STMF calls this on its one service thread, so an offline request does not
 * wait here for the controllers: nvmft_controller_terminate() completes it
 * when the last of them is gone.
 */
static void
nvmft_lport_ctl(stmf_local_port_t *lport, int cmd, void *arg)
{
	nvmft_port_t *np = lport->lport_port_private;
	stmf_change_status_t cstatus;
	nvmft_controller_t *ctrlr;
	boolean_t done;

	_NOTE(ARGUNUSED(arg));

	switch (cmd) {
	case STMF_CMD_LPORT_ONLINE:
		mutex_enter(&np->np_lock);
		np->np_online = B_TRUE;
		np->np_offline = B_FALSE;
		mutex_exit(&np->np_lock);
		cstatus.st_completion_status = STMF_SUCCESS;
		cstatus.st_additional_info = NULL;
		(void) stmf_ctl(STMF_CMD_LPORT_ONLINE_COMPLETE, lport,
		    &cstatus);
		break;
	case STMF_CMD_LPORT_OFFLINE:
		mutex_enter(&np->np_lock);
		np->np_online = B_FALSE;
		for (ctrlr = list_head(&np->np_controllers); ctrlr != NULL;
		    ctrlr = list_next(&np->np_controllers, ctrlr))
			nvmft_controller_offline(ctrlr);
		done = list_is_empty(&np->np_controllers);
		np->np_offlining = !done;
		mutex_exit(&np->np_lock);
		if (done)
			nvmft_port_offline_complete(np);
		break;
	case STMF_ACK_LPORT_OFFLINE_COMPLETE:
		mutex_enter(&np->np_lock);
		np->np_offline = B_TRUE;
		cv_broadcast(&np->np_cv);
		mutex_exit(&np->np_lock);
		break;
	default:
		break;
	}
}

static stmf_status_t
nvmft_lport_info(uint32_t cmd, stmf_local_port_t *lport, void *arg,
    uint8_t *buf, uint32_t *bufsizep)
{
	_NOTE(ARGUNUSED(cmd, lport, arg, buf, bufsizep));
	return (STMF_SUCCESS);
}

static void
nvmft_lport_event_handler(stmf_local_port_t *lport, int eventid, void *arg,
    uint32_t flags)
{
	_NOTE(ARGUNUSED(lport, eventid, arg, flags));
}

/*
 * A SCSI device ID descriptor that names a port by its NQN.  SCSI has no
 * protocol identifier for NVMe over Fabrics.  Returns NULL unless the NQN has
 * a valid length, which also fits ident_length.
 */
static scsi_devid_desc_t *
nvmft_alloc_scsi_devid_desc(const char *nqn)
{
	scsi_devid_desc_t *sdd;
	size_t nqnlen = strlen(nqn);

	if (nqnlen == 0 || nqnlen > NVMF_NQN_MAX_LEN)
		return (NULL);
	sdd = kmem_zalloc(sizeof (*sdd) + nqnlen, KM_SLEEP);
	sdd->protocol_id = PROTOCOL_ANY;
	sdd->code_set = CODE_SET_ASCII;
	sdd->ident_type = ID_TYPE_SCSI_NAME_STRING;
	sdd->ident_length = (uint8_t)nqnlen;
	(void) memcpy(sdd->ident, nqn, nqnlen);
	return (sdd);
}

static void
nvmft_free_scsi_devid_desc(scsi_devid_desc_t *sdd)
{
	kmem_free(sdd, sizeof (*sdd) + sdd->ident_length);
}

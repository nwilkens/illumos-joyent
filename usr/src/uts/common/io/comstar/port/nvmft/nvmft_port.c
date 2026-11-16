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
 * Allocate and register an stmf_local_port_t for a subsystem (SubNQN).
 *
 * PORT-TODO (FreeBSD ctl_frontend_nvmf.c:nvmft_port_create): the parameter
 * parsing (subnqn/portid/serial/max_io_qsize/ioccsz/iorcsz/nn) is driven by a
 * ctl_req nvlist in FreeBSD.  On illumos the equivalent comes from the STMF
 * provider configuration ioctl path (stmfadm / libstmf), delivered through the
 * pp_cb STMF_PROVIDER_DATA_UPDATED callback in nvmft.c.  This function takes the
 * already-parsed values; wire the nvlist plumbing in nvmft.c.
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
	np->np_online = B_FALSE;
	np->np_refs = 1;

	mutex_init(&np->np_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&np->np_controllers_cv, NULL, CV_DRIVER, NULL);
	np->np_ids = id_space_create("nvmft_cntlid", 0,
	    NVMF_CNTLID_STATIC_MAX);
	list_create(&np->np_controllers, sizeof (nvmft_controller_t),
	    offsetof(nvmft_controller_t, ctrlr_link));

	/* The cntlid is set per-controller; pass 0 for the template. */
	_nvmf_init_io_controller_data(0, max_io_qsize, serial, utsname.sysname,
	    utsname.version, subnqn, nn, ioccsz, iorcsz, &np->np_cdata);

	/*
	 * PORT-TODO (FreeBSD nvmft_port_create): set cdata.aerl = NVMFT_NUM_AER-1,
	 * cdata.oaes (NS_ATTRIBUTE), cdata.oncs (WRZERO|DSM once backed by
	 * WRITE SAME / UNMAP translations), cdata.fuses (CNW), and the
	 * firmware-slot page np_fp, using the nvme_identify_ctrl_t field names.
	 * Also advertise CMIC/ANACAP here for ANA (NVMEOF.md 9.4 /
	 * nvmft_ana.c:nvmft_ana_init_identify).
	 *
	 * Do NOT advertise the ONCS COMPARE bit (id_oncs.on_nvmcpys): stmf_sbd
	 * has no read-and-compare path, so nvmft_translate_cmd() rejects
	 * NVME_OPC_NVM_COMPARE.  The bit is left clear (cdata is bzero'd in
	 * _nvmf_init_io_controller_data) so hosts will not issue COMPARE.
	 */

	np->np_devid = nvmft_alloc_scsi_devid_desc(subnqn);
	lport->lport_id = np->np_devid;
	lport->lport_pp = nvmft_global->ns_pp;
	/*
	 * STMF requires a dbuf store; stmf_alloc_dbuf() unconditionally calls
	 * ds_alloc_data_buf.  For the TCP transport this is a kmem copy buffer.
	 * (NVMEOF.md section 8: an RDMA transport would back this with
	 * registered memory; the store seam is identical.)
	 */
	lport->lport_ds = nvmft_dbuf_store_create();
	if (lport->lport_ds == NULL) {
		nvmft_free_scsi_devid_desc(np->np_devid);
		np->np_devid = NULL;
		id_space_destroy(np->np_ids);
		np->np_ids = NULL;
		cv_destroy(&np->np_controllers_cv);
		mutex_destroy(&np->np_lock);
		list_destroy(&np->np_controllers);
		stmf_free(lport);
		return (NULL);
	}
	lport->lport_xfer_data = nvmft_lport_xfer_data;
	lport->lport_send_status = nvmft_lport_send_status;
	lport->lport_task_free = nvmft_lport_task_free;
	lport->lport_abort = nvmft_lport_abort;
	lport->lport_abort_timeout = 300;	/* 5 minutes */
	lport->lport_task_poll = nvmft_lport_task_poll;
	lport->lport_ctl = nvmft_lport_ctl;
	lport->lport_info = nvmft_lport_info;
	lport->lport_event_handler = nvmft_lport_event_handler;

	/* Participate in ALUA so ANA state can be coordinated (NVMEOF.md 9.2). */
	stmf_set_port_alua(lport);

	if (stmf_register_local_port(lport) != STMF_SUCCESS) {
		nvmft_port_free(np);
		return (NULL);
	}

	mutex_enter(&nvmft_global->ns_lock);
	list_insert_tail(&nvmft_global->ns_ports, np);
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
	if (np->np_ids != NULL)
		id_space_destroy(np->np_ids);
	if (np->np_active_ns != NULL)
		kmem_free(np->np_active_ns,
		    np->np_num_ns * sizeof (uint32_t));
	list_destroy(&np->np_controllers);
	cv_destroy(&np->np_controllers_cv);
	mutex_destroy(&np->np_lock);
	/* stmf_free() releases the lport and the embedded nvmft_port. */
	stmf_free(np->np_lport);
}

/*
 * Register a per-controller STMF SCSI session for one host association.
 *
 * stmf_task_alloc() dereferences task->task_session->ss_stmf_private, and
 * sbd_pgr keys SCSI-3 persistent reservations off task_session->ss_rport, so
 * every association needs a registered stmf_scsi_session_t before any command
 * is dispatched.  FreeBSD's CTL frontend created this I_T nexus implicitly; on
 * STMF we create it explicitly here, once per controller (Fabrics association),
 * modeled on srpt_stp_alloc_session().
 *
 * The remote-port identity is the host NQN as a SCSI name string.  We leave
 * ss_rport NULL so stmf_register_scsi_session() builds the transport id from
 * ss_rport_id via stmf_scsilib_devid_to_remote_port() (the default-tptid path,
 * since there is no NVMe SCSI protocol identifier).  Registration requires the
 * local port to be online, which the admin-queue handoff has already confirmed.
 */
int
nvmft_session_register(nvmft_controller_t *ctrlr)
{
	nvmft_port_t *np = ctrlr->ctrlr_np;
	stmf_scsi_session_t *ss;
	scsi_devid_desc_t *rport_id;
	char hostnqn[NVMF_NQN_FIELD_SIZE + 1];

	/*
	 * nfcd_hostnqn is a fixed 256-byte, NUL-padded on-wire field that is not
	 * guaranteed terminated; copy the field and terminate it explicitly
	 * before strlen() in nvmft_alloc_scsi_devid_desc().
	 */
	(void) memcpy(hostnqn, ctrlr->ctrlr_hostnqn, NVMF_NQN_FIELD_SIZE);
	hostnqn[NVMF_NQN_FIELD_SIZE] = '\0';

	ss = stmf_alloc(STMF_STRUCT_SCSI_SESSION, 0, 0);
	if (ss == NULL)
		return (ENOMEM);

	rport_id = nvmft_alloc_scsi_devid_desc(hostnqn);
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

/*
 * Tear down the controller's STMF session.  stmf_deregister_scsi_session()
 * aborts the nexus's outstanding tasks and frees the transport id it built
 * (ISS_NULL_TPTID); we free the devid we supplied and the session itself.
 */
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


/*
 * lport control callback: STMF online/offline transitions.  Modeled on
 * srpt_stp_ctl().
 */
static void
nvmft_lport_ctl(stmf_local_port_t *lport, int cmd, void *arg)
{
	nvmft_port_t *np = lport->lport_port_private;
	stmf_change_status_t cstatus;

	cstatus.st_completion_status = STMF_SUCCESS;
	cstatus.st_additional_info = NULL;

	switch (cmd) {
	case STMF_CMD_LPORT_ONLINE:
		mutex_enter(&np->np_lock);
		np->np_online = B_TRUE;
		mutex_exit(&np->np_lock);
		(void) stmf_ctl(STMF_CMD_LPORT_ONLINE_COMPLETE, lport,
		    &cstatus);
		break;
	case STMF_CMD_LPORT_OFFLINE: {
		nvmft_controller_t *ctrlr;

		/*
		 * Mirror FreeBSD nvmft_offline(): mark the port offline, fault
		 * every controller so its association terminates, and wait for
		 * the controller list to drain before reporting offline complete.
		 * Otherwise controllers and their in-flight tasks could outlive
		 * the offline and race port teardown.
		 *
		 * nvmft_controller_error() takes ctrlr_lock (the np_lock ->
		 * ctrlr_lock order is preserved here) and schedules an
		 * asynchronous terminate; the terminate path removes the
		 * controller under np_lock and broadcasts np_controllers_cv when
		 * the list empties while offline.  cv_wait() drops np_lock so
		 * those drainers can make progress.
		 */
		mutex_enter(&np->np_lock);
		np->np_online = B_FALSE;
		for (ctrlr = list_head(&np->np_controllers); ctrlr != NULL;
		    ctrlr = list_next(&np->np_controllers, ctrlr)) {
			(void) nvmft_printf(ctrlr,
			    "shutting down due to port going offline\n");
			nvmft_controller_error(ctrlr, NULL, ENODEV);
		}
		while (!list_is_empty(&np->np_controllers))
			cv_wait(&np->np_controllers_cv, &np->np_lock);
		mutex_exit(&np->np_lock);
		(void) stmf_ctl(STMF_CMD_LPORT_OFFLINE_COMPLETE, lport,
		    &cstatus);
		break;
	}
	case STMF_ACK_LPORT_ONLINE_COMPLETE:
	case STMF_ACK_LPORT_OFFLINE_COMPLETE:
		break;
	default:
		NVMFT_DPRINTF_L2("nvmft_lport_ctl: cmd %d not handled", cmd);
		break;
	}
}

/* ARGSUSED */
static stmf_status_t
nvmft_lport_info(uint32_t cmd, stmf_local_port_t *lport, void *arg,
    uint8_t *buf, uint32_t *bufsizep)
{
	_NOTE(ARGUNUSED(cmd, lport, arg, buf, bufsizep));
	return (STMF_SUCCESS);
}

/* ARGSUSED */
static void
nvmft_lport_event_handler(stmf_local_port_t *lport, int eventid, void *arg,
    uint32_t flags)
{
	/*
	 * PORT-TODO (NVMEOF.md 9.2/9.4): STMF fires LU access-state related
	 * events here.  Consume them as a coarse input to ANA group state
	 * (standby LU -> Inaccessible/Non-Optimized) via nvmft_ana.c.
	 */
	_NOTE(ARGUNUSED(lport, eventid, arg, flags));
}

/*
 * Build a SCSI device id descriptor from the SubNQN so the port has a stable
 * STMF identity.  Modeled on srpt_stp_alloc_scsi_devid_desc(), which used an
 * EUI-64; here we use the NQN as a T10/SCSI name string.
 */
static scsi_devid_desc_t *
nvmft_alloc_scsi_devid_desc(const char *nqn)
{
	scsi_devid_desc_t *sdd;
	size_t nqnlen, total;

	nqnlen = strlen(nqn);
	total = sizeof (scsi_devid_desc_t) - 1 + nqnlen + 1;
	sdd = kmem_zalloc(total, KM_SLEEP);
	/*
	 * STMF/SCSI has no protocol identifier for NVMe-oF.  PROTOCOL_ANY (15)
	 * is the SPC "no specific protocol" wildcard, but it must NOT be used as
	 * a concrete port protocol_id: STMF's stmf_create_kstat_lport() indexes
	 * protocol_ident[protocol_id], that table is sized [PROTOCOL_ANY], and
	 * its only guard is "> PROTOCOL_ANY" -- so protocol_id == PROTOCOL_ANY
	 * reads one element past the table end and panics on the resulting
	 * garbage string pointer.  Use the highest in-bounds slot, which STMF
	 * renders as "UNKNOWN", rather than mislabel the port as a real SCSI
	 * transport (iSCSI/SRP/...).
	 */
	sdd->protocol_id = PROTOCOL_ANY - 1;
	sdd->code_set = CODE_SET_ASCII;
	sdd->ident_type = ID_TYPE_SCSI_NAME_STRING;
	sdd->ident_length = (uint8_t)nqnlen;
	(void) memcpy(sdd->ident, nqn, nqnlen);
	return (sdd);
}

static void
nvmft_free_scsi_devid_desc(scsi_devid_desc_t *sdd)
{
	size_t total;

	total = sizeof (scsi_devid_desc_t) - 1 + sdd->ident_length + 1;
	kmem_free(sdd, total);
}

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
 * NEW file.  Replaces FreeBSD sys/dev/nvmf/controller/ctl_frontend_nvmf.c.
 *
 * This is the COMSTAR/STMF binding for the NVMe-over-Fabrics controller.  Where
 * FreeBSD's ctl_frontend_nvmf.c bound the controller protocol layer to CTL
 * (union ctl_io, ctl_run, ctl_datamove, struct ctl_port), this file binds it to
 * illumos STMF:
 *
 *   FreeBSD CTL                              illumos STMF
 *   -----------                              ------------
 *   struct ctl_port (registered via          stmf_local_port_t (registered via
 *     ctl_port_register)                       stmf_register_local_port)
 *   ctl_frontend.init/shutdown               stmf_port_provider_t pp_cb
 *   union ctl_io / ctl_alloc_io / ctl_run    scsi_task_t / stmf_task_alloc /
 *                                              stmf_post_task
 *   io->nvmeio.cmd (raw NVMe SQE)            translate NVMe SQE -> SCSI CDB on
 *                                              the scsi_task_t (this file)
 *   fe_datamove / ctl_datamove               lport_xfer_data + dbuf store
 *   nvmf_receive/send_controller_data        same transport API, fed from dbufs
 *   port.lun_enable/lun_disable              STMF LU bind via the standard
 *                                              port-provider session/LU model
 *
 * The NVMe->SCSI command translation that FreeBSD performs inside CTL is done
 * here against the STMF scsi_task_t so the existing stmf_sbd LU (over a zvol)
 * executes the I/O and enforces SCSI-3 persistent reservations (see
 * nvmft_resv.c and NVMEOF.md sections 7.2 / 7.4).
 *
 * The per-IO control flow modeled on srpt_stp.c is:
 *   capsule received -> nvmft_dispatch_command()
 *     -> stmf_task_alloc() + translate NVMe cmd to CDB + set TF flags/xfer len
 *     -> stmf_post_task()
 *   LU calls stmf_xfer_data() -> our lport_xfer_data() moves dbuf data as
 *     C2H/H2C via nvmf_send_controller_data/nvmf_receive_controller_data
 *   LU calls stmf_send_scsi_status() -> our lport_send_status() emits the
 *     NVMe completion capsule
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/kmem.h>
#include <sys/list.h>
#include <sys/taskq.h>
#include <sys/id_space.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/stream.h>
#include <sys/scsi/scsi.h>
#include <sys/scsi/generic/commands.h>
#include <sys/scsi/generic/status.h>
#include <sys/scsi/generic/sense.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include <sys/time.h>		/* hrtime_t (sys/stmf.h needs it) */
#include <sys/stmf.h>
#include <sys/lpif.h>		/* stmf_lu_t (referenced by stmf_impl.h) */
#include <sys/stmf_ioctl.h>
#include <sys/portif.h>
/*
 * The session LU map (which STMF LUNs are visible to a controller) is the
 * source of truth for the namespace list and per-namespace identity.  STMF
 * exposes it only through these in-tree headers, the same way dlun0 reads it to
 * answer SCSI REPORT LUNS.  (stmf_i_scsi_session_t::iss_sm / iss_lockp,
 * stmf_lun_map_ent_t, stmf_get_ent_from_map().)
 */
#include "../../stmf/stmf_impl.h"
#include "../../stmf/lun_map.h"

#include "nvmft_var.h"
/*
 * nvmf_memdesc_t (the controller-side receive-buffer descriptor consumed by
 * nvmf_receive_controller_data()) lives in the transport-internal header, which
 * the in-tree consumers (the bd(9)-backed host and this STMF controller)
 * include directly.  (FreeBSD passed a "struct memdesc" here.)
 */
#include "../../../nvmf/nvmf_transport_internal.h"

/*
 * Per-task scratch carried in scsi_task_t->task_port_private.  Ties an STMF task
 * back to the Fabrics capsule and qpair it was created from, so the data and
 * status phases can drive the transport.  (FreeBSD stashed nc/qp in
 * io->io_hdr.ctl_private[CTL_PRIV_FRONTEND].)
 */
typedef struct nvmft_task_priv {
	struct nvmf_capsule	*ntp_nc;
	struct nvmft_qpair	*ntp_qp;
	boolean_t		ntp_success_sent;
	/*
	 * Non-NULL for driver-issued (internal) tasks that capture their read
	 * data into a local buffer instead of sending it to a host -- used to
	 * query LU geometry via READ CAPACITY(16) for Identify Namespace.
	 */
	struct nvmft_internal_io *ntp_iio;
} nvmft_task_priv_t;

/*
 * Completion rendezvous for an internal (driver-issued) STMF task.  The issuing
 * thread blocks on iio_cv; the data phase copies into iio_buf and the status
 * phase (or a phase-collapsed final data buffer) sets iio_done and signals.
 */
typedef struct nvmft_internal_io {
	kmutex_t	iio_lock;
	kcondvar_t	iio_cv;
	boolean_t	iio_done;
	uint8_t		iio_scsi_status;
	uint8_t		*iio_buf;
	uint32_t	iio_buflen;
	uint32_t	iio_xfered;
} nvmft_internal_io_t;

/* lport ops (modeled on srpt_stp.c). */
static stmf_status_t nvmft_lport_xfer_data(scsi_task_t *task,
    stmf_data_buf_t *dbuf, uint32_t ioflags);
static stmf_status_t nvmft_lport_send_status(scsi_task_t *task,
    uint32_t ioflags);
static void nvmft_lport_task_free(scsi_task_t *task);
static stmf_status_t nvmft_lport_abort(stmf_local_port_t *lport, int abort_cmd,
    void *arg, uint32_t flags);
static void nvmft_lport_task_poll(scsi_task_t *task);
static void nvmft_lport_ctl(stmf_local_port_t *lport, int cmd, void *arg);
static stmf_status_t nvmft_lport_info(uint32_t cmd, stmf_local_port_t *lport,
    void *arg, uint8_t *buf, uint32_t *bufsizep);
static void nvmft_lport_event_handler(stmf_local_port_t *lport, int eventid,
    void *arg, uint32_t flags);

static scsi_devid_desc_t *nvmft_alloc_scsi_devid_desc(const char *nqn);
static void nvmft_free_scsi_devid_desc(scsi_devid_desc_t *sdd);

/* dbuf store: a kmem-backed copy buffer used by the TCP transport. */
static stmf_data_buf_t *nvmft_dbuf_alloc(scsi_task_t *task, uint32_t size,
    uint32_t *pminsize, uint32_t flags);
static void nvmft_dbuf_free(stmf_dbuf_store_t *ds, stmf_data_buf_t *dbuf);
static stmf_dbuf_store_t *nvmft_dbuf_store_create(void);
static void nvmft_dbuf_store_destroy(stmf_dbuf_store_t *ds);

/*
 * Per-transfer state for an in-flight WRITE (H2C) datamove.  The transport
 * receive callback runs asynchronously and uses this to complete the dbuf.
 * nx_mp is the mblk chain (if any) wrapping the dbuf sglist, freed on
 * completion.
 */
typedef struct nvmft_xfer {
	scsi_task_t	*nx_task;
	stmf_data_buf_t	*nx_dbuf;
	mblk_t		*nx_mp;
} nvmft_xfer_t;

/*
 * ============================================================================
 * Port lifecycle (replaces nvmft_port_create / nvmft_port_remove)
 * ============================================================================
 */

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
 * ============================================================================
 * Namespace presentation (active list, Identify Namespace, NS descriptor)
 * ============================================================================
 *
 * A namespace (NSID) is the STMF LUN (NSID - 1) in the controller's session LU
 * map.  The map (populated by stmfadm add-view) is the source of truth for both
 * the active-namespace list (Identify CNS 2) and per-namespace identity
 * (CNS 0 / CNS 3); we read it under the session's ilport_lock, exactly as the
 * STMF dlun0 REPORT LUNS path does.
 *
 * Block geometry (namespace size / LBA size) is not exposed by stmf_lu_t, so it
 * is obtained by issuing an internal READ CAPACITY(16) to the LU through the
 * normal STMF task path.  This is provider-agnostic (any STMF LU, not only
 * sbd).  The internal task captures its read data into a local buffer rather
 * than sending it to a host; see ntp_iio handling in nvmft_lport_xfer_data(),
 * nvmft_lport_send_status() and nvmft_lport_task_free().
 */

/*
 * Look up the controller session's LU map entry for nsid (LUN nsid-1).  Returns
 * B_TRUE if a LU is mapped, optionally copying its 16-byte identity (the LU
 * GUID) for the namespace identification descriptor.  Read under iss_lockp.
 */
static boolean_t
nvmft_ns_mapped(nvmft_controller_t *ctrlr, uint32_t nsid, uint8_t *guid)
{
	stmf_scsi_session_t *ss = ctrlr->ctrlr_session;
	stmf_i_scsi_session_t *iss;
	stmf_lun_map_ent_t *ent;
	uint32_t lun_id;
	boolean_t mapped = B_FALSE;

	if (ss == NULL || nsid == 0)
		return (B_FALSE);
	lun_id = nsid - 1;
	if (lun_id > 0x3fff)		/* STMF single-level LUN limit */
		return (B_FALSE);

	iss = (stmf_i_scsi_session_t *)ss->ss_stmf_private;
	rw_enter(iss->iss_lockp, RW_READER);
	/*
	 * iss_sm is NULL during the session create/teardown window (stmf clears
	 * it under iss_lockp); stmf_get_ent_from_map() would deref it.  Guard as
	 * nvmft_build_active_nslist() does.
	 */
	if (iss->iss_sm != NULL) {
		ent = (stmf_lun_map_ent_t *)stmf_get_ent_from_map(iss->iss_sm,
		    (uint16_t)lun_id);
		if (ent != NULL && ent->ent_lu != NULL) {
			mapped = B_TRUE;
			if (guid != NULL) {
				scsi_devid_desc_t *id = ent->ent_lu->lu_id;

				(void) bzero(guid, 16);
				if (id != NULL)
					(void) memcpy(guid, id->ident,
					    MIN(id->ident_length, 16));
			}
		}
	}
	rw_exit(iss->iss_lockp);
	return (mapped);
}

/*
 * Issue an internal READ CAPACITY(16) to the LU backing nsid and return its
 * block count and block size.  Blocks until the LU completes the command on an
 * STMF worker thread (independent of this thread), so it must run in thread
 * context (it is called from the admin-command handler).
 */
static int
nvmft_lu_read_capacity(nvmft_controller_t *ctrlr, uint32_t nsid,
    uint64_t *nblocksp, uint32_t *blksizep)
{
	scsi_task_t *task;
	nvmft_task_priv_t *priv;
	nvmft_internal_io_t iio;
	uint8_t capbuf[32];
	uint8_t lun[8];
	uint32_t lun_id = nsid - 1;
	int rc;

	if (ctrlr->ctrlr_session == NULL)
		return (ENXIO);

	(void) bzero(lun, sizeof (lun));
	lun[0] = (uint8_t)((lun_id >> 8) & 0x3f);
	lun[1] = (uint8_t)(lun_id & 0xff);

	task = stmf_task_alloc(ctrlr->ctrlr_np->np_lport, ctrlr->ctrlr_session,
	    lun, 16, 0);
	if (task == NULL)
		return (ENOMEM);

	(void) bzero(&iio, sizeof (iio));
	mutex_init(&iio.iio_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&iio.iio_cv, NULL, CV_DRIVER, NULL);
	iio.iio_buf = capbuf;
	iio.iio_buflen = sizeof (capbuf);

	priv = kmem_zalloc(sizeof (*priv), KM_SLEEP);
	priv->ntp_iio = &iio;
	task->task_port_private = priv;
	task->task_flags |= TF_READ_DATA | TF_ATTR_SIMPLE_QUEUE;

	(void) bzero(task->task_cdb, task->task_cdb_length);
	task->task_cdb[0] = SCMD_SVC_ACTION_IN_G4;
	task->task_cdb[1] = SSVC_ACTION_READ_CAPACITY_G4;
	task->task_cdb[13] = sizeof (capbuf);	/* allocation length (bytes 10-13) */
	task->task_expected_xfer_length = sizeof (capbuf);
	task->task_cmd_xfer_length = sizeof (capbuf);
	task->task_max_nbufs = 1;
	task->task_max_xfer_len = sizeof (capbuf);
	task->task_1st_xfer_len = sizeof (capbuf);

	stmf_post_task(task, NULL);

	mutex_enter(&iio.iio_lock);
	while (!iio.iio_done)
		cv_wait(&iio.iio_cv, &iio.iio_lock);
	mutex_exit(&iio.iio_lock);

	/* READ CAPACITY(16): max LBA at bytes 0-7, block length at bytes 8-11. */
	if (iio.iio_scsi_status != STATUS_GOOD || iio.iio_xfered < 12) {
		rc = EIO;
	} else {
		*nblocksp = BE_IN64(&capbuf[0]) + 1;
		*blksizep = BE_IN32(&capbuf[8]);
		rc = 0;
	}

	mutex_destroy(&iio.iio_lock);
	cv_destroy(&iio.iio_cv);
	return (rc);
}

/*
 * Build the active-namespace list (Identify CNS 2): every mapped LUN whose NSID
 * exceeds start_nsid, in increasing order.  (Replaces the np_active_ns scan,
 * which was never populated.)
 */
void
nvmft_build_active_nslist(nvmft_controller_t *ctrlr, uint32_t start_nsid,
    nvme_identify_nsid_list_t *nslist)
{
	stmf_scsi_session_t *ss = ctrlr->ctrlr_session;
	stmf_i_scsi_session_t *iss;
	uint_t nitems, count = 0;
	uint16_t i;

	nitems = sizeof (nslist->nl_nsid) / sizeof (nslist->nl_nsid[0]);
	if (ss == NULL)
		return;
	iss = (stmf_i_scsi_session_t *)ss->ss_stmf_private;

	rw_enter(iss->iss_lockp, RW_READER);
	if (iss->iss_sm != NULL) {
		for (i = 0; i < iss->iss_sm->lm_nentries && count < nitems; i++) {
			stmf_lun_map_ent_t *ent =
			    (stmf_lun_map_ent_t *)iss->iss_sm->lm_plus[i];
			uint32_t nsid = (uint32_t)i + 1;

			if (ent == NULL || ent->ent_lu == NULL)
				continue;
			if (nsid <= start_nsid)
				continue;
			nslist->nl_nsid[count++] = LE_32(nsid);
		}
	}
	rw_exit(iss->iss_lockp);
}

/*
 * Build Identify Namespace (CNS 0) for nsid from the LU geometry.  Returns
 * B_FALSE if nsid is not a mapped/active namespace.
 */
boolean_t
nvmft_build_identify_nsid(nvmft_controller_t *ctrlr, uint32_t nsid,
    nvme_identify_nsid_t *nsdata)
{
	uint64_t nblocks;
	uint32_t blksize;

	if (!nvmft_ns_mapped(ctrlr, nsid, NULL))
		return (B_FALSE);
	if (nvmft_lu_read_capacity(ctrlr, nsid, &nblocks, &blksize) != 0)
		return (B_FALSE);
	if (blksize == 0 || (blksize & (blksize - 1)) != 0)
		return (B_FALSE);		/* expect a power-of-two LBA size */

	(void) bzero(nsdata, sizeof (*nsdata));
	nsdata->id_nsize = LE_64(nblocks);
	nsdata->id_ncap = LE_64(nblocks);
	nsdata->id_nuse = LE_64(nblocks);
	nsdata->id_nlbaf = 0;			/* a single LBA format */
	nsdata->id_flbas.lba_format = 0;	/* format 0 in use */
	nsdata->id_lbaf[0].lbaf_lbads = (uint8_t)(highbit(blksize) - 1);
	return (B_TRUE);
}

/*
 * Build the Namespace Identification Descriptor list (CNS 3) for nsid: a single
 * NGUID descriptor from the LU identity, terminated by a zero-length
 * descriptor.  buf is the (zeroed) 4096-byte Identify payload.
 */
boolean_t
nvmft_build_nsid_desc(nvmft_controller_t *ctrlr, uint32_t nsid, uint8_t *buf,
    size_t buflen)
{
	uint8_t guid[16];

	/* NIDT(1) + NIDL(1) + rsvd(2) + NGUID(16), then a zero terminator. */
	if (buflen < 4 + 16 + 1)
		return (B_FALSE);
	if (!nvmft_ns_mapped(ctrlr, nsid, guid))
		return (B_FALSE);

	buf[0] = 0x02;		/* NIDT = NGUID */
	buf[1] = 16;		/* NIDL */
	(void) memcpy(&buf[4], guid, 16);
	/* buf[20] (next NIDT) stays 0: end-of-list terminator. */
	return (B_TRUE);
}

/*
 * ============================================================================
 * Active namespace bookkeeping (ported from ctl_frontend_nvmf.c)
 * ============================================================================
 */

void
nvmft_populate_active_nslist(nvmft_port_t *np, uint32_t nsid,
    nvme_identify_nsid_list_t *nslist)
{
	uint_t i, count, nitems;

	nitems = sizeof (nslist->nl_nsid) / sizeof (nslist->nl_nsid[0]);
	mutex_enter(&np->np_lock);
	count = 0;
	for (i = 0; i < np->np_num_ns; i++) {
		if (np->np_active_ns[i] <= nsid)
			continue;
		nslist->nl_nsid[count] = LE_32(np->np_active_ns[i]);
		count++;
		if (count == nitems)
			break;
	}
	mutex_exit(&np->np_lock);
}

/*
 * ============================================================================
 * NVMe command -> scsi_task_t translation + dispatch
 * ============================================================================
 */

/*
 * Encode an unsigned integer big-endian into a CDB field.
 */
static void
nvmft_cdb_put64(uint8_t *p, uint64_t v)
{
	p[0] = (uint8_t)(v >> 56);
	p[1] = (uint8_t)(v >> 48);
	p[2] = (uint8_t)(v >> 40);
	p[3] = (uint8_t)(v >> 32);
	p[4] = (uint8_t)(v >> 24);
	p[5] = (uint8_t)(v >> 16);
	p[6] = (uint8_t)(v >> 8);
	p[7] = (uint8_t)v;
}

static void
nvmft_cdb_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/*
 * Translate an NVMe NVM command into a SCSI CDB on the supplied scsi_task_t and
 * set the data-direction/transfer-length task fields.  The translated I/O is
 * executed by the STMF LU (stmf_sbd over a zvol) which also enforces SCSI-3
 * persistent reservations (NVMEOF.md 7.2 / 7.4).
 *
 * READ/WRITE map to the 16-byte CDB variants so the 64-bit NVMe SLBA fits.
 * NVMe carries SLBA in cdw10 (low) / cdw11 (high) and the 0's-based NLB in the
 * low 16 bits of cdw12; the SCSI transfer length is NLB + 1 logical blocks.
 * The byte transfer length comes from the capsule data length so it is
 * independent of the LU's logical block size.
 *
 * NVMe COMPARE is deliberately not translated.  The only SCSI analogue is
 * VERIFY(16)/BYTCHK=1, which stmf_sbd does not implement: SCMD_VERIFY_G4 is not
 * recognised by sbd_handle_cmd() (it terminates as INVALID OPCODE) and even the
 * VERIFY(10) path it does accept neither drains the requested data-out nor
 * performs a miscompare.  Advertising COMPARE without a backing read-and-compare
 * path would fail every host COMPARE, so the ONCS COMPARE bit is left clear and
 * the opcode is rejected here (-> INVALID OPCODE) until a real path exists.
 *
 * (FreeBSD performed this NVMe->SCSI mapping inside CTL's ctl_nvmeio handling;
 * on illumos STMF the mapping is done here against the scsi_task_t.)
 */
static boolean_t
nvmft_translate_cmd(scsi_task_t *task, const nvme_sqe_t *cmd,
    uint32_t data_len)
{
	uint8_t *cdb = task->task_cdb;
	uint64_t slba;
	uint32_t nlb;
	uint8_t opc = cmd->sqe_opc;

	(void) bzero(cdb, task->task_cdb_length);

	slba = (uint64_t)LE_32(cmd->sqe_cdw10) |
	    ((uint64_t)LE_32(cmd->sqe_cdw11) << 32);
	nlb = (LE_32(cmd->sqe_cdw12) & 0xffff) + 1;	/* NLB is 0's based */

	switch (opc) {
	case NVME_OPC_NVM_READ:
		cdb[0] = SCMD_READ_G4;			/* READ(16) */
		nvmft_cdb_put64(&cdb[2], slba);
		nvmft_cdb_put32(&cdb[10], nlb);
		task->task_flags |= TF_READ_DATA;
		task->task_expected_xfer_length = data_len;
		break;
	case NVME_OPC_NVM_WRITE:
		cdb[0] = SCMD_WRITE_G4;			/* WRITE(16) */
		nvmft_cdb_put64(&cdb[2], slba);
		nvmft_cdb_put32(&cdb[10], nlb);
		task->task_flags |= TF_WRITE_DATA;
		task->task_expected_xfer_length = data_len;
		break;
	case NVME_OPC_NVM_FLUSH:
		cdb[0] = SCMD_SYNCHRONIZE_CACHE;	/* SYNCHRONIZE CACHE(10) */
		task->task_expected_xfer_length = 0;
		break;
	default:
		/*
		 * PORT-TODO (NVMEOF.md R3): WRITE_ZERO -> WRITE SAME(16),
		 * DSET_MGMT -> UNMAP.  RESV_* are handled before translation by
		 * nvmft_resv_dispatch().
		 */
		return (B_FALSE);
	}

	if (task->task_expected_xfer_length == 0)
		task->task_additional_flags |= TASK_AF_NO_EXPECTED_XFER_LENGTH;
	task->task_cmd_xfer_length = task->task_expected_xfer_length;

	return (B_TRUE);
}

/*
 * Whether nvmft_translate_cmd() can map this opcode to a SCSI CDB.  Must stay in
 * sync with the switch above: it gates task allocation so we never create an
 * STMF task we cannot post (see nvmft_dispatch_command).
 */
static boolean_t
nvmft_cmd_translatable(uint8_t opc)
{
	switch (opc) {
	case NVME_OPC_NVM_READ:
	case NVME_OPC_NVM_WRITE:
	case NVME_OPC_NVM_FLUSH:
		return (B_TRUE);
	default:
		return (B_FALSE);
	}
}

/*
 * Dispatch a received command capsule to the STMF LU.
 *
 * (FreeBSD: nvmft_dispatch_command -> ctl_alloc_io / ctl_run.)
 */
void
nvmft_dispatch_command(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
    boolean_t admin)
{
	nvmft_controller_t *ctrlr = nvmft_qpair_ctrlr(qp);
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);
	nvmft_port_t *np = ctrlr->ctrlr_np;
	scsi_task_t *task;
	nvmft_task_priv_t *priv;
	uint32_t data_len;
	uint8_t lun[8];

	_NOTE(ARGUNUSED(admin));

	if (cmd->sqe_nsid == LE_32(0)) {
		(void) nvmft_send_generic_error(qp, nc,
		    0x0b /* INVALID_NAMESPACE_OR_FORMAT */);
		nvmf_free_capsule(nc);
		return;
	}

	/*
	 * Defensive: the per-controller STMF session is created during the admin
	 * handoff (nvmft_session_register), so it is non-NULL for any controller
	 * that can receive commands.  Guard anyway -- stmf_task_alloc() derefs
	 * the session unconditionally, so a NULL here would panic the box.
	 */
	if (ctrlr->ctrlr_session == NULL) {
		(void) nvmft_send_generic_error(qp, nc,
		    NVME_CQE_SC_GEN_INTERNAL_ERR);
		nvmf_free_capsule(nc);
		return;
	}

	/*
	 * Reject opcodes we cannot translate BEFORE allocating an STMF task.
	 * stmf_task_alloc() links the task into the LU's ilu_tasks list and bumps
	 * its task counters; an allocated task may only be disposed through STMF's
	 * lifecycle (post -> complete/abort -> task_lu_free), never kmem_free'd.
	 * Freeing a half-registered task corrupts ilu_tasks (use-after-free) and
	 * leaks the counters so LU offline/deregister hangs.  (FreeBSD rejects
	 * these inside CTL on a pooled io; STMF has no allocated-but-never-posted
	 * free path.)
	 */
	if (!nvmft_cmd_translatable(cmd->sqe_opc)) {
		(void) nvmft_send_generic_error(qp, nc, NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		return;
	}

	mutex_enter(&ctrlr->ctrlr_lock);
	if (ctrlr->ctrlr_pending_commands == 0)
		ctrlr->ctrlr_start_busy = gethrtime();
	ctrlr->ctrlr_pending_commands++;
	mutex_exit(&ctrlr->ctrlr_lock);

	/*
	 * STMF LUN is the NVMe NSID minus one, encoded as a single-level LUN.
	 * stmf_task_alloc() decodes luNbr = lun[1] | ((lun[0] & 0x3f) << 8), so
	 * the 14-bit LUN must be split across lun[0:1] for NSIDs >= 256.
	 */
	(void) bzero(lun, sizeof (lun));
	lun[0] = (uint8_t)(((LE_32(cmd->sqe_nsid) - 1) >> 8) & 0x3f);
	lun[1] = (uint8_t)((LE_32(cmd->sqe_nsid) - 1) & 0xff);

	task = stmf_task_alloc(np->np_lport, ctrlr->ctrlr_session, lun,
	    16 /* cdb_length */, 0);
	if (task == NULL) {
		(void) nvmft_send_generic_error(qp, nc,
		    NVME_CQE_SC_GEN_INTERNAL_ERR);
		nvmf_free_capsule(nc);
		mutex_enter(&ctrlr->ctrlr_lock);
		ASSERT3U(ctrlr->ctrlr_pending_commands, >, 0);
		ctrlr->ctrlr_pending_commands--;
		if (ctrlr->ctrlr_pending_commands == 0)
			cv_signal(&ctrlr->ctrlr_pending_cv);
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}

	priv = kmem_zalloc(sizeof (*priv), KM_SLEEP);
	priv->ntp_nc = nc;
	priv->ntp_qp = qp;
	task->task_port_private = priv;
	task->task_flags |= TF_ATTR_SIMPLE_QUEUE;

	data_len = (uint32_t)nvmf_capsule_data_len(nc);

	/*
	 * The Fabrics transport consumes a command's data as one logically
	 * contiguous, in-order byte stream: nvmf_send_controller_data() (C2H)
	 * requires each chunk's offset to be sequential over the whole command
	 * and only stamps the LAST_PDU / implicit-SUCCESS flag on the chunk that
	 * ends the transfer.  FreeBSD's CTL hands the controller the entire
	 * transfer in a single memdesc; stmf_sbd, left to itself, would split a
	 * READ into up to task_max_nbufs concurrent dbufs completed out of order.
	 *
	 * Force a single in-flight dbuf so sbd issues the data sequentially at
	 * advancing db_relative_offset and marks DB_SEND_STATUS_GOOD only on the
	 * final chunk.  task_max_xfer_len / task_1st_xfer_len are pinned to the
	 * full transfer length so the LU prefers one buffer for the whole
	 * command where its own sl_max_xfer_len permits.
	 */
	task->task_max_nbufs = 1;
	task->task_max_xfer_len = data_len;
	task->task_1st_xfer_len = data_len;

	/*
	 * The opcode was already vetted by nvmft_cmd_translatable() above, so the
	 * translation cannot fail here.  VERIFY the invariant rather than
	 * kmem_free()'ing an allocated-and-linked STMF task (which would corrupt
	 * the LU task list); a posted task is disposed only through STMF.
	 */
	VERIFY(nvmft_translate_cmd(task, cmd, data_len));

	stmf_post_task(task, NULL);
}

void
nvmft_terminate_commands(nvmft_controller_t *ctrlr)
{
	/*
	 * FreeBSD issues a CTL_TASK_I_T_NEXUS_RESET to proactively abort this
	 * nexus's in-flight CTL commands so the drain below returns sooner.  STMF
	 * has no single nexus-reset primitive (only per-task stmf_abort(), which
	 * would need a per-controller task list we do not yet keep), so we rely on
	 * the slower but correct path: nvmft_qpair_shutdown() has already freed
	 * each I/O qpair, and freeing the transport qpair aborts any registered
	 * H2C receive (tcp_free_qpair -> nvmf_complete_io_request -> the dbuf is
	 * failed back to STMF), while nvmft_lport_xfer_data() fails fast once
	 * qp_qp == NULL.  Both drive every in-flight task to completion, so
	 * ctrlr_pending_commands drains without an explicit abort here.  Proactive
	 * per-task abort remains a future optimization.
	 */
	mutex_enter(&ctrlr->ctrlr_lock);
	if (ctrlr->ctrlr_pending_commands == 0)
		cv_signal(&ctrlr->ctrlr_pending_cv);
	mutex_exit(&ctrlr->ctrlr_lock);
}

/*
 * ============================================================================
 * Data movement: lport_xfer_data + handle_datamove
 * ============================================================================
 */

/*
 * Build an mblk chain holding the db_data_size bytes of dbuf's sglist, for
 * transmission to the host (C2H / READ).  The bytes are copied out of the
 * sglist buffers, mirroring FreeBSD nvmft_copy_data(); the dbuf's storage is
 * left intact and reclaimed by stmf_data_xfer_done().
 */
static mblk_t *
nvmft_dbuf_to_mblk(stmf_data_buf_t *dbuf)
{
	mblk_t *mp;
	uint8_t *dst;
	uint32_t resid = dbuf->db_data_size;
	uint16_t i;

	mp = allocb(resid, BPRI_MED);
	if (mp == NULL)
		return (NULL);
	dst = mp->b_wptr;
	for (i = 0; i < dbuf->db_sglist_length && resid != 0; i++) {
		uint32_t todo = dbuf->db_sglist[i].seg_length;

		if (todo > resid)
			todo = resid;
		bcopy(dbuf->db_sglist[i].seg_addr, dst, todo);
		dst += todo;
		resid -= todo;
	}
	mp->b_wptr = dst;
	return (mp);
}

/*
 * Free callback for the desballoc'd receive mblks that wrap a dbuf's sglist
 * segments.  No storage is owned by the mblk itself; the dbuf is freed
 * separately by STMF, so this only releases the frtn_t.
 */
static void
nvmft_rxseg_free(caddr_t arg)
{
	kmem_free(arg, sizeof (frtn_t));
}

/*
 * Wrap a dbuf's sglist as an nvmf_memdesc_t for receiving host data (H2C /
 * WRITE).  Single-segment dbufs (the common case from our store) use a flat
 * VADDR descriptor; multi-segment dbufs are wrapped as an mblk chain via
 * desballoc() so the transport writes directly into the segment buffers.
 *
 * On success *mpp holds the mblk chain to free after the transfer (NULL for the
 * VADDR case).  Returns B_FALSE on allocation failure.
 */
static boolean_t
nvmft_dbuf_to_memdesc(stmf_data_buf_t *dbuf, nvmf_memdesc_t *mem, mblk_t **mpp)
{
	mblk_t *head = NULL, *tail = NULL;
	uint32_t resid = dbuf->db_data_size;
	uint16_t i;

	*mpp = NULL;

	if (dbuf->db_sglist_length == 1) {
		mem->nmd_type = NVMF_MEMDESC_VADDR;
		mem->nmd_len = resid;
		mem->nmd_u.nmd_vaddr = dbuf->db_sglist[0].seg_addr;
		return (B_TRUE);
	}

	for (i = 0; i < dbuf->db_sglist_length && resid != 0; i++) {
		uint32_t todo = dbuf->db_sglist[i].seg_length;
		frtn_t *frtn;
		mblk_t *mp;

		if (todo > resid)
			todo = resid;
		frtn = kmem_zalloc(sizeof (frtn_t), KM_SLEEP);
		frtn->free_func = nvmft_rxseg_free;
		frtn->free_arg = (caddr_t)frtn;
		mp = desballoc(dbuf->db_sglist[i].seg_addr, todo, BPRI_MED, frtn);
		if (mp == NULL) {
			kmem_free(frtn, sizeof (frtn_t));
			freemsg(head);
			return (B_FALSE);
		}
		mp->b_wptr = mp->b_rptr + todo;
		if (head == NULL)
			head = mp;
		else
			tail->b_cont = mp;
		tail = mp;
		resid -= todo;
	}

	mem->nmd_type = NVMF_MEMDESC_MBLK;
	mem->nmd_len = dbuf->db_data_size;
	mem->nmd_u.nmd_mp = head;
	*mpp = head;
	return (B_TRUE);
}

/*
 * Transport receive completion for an H2C (WRITE) datamove.  Runs from the
 * transport (possibly before nvmf_receive_controller_data() returns).  Mirrors
 * FreeBSD nvmft_datamove_out_cb(): translate the error, free the wrapping
 * resources, and hand the dbuf back to STMF.
 */
static void
nvmft_datamove_in_cb(void *arg, size_t xfered, int error)
{
	nvmft_xfer_t *nx = arg;
	scsi_task_t *task = nx->nx_task;
	stmf_data_buf_t *dbuf = nx->nx_dbuf;

	if (nx->nx_mp != NULL)
		freemsg(nx->nx_mp);
	kmem_free(nx, sizeof (*nx));

	if (error != 0) {
		dbuf->db_xfer_status = STMF_FAILURE;
	} else {
		VERIFY3U(xfered, ==, dbuf->db_data_size);
		dbuf->db_xfer_status = STMF_SUCCESS;
	}
	stmf_data_xfer_done(task, dbuf, 0);
}

/*
 * STMF calls lport_xfer_data() to move one dbuf either to (C2H, READ) or from
 * (H2C, WRITE) the remote host.  We translate the dbuf's sglist into an mblk
 * chain / memdesc and hand it to the transport's send/receive controller-data
 * API.
 *
 * Modeled on srpt_stp_xfer_data(), but instead of posting RDMA work requests we
 * drive nvmf_send_controller_data() (C2H) or nvmf_receive_controller_data()
 * (H2C).  Over TCP this copies between the dbuf and socket mblks; over RDMA
 * (Phase 4) the same seam becomes an RDMA WRITE/READ (NVMEOF.md 7.1 / 8).
 *
 * For C2H the transport requires each chunk's db_relative_offset to be
 * sequential and contiguous over the whole command, and only folds the implicit
 * SUCCESS into the chunk that ends the transfer.  nvmft_dispatch_command() pins
 * task_max_nbufs = 1 so STMF/sbd hand us exactly one in-flight dbuf at a time at
 * an advancing offset, satisfying that contract.
 */
static stmf_status_t
nvmft_lport_xfer_data(scsi_task_t *task, stmf_data_buf_t *dbuf,
    uint32_t ioflags)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	struct nvmf_capsule *nc;
	struct nvmft_qpair *qp;
	struct nvmf_qpair *nq;
	stmf_status_t ret;
	boolean_t do_xfer_done = B_FALSE;
	uint32_t xfer_iof = 0;

	_NOTE(ARGUNUSED(ioflags));

	ASSERT((dbuf->db_flags & (DB_DIRECTION_TO_RPORT |
	    DB_DIRECTION_FROM_RPORT)) != (DB_DIRECTION_TO_RPORT |
	    DB_DIRECTION_FROM_RPORT));

	/*
	 * Internal (driver-issued) task: capture the read data into the issuing
	 * thread's buffer instead of sending it to a host.  These are always
	 * small single-buffer C2H reads (READ CAPACITY).
	 */
	if (priv->ntp_iio != NULL) {
		nvmft_internal_io_t *iio = priv->ntp_iio;

		if (dbuf->db_flags & DB_DIRECTION_TO_RPORT) {
			uint32_t off = dbuf->db_relative_offset;
			uint32_t resid = dbuf->db_data_size;
			uint16_t i;

			for (i = 0; i < dbuf->db_sglist_length && resid != 0 &&
			    off < iio->iio_buflen; i++) {
				uint32_t todo = dbuf->db_sglist[i].seg_length;

				if (todo > resid)
					todo = resid;
				if (todo > iio->iio_buflen - off)
					todo = iio->iio_buflen - off;
				bcopy(dbuf->db_sglist[i].seg_addr,
				    iio->iio_buf + off, todo);
				off += todo;
				resid -= todo;
			}
			if (off > iio->iio_xfered)
				iio->iio_xfered = off;
		}
		dbuf->db_xfer_status = STMF_SUCCESS;
		/*
		 * If the LU collapsed status into this final data buffer no
		 * send_status will follow, so complete the rendezvous here and
		 * release the task (STMF_IOF_LPORT_DONE) so the LU's
		 * stmf_task_lu_done() does not panic on a still-owned task.
		 * The non-collapsed case (the sbd READ CAPACITY short read)
		 * leaves iof=0 and is completed by send_status, which now
		 * carries STMF_IOF_LPORT_DONE.
		 */
		if (dbuf->db_flags & DB_SEND_STATUS_GOOD) {
			mutex_enter(&iio->iio_lock);
			iio->iio_scsi_status = STATUS_GOOD;
			iio->iio_done = B_TRUE;
			cv_signal(&iio->iio_cv);
			mutex_exit(&iio->iio_lock);
			stmf_data_xfer_done(task, dbuf, STMF_IOF_LPORT_DONE);
			return (STMF_SUCCESS);
		}
		stmf_data_xfer_done(task, dbuf, 0);
		return (STMF_SUCCESS);
	}

	nc = priv->ntp_nc;
	qp = priv->ntp_qp;

	/*
	 * The send/receive below dereferences the transport qpair, which a racing
	 * controller teardown frees (nvmft_qpair_shutdown -> nvmf_free_qpair).
	 * Hold a reference across the transfer (nvmft_qpair_data_hold/rele, the
	 * same handshake _nvmft_send_response() uses): if the qpair is already
	 * shut down, fail the dbuf back to STMF (iof=0, as in the synchronous
	 * failure path below) so STMF completes the task -- via abort or
	 * lport_send_status(), which finds the qpair gone -- instead of touching a
	 * freed qpair.  An H2C receive only needs the reference to span
	 * registering the transfer: its async completion (nvmft_datamove_in_cb)
	 * touches no qpair state, and freeing the qpair aborts any still-registered
	 * receive (tcp_free_qpair -> nvmf_complete_io_request), which is what lets
	 * the controller drain.
	 */
	nq = nvmft_qpair_data_hold(qp);
	if (nq == NULL) {
		dbuf->db_xfer_status = STMF_FAILURE;
		stmf_data_xfer_done(task, dbuf, 0);
		return (STMF_SUCCESS);
	}

	if (dbuf->db_flags & DB_DIRECTION_TO_RPORT) {
		/* C2H: send controller data (READ). */
		mblk_t *mp;
		uint_t status;

		mp = nvmft_dbuf_to_mblk(dbuf);
		if (mp == NULL) {
			ret = STMF_ALLOC_FAILURE;
			goto done;
		}

		status = nvmf_send_controller_data(nc,
		    dbuf->db_relative_offset, mp, dbuf->db_data_size);
		switch (status) {
		case NVMF_SUCCESS_SENT:
			/*
			 * The transport sent an implicit success CQE (e.g. the
			 * TCP SUCCESS flag) folded into the final C2H PDU; no
			 * response capsule is needed for this command.
			 */
			priv->ntp_success_sent = B_TRUE;
			nvmft_command_completed(qp, nc);
			dbuf->db_xfer_status = STMF_SUCCESS;
			/*
			 * If the LU collapsed status into this final (zero-copy)
			 * data buffer it will call stmf_task_lu_done(), which
			 * panics if the port has not already released the task.
			 * Complete with STMF_IOF_LPORT_DONE so STMF clears
			 * ITASK_KNOWN_TO_TGT_PORT before the LU finishes.
			 */
			if (dbuf->db_flags & DB_SEND_STATUS_GOOD) {
				xfer_iof = STMF_IOF_LPORT_DONE;
				do_xfer_done = B_TRUE;
				ret = STMF_SUCCESS;
				goto done;
			}
			break;
		case NVME_CQE_SC_GEN_SUCCESS:
			/*
			 * Final chunk delivered, but the transport did NOT fold
			 * an implicit success CQE -- the host negotiated SQ flow
			 * control.  When stmf_sbd collapses status into the
			 * final (zero-copy) data buffer it calls
			 * stmf_task_lu_done() rather than lport_send_status(),
			 * so the completion would never reach the host and the
			 * READ would hang.  Emit the CQE here in that case;
			 * otherwise lport_send_status() emits it.
			 */
			dbuf->db_xfer_status = STMF_SUCCESS;
			if (dbuf->db_flags & DB_SEND_STATUS_GOOD) {
				nvme_cqe_t cpl;
				const nvme_sqe_t *scmd = nvmf_capsule_sqe(nc);

				(void) bzero(&cpl, sizeof (cpl));
				cpl.cqe_cid = scmd->sqe_cid;
				cpl.cqe_sf.sf_sct = NVME_CQE_SCT_GENERIC;
				cpl.cqe_sf.sf_sc = NVME_CQE_SC_GEN_SUCCESS;
				priv->ntp_success_sent = B_TRUE;
				(void) nvmft_send_response(qp, &cpl);
				xfer_iof = STMF_IOF_LPORT_DONE;
				do_xfer_done = B_TRUE;
				ret = STMF_SUCCESS;
				goto done;
			}
			break;
		case NVMF_MORE:
			dbuf->db_xfer_status = STMF_SUCCESS;
			break;
		default:
			dbuf->db_xfer_status = STMF_FAILURE;
			break;
		}
		/*
		 * nvmf_send_controller_data() consumed the mblk chain.  The
		 * transfer is complete synchronously for the send path.
		 */
		do_xfer_done = B_TRUE;
		ret = STMF_SUCCESS;
		goto done;
	} else {
		/* H2C: receive controller data (WRITE). */
		nvmft_xfer_t *nx;
		nvmf_memdesc_t mem;
		mblk_t *mp;
		int error;

		if (!nvmft_dbuf_to_memdesc(dbuf, &mem, &mp)) {
			ret = STMF_ALLOC_FAILURE;
			goto done;
		}

		nx = kmem_zalloc(sizeof (*nx), KM_SLEEP);
		nx->nx_task = task;
		nx->nx_dbuf = dbuf;
		nx->nx_mp = mp;

		error = nvmf_receive_controller_data(nc,
		    dbuf->db_relative_offset, &mem, dbuf->db_data_size,
		    nvmft_datamove_in_cb, nx);
		if (error != 0) {
			if (mp != NULL)
				freemsg(mp);
			kmem_free(nx, sizeof (*nx));
			(void) nvmft_printf(nvmft_qpair_ctrlr(qp),
			    "Failed to request capsule data: 0x%x\n", error);
			ret = STMF_FAILURE;
			goto done;
		}
		/* Completion is asynchronous via nvmft_datamove_in_cb(). */
		ret = STMF_SUCCESS;
		goto done;
	}

done:
	/*
	 * Drop the qpair reference BEFORE completing the transfer: with
	 * STMF_IOF_LPORT_DONE, stmf_data_xfer_done() can synchronously free the
	 * task (stmf_task_free -> nvmft_lport_task_free), which drops
	 * ctrlr_pending_commands and lets the controller-shutdown drain destroy
	 * the qpair (struct nvmft_qpair).  nvmft_qpair_data_rele() touches that
	 * struct, so it must run while the task is still pending and the qpair is
	 * still alive.
	 */
	nvmft_qpair_data_rele(qp, nq);
	if (do_xfer_done)
		stmf_data_xfer_done(task, dbuf, xfer_iof);
	return (ret);
}

/*
 * Deferred datamove worker invoked from the qpair datamove taskq for tasks
 * queued by nvmft_qpair_datamove().  (FreeBSD: nvmft_handle_datamove(union
 * ctl_io) dispatched from the per-qpair datamove queue.)  STMF drives transfers
 * directly through lport_xfer_data(), so this path exists only for transfers
 * the transport asked us to defer; it is currently unused because
 * nvmft_lport_xfer_data() handles both directions inline.
 */
void
nvmft_handle_datamove(scsi_task_t *task)
{
	_NOTE(ARGUNUSED(task));
}

/*
 * The qpair is gone before a queued datamove could run.  Fail the task so STMF
 * tears it down.  (FreeBSD nvmft_abort_datamove marked the ctl_io aborted and
 * called ctl_datamove_done.)
 */
void
nvmft_abort_datamove(scsi_task_t *task)
{
	stmf_abort(STMF_QUEUE_TASK_ABORT, task, STMF_ABORTED, NULL);
}

/*
 * ============================================================================
 * Status phase: lport_send_status
 * ============================================================================
 */

/*
 * Add a 64-bit addend to a little-endian 128-bit SMART counter.
 * (FreeBSD: hip_add.)
 */
static void
nvmft_hip_add(nvme_uint128_t *val, uint64_t addend)
{
	uint64_t old, new;

	old = LE_64(val->lo);
	new = old + addend;
	val->lo = LE_64(new);
	if (new < old)
		val->hi = LE_64(LE_64(val->hi) + 1);
}

/*
 * Translate a SCSI sense key / ASC / ASCQ into an NVMe (SCT, SC) status.  Only
 * the mappings that the stmf_sbd LU actually produces for the translated
 * READ/WRITE/FLUSH commands are covered; anything else falls back to a generic
 * Internal Error.  Both the status code type (*sctp) and the status code (*scp)
 * are output: most mappings stay in the Generic type, but some (e.g. a compare
 * miscompare) require a Command Specific type.  (FreeBSD let CTL set ctl_nvmeio
 * status directly; on STMF the LU speaks SCSI so we reverse the mapping here.)
 */
static void
nvmft_scsi_sense_to_nvme(scsi_task_t *task, uint8_t *sctp, uint8_t *scp)
{
	struct scsi_extended_sense *sense;
	uint8_t key;

	*sctp = NVME_CQE_SCT_GENERIC;
	*scp = NVME_CQE_SC_GEN_INTERNAL_ERR;

	if (task->task_sense_length < sizeof (*sense) ||
	    task->task_sense_data == NULL)
		return;

	sense = (struct scsi_extended_sense *)task->task_sense_data;
	key = sense->es_key;

	switch (key) {
	case KEY_NO_SENSE:
		*scp = NVME_CQE_SC_GEN_SUCCESS;
		break;
	case KEY_NOT_READY:
		*scp = NVME_CQE_SC_GEN_NVM_NS_NOTRDY;
		break;
	case KEY_MEDIUM_ERROR:
		*scp = NVME_CQE_SC_GEN_DATA_XFR_ERR;
		break;
	case KEY_MISCOMPARE:
		/*
		 * A VERIFY/BYTCHK miscompare maps to NVMe Compare Failure, which
		 * is a Command Specific status (SCT 1), not a Generic one.  We do
		 * not translate COMPARE today (see nvmft_translate_cmd), so the LU
		 * should never emit this, but report it correctly if it does.
		 */
		*sctp = NVME_CQE_SCT_SPECIFIC;
		*scp = NVME_CQE_SC_INT_NVM_COMPARE;
		break;
	case KEY_ILLEGAL_REQUEST:
		/* LBA out of range -> ASC 0x21. */
		if (sense->es_add_code == 0x21)
			*scp = NVME_CQE_SC_GEN_NVM_LBA_RANGE;
		else
			*scp = NVME_CQE_SC_GEN_INV_FLD;
		break;
	case KEY_DATA_PROTECT:
		*scp = NVME_CQE_SC_GEN_NS_RDONLY;
		break;
	default:
		*scp = NVME_CQE_SC_GEN_INTERNAL_ERR;
		break;
	}
}

/*
 * Fold the SMART host read/write command and data-units counters for a
 * completed READ/WRITE.  The COMPARE case is retained for fidelity with FreeBSD
 * nvmft_done() (COMPARE counts as a host read) even though COMPARE is not
 * currently translated.  (FreeBSD nvmft_done.)
 */
static void
nvmft_account_smart(nvmft_controller_t *ctrlr, uint8_t opc, size_t data_len,
    boolean_t success)
{
	size_t len = success ? data_len / 512 : 0;

	switch (opc) {
	case NVME_OPC_NVM_WRITE:
		mutex_enter(&ctrlr->ctrlr_lock);
		nvmft_hip_add(&ctrlr->ctrlr_hip.hl_host_write, 1);
		len += ctrlr->ctrlr_partial_duw;
		if (len > 1000)
			nvmft_hip_add(&ctrlr->ctrlr_hip.hl_data_write,
			    len / 1000);
		ctrlr->ctrlr_partial_duw = len % 1000;
		mutex_exit(&ctrlr->ctrlr_lock);
		break;
	case NVME_OPC_NVM_READ:
	case NVME_OPC_NVM_COMPARE:
		mutex_enter(&ctrlr->ctrlr_lock);
		nvmft_hip_add(&ctrlr->ctrlr_hip.hl_host_read, 1);
		len += ctrlr->ctrlr_partial_dur;
		if (len > 1000)
			nvmft_hip_add(&ctrlr->ctrlr_hip.hl_data_read,
			    len / 1000);
		ctrlr->ctrlr_partial_dur = len % 1000;
		mutex_exit(&ctrlr->ctrlr_lock);
		break;
	}
}

/*
 * STMF calls lport_send_status() once the LU has produced SCSI status.  We
 * translate that SCSI status back to an NVMe completion and emit a response
 * capsule.  (FreeBSD did the equivalent in nvmft_done().)
 */
static stmf_status_t
nvmft_lport_send_status(scsi_task_t *task, uint32_t ioflags)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	struct nvmf_capsule *nc;
	nvmft_controller_t *ctrlr;
	const nvme_sqe_t *cmd;
	boolean_t good = (task->task_scsi_status == STATUS_GOOD);
	nvme_cqe_t cpl;
	uint8_t sct, sc;

	_NOTE(ARGUNUSED(ioflags));

	/*
	 * Internal (driver-issued) task: there is no host capsule.  Hand the
	 * SCSI status back to the issuing thread and complete.
	 */
	if (priv->ntp_iio != NULL) {
		nvmft_internal_io_t *iio = priv->ntp_iio;

		mutex_enter(&iio->iio_lock);
		if (!iio->iio_done) {
			iio->iio_scsi_status = task->task_scsi_status;
			iio->iio_done = B_TRUE;
			cv_signal(&iio->iio_cv);
		}
		mutex_exit(&iio->iio_lock);
		stmf_send_status_done(task, STMF_SUCCESS, STMF_IOF_LPORT_DONE);
		return (STMF_SUCCESS);
	}

	nc = priv->ntp_nc;
	ctrlr = nvmft_qpair_ctrlr(priv->ntp_qp);
	cmd = nvmf_capsule_sqe(nc);

	nvmft_account_smart(ctrlr, cmd->sqe_opc, nvmf_capsule_data_len(nc),
	    good);

	if (priv->ntp_success_sent) {
		/*
		 * The transport already sent an implicit success CQE folded into
		 * the final C2H data PDU; STMF still wants its completion.
		 *
		 * Forcing a single in-order C2H dbuf (see nvmft_dispatch_command)
		 * means the implicit SUCCESS is only stamped on the final chunk,
		 * so reaching here normally implies the whole transfer succeeded.
		 * A late backend error on the last dbuf could still leave the LU
		 * reporting CHECK while the host has already seen success: that is
		 * an unrecoverable protocol inconsistency, but it must not panic
		 * the controller.  Warn and honour the success already on the wire.
		 */
		if (!good) {
			(void) nvmft_printf(ctrlr,
			    "implicit success already sent but LU reported "
			    "SCSI status 0x%x; host already saw success\n",
			    task->task_scsi_status);
		}
		stmf_send_status_done(task, STMF_SUCCESS, STMF_IOF_LPORT_DONE);
		return (STMF_SUCCESS);
	}

	if (good) {
		sct = NVME_CQE_SCT_GENERIC;
		sc = NVME_CQE_SC_GEN_SUCCESS;
	} else {
		nvmft_scsi_sense_to_nvme(task, &sct, &sc);
	}

	(void) bzero(&cpl, sizeof (cpl));
	cpl.cqe_cid = cmd->sqe_cid;
	cpl.cqe_sf.sf_sct = sct;
	cpl.cqe_sf.sf_sc = sc;
	(void) nvmft_send_response(priv->ntp_qp, &cpl);

	stmf_send_status_done(task, STMF_SUCCESS, STMF_IOF_LPORT_DONE);
	return (STMF_SUCCESS);
}

static void
nvmft_lport_task_free(scsi_task_t *task)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	nvmft_controller_t *ctrlr;

	if (priv == NULL)
		return;

	/*
	 * Internal (driver-issued) task: no host capsule, no pending-command
	 * accounting, and no qpair.  The issuing thread owns the rendezvous
	 * (iio) on its stack; we only release the private block here.
	 */
	if (priv->ntp_iio != NULL) {
		task->task_port_private = NULL;
		kmem_free(priv, sizeof (*priv));
		return;
	}

	ctrlr = nvmft_qpair_ctrlr(priv->ntp_qp);
	if (priv->ntp_nc != NULL)
		nvmf_free_capsule(priv->ntp_nc);

	mutex_enter(&ctrlr->ctrlr_lock);
	ASSERT3U(ctrlr->ctrlr_pending_commands, >, 0);
	ctrlr->ctrlr_pending_commands--;
	if (ctrlr->ctrlr_pending_commands == 0) {
		ctrlr->ctrlr_busy_total +=
		    gethrtime() - ctrlr->ctrlr_start_busy;
		cv_signal(&ctrlr->ctrlr_pending_cv);
	}
	mutex_exit(&ctrlr->ctrlr_lock);

	task->task_port_private = NULL;
	kmem_free(priv, sizeof (*priv));
}

/*
 * STMF asks us to abort a task.  For STMF_LPORT_ABORT_TASK, arg is the
 * scsi_task_t.  Cancel any in-flight transport data transfer; the transport
 * invokes our receive completion callback with an error, which fails the dbuf
 * back to STMF.  STMF then frees the task through lport_task_free, which frees
 * the capsule.  (FreeBSD: nvmft_abort_datamove / nvmf_abort_capsule_data.)
 *
 * Locking / lifecycle: STMF guarantees the task referenced by arg stays valid
 * for the duration of this call and serialises abort against lport_task_free,
 * so task_port_private (priv) and priv->ntp_nc cannot be freed underneath us
 * here.  We still guard both against NULL for the pre-translation / already-
 * completed windows.  nvmf_abort_capsule_data() is a no-op when no H2C receive
 * is outstanding (its io_len is 0), so it is safe to call unconditionally for a
 * task that has a capsule; we rely on that rather than tracking outstanding-xfer
 * state in the task private (see todos).
 */
/* ARGSUSED */
static stmf_status_t
nvmft_lport_abort(stmf_local_port_t *lport, int abort_cmd, void *arg,
    uint32_t flags)
{
	scsi_task_t *task = arg;
	nvmft_task_priv_t *priv;

	_NOTE(ARGUNUSED(lport, flags));

	if (abort_cmd != STMF_LPORT_ABORT_TASK)
		return (STMF_ABORT_SUCCESS);

	priv = task->task_port_private;
	if (priv == NULL)
		return (STMF_ABORT_SUCCESS);

	/*
	 * Internal (driver-issued) task: no send_status will follow an abort,
	 * so release the issuing thread with a failure status instead of
	 * leaving it blocked in nvmft_lu_read_capacity().
	 */
	if (priv->ntp_iio != NULL) {
		nvmft_internal_io_t *iio = priv->ntp_iio;

		mutex_enter(&iio->iio_lock);
		if (!iio->iio_done) {
			iio->iio_scsi_status = STATUS_CHECK;
			iio->iio_done = B_TRUE;
			cv_signal(&iio->iio_cv);
		}
		mutex_exit(&iio->iio_lock);
		return (STMF_ABORT_SUCCESS);
	}

	if (priv->ntp_nc != NULL)
		nvmf_abort_capsule_data(priv->ntp_nc, ECANCELED);

	return (STMF_ABORT_SUCCESS);
}

/* ARGSUSED */
static void
nvmft_lport_task_poll(scsi_task_t *task)
{
	_NOTE(ARGUNUSED(task));
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
 * ============================================================================
 * SCSI device id descriptor for the local port identity
 * ============================================================================
 */

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

/*
 * ============================================================================
 * dbuf store
 * ============================================================================
 *
 * STMF requires every local port to supply a dbuf store; the LU calls
 * stmf_alloc_dbuf() (which dispatches to ds_alloc_data_buf) to obtain a buffer,
 * fills it (WRITE) or has us fill it (READ), and drives the transfer through
 * lport_xfer_data().  For the TCP transport these are plain kmem copy buffers
 * with a single-segment sglist; lport_xfer_data() copies between the sglist and
 * the transport's PDU mblks.  (NVMEOF.md section 8: an RDMA transport would back
 * the same store with registered memory.)
 */

/* Per-dbuf private state: remembers the kmem buffer to free. */
typedef struct nvmft_dbuf_priv {
	void		*ndp_buf;
	uint32_t	ndp_size;
} nvmft_dbuf_priv_t;

/* ARGSUSED */
static stmf_data_buf_t *
nvmft_dbuf_alloc(scsi_task_t *task, uint32_t size, uint32_t *pminsize,
    uint32_t flags)
{
	stmf_data_buf_t *dbuf;
	nvmft_dbuf_priv_t *ndp;
	void *buf;

	_NOTE(ARGUNUSED(task, pminsize, flags));

	if (size == 0)
		return (NULL);

	buf = kmem_alloc(size, KM_NOSLEEP);
	if (buf == NULL)
		return (NULL);

	dbuf = stmf_alloc(STMF_STRUCT_DATA_BUF, sizeof (nvmft_dbuf_priv_t), 0);
	if (dbuf == NULL) {
		kmem_free(buf, size);
		return (NULL);
	}

	ndp = dbuf->db_port_private;
	ndp->ndp_buf = buf;
	ndp->ndp_size = size;

	dbuf->db_flags = DB_DONT_CACHE;
	dbuf->db_buf_size = size;
	dbuf->db_data_size = size;
	dbuf->db_sglist_length = 1;
	dbuf->db_sglist[0].seg_addr = buf;
	dbuf->db_sglist[0].seg_length = size;
	return (dbuf);
}

/* ARGSUSED */
static void
nvmft_dbuf_free(stmf_dbuf_store_t *ds, stmf_data_buf_t *dbuf)
{
	nvmft_dbuf_priv_t *ndp = dbuf->db_port_private;

	_NOTE(ARGUNUSED(ds));

	kmem_free(ndp->ndp_buf, ndp->ndp_size);
	stmf_free(dbuf);
}

static stmf_dbuf_store_t *
nvmft_dbuf_store_create(void)
{
	stmf_dbuf_store_t *ds;

	ds = stmf_alloc(STMF_STRUCT_DBUF_STORE, 0, 0);
	if (ds == NULL)
		return (NULL);
	ds->ds_alloc_data_buf = nvmft_dbuf_alloc;
	ds->ds_free_data_buf = nvmft_dbuf_free;
	ds->ds_setup_dbuf = NULL;
	ds->ds_teardown_dbuf = NULL;
	return (ds);
}

static void
nvmft_dbuf_store_destroy(stmf_dbuf_store_t *ds)
{
	stmf_free(ds);
}

/*
 * ============================================================================
 * taskq helpers (FreeBSD nvmft_enqueue_task / nvmft_drain_task)
 * ============================================================================
 */

void
nvmft_enqueue_task(taskq_ent_t *task, task_func_t *func, void *arg)
{
	taskq_dispatch_ent(nvmft_global->ns_taskq, func, arg, 0, task);
}

void
nvmft_drain_task(taskq_ent_t *task)
{
	/*
	 * PORT-TODO: illumos taskq has no per-entry drain; wait_for_completion
	 * via taskq_wait or track completion on the entry.  For the scaffold a
	 * full taskq_wait is a safe (if coarse) drain.
	 */
	_NOTE(ARGUNUSED(task));
	taskq_wait(nvmft_global->ns_taskq);
}

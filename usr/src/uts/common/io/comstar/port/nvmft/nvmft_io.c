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
 * The I/O path: NVMe commands become STMF tasks, and the local port entry
 * points move their data and return their status.
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
#include <sys/taskq.h>
#include <sys/stream.h>
#include <sys/atomic.h>
#include <sys/scsi/scsi.h>
#include <sys/scsi/generic/commands.h>
#include <sys/scsi/generic/status.h>
#include <sys/scsi/generic/sense.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include <sys/time.h>
#include <sys/stmf.h>
#include <sys/lpif.h>
#include <sys/stmf_ioctl.h>
#include <sys/portif.h>

#include "nvmft_io.h"

/*
 * Bound the number of commands concurrently in flight per controller
 * (nvmft_max_active_commands) so a fast writeback-cached initiator cannot overrun
 * the backing store's drain rate.  Over-cap commands are DEFERRED (queued and
 * re-dispatched on completion) rather than refused, so the initiator paces itself
 * via SQ-credit backpressure: the host maps any non-zero CQE to EIO and will not
 * retry.  nvmft_max_pending_bytes is a secondary byte ceiling.  Either cap is
 * disabled when 0 (with the count cap off the legacy nvmft_max_pending_commands
 * refuse-path still applies as a hard backstop).
 */
uint_t nvmft_max_active_commands = 16;
uint_t nvmft_max_pending_commands = 8192;
uint64_t nvmft_max_pending_bytes = 1024ULL * 1024 * 1024;	/* 1 GiB */

/*
 * A host command's task private lives in its capsule, so the receive path does
 * not allocate.  Commands deferred by the in-flight cap above wait on
 * ctrlr_deferred, linked through ntp_link.
 */
CTASSERT(sizeof (nvmft_task_priv_t) <=
    sizeof (((struct nvmf_capsule *)0)->nc_consumer));

static nvmft_task_priv_t *
nvmft_task_priv_init(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
    uint32_t data_len)
{
	nvmft_task_priv_t *priv = NVMF_CAPSULE_CONSUMER(nc);

	bzero(priv, sizeof (*priv));
	priv->ntp_nc = nc;
	priv->ntp_qp = qp;
	priv->ntp_data_len = data_len;
	mutex_init(&priv->ntp_lock, NULL, MUTEX_DRIVER, NULL);
	return (priv);
}

/* Free a host command's capsule, and with it the task private. */
static void
nvmft_task_priv_free(nvmft_task_priv_t *priv)
{
	struct nvmf_capsule *nc = priv->ntp_nc;

	ASSERT0(priv->ntp_xfers);
	mutex_destroy(&priv->ntp_lock);
	nvmf_free_capsule(nc);
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
		BE_OUT64(&cdb[2], slba);
		BE_OUT32(&cdb[10], nlb);
		task->task_flags |= TF_READ_DATA;
		task->task_expected_xfer_length = data_len;
		break;
	case NVME_OPC_NVM_WRITE:
		cdb[0] = SCMD_WRITE_G4;			/* WRITE(16) */
		BE_OUT64(&cdb[2], slba);
		BE_OUT32(&cdb[10], nlb);
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
		 * DSET_MGMT -> UNMAP.  The reservation opcodes (RESV_*) are a
		 * deferred decision (see nvmft_resv.c) and are not yet dispatched.
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
 * Whether a newly arrived command must be deferred rather than posted now.
 * Called with ctrlr_lock held.  Always admit when nothing is in flight so a
 * single command larger than the byte cap still makes progress.
 */
static boolean_t
nvmft_should_defer(nvmft_controller_t *ctrlr, uint32_t data_len)
{
	ASSERT(MUTEX_HELD(&ctrlr->ctrlr_lock));

	if (ctrlr->ctrlr_pending_commands == 0)
		return (B_FALSE);
	if (nvmft_max_active_commands != 0 &&
	    ctrlr->ctrlr_pending_commands >= nvmft_max_active_commands)
		return (B_TRUE);
	if (nvmft_max_pending_commands != 0 &&
	    ctrlr->ctrlr_pending_commands >= nvmft_max_pending_commands)
		return (B_TRUE);
	if (nvmft_max_pending_bytes != 0 &&
	    ctrlr->ctrlr_pending_bytes + data_len > nvmft_max_pending_bytes)
		return (B_TRUE);
	return (B_FALSE);
}

/*
 * Pop the next deferred command if there is now room to post it, charging it
 * against the in-flight accounting.  Called with ctrlr_lock held; the caller
 * posts the returned command (via nvmft_post_command) after dropping the lock.
 * Returns NULL when the queue is empty or still over a cap.
 */
static nvmft_task_priv_t *
nvmft_admit_one_deferred(nvmft_controller_t *ctrlr)
{
	nvmft_task_priv_t *d;

	ASSERT(MUTEX_HELD(&ctrlr->ctrlr_lock));

	if (ctrlr->ctrlr_deferred_commands == 0)
		return (NULL);
	d = list_head(&ctrlr->ctrlr_deferred);
	if (nvmft_should_defer(ctrlr, d->ntp_data_len))
		return (NULL);

	list_remove(&ctrlr->ctrlr_deferred, d);
	ctrlr->ctrlr_deferred_commands--;
	if (ctrlr->ctrlr_pending_commands == 0)
		ctrlr->ctrlr_start_busy = gethrtime();
	ctrlr->ctrlr_pending_commands++;
	ctrlr->ctrlr_pending_bytes += d->ntp_data_len;
	return (d);
}

/* Controller lifecycle hooks for the deferred-command queue (nvmft_var.h). */
void
nvmft_deferred_init(nvmft_controller_t *ctrlr)
{
	list_create(&ctrlr->ctrlr_deferred, sizeof (nvmft_task_priv_t),
	    offsetof(nvmft_task_priv_t, ntp_link));
}

/*
 * Drop every queued (never-posted) command, freeing its capsule.  Called from
 * controller shutdown after the I/O qpairs are stopped, so no new command can
 * be deferred.  Queued commands were never posted to STMF and are not counted
 * in ctrlr_pending_commands, so they are simply discarded -- the host's
 * association is tearing down and will not see a completion for them.
 */
void
nvmft_deferred_drain(nvmft_controller_t *ctrlr)
{
	list_t drain;
	nvmft_task_priv_t *d;

	list_create(&drain, sizeof (nvmft_task_priv_t),
	    offsetof(nvmft_task_priv_t, ntp_link));

	mutex_enter(&ctrlr->ctrlr_lock);
	list_move_tail(&drain, &ctrlr->ctrlr_deferred);
	ctrlr->ctrlr_deferred_commands = 0;
	mutex_exit(&ctrlr->ctrlr_lock);

	while ((d = list_remove_head(&drain)) != NULL)
		nvmft_task_priv_free(d);
	list_destroy(&drain);
}

void
nvmft_deferred_fini(nvmft_controller_t *ctrlr)
{
	VERIFY0(ctrlr->ctrlr_deferred_commands);
	list_destroy(&ctrlr->ctrlr_deferred);
}

/*
 * Allocate and post the STMF task for an admitted command.  The in-flight
 * accounting was already charged (in dispatch or admit).  On STMF task-alloc
 * failure -- rare, STMF resource exhaustion -- the slot is rolled back and the
 * next deferred command is admitted and posted in the same loop (iteratively,
 * never recursively), so the deferred queue cannot stall on a failed post.
 */
static void
nvmft_post_command(nvmft_task_priv_t *priv)
{
	nvmft_controller_t *ctrlr = nvmft_qpair_ctrlr(priv->ntp_qp);
	nvmft_port_t *np = ctrlr->ctrlr_np;

	for (;;) {
		struct nvmft_qpair *qp = priv->ntp_qp;
		struct nvmf_capsule *nc = priv->ntp_nc;
		uint32_t data_len = priv->ntp_data_len;
		const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);
		scsi_task_t *task;
		uint8_t lun[8];

		/*
		 * STMF LUN is the NVMe NSID minus one, encoded as a single-level
		 * LUN.  stmf_task_alloc() decodes luNbr = lun[1] | ((lun[0] & 0x3f)
		 * << 8), so the 14-bit LUN splits across lun[0:1] for NSIDs >= 256.
		 */
		(void) bzero(lun, sizeof (lun));
		lun[0] = (uint8_t)(((LE_32(cmd->sqe_nsid) - 1) >> 8) & 0x3f);
		lun[1] = (uint8_t)((LE_32(cmd->sqe_nsid) - 1) & 0xff);

		task = stmf_task_alloc(np->np_lport, ctrlr->ctrlr_session, lun,
		    16 /* cdb_length */, 0);
		if (task != NULL) {
			task->task_port_private = priv;
			task->task_flags |= TF_ATTR_SIMPLE_QUEUE;

			/*
			 * NVMe/TCP needs a command's data as one in-order byte
			 * stream: C2H offsets must be sequential, and only the
			 * chunk that ends the transfer carries LAST_PDU and the
			 * implicit SUCCESS.  Unless the transport takes chunks
			 * in any order, allow one dbuf in flight so sbd issues
			 * the data at advancing offsets.  The transfer lengths
			 * are pinned so the LU prefers one buffer per command.
			 */
			task->task_max_nbufs = (nvmft_qpair_caps(qp) &
			    NVMF_QP_CAP_UNORDERED_DATA) ? STMF_BUFS_MAX : 1;
			if (nvmft_qpair_caps(qp) & NVMF_QP_CAP_LU_DBUF) {
				task->task_additional_flags |=
				    TASK_AF_ACCEPT_LU_DBUF;
			}
			task->task_max_xfer_len = data_len;
			task->task_1st_xfer_len = data_len;

			/*
			 * The opcode was already vetted by nvmft_cmd_translatable(),
			 * so the translation cannot fail here.  VERIFY the invariant
			 * rather than kmem_free()'ing an allocated-and-linked STMF
			 * task (which would corrupt the LU task list); a posted task
			 * is disposed only through STMF.
			 */
			VERIFY(nvmft_translate_cmd(task, cmd, data_len));
			stmf_post_task(task, NULL);
			return;
		}

		/*
		 * Could not allocate the STMF task.  Error this command back to the
		 * host, release its in-flight slot, and admit the next deferred
		 * command (if any) to post on the next loop iteration -- so a failed
		 * post still drains the backpressure queue.
		 */
		(void) nvmft_send_generic_error(qp, nc,
		    NVME_CQE_SC_GEN_INTERNAL_ERR);
		nvmft_task_priv_free(priv);

		mutex_enter(&ctrlr->ctrlr_lock);
		ASSERT3U(ctrlr->ctrlr_pending_commands, >, 0);
		ctrlr->ctrlr_pending_commands--;
		ctrlr->ctrlr_pending_bytes -= data_len;
		if (ctrlr->ctrlr_pending_commands == 0)
			cv_signal(&ctrlr->ctrlr_pending_cv);
		priv = nvmft_admit_one_deferred(ctrlr);
		mutex_exit(&ctrlr->ctrlr_lock);

		if (priv == NULL)
			return;
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
	nvmft_task_priv_t *priv;
	uint32_t data_len;

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

	data_len = (uint32_t)nvmf_capsule_data_len(nc);
	priv = nvmft_task_priv_init(qp, nc, data_len);

	mutex_enter(&ctrlr->ctrlr_lock);
	if (nvmft_should_defer(ctrlr, data_len)) {
		/*
		 * At the in-flight cap.  Queue the command rather than refuse it:
		 * the host maps any non-zero CQE to EIO and does not retry
		 * NS_NOTRDY, so refusing would corrupt the I/O.  Deferring leaves
		 * the command outstanding (no CQE), so the initiator simply runs
		 * out of SQ credits and paces itself; nvmft_lport_task_free()
		 * re-dispatches it when a slot frees.
		 */
		list_insert_tail(&ctrlr->ctrlr_deferred, priv);
		ctrlr->ctrlr_deferred_commands++;
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	if (ctrlr->ctrlr_pending_commands == 0)
		ctrlr->ctrlr_start_busy = gethrtime();
	ctrlr->ctrlr_pending_commands++;
	ctrlr->ctrlr_pending_bytes += data_len;
	mutex_exit(&ctrlr->ctrlr_lock);

	nvmft_post_command(priv);
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

CTASSERT(sizeof (nvmf_seg_t) == sizeof (stmf_sglist_ent_t));
CTASSERT(offsetof(nvmf_seg_t, nsg_len) ==
    offsetof(stmf_sglist_ent_t, seg_length));
CTASSERT(offsetof(nvmf_seg_t, nsg_addr) ==
    offsetof(stmf_sglist_ent_t, seg_addr));

/* Describe the db_data_size bytes of a dbuf's sglist as a memdesc. */
static void
nvmft_dbuf_to_memdesc(stmf_data_buf_t *dbuf, const nvmft_dbuf_priv_t *ndp,
    nvmf_memdesc_t *mem)
{
	bzero(mem, sizeof (*mem));
	mem->nmd_len = dbuf->db_data_size;
	if (ndp->ndp_transport) {
		mem->nmd_type = NVMF_MEMDESC_SGL;
		mem->nmd_u.nmd_sgl.nmd_segs =
		    (const nvmf_seg_t *)dbuf->db_sglist;
		mem->nmd_u.nmd_sgl.nmd_nsegs = dbuf->db_sglist_length;
		mem->nmd_u.nmd_sgl.nmd_cookies = ndp->ndp_db.ndb_cookies;
		mem->nmd_u.nmd_sgl.nmd_ncookies = ndp->ndp_db.ndb_ncookies;
		return;
	}
	if (dbuf->db_sglist_length == 1) {
		mem->nmd_type = NVMF_MEMDESC_VADDR;
		mem->nmd_u.nmd_vaddr = dbuf->db_sglist[0].seg_addr;
		return;
	}
	mem->nmd_type = NVMF_MEMDESC_SGL;
	mem->nmd_u.nmd_sgl.nmd_segs = (const nvmf_seg_t *)dbuf->db_sglist;
	mem->nmd_u.nmd_sgl.nmd_nsegs = dbuf->db_sglist_length;
}

/* Count a transfer the transport is about to hold, unless we are aborting. */
static boolean_t
nvmft_xfer_begin(nvmft_task_priv_t *priv)
{
	boolean_t ok;

	mutex_enter(&priv->ntp_lock);
	ok = !priv->ntp_aborting;
	if (ok)
		priv->ntp_xfers++;
	mutex_exit(&priv->ntp_lock);
	return (ok);
}

/*
 * The transport is done with a transfer.  Returns B_FALSE if the task is being
 * aborted: STMF must then not see the dbuf back, and the abort is retried.
 */
static boolean_t
nvmft_xfer_end(scsi_task_t *task)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	boolean_t aborting;

	mutex_enter(&priv->ntp_lock);
	ASSERT3U(priv->ntp_xfers, >, 0);
	priv->ntp_xfers--;
	aborting = priv->ntp_aborting;
	mutex_exit(&priv->ntp_lock);
	if (aborting) {
		stmf_abort(STMF_REQUEUE_TASK_ABORT_LPORT, task, STMF_ABORTED,
		    NULL);
		return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * The transport refused a transfer.  sbd waits for every dbuf it hands out
 * and does not act on an error from stmf_xfer_data(), so the dbuf goes back
 * failed; otherwise the task waits for STMF's task timeout.
 */
static stmf_status_t
nvmft_xfer_refused(scsi_task_t *task, stmf_data_buf_t *dbuf, boolean_t *donep)
{
	dbuf->db_xfer_status = STMF_FAILURE;
	*donep = nvmft_xfer_end(task);
	return (*donep ? STMF_SUCCESS : STMF_ABORTED);
}

static boolean_t
nvmft_xfer_arrive(nvmft_xfer_t *nx, uint_t who)
{
	uint_t both = NVMFT_XFER_SUBMITTED | NVMFT_XFER_COMPLETED;

	membar_producer();
	if (atomic_or_uint_nv(&nx->nx_state, who) != both)
		return (B_FALSE);
	membar_consumer();
	return (B_TRUE);
}

/* Hand the dbuf back to STMF.  (FreeBSD: nvmft_datamove_*_cb.) */
static void
nvmft_xfer_finish(nvmft_xfer_t *nx)
{
	scsi_task_t *task = nx->nx_task;
	stmf_data_buf_t *dbuf = nx->nx_dbuf;
	nvmft_task_priv_t *priv = task->task_port_private;
	uint32_t iof = 0;

	/* Account for a response on the wire even if the task is aborting. */
	if (nx->nx_to_rport && (nx->nx_final ||
	    nx->nx_status == NVMF_SUCCESS_SENT)) {
		ASSERT(nx->nx_final || !(nvmft_qpair_caps(priv->ntp_qp) &
		    NVMF_QP_CAP_ALWAYS_RESPONSE));
		if (!nx->nx_final)
			nvmft_command_completed(priv->ntp_qp, priv->ntp_nc);
		priv->ntp_success_sent = B_TRUE;
	}
	if (!nvmft_xfer_end(task))
		return;

	if (!nx->nx_to_rport) {
		dbuf->db_xfer_status = (nx->nx_status == 0) ? STMF_SUCCESS :
		    STMF_FAILURE;
		stmf_data_xfer_done(task, dbuf, 0);
		return;
	}

	switch (nx->nx_status) {
	case NVMF_SUCCESS_SENT:
		/*
		 * The response is on the wire, either our final_cqe or success
		 * folded into the data (the TCP SUCCESS flag).  If the LU put
		 * status in this dbuf it calls stmf_task_lu_done(), which needs
		 * the port to have released the task already.
		 */
		dbuf->db_xfer_status = STMF_SUCCESS;
		if (dbuf->db_flags & DB_SEND_STATUS_GOOD)
			iof = STMF_IOF_LPORT_DONE;
		break;
	case NVME_CQE_SC_GEN_SUCCESS:
	case NVMF_MORE:
		ASSERT(!nx->nx_final);
		dbuf->db_xfer_status = STMF_SUCCESS;
		break;
	default:
		dbuf->db_xfer_status = STMF_FAILURE;
		if (nx->nx_final)
			iof = STMF_IOF_LPORT_DONE;
		break;
	}
	stmf_data_xfer_done(task, dbuf, iof);
}

static void
nvmft_datamove_out_cb(void *arg, uint_t status)
{
	nvmft_xfer_t *nx = arg;

	nx->nx_status = status;
	if (nvmft_xfer_arrive(nx, NVMFT_XFER_COMPLETED))
		nvmft_xfer_finish(nx);
}

static void
nvmft_datamove_in_cb(void *arg, size_t xfered, int error)
{
	nvmft_xfer_t *nx = arg;

	if (error == 0 && xfered != nx->nx_dbuf->db_data_size)
		error = EIO;
	nx->nx_status = (uint_t)error;
	if (nvmft_xfer_arrive(nx, NVMFT_XFER_COMPLETED))
		nvmft_xfer_finish(nx);
}

/*
 * STMF calls lport_xfer_data() to move one dbuf either to (C2H, READ) or from
 * (H2C, WRITE) the remote host.  We describe the dbuf's sglist as a memdesc
 * and hand it to nvmf_send_controller_data_io() (C2H) or
 * nvmf_receive_controller_data() (H2C).  Both complete asynchronously.  When
 * the LU puts good status in the dbuf, the transport also sends the response.
 *
 * Without NVMF_QP_CAP_UNORDERED_DATA, nvmft_post_command() allows one dbuf
 * in flight, so C2H chunks arrive at advancing offsets.
 */
stmf_status_t
nvmft_lport_xfer_data(scsi_task_t *task, stmf_data_buf_t *dbuf,
    uint32_t ioflags)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	struct nvmf_capsule *nc;
	struct nvmft_qpair *qp;
	struct nvmf_qpair *nq;
	nvmft_dbuf_priv_t *ndp;
	nvmft_xfer_t *nx = NULL;
	nvmf_memdesc_t mem;
	stmf_status_t ret;
	boolean_t do_xfer_done = B_FALSE, submitted = B_FALSE;
	uint32_t xfer_iof = 0;
	int error;

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
	 * freed qpair.  The reference only needs to span submitting the
	 * transfer: its completion touches no transport state, and freeing the
	 * qpair completes any transfer still registered (tcp_free_qpair ->
	 * nvmf_complete_io_request), which is what lets the controller drain.
	 */
	nq = nvmft_qpair_data_hold(qp);
	if (nq == NULL) {
		dbuf->db_xfer_status = STMF_FAILURE;
		stmf_data_xfer_done(task, dbuf, 0);
		return (STMF_SUCCESS);
	}

	ndp = dbuf->db_port_private;
	if (ndp == NULL || ndp->ndp_magic != NVMFT_DBUF_MAGIC) {
		ret = STMF_FAILURE;
		goto done;
	}
	if (!nvmft_xfer_begin(priv)) {
		ret = STMF_ABORTED;
		goto done;
	}
	nx = &ndp->ndp_xfer;
	bzero(nx, sizeof (*nx));
	nx->nx_task = task;
	nx->nx_dbuf = dbuf;
	nvmft_dbuf_to_memdesc(dbuf, ndp, &mem);

	if (dbuf->db_flags & DB_DIRECTION_TO_RPORT) {
		/* C2H: send controller data (READ). */
		nx->nx_to_rport = B_TRUE;
		nx->nx_final = (dbuf->db_flags & DB_SEND_STATUS_GOOD) != 0;
		if (nx->nx_final) {
			nvmft_init_cqe(&nx->nx_cqe, nc, 0);
			nvmft_prepare_response(qp, &nx->nx_cqe);
		}
		error = nvmf_send_controller_data_io(nc,
		    dbuf->db_relative_offset, &mem, dbuf->db_data_size,
		    nx->nx_final ? &nx->nx_cqe : NULL, nvmft_datamove_out_cb,
		    nx);
		if (error != 0 && nx->nx_final) {
			/* The response is ours to send after all. */
			nx->nx_cqe.cqe_sf.sf_sct = NVME_CQE_SCT_GENERIC;
			nx->nx_cqe.cqe_sf.sf_sc = NVME_CQE_SC_GEN_DATA_XFR_ERR;
			(void) nvmft_transmit_response(qp, &nx->nx_cqe);
			priv->ntp_success_sent = B_TRUE;
			dbuf->db_xfer_status = STMF_FAILURE;
			xfer_iof = STMF_IOF_LPORT_DONE;
			do_xfer_done = nvmft_xfer_end(task);
			ret = STMF_SUCCESS;
			goto done;
		}
		if (error != 0) {
			ret = nvmft_xfer_refused(task, dbuf, &do_xfer_done);
			goto done;
		}
	} else {
		/* H2C: receive controller data (WRITE). */
		error = nvmf_receive_controller_data(nc,
		    dbuf->db_relative_offset, &mem, dbuf->db_data_size,
		    nvmft_datamove_in_cb, nx);
		if (error != 0) {
			(void) nvmft_printf(nvmft_qpair_ctrlr(qp),
			    "Failed to request capsule data: 0x%x\n", error);
			ret = nvmft_xfer_refused(task, dbuf, &do_xfer_done);
			goto done;
		}
	}
	submitted = B_TRUE;
	ret = STMF_SUCCESS;

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
	if (submitted && nvmft_xfer_arrive(nx, NVMFT_XFER_SUBMITTED))
		nvmft_xfer_finish(nx);
	if (do_xfer_done)
		stmf_data_xfer_done(task, dbuf, xfer_iof);
	return (ret);
}

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
stmf_status_t
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
			if (task->task_scsi_status == STATUS_CHECK &&
			    task->task_sense_data != NULL) {
				iio->iio_sense_key =
				    task->task_sense_data[2] & 0x0f;
			}
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

void
nvmft_lport_task_free(scsi_task_t *task)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	nvmft_controller_t *ctrlr;
	nvmft_task_priv_t *d;
	uint32_t data_len;

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
	data_len = priv->ntp_data_len;
	task->task_port_private = NULL;
	nvmft_task_priv_free(priv);

	mutex_enter(&ctrlr->ctrlr_lock);
	ASSERT3U(ctrlr->ctrlr_pending_commands, >, 0);
	ctrlr->ctrlr_pending_commands--;
	ctrlr->ctrlr_pending_bytes -= data_len;
	if (ctrlr->ctrlr_pending_commands == 0) {
		ctrlr->ctrlr_busy_total +=
		    gethrtime() - ctrlr->ctrlr_start_busy;
		cv_signal(&ctrlr->ctrlr_pending_cv);
	}
	/* This slot just freed: re-dispatch the next deferred command, if any. */
	d = nvmft_admit_one_deferred(ctrlr);
	mutex_exit(&ctrlr->ctrlr_lock);

	if (d != NULL)
		nvmft_post_command(d);
}

/*
 * STMF asks us to abort a task.  For STMF_LPORT_ABORT_TASK, arg is the
 * scsi_task_t.  While the transport still holds a transfer into one of the
 * task's dbufs we return STMF_BUSY, because STMF frees the dbufs of an aborted
 * task; the transfer's completion asks STMF to retry the abort.
 *
 * STMF keeps the task valid for the duration of this call and serialises abort
 * against lport_task_free.  nvmf_abort_capsule_data() is a no-op when no H2C
 * receive is outstanding.
 */
/* ARGSUSED */
stmf_status_t
nvmft_lport_abort(stmf_local_port_t *lport, int abort_cmd, void *arg,
    uint32_t flags)
{
	scsi_task_t *task = arg;
	nvmft_task_priv_t *priv;
	boolean_t busy;

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

	mutex_enter(&priv->ntp_lock);
	priv->ntp_aborting = B_TRUE;
	busy = (priv->ntp_xfers != 0);
	mutex_exit(&priv->ntp_lock);
	return (busy ? STMF_BUSY : STMF_ABORT_SUCCESS);
}

/* ARGSUSED */
void
nvmft_lport_task_poll(scsi_task_t *task)
{
	_NOTE(ARGUNUSED(task));
}

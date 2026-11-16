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
#include <sys/sdt.h>
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

static void	nvmft_io_task(void *arg);

nvmft_controller_t *
nvmft_task_ctrlr(nvmft_task_priv_t *priv)
{
	if (priv->ntp_iio != NULL)
		return (priv->ntp_iio->iio_ctrlr);
	return (nvmft_qpair_ctrlr(priv->ntp_qp));
}

static nvmft_task_priv_t *
nvmft_task_priv_init(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
    uint32_t data_len)
{
	nvmft_task_priv_t *priv = NVMF_CAPSULE_CONSUMER(nc);

	bzero(priv, sizeof (*priv));
	priv->ntp_nc = nc;
	priv->ntp_qp = qp;
	priv->ntp_data_len = data_len;
	priv->ntp_sc = NVME_CQE_SC_GEN_ABORT_REQUEST;
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
 * The task is about to be posted.  Listing it first lets it complete, and
 * leave the list, at any time after the post.
 */
void
nvmft_task_listed(nvmft_controller_t *ctrlr, nvmft_task_priv_t *priv,
    scsi_task_t *task)
{
	mutex_enter(&ctrlr->ctrlr_lock);
	priv->ntp_task = task;
	list_insert_tail(&ctrlr->ctrlr_tasks, priv);
	if (priv->ntp_iio != NULL)
		ctrlr->ctrlr_internal_tasks++;
	mutex_exit(&ctrlr->ctrlr_lock);
}

static void
nvmft_task_unlist_locked(nvmft_controller_t *ctrlr, nvmft_task_priv_t *priv)
{
	ASSERT(MUTEX_HELD(&ctrlr->ctrlr_lock));
	if (list_link_active(&priv->ntp_task_link))
		list_remove(&ctrlr->ctrlr_tasks, priv);
}

/* The port is about to tell STMF that it is done with the task. */
void
nvmft_task_unlist(nvmft_controller_t *ctrlr, nvmft_task_priv_t *priv)
{
	mutex_enter(&ctrlr->ctrlr_lock);
	nvmft_task_unlist_locked(ctrlr, priv);
	mutex_exit(&ctrlr->ctrlr_lock);
}

/*
 * Abort the tasks of the controller, or only those of host commands.  Called
 * with ctrlr_lock held: a listed task is one that STMF has not freed.
 * lport_task_free takes ctrlr_lock under the lock of its own task, which is
 * no longer listed, and lport_abort only tries ctrlr_lock.
 */
static void
nvmft_abort_tasks(nvmft_controller_t *ctrlr, boolean_t internal)
{
	nvmft_task_priv_t *priv;

	ASSERT(MUTEX_HELD(&ctrlr->ctrlr_lock));
	for (priv = list_head(&ctrlr->ctrlr_tasks); priv != NULL;
	    priv = list_next(&ctrlr->ctrlr_tasks, priv)) {
		if (!internal && priv->ntp_iio != NULL)
			continue;
		stmf_abort(STMF_QUEUE_TASK_ABORT, priv->ntp_task, STMF_ABORTED,
		    NULL);
	}
}

void
nvmft_terminate_commands(nvmft_controller_t *ctrlr)
{
	mutex_enter(&ctrlr->ctrlr_lock);
	nvmft_abort_tasks(ctrlr, B_TRUE);
	mutex_exit(&ctrlr->ctrlr_lock);
}

/*
 * Abort the host commands and wait until none is left.  The I/O queues are
 * shut down and nothing is deferred, so no command starts after this returns.
 * A task posted after one pass is aborted by the next.
 */
void
nvmft_io_drain(nvmft_controller_t *ctrlr)
{
	mutex_enter(&ctrlr->ctrlr_lock);
	while (ctrlr->ctrlr_pending_commands != 0 || ctrlr->ctrlr_io_running) {
		nvmft_abort_tasks(ctrlr, B_FALSE);
		(void) cv_reltimedwait(&ctrlr->ctrlr_pending_cv,
		    &ctrlr->ctrlr_lock, drv_usectohz(100000), TR_CLOCK_TICK);
	}
	mutex_exit(&ctrlr->ctrlr_lock);
}

/* Wait until STMF has freed the internal tasks; the session can then go. */
void
nvmft_internal_drain(nvmft_controller_t *ctrlr)
{
	mutex_enter(&ctrlr->ctrlr_lock);
	while (ctrlr->ctrlr_internal_tasks != 0) {
		nvmft_abort_tasks(ctrlr, B_TRUE);
		(void) cv_reltimedwait(&ctrlr->ctrlr_pending_cv,
		    &ctrlr->ctrlr_lock, drv_usectohz(100000), TR_CLOCK_TICK);
	}
	mutex_exit(&ctrlr->ctrlr_lock);
}

/*
 * Check the fields of an I/O command that select the LU and size the transfer,
 * and that the command is one nvmft_translate_cmd() handles.  NSID n is LUN
 * n - 1.
 */
static uint8_t
nvmft_check_io_command(uint8_t opc, uint32_t nsid, size_t data_len)
{
	if (nsid == 0 || nsid - 1 > NVMFT_MAX_LUN)
		return (NVME_CQE_SC_GEN_INV_NS);

	switch (opc) {
	case NVME_OPC_NVM_READ:
	case NVME_OPC_NVM_WRITE:
		/* NLB counts from 0, so a transfer is never empty. */
		if (data_len == 0 || data_len > UINT32_MAX)
			return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
		break;
	case NVME_OPC_NVM_FLUSH:
		break;
	default:
		return (NVME_CQE_SC_GEN_INV_OPC);
	}
	return (NVME_CQE_SC_GEN_SUCCESS);
}

/*
 * Translate an NVMe NVM command into a SCSI CDB on the supplied scsi_task_t and
 * set the data-direction/transfer-length task fields.
 *
 * READ/WRITE map to the 16-byte CDB variants so the 64-bit NVMe SLBA fits.
 * NVMe carries SLBA in cdw10 (low) / cdw11 (high) and the 0's-based NLB in the
 * low 16 bits of cdw12; the SCSI transfer length is NLB + 1 logical blocks.
 * The expected transfer length is the host's SGL length.  The LU derives its
 * own length from the CDB, and nvmft_lport_xfer_data() compares the two.
 *
 * NVMe COMPARE is not translated: stmf_sbd has no read-and-compare path.
 */
static void
nvmft_translate_cmd(scsi_task_t *task, const nvme_sqe_t *cmd,
    uint32_t data_len)
{
	uint8_t *cdb = task->task_cdb;
	uint64_t slba;
	uint32_t nlb;

	(void) bzero(cdb, task->task_cdb_length);

	slba = (uint64_t)LE_32(cmd->sqe_cdw10) |
	    ((uint64_t)LE_32(cmd->sqe_cdw11) << 32);
	nlb = (LE_32(cmd->sqe_cdw12) & 0xffff) + 1;

	switch (cmd->sqe_opc) {
	case NVME_OPC_NVM_READ:
		cdb[0] = SCMD_READ_G4;
		BE_OUT64(&cdb[2], slba);
		BE_OUT32(&cdb[10], nlb);
		task->task_flags |= TF_READ_DATA;
		task->task_expected_xfer_length = data_len;
		break;
	case NVME_OPC_NVM_WRITE:
		cdb[0] = SCMD_WRITE_G4;
		BE_OUT64(&cdb[2], slba);
		BE_OUT32(&cdb[10], nlb);
		task->task_flags |= TF_WRITE_DATA;
		task->task_expected_xfer_length = data_len;
		break;
	default:
		ASSERT3U(cmd->sqe_opc, ==, NVME_OPC_NVM_FLUSH);
		cdb[0] = SCMD_SYNCHRONIZE_CACHE;
		task->task_expected_xfer_length = 0;
		task->task_additional_flags |= TASK_AF_NO_EXPECTED_XFER_LENGTH;
		break;
	}
	task->task_cmd_xfer_length = task->task_expected_xfer_length;
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
 * against the in-flight accounting.  Called with ctrlr_lock held.  Returns
 * NULL when the queue is empty or still over a cap.
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

/*
 * A command that was in flight is gone.  Called with ctrlr_lock held.  Returns
 * the deferred command that takes its place, for the caller to post.
 */
static nvmft_task_priv_t *
nvmft_command_done(nvmft_controller_t *ctrlr, uint32_t data_len)
{
	ASSERT(MUTEX_HELD(&ctrlr->ctrlr_lock));
	ASSERT3U(ctrlr->ctrlr_pending_commands, >, 0);

	ctrlr->ctrlr_pending_commands--;
	ctrlr->ctrlr_pending_bytes -= data_len;
	if (ctrlr->ctrlr_pending_commands == 0) {
		ctrlr->ctrlr_busy_total +=
		    gethrtime() - ctrlr->ctrlr_start_busy;
		cv_broadcast(&ctrlr->ctrlr_pending_cv);
	}
	return (nvmft_admit_one_deferred(ctrlr));
}

void
nvmft_io_init(nvmft_controller_t *ctrlr)
{
	list_create(&ctrlr->ctrlr_deferred, sizeof (nvmft_task_priv_t),
	    offsetof(nvmft_task_priv_t, ntp_link));
	list_create(&ctrlr->ctrlr_io_work, sizeof (nvmft_task_priv_t),
	    offsetof(nvmft_task_priv_t, ntp_link));
	list_create(&ctrlr->ctrlr_tasks, sizeof (nvmft_task_priv_t),
	    offsetof(nvmft_task_priv_t, ntp_task_link));
}

void
nvmft_io_fini(nvmft_controller_t *ctrlr)
{
	VERIFY0(ctrlr->ctrlr_deferred_commands);
	VERIFY(!ctrlr->ctrlr_io_running);
	list_destroy(&ctrlr->ctrlr_tasks);
	list_destroy(&ctrlr->ctrlr_io_work);
	list_destroy(&ctrlr->ctrlr_deferred);
}

/*
 * Drop every deferred command.  Called from controller shutdown after the I/O
 * qpairs are stopped, so no new command can be deferred.  These commands were
 * never posted and are not counted in ctrlr_pending_commands; the host's
 * queues are gone, so it sees no completion for them.
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

/*
 * Allocate and post the STMF task for an admitted command, which is already
 * charged to the in-flight accounting.  If STMF has no task, the command
 * fails and the deferred command that takes its place is posted instead.
 * This takes the session's lock in STMF, so it must not run in a callback
 * that STMF makes with that lock held.
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
		uint32_t lun_id = LE_32(cmd->sqe_nsid) - 1;
		scsi_task_t *task;
		uint8_t lun[8];

		/* A single level LUN, as stmf_task_alloc() decodes it. */
		(void) bzero(lun, sizeof (lun));
		lun[0] = (uint8_t)((lun_id >> 8) & 0x3f);
		lun[1] = (uint8_t)(lun_id & 0xff);

		task = stmf_task_alloc(np->np_lport, ctrlr->ctrlr_session, lun,
		    16, 0);
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

			nvmft_translate_cmd(task, cmd, data_len);
			nvmft_task_listed(ctrlr, priv, task);
			stmf_post_task(task, NULL);
			return;
		}

		(void) nvmft_send_generic_error(qp, nc,
		    NVME_CQE_SC_GEN_INTERNAL_ERR);
		nvmft_task_priv_free(priv);

		mutex_enter(&ctrlr->ctrlr_lock);
		priv = nvmft_command_done(ctrlr, data_len);
		mutex_exit(&ctrlr->ctrlr_lock);

		if (priv == NULL)
			return;
	}
}

/*
 * Leave a command to ns_io_taskq: an admitted command to post, or a command
 * whose task STMF freed before the host had a response.  Called with
 * ctrlr_lock held.
 */
static void
nvmft_io_work(nvmft_controller_t *ctrlr, nvmft_task_priv_t *priv)
{
	ASSERT(MUTEX_HELD(&ctrlr->ctrlr_lock));

	list_insert_tail(&ctrlr->ctrlr_io_work, priv);
	if (!ctrlr->ctrlr_io_running) {
		ctrlr->ctrlr_io_running = B_TRUE;
		taskq_dispatch_ent(nvmft_global->ns_io_taskq, nvmft_io_task,
		    ctrlr, 0, &ctrlr->ctrlr_io_task);
	}
}

/* Answer a command whose task ended without a response, and free it. */
static void
nvmft_answer_command(nvmft_controller_t *ctrlr, nvmft_task_priv_t *priv)
{
	uint32_t data_len = priv->ntp_data_len;
	nvmft_task_priv_t *d;

	(void) nvmft_send_generic_error(priv->ntp_qp, priv->ntp_nc,
	    priv->ntp_sc);
	nvmft_task_priv_free(priv);

	mutex_enter(&ctrlr->ctrlr_lock);
	d = nvmft_command_done(ctrlr, data_len);
	if (d != NULL)
		list_insert_tail(&ctrlr->ctrlr_io_work, d);
	mutex_exit(&ctrlr->ctrlr_lock);
}

static void
nvmft_io_task(void *arg)
{
	nvmft_controller_t *ctrlr = arg;
	nvmft_task_priv_t *priv;

	mutex_enter(&ctrlr->ctrlr_lock);
	while ((priv = list_remove_head(&ctrlr->ctrlr_io_work)) != NULL) {
		mutex_exit(&ctrlr->ctrlr_lock);
		/* A command that had a task is here for its response. */
		if (priv->ntp_task != NULL)
			nvmft_answer_command(ctrlr, priv);
		else
			nvmft_post_command(priv);
		mutex_enter(&ctrlr->ctrlr_lock);
	}
	ctrlr->ctrlr_io_running = B_FALSE;
	cv_broadcast(&ctrlr->ctrlr_pending_cv);
	mutex_exit(&ctrlr->ctrlr_lock);
}

/* Hand a received I/O command to the LU.  This consumes the capsule. */
void
nvmft_dispatch_command(struct nvmft_qpair *qp, struct nvmf_capsule *nc)
{
	nvmft_controller_t *ctrlr = nvmft_qpair_ctrlr(qp);
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);
	nvmft_task_priv_t *priv;
	size_t data_len = nvmf_capsule_data_len(nc);
	uint8_t sc;

	/*
	 * A task that STMF allocated can only be posted, so a command is
	 * refused before it has one.
	 */
	sc = nvmft_check_io_command(cmd->sqe_opc, LE_32(cmd->sqe_nsid),
	    data_len);
	if (sc != NVME_CQE_SC_GEN_SUCCESS) {
		DTRACE_PROBE3(nvmft__io__refused, nvmft_controller_t *, ctrlr,
		    const nvme_sqe_t *, cmd, uint8_t, sc);
		(void) nvmft_send_generic_error(qp, nc, sc);
		nvmf_free_capsule(nc);
		return;
	}

	priv = nvmft_task_priv_init(qp, nc, (uint32_t)data_len);

	mutex_enter(&ctrlr->ctrlr_lock);
	if (nvmft_should_defer(ctrlr, priv->ntp_data_len)) {
		/*
		 * The command stays outstanding, so the host runs out of
		 * queue slots and paces itself.  A completion admits it.
		 */
		list_insert_tail(&ctrlr->ctrlr_deferred, priv);
		ctrlr->ctrlr_deferred_commands++;
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	if (ctrlr->ctrlr_pending_commands == 0)
		ctrlr->ctrlr_start_busy = gethrtime();
	ctrlr->ctrlr_pending_commands++;
	ctrlr->ctrlr_pending_bytes += priv->ntp_data_len;
	mutex_exit(&ctrlr->ctrlr_lock);

	nvmft_post_command(priv);
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
 * Count a completed READ or WRITE in the SMART log, in commands and in units
 * of 1000 512-byte blocks.  Called with ctrlr_lock held.
 */
static void
nvmft_account_smart(nvmft_controller_t *ctrlr, uint8_t opc, size_t data_len,
    boolean_t success)
{
	size_t len = success ? data_len / 512 : 0;

	ASSERT(MUTEX_HELD(&ctrlr->ctrlr_lock));

	switch (opc) {
	case NVME_OPC_NVM_WRITE:
		nvmft_hip_add(&ctrlr->ctrlr_hip.hl_host_write, 1);
		len += ctrlr->ctrlr_partial_duw;
		if (len > 1000)
			nvmft_hip_add(&ctrlr->ctrlr_hip.hl_data_write,
			    len / 1000);
		ctrlr->ctrlr_partial_duw = len % 1000;
		break;
	case NVME_OPC_NVM_READ:
		nvmft_hip_add(&ctrlr->ctrlr_hip.hl_host_read, 1);
		len += ctrlr->ctrlr_partial_dur;
		if (len > 1000)
			nvmft_hip_add(&ctrlr->ctrlr_hip.hl_data_read,
			    len / 1000);
		ctrlr->ctrlr_partial_dur = len % 1000;
		break;
	}
}

/*
 * The port is done with a host command's task.  The caller tells STMF so
 * next, and STMF may free the task before that call returns.
 */
static void
nvmft_task_finish(nvmft_task_priv_t *priv, boolean_t good)
{
	nvmft_controller_t *ctrlr = nvmft_qpair_ctrlr(priv->ntp_qp);
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(priv->ntp_nc);

	mutex_enter(&ctrlr->ctrlr_lock);
	nvmft_task_unlist_locked(ctrlr, priv);
	nvmft_account_smart(ctrlr, cmd->sqe_opc, priv->ntp_data_len, good);
	mutex_exit(&ctrlr->ctrlr_lock);
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
 * aborted: STMF must then not see the dbuf back, and finds no transfer in
 * flight the next time it calls nvmft_lport_abort().
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
	return (!aborting);
}

/*
 * Give a dbuf back to STMF.  With last, the port is done with the task as
 * well, because the LU sent its status with this dbuf.
 */
static void
nvmft_xfer_done(scsi_task_t *task, stmf_data_buf_t *dbuf, boolean_t good,
    boolean_t last)
{
	dbuf->db_xfer_status = good ? STMF_SUCCESS : STMF_FAILURE;
	if (!last) {
		stmf_data_xfer_done(task, dbuf, 0);
		return;
	}
	nvmft_task_finish(task->task_port_private, good);
	stmf_data_xfer_done(task, dbuf, STMF_IOF_LPORT_DONE);
}

/*
 * A transfer that was not submitted.  sbd waits for every dbuf it hands out
 * and does not act on an error from stmf_xfer_data(), so the dbuf goes back
 * failed and the LU aborts the task.  If the LU sent its status with the
 * dbuf, it is done with the task; the host gets sc and the task ends here.
 */
static stmf_status_t
nvmft_xfer_refuse(scsi_task_t *task, stmf_data_buf_t *dbuf, boolean_t final,
    uint8_t sc)
{
	nvmft_task_priv_t *priv = task->task_port_private;

	if (final) {
		(void) nvmft_send_generic_error(priv->ntp_qp, priv->ntp_nc,
		    sc);
		priv->ntp_responded = B_TRUE;
	} else {
		priv->ntp_sc = sc;
	}
	if (!nvmft_xfer_end(task))
		return (STMF_ABORTED);
	nvmft_xfer_done(task, dbuf, B_FALSE, final);
	return (STMF_SUCCESS);
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
	boolean_t good, last = B_FALSE;

	if (!nx->nx_to_rport) {
		good = (nx->nx_status == 0);
	} else if (nx->nx_status == NVMF_SUCCESS_SENT) {
		/*
		 * The response is on the wire, either our final_cqe or success
		 * folded into the data (the TCP SUCCESS flag).
		 */
		ASSERT(nx->nx_final || !(nvmft_qpair_caps(priv->ntp_qp) &
		    NVMF_QP_CAP_ALWAYS_RESPONSE));
		if (!nx->nx_final)
			nvmft_command_completed(priv->ntp_qp, priv->ntp_nc);
		priv->ntp_responded = B_TRUE;
		good = B_TRUE;
		last = (dbuf->db_flags & DB_SEND_STATUS_GOOD) != 0;
	} else if (nx->nx_status == NVME_CQE_SC_GEN_SUCCESS ||
	    nx->nx_status == NVMF_MORE) {
		ASSERT(!nx->nx_final);
		good = B_TRUE;
	} else {
		/* With a final_cqe the transport sent an error response. */
		good = B_FALSE;
		if (nx->nx_final) {
			priv->ntp_responded = B_TRUE;
			last = B_TRUE;
		}
	}
	if (!good && !priv->ntp_responded)
		priv->ntp_sc = NVME_CQE_SC_GEN_DATA_XFR_ERR;

	if (nvmft_xfer_end(task))
		nvmft_xfer_done(task, dbuf, good, last);
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
 * Every path gives the dbuf back to STMF once, unless the task is being
 * aborted: STMF then frees the dbuf with the task.
 *
 * Without NVMF_QP_CAP_UNORDERED_DATA, nvmft_post_command() allows one dbuf
 * in flight, so C2H chunks arrive at advancing offsets.
 */
stmf_status_t
nvmft_lport_xfer_data(scsi_task_t *task, stmf_data_buf_t *dbuf,
    uint32_t ioflags)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	nvmft_dbuf_priv_t *ndp = dbuf->db_port_private;
	struct nvmf_capsule *nc = priv->ntp_nc;
	struct nvmft_qpair *qp = priv->ntp_qp;
	nvmft_xfer_t *nx;
	nvmf_memdesc_t mem;
	boolean_t to_rport = (dbuf->db_flags & DB_DIRECTION_TO_RPORT) != 0;
	boolean_t final = to_rport &&
	    (dbuf->db_flags & DB_SEND_STATUS_GOOD) != 0;
	int error;

	_NOTE(ARGUNUSED(ioflags));

	ASSERT((dbuf->db_flags & (DB_DIRECTION_TO_RPORT |
	    DB_DIRECTION_FROM_RPORT)) != (DB_DIRECTION_TO_RPORT |
	    DB_DIRECTION_FROM_RPORT));

	if (priv->ntp_iio != NULL) {
		nvmft_internal_xfer_data(task, dbuf);
		return (STMF_SUCCESS);
	}

	if (!nvmft_xfer_begin(priv))
		return (STMF_ABORTED);

	/*
	 * The LU has set the length that the CDB asks for, from NLB and its
	 * block size.  The host's SGL must be exactly that long, or the LU
	 * would move less data than the command names.
	 */
	if (task->task_cmd_xfer_length != task->task_expected_xfer_length) {
		return (nvmft_xfer_refuse(task, dbuf, final,
		    NVME_CQE_SC_GEN_INV_DSGL_LEN));
	}
	if (ndp == NULL || ndp->ndp_magic != NVMFT_DBUF_MAGIC) {
		return (nvmft_xfer_refuse(task, dbuf, final,
		    NVME_CQE_SC_GEN_INTERNAL_ERR));
	}
	/* The reference covers the submission; the completion needs none. */
	if (nvmft_qpair_data_hold(qp) == NULL) {
		return (nvmft_xfer_refuse(task, dbuf, final,
		    NVME_CQE_SC_GEN_ABORT_REQUEST));
	}

	nx = &ndp->ndp_xfer;
	bzero(nx, sizeof (*nx));
	nx->nx_task = task;
	nx->nx_dbuf = dbuf;
	nvmft_dbuf_to_memdesc(dbuf, ndp, &mem);

	if (to_rport) {
		nx->nx_to_rport = B_TRUE;
		nx->nx_final = final;
		if (final) {
			nvmft_init_cqe(&nx->nx_cqe, nc, 0);
			nvmft_prepare_response(qp, &nx->nx_cqe);
		}
		error = nvmf_send_controller_data_io(nc,
		    dbuf->db_relative_offset, &mem, dbuf->db_data_size,
		    final ? &nx->nx_cqe : NULL, nvmft_datamove_out_cb, nx);
		if (error != 0 && final) {
			/* The response is ours to send after all. */
			nx->nx_cqe.cqe_sf.sf_sct = NVME_CQE_SCT_GENERIC;
			nx->nx_cqe.cqe_sf.sf_sc = NVME_CQE_SC_GEN_DATA_XFR_ERR;
			(void) nvmft_transmit_response(qp, &nx->nx_cqe);
			priv->ntp_responded = B_TRUE;
		}
	} else {
		error = nvmf_receive_controller_data(nc,
		    dbuf->db_relative_offset, &mem, dbuf->db_data_size,
		    nvmft_datamove_in_cb, nx);
	}

	/*
	 * Giving the dbuf back can end the task, and with the last task the
	 * controller frees its queues, so the queue is let go first.
	 */
	nvmft_qpair_data_rele(qp);

	if (error != 0) {
		DTRACE_PROBE3(nvmft__xfer__refused, scsi_task_t *, task,
		    stmf_data_buf_t *, dbuf, int, error);
		if (!final)
			priv->ntp_sc = NVME_CQE_SC_GEN_DATA_XFR_ERR;
		if (!nvmft_xfer_end(task))
			return (STMF_ABORTED);
		nvmft_xfer_done(task, dbuf, B_FALSE, final);
		return (STMF_SUCCESS);
	}

	if (nvmft_xfer_arrive(nx, NVMFT_XFER_SUBMITTED))
		nvmft_xfer_finish(nx);
	return (STMF_SUCCESS);
}

/*
 * The NVMe status for the SCSI status of a translated command.  sense is
 * fixed or descriptor format sense data, and may be short or absent.
 */
static void
nvmft_scsi_to_nvme(uint8_t status, const uint8_t *sense, size_t len,
    uint8_t *sctp, uint8_t *scp)
{
	uint8_t key, asc;

	*sctp = NVME_CQE_SCT_GENERIC;
	switch (status) {
	case STATUS_GOOD:
		*scp = NVME_CQE_SC_GEN_SUCCESS;
		return;
	case STATUS_CHECK:
		break;
	case STATUS_RESERVATION_CONFLICT:
		*scp = NVME_CQE_SC_GEN_NVM_RSV_CNFLCT;
		return;
	case STATUS_BUSY:
	case STATUS_QFULL:
		/* The host is to send the command again. */
		*scp = NVME_CQE_SC_GEN_CMD_INTR;
		return;
	case STATUS_TASK_ABORT:
		*scp = NVME_CQE_SC_GEN_ABORT_REQUEST;
		return;
	default:
		*scp = NVME_CQE_SC_GEN_INTERNAL_ERR;
		return;
	}

	*scp = NVME_CQE_SC_GEN_INTERNAL_ERR;
	if (sense == NULL || len == 0)
		return;
	switch (sense[0] & 0x7f) {
	case 0x70:
	case 0x71:
		if (len < 14)
			return;
		key = sense[2] & 0x0f;
		asc = sense[12];
		break;
	case 0x72:
	case 0x73:
		if (len < 4)
			return;
		key = sense[1] & 0x0f;
		asc = sense[2];
		break;
	default:
		return;
	}

	switch (key) {
	case KEY_NO_SENSE:
	case KEY_RECOVERABLE_ERROR:
		*scp = NVME_CQE_SC_GEN_SUCCESS;
		break;
	case KEY_NOT_READY:
		*scp = NVME_CQE_SC_GEN_NVM_NS_NOTRDY;
		break;
	case KEY_MEDIUM_ERROR:
		*sctp = NVME_CQE_SCT_INTEGRITY;
		/* 11h is an unrecovered read error. */
		*scp = (asc == 0x11) ? NVME_CQE_SC_INT_NVM_READ :
		    NVME_CQE_SC_INT_NVM_WRITE;
		break;
	case KEY_ILLEGAL_REQUEST:
		if (asc == 0x21)
			*scp = NVME_CQE_SC_GEN_NVM_LBA_RANGE;
		else if (asc == 0x20)
			*scp = NVME_CQE_SC_GEN_INV_OPC;
		else if (asc == 0x25)
			*scp = NVME_CQE_SC_GEN_INV_NS;
		else
			*scp = NVME_CQE_SC_GEN_INV_FLD;
		break;
	case KEY_UNIT_ATTENTION:
		/* The LU has reported its condition; a retry succeeds. */
		*scp = NVME_CQE_SC_GEN_CMD_INTR;
		break;
	case KEY_WRITE_PROTECT:
		*scp = NVME_CQE_SC_GEN_NS_RDONLY;
		break;
	case KEY_ABORTED_COMMAND:
		*scp = NVME_CQE_SC_GEN_ABORT_REQUEST;
		break;
	case KEY_MISCOMPARE:
		*sctp = NVME_CQE_SCT_INTEGRITY;
		*scp = NVME_CQE_SC_INT_NVM_COMPARE;
		break;
	default:
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
	boolean_t good = (task->task_scsi_status == STATUS_GOOD);
	nvme_cqe_t cpl;
	uint8_t sct, sc;

	_NOTE(ARGUNUSED(ioflags));

	if (priv->ntp_iio != NULL) {
		nvmft_internal_send_status(task);
		return (STMF_SUCCESS);
	}

	/*
	 * If the host has its response, it was success with the last of the
	 * data, and a failure that the LU reports now is too late for it.
	 */
	if (priv->ntp_responded) {
		if (!good) {
			DTRACE_PROBE1(nvmft__late__failure, scsi_task_t *,
			    task);
		}
	} else {
		nvmft_scsi_to_nvme(task->task_scsi_status,
		    task->task_sense_data, task->task_sense_length, &sct, &sc);
		nvmft_init_cqe(&cpl, priv->ntp_nc, 0);
		cpl.cqe_sf.sf_sct = sct;
		cpl.cqe_sf.sf_sc = sc;
		(void) nvmft_send_response(priv->ntp_qp, &cpl);
		priv->ntp_responded = B_TRUE;
	}

	nvmft_task_finish(priv, good);
	stmf_send_status_done(task, STMF_SUCCESS, STMF_IOF_LPORT_DONE);
	return (STMF_SUCCESS);
}

/*
 * STMF calls this with the task's lock and the session's lock held, so the
 * deferred command that takes the place of this one is posted by ns_io_taskq.
 * So is the response to a command that ends without one, which is a command
 * that was aborted.
 */
void
nvmft_lport_task_free(scsi_task_t *task)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	nvmft_controller_t *ctrlr;
	nvmft_task_priv_t *d;

	if (priv == NULL)
		return;
	task->task_port_private = NULL;
	ctrlr = nvmft_task_ctrlr(priv);

	mutex_enter(&ctrlr->ctrlr_lock);
	nvmft_task_unlist_locked(ctrlr, priv);
	if (priv->ntp_iio != NULL) {
		ASSERT3U(ctrlr->ctrlr_internal_tasks, >, 0);
		if (--ctrlr->ctrlr_internal_tasks == 0)
			cv_broadcast(&ctrlr->ctrlr_pending_cv);
		mutex_exit(&ctrlr->ctrlr_lock);
		nvmft_internal_rele(priv->ntp_iio);
		return;
	}
	if (!priv->ntp_responded) {
		nvmft_io_work(ctrlr, priv);
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	d = nvmft_command_done(ctrlr, priv->ntp_data_len);
	if (d != NULL)
		nvmft_io_work(ctrlr, d);
	mutex_exit(&ctrlr->ctrlr_lock);

	nvmft_task_priv_free(priv);
}

/*
 * STMF asks us to abort a task, with the task's lock held.  While the
 * transport still holds a transfer into one of the task's dbufs this returns
 * STMF_BUSY, because STMF frees the dbufs of an aborted task, and STMF asks
 * again.  The host gets its response when STMF frees the task.
 */
stmf_status_t
nvmft_lport_abort(stmf_local_port_t *lport, int abort_cmd, void *arg,
    uint32_t flags)
{
	scsi_task_t *task = arg;
	nvmft_task_priv_t *priv;
	nvmft_controller_t *ctrlr;
	boolean_t busy;

	_NOTE(ARGUNUSED(lport, flags));

	if (abort_cmd != STMF_LPORT_ABORT_TASK)
		return (STMF_ABORT_SUCCESS);

	priv = task->task_port_private;
	if (priv == NULL)
		return (STMF_ABORT_SUCCESS);

	mutex_enter(&priv->ntp_lock);
	priv->ntp_aborting = B_TRUE;
	busy = (priv->ntp_xfers != 0);
	mutex_exit(&priv->ntp_lock);
	if (busy)
		return (STMF_BUSY);

	/* nvmft_abort_tasks() holds ctrlr_lock and waits for the task's. */
	ctrlr = nvmft_task_ctrlr(priv);
	if (!mutex_tryenter(&ctrlr->ctrlr_lock))
		return (STMF_BUSY);
	nvmft_task_unlist_locked(ctrlr, priv);
	mutex_exit(&ctrlr->ctrlr_lock);

	if (priv->ntp_iio != NULL)
		nvmft_internal_aborted(priv->ntp_iio);
	return (STMF_ABORT_SUCCESS);
}

void
nvmft_lport_task_poll(scsi_task_t *task)
{
	_NOTE(ARGUNUSED(task));
}

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
 * Provenance: ported to illumos from FreeBSD
 * sys/dev/nvmf/controller/nvmft_controller.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * The controller (host association) state machine: admin queue handoff, I/O
 * queue handoff, keep-alive, shutdown and termination.  The deferred-work
 * primitives (callout / taskqueue) become timeout(9F) and the shared nvmft
 * taskq.  Admin commands are in nvmft_admin.c.
 *
 *   FreeBSD                              illumos
 *   -------                              -------
 *   malloc(M_NVMFT, M_WAITOK|M_ZERO)     kmem_zalloc(..., KM_SLEEP)
 *   mtx_lock/unlock                      mutex_enter/exit
 *   callout_*                            timeout(9F) / untimeout(9F)
 *   TASK_INIT/taskqueue_enqueue          taskq_dispatch_ent on nvmft taskq
 *   TIMEOUT_TASK_INIT/_enqueue_timeout   timeout(9F) for the delayed terminate
 *   atomic_readandclear_int              atomic_swap_uint
 *   sbinuptime / mstosbt                 gethrtime / drv_usectohz
 *   m_getm2 / m_copyback                 allocb / mblk fill
 *   le32toh / htole16                    LE_32 / LE_16
 *   nvmf_send_controller_data(nc,off,m,len) same signature, mblk_t *m
 *
 * The log-page/Identify replies build an mblk_t chain and hand it to
 * nvmf_send_controller_data(), which matches the illumos transport contract
 * (see <sys/nvme/nvmf_transport.h>).
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
#include <sys/strsubr.h>
#include <sys/varargs.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include <sys/time.h>		/* hrtime_t (sys/stmf.h needs it) */
#include <sys/stmf.h>

#include "nvmft_var.h"


static void	nvmft_controller_shutdown(void *arg);
static void	nvmft_controller_terminate(void *arg);
static void	nvmft_controller_terminate_timeout(void *arg);

/*
 * cmn_err-style logging tagged with the controller id.  (FreeBSD: nvmft_printf
 * built an sbuf; here we format into a stack buffer and call cmn_err.)
 */
int
nvmft_printf(nvmft_controller_t *ctrlr, const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	int n, m;

	n = snprintf(buf, sizeof (buf), "nvmft%u: ",
	    ctrlr != NULL ? ctrlr->ctrlr_cntlid : 0);
	va_start(ap, fmt);
	m = vsnprintf(buf + n, sizeof (buf) - n, fmt, ap);
	va_end(ap);

	cmn_err(CE_NOTE, "!%s", buf);
	return (n + m);
}

static nvmft_controller_t *
nvmft_controller_alloc(nvmft_port_t *np, uint16_t cntlid,
    const nvmf_fabric_connect_data_t *data)
{
	nvmft_controller_t *ctrlr;

	ctrlr = kmem_zalloc(sizeof (*ctrlr), KM_SLEEP);
	ctrlr->ctrlr_cntlid = cntlid;
	ctrlr->ctrlr_np = np;
	mutex_init(&ctrlr->ctrlr_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ctrlr->ctrlr_pending_cv, NULL, CV_DRIVER, NULL);
	cv_init(&ctrlr->ctrlr_admin_cv, NULL, CV_DRIVER, NULL);
	nvmft_admin_init(ctrlr);
	nvmft_deferred_init(ctrlr);

	ctrlr->ctrlr_cdata = np->np_cdata;
	ctrlr->ctrlr_cdata.id_cntlid = LE_16(cntlid);
	(void) memcpy(ctrlr->ctrlr_hostid, data->nfcd_hostid,
	    sizeof (ctrlr->ctrlr_hostid));
	(void) memcpy(ctrlr->ctrlr_hostnqn, data->nfcd_hostnqn,
	    sizeof (ctrlr->ctrlr_hostnqn));
	/* FreeBSD: hip.power_cycles[0] = 1 (low 64 bits of the 128-bit count). */
	ctrlr->ctrlr_hip.hl_power_cycles.lo = 1;
	ctrlr->ctrlr_create_time = gethrtime();

	ctrlr->ctrlr_changed_ns = kmem_zalloc(
	    sizeof (*ctrlr->ctrlr_changed_ns), KM_SLEEP);

	/*
	 * Register the per-association STMF session now: stmf_task_alloc() in the
	 * I/O dispatch path dereferences task_session, so the nexus must exist
	 * before any command is handed to the LU.  Registration also enforces
	 * that the local port is online, subsuming the np_online check below.
	 */
	if (nvmft_session_register(ctrlr) != 0) {
		kmem_free(ctrlr->ctrlr_changed_ns,
		    sizeof (*ctrlr->ctrlr_changed_ns));
		nvmft_deferred_fini(ctrlr);
		nvmft_admin_fini(ctrlr);
		cv_destroy(&ctrlr->ctrlr_admin_cv);
		cv_destroy(&ctrlr->ctrlr_pending_cv);
		mutex_destroy(&ctrlr->ctrlr_lock);
		kmem_free(ctrlr, sizeof (*ctrlr));
		return (NULL);
	}

	return (ctrlr);
}

static void
nvmft_controller_free(nvmft_controller_t *ctrlr)
{
	ASSERT3P(ctrlr->ctrlr_io_qpairs, ==, NULL);
	nvmft_session_deregister(ctrlr);
	nvmft_deferred_fini(ctrlr);
	nvmft_admin_fini(ctrlr);
	cv_destroy(&ctrlr->ctrlr_admin_cv);
	cv_destroy(&ctrlr->ctrlr_pending_cv);
	mutex_destroy(&ctrlr->ctrlr_lock);
	kmem_free(ctrlr->ctrlr_changed_ns, sizeof (*ctrlr->ctrlr_changed_ns));
	kmem_free(ctrlr, sizeof (*ctrlr));
}

static void
nvmft_keep_alive_timer(void *arg)
{
	nvmft_controller_t *ctrlr = arg;
	uint_t traffic;

	if (ctrlr->ctrlr_shutdown)
		return;

	traffic = atomic_swap_uint(&ctrlr->ctrlr_ka_active_traffic, 0);
	if (traffic == 0) {
		(void) nvmft_printf(ctrlr,
		    "disconnecting due to KeepAlive timeout\n");
		nvmft_controller_error(ctrlr, NULL, ETIMEDOUT);
		return;
	}

	/*
	 * Reschedule under ctrlr_lock so the published timer id is synchronized
	 * with the cancel paths, and so we do not re-arm after a shutdown has
	 * begun (which would leak a timer the cancel paths already observed as
	 * cleared).
	 */
	mutex_enter(&ctrlr->ctrlr_lock);
	if (!ctrlr->ctrlr_shutdown)
		ctrlr->ctrlr_ka_timer = timeout(nvmft_keep_alive_timer, ctrlr,
		    ctrlr->ctrlr_ka_ticks);
	mutex_exit(&ctrlr->ctrlr_lock);
}

static void
nvmft_update_cdata(nvmft_controller_t *ctrlr)
{
	uint32_t val, ioccsz, le_ioccsz;
	uint64_t mdts;
	uint_t shift;

	/*
	 * Clamp Identify Controller IOCCSZ to the transport's maximum in-capsule
	 * command size (FreeBSD nvmft_update_cdata).  The NVMe-oF transport
	 * region of nvme_identify_ctrl_t is exposed by <sys/nvme.h> as the opaque
	 * id_nvmof[] byte array; IOCCSZ is a little-endian uint32 at id_nvmof[0]
	 * expressed in 16-byte units (see nvmft_subr.c).  nvmft_max_ioccsz()
	 * returns the limit in bytes.
	 */
	nvmft_init_sgls(&ctrlr->ctrlr_cdata,
	    nvmft_qpair_caps(ctrlr->ctrlr_admin));

	/* MDTS is a power of two in units of the 4 KiB CAP.MPSMIN page. */
	mdts = nvmft_max_xfer_size(ctrlr->ctrlr_admin) / 4096;
	if (mdts != 0) {
		for (shift = 0; shift < 15 && (2ULL << shift) <= mdts; shift++)
			;
		ctrlr->ctrlr_cdata.id_mdts = (uint8_t)shift;
	}

	val = nvmft_max_ioccsz(ctrlr->ctrlr_admin);
	if (val == 0)
		return;

	(void) memcpy(&le_ioccsz, &ctrlr->ctrlr_cdata.id_nvmof[0],
	    sizeof (le_ioccsz));
	ioccsz = LE_32(le_ioccsz) * 16;
	if (val < ioccsz) {
		le_ioccsz = LE_32(val / 16);
		(void) memcpy(&ctrlr->ctrlr_cdata.id_nvmof[0], &le_ioccsz,
		    sizeof (le_ioccsz));
	}
}

int
nvmft_handoff_admin_queue(nvmft_port_t *np, nvmf_trtype_t trtype,
    const nvlist_t *params, const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data)
{
	struct nvmft_qpair *qp;

	if (cmd->nfcc_qid != LE_16(0))
		return (EINVAL);

	qp = nvmft_qpair_init(trtype, params, 0, "admin queue");
	if (qp == NULL) {
		NVMFT_DPRINTF_L1("Failed to setup admin queue from %.*s",
		    (int)sizeof (data->nfcd_hostnqn), data->nfcd_hostnqn);
		return (ENXIO);
	}

	return (nvmft_connect_admin_queue(np, qp, cmd, data));
}

/*
 * Create the association for a new admin queue.  On failure this destroys qp,
 * after sending a Connect error where the handoff path did.
 */
int
nvmft_connect_admin_queue(nvmft_port_t *np, struct nvmft_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data)
{
	nvmft_controller_t *ctrlr;
	uint32_t kato;
	id_t cntlid;

	mutex_enter(&np->np_lock);
	cntlid = id_alloc_nosleep(np->np_ids);
	if (cntlid == -1) {
		mutex_exit(&np->np_lock);
		NVMFT_DPRINTF_L1("Unable to allocate controller for %.*s",
		    (int)sizeof (data->nfcd_hostnqn), data->nfcd_hostnqn);
		nvmft_connect_error(qp, cmd, NVME_CQE_SCT_SPECIFIC,
		    NVMF_FABRIC_SC_INVALID_HOST);
		nvmft_qpair_destroy(qp);
		return (ENOMEM);
	}
	mutex_exit(&np->np_lock);

	ctrlr = nvmft_controller_alloc(np, (uint16_t)cntlid, data);
	if (ctrlr == NULL) {
		/*
		 * The only failure is STMF session registration, which fails
		 * when the local port is not online; reject the association.
		 */
		id_free(np->np_ids, cntlid);
		nvmft_connect_error(qp, cmd, NVME_CQE_SCT_GENERIC,
		    NVME_CQE_SC_GEN_INTERNAL_ERR);
		nvmft_qpair_destroy(qp);
		return (ENXIO);
	}

	mutex_enter(&np->np_lock);
	if (!np->np_online) {
		mutex_exit(&np->np_lock);
		nvmft_controller_free(ctrlr);
		id_free(np->np_ids, cntlid);
		nvmft_qpair_destroy(qp);
		return (ENXIO);
	}
	np->np_refs++;
	list_insert_tail(&np->np_controllers, ctrlr);

	(void) nvmft_printf(ctrlr, "associated with %.*s\n",
	    (int)sizeof (data->nfcd_hostnqn), data->nfcd_hostnqn);
	ctrlr->ctrlr_admin = qp;
	ctrlr->ctrlr_trtype = nvmft_qpair_trtype(qp);
	nvmft_update_cdata(ctrlr);

	/*
	 * The spec requires a non-zero KeepAlive timer, but allow a zero KATO
	 * to match Linux.  KATO is in milliseconds; round up to 1s granularity.
	 */
	kato = LE_32(cmd->nfcc_kato);
	if (kato != 0) {
		/*
		 * Round KATO (ms) up to 1s granularity.  P2ROUNDUP() is only
		 * valid for power-of-2 alignments, and 1000 is not, so compute
		 * the round-up directly; do it in 64-bit so a host-chosen KATO
		 * near UINT32_MAX cannot overflow to 0 (which would tear the
		 * association down immediately).
		 */
		uint64_t kato_ms = ((uint64_t)kato + 999) / 1000 * 1000;
		ctrlr->ctrlr_ka_ticks =
		    drv_usectohz((clock_t)(kato_ms * 1000));
		ctrlr->ctrlr_ka_timer = timeout(nvmft_keep_alive_timer, ctrlr,
		    ctrlr->ctrlr_ka_ticks);
	}
	mutex_exit(&np->np_lock);

	(void) nvmft_finish_accept(qp, cmd, ctrlr);

	return (0);
}

int
nvmft_handoff_io_queue(nvmft_port_t *np, nvmf_trtype_t trtype,
    const nvlist_t *params, const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data)
{
	struct nvmft_qpair *qp;
	char name[16];
	uint16_t qid;

	qid = LE_16(cmd->nfcc_qid);
	if (qid == 0)
		return (EINVAL);

	(void) snprintf(name, sizeof (name), "I/O queue %u", qid);
	qp = nvmft_qpair_init(trtype, params, qid, name);
	if (qp == NULL) {
		NVMFT_DPRINTF_L1("Failed to setup I/O queue %u from %.*s", qid,
		    (int)sizeof (data->nfcd_hostnqn), data->nfcd_hostnqn);
		return (ENXIO);
	}

	return (nvmft_connect_io_queue(np, qp, cmd, data));
}

/*
 * Attach a new I/O queue to its association.  On failure this destroys qp,
 * after sending a Connect error.
 */
int
nvmft_connect_io_queue(nvmft_port_t *np, struct nvmft_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data)
{
	nvmft_controller_t *ctrlr;
	uint16_t cntlid, qid;

	qid = LE_16(cmd->nfcc_qid);
	cntlid = LE_16(data->nfcd_cntlid);

	mutex_enter(&np->np_lock);
	for (ctrlr = list_head(&np->np_controllers); ctrlr != NULL;
	    ctrlr = list_next(&np->np_controllers, ctrlr)) {
		if (ctrlr->ctrlr_cntlid == cntlid)
			break;
	}
	if (ctrlr == NULL) {
		mutex_exit(&np->np_lock);
		nvmft_connect_invalid_parameters(qp, cmd, B_TRUE,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_cntlid));
		nvmft_qpair_destroy(qp);
		return (ENOENT);
	}

	if (ctrlr->ctrlr_trtype != nvmft_qpair_trtype(qp)) {
		mutex_exit(&np->np_lock);
		nvmft_connect_invalid_parameters(qp, cmd, B_TRUE,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_cntlid));
		nvmft_qpair_destroy(qp);
		return (EINVAL);
	}
	if (memcmp(ctrlr->ctrlr_hostid, data->nfcd_hostid,
	    sizeof (ctrlr->ctrlr_hostid)) != 0) {
		mutex_exit(&np->np_lock);
		(void) nvmft_printf(ctrlr,
		    "hostid mismatch for I/O queue %u\n", qid);
		nvmft_connect_invalid_parameters(qp, cmd, B_TRUE,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_hostid));
		nvmft_qpair_destroy(qp);
		return (EINVAL);
	}
	if (memcmp(ctrlr->ctrlr_hostnqn, data->nfcd_hostnqn,
	    sizeof (ctrlr->ctrlr_hostnqn)) != 0) {
		mutex_exit(&np->np_lock);
		(void) nvmft_printf(ctrlr,
		    "hostnqn mismatch for I/O queue %u\n", qid);
		nvmft_connect_invalid_parameters(qp, cmd, B_TRUE,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_hostnqn));
		nvmft_qpair_destroy(qp);
		return (EINVAL);
	}

	mutex_enter(&ctrlr->ctrlr_lock);
	if (ctrlr->ctrlr_shutdown) {
		mutex_exit(&ctrlr->ctrlr_lock);
		mutex_exit(&np->np_lock);
		nvmft_connect_invalid_parameters(qp, cmd, B_TRUE,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_cntlid));
		nvmft_qpair_destroy(qp);
		return (EINVAL);
	}
	if (ctrlr->ctrlr_num_io_queues == 0) {
		mutex_exit(&ctrlr->ctrlr_lock);
		mutex_exit(&np->np_lock);
		nvmft_connect_error(qp, cmd, NVME_CQE_SCT_GENERIC,
		    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
		nvmft_qpair_destroy(qp);
		return (EINVAL);
	}
	if (qid > ctrlr->ctrlr_num_io_queues) {
		mutex_exit(&ctrlr->ctrlr_lock);
		mutex_exit(&np->np_lock);
		nvmft_connect_invalid_parameters(qp, cmd, B_FALSE,
		    offsetof(nvmf_fabric_connect_cmd_t, nfcc_qid));
		nvmft_qpair_destroy(qp);
		return (EINVAL);
	}
	if (ctrlr->ctrlr_io_qpairs[qid - 1].nio_qp != NULL) {
		mutex_exit(&ctrlr->ctrlr_lock);
		mutex_exit(&np->np_lock);
		nvmft_connect_error(qp, cmd, NVME_CQE_SCT_GENERIC,
		    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
		nvmft_qpair_destroy(qp);
		return (EINVAL);
	}

	ctrlr->ctrlr_io_qpairs[qid - 1].nio_qp = qp;
	mutex_exit(&ctrlr->ctrlr_lock);
	mutex_exit(&np->np_lock);
	(void) nvmft_finish_accept(qp, cmd, ctrlr);

	return (0);
}

static void
nvmft_controller_shutdown(void *arg)
{
	nvmft_controller_t *ctrlr = arg;
	nvme_reg_csts_t csts;

	/*
	 * Shutdown all I/O queues to terminate pending datamoves and stop
	 * receiving new commands.
	 */
	mutex_enter(&ctrlr->ctrlr_lock);
	for (uint_t i = 0; i < ctrlr->ctrlr_num_io_queues; i++) {
		if (ctrlr->ctrlr_io_qpairs[i].nio_qp != NULL) {
			ctrlr->ctrlr_io_qpairs[i].nio_shutdown = B_TRUE;
			mutex_exit(&ctrlr->ctrlr_lock);
			nvmft_qpair_shutdown(ctrlr->ctrlr_io_qpairs[i].nio_qp);
			mutex_enter(&ctrlr->ctrlr_lock);
		}
	}
	mutex_exit(&ctrlr->ctrlr_lock);

	/*
	 * The I/O qpairs are shut down, so no new command can arrive or be
	 * deferred.  Discard any commands still queued for backpressure (they were
	 * never posted to STMF) before draining the in-flight ones below.
	 */
	nvmft_deferred_drain(ctrlr);

	/* Terminate active STMF tasks. */
	nvmft_terminate_commands(ctrlr);

	/* Wait for all pending STMF commands to complete. */
	mutex_enter(&ctrlr->ctrlr_lock);
	while (ctrlr->ctrlr_pending_commands != 0)
		(void) cv_timedwait(&ctrlr->ctrlr_pending_cv,
		    &ctrlr->ctrlr_lock, ddi_get_lbolt() + drv_usectohz(10000));
	mutex_exit(&ctrlr->ctrlr_lock);

	/* Delete all of the I/O queues. */
	for (uint_t i = 0; i < ctrlr->ctrlr_num_io_queues; i++) {
		if (ctrlr->ctrlr_io_qpairs[i].nio_qp != NULL)
			nvmft_qpair_destroy(ctrlr->ctrlr_io_qpairs[i].nio_qp);
	}
	kmem_free(ctrlr->ctrlr_io_qpairs,
	    ctrlr->ctrlr_num_io_queues * sizeof (nvmft_io_qpair_t));
	ctrlr->ctrlr_io_qpairs = NULL;

	mutex_enter(&ctrlr->ctrlr_lock);
	ctrlr->ctrlr_num_io_queues = 0;

	csts.r = ctrlr->ctrlr_csts;
	/* Mark shutdown complete if it was in progress. */
	if (csts.b.csts_shst == NVME_CSTS_SHN_OCCURING)
		csts.b.csts_shst = NVME_CSTS_SHN_COMPLETE;
	/* Drop ready (and clear the shutting-down flag) unless fatal. */
	if (csts.b.csts_cfs == 0) {
		csts.b.csts_rdy = 0;
		ctrlr->ctrlr_shutdown = B_FALSE;
	}
	ctrlr->ctrlr_csts = csts.r;
	mutex_exit(&ctrlr->ctrlr_lock);

	/*
	 * If the admin queue was closed while shutting down or a fatal
	 * controller error occurred, terminate the association immediately;
	 * otherwise wait up to 2 minutes (NVMe-oF 1.1 section 4.6).
	 *
	 * The immediate path runs inline because this function already executes
	 * on ns_taskq (thread context), so the blocking work in
	 * nvmft_controller_terminate() is legal here.  The delayed path arms a
	 * timeout(9F) on nvmft_controller_terminate_timeout(), which only
	 * re-dispatches the blocking work onto ns_taskq rather than running it
	 * in callout context.
	 */
	mutex_enter(&ctrlr->ctrlr_lock);
	if (ctrlr->ctrlr_admin_closed || csts.b.csts_cfs != 0) {
		/*
		 * Terminate immediately, inline.  Claim ctrlr_terminate_queued
		 * first so a graceful terminate timer that fires concurrently
		 * cannot also dispatch the terminate task (double run on the same
		 * controller); if a terminate is already queued/running, let it
		 * proceed rather than running a second one here.
		 */
		boolean_t run = !ctrlr->ctrlr_terminate_queued;
		ctrlr->ctrlr_terminate_queued = B_TRUE;
		mutex_exit(&ctrlr->ctrlr_lock);
		if (run)
			nvmft_controller_terminate(ctrlr);
	} else {
		/*
		 * Arm the 2-minute graceful terminate under ctrlr_lock so the
		 * published timer id stays synchronized with the cancel paths
		 * (nvmft_controller_error / terminate).  An unlocked store here
		 * races those and can leave a second, uncancelled callout ->
		 * double nvmft_controller_terminate -> controller double-free.
		 */
		ctrlr->ctrlr_terminate_timer = timeout(
		    nvmft_controller_terminate_timeout, ctrlr,
		    drv_usectohz(2 * 60 * 1000000));
		mutex_exit(&ctrlr->ctrlr_lock);
	}
}

/*
 * timeout(9F) trampoline for the delayed terminate.  Runs in callout context,
 * where blocking (taskq_wait / KM_SLEEP / nvmf_free_qpair) is illegal, so it
 * merely re-dispatches the real terminate work onto ns_taskq (thread context).
 * This mirrors FreeBSD's TIMEOUT_TASK landing on taskqueue_thread.
 */
static void
nvmft_controller_terminate_timeout(void *arg)
{
	nvmft_controller_t *ctrlr = arg;

	mutex_enter(&ctrlr->ctrlr_lock);
	ctrlr->ctrlr_terminate_timer = 0;	/* this timer has now fired */
	if (ctrlr->ctrlr_terminate_queued) {
		/* A terminate is already queued/running; do not re-dispatch. */
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	ctrlr->ctrlr_terminate_queued = B_TRUE;
	mutex_exit(&ctrlr->ctrlr_lock);

	taskq_dispatch_ent(nvmft_global->ns_taskq, nvmft_controller_terminate,
	    ctrlr, 0, &ctrlr->ctrlr_terminate_task);
}

static void
nvmft_controller_terminate(void *arg)
{
	nvmft_controller_t *ctrlr = arg;
	nvmft_port_t *np;
	nvme_reg_cc_t cc;
	timeout_id_t ka_timer;
	timeout_id_t term_timer;

	/* If the controller has been re-enabled, nothing to do. */
	mutex_enter(&ctrlr->ctrlr_lock);
	cc.r = ctrlr->ctrlr_cc;
	if (cc.b.cc_en != 0) {
		/*
		 * Re-enabled: do not free.  Clear ctrlr_terminate_queued so a
		 * later shutdown can schedule terminate again, and re-arm the
		 * keep-alive timer under ctrlr_lock so the published id stays
		 * synchronized with nvmft_keep_alive_timer() and the cancel
		 * paths.
		 */
		ctrlr->ctrlr_terminate_queued = B_FALSE;
		if (ctrlr->ctrlr_ka_ticks != 0)
			ctrlr->ctrlr_ka_timer = timeout(nvmft_keep_alive_timer,
			    ctrlr, ctrlr->ctrlr_ka_ticks);
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}

	/* Disable updates to CC while destroying the admin qpair. */
	ctrlr->ctrlr_shutdown = B_TRUE;
	mutex_exit(&ctrlr->ctrlr_lock);

	nvmft_admin_stop(ctrlr);
	nvmft_qpair_destroy(ctrlr->ctrlr_admin);

	/* Remove the association (CNTLID). */
	np = ctrlr->ctrlr_np;
	mutex_enter(&np->np_lock);
	list_remove(&np->np_controllers, ctrlr);
	/*
	 * FreeBSD wakeup(np): the STMF OFFLINE path (nvmft_lport_ctl() in
	 * nvmft_stmf.c) faults every controller and then cv_wait()s on
	 * np_controllers_cv under np_lock until np_controllers drains.  Wake it
	 * here, while still holding np_lock, once the list empties on an offline
	 * port so the waiter re-checks list_is_empty() under the same lock.
	 *
	 * HEADER-NEEDED: kcondvar_t np_controllers_cv must be added to
	 * nvmft_port_t in nvmft_var.h and cv_init()/cv_destroy()'d in
	 * nvmft_port_alloc()/nvmft_port_free() (nvmft_stmf.c), which already
	 * references this condvar in its OFFLINE waiter.
	 */
	if (!np->np_online && list_is_empty(&np->np_controllers))
		cv_broadcast(&np->np_controllers_cv);
	mutex_exit(&np->np_lock);
	id_free(np->np_ids, ctrlr->ctrlr_cntlid);

	/*
	 * Snapshot and clear the keep-alive and (delayed) terminate ids under
	 * the lock before cancel.  Cancelling our own pending terminate timer
	 * here is what prevents a still-armed 2-minute timer from later firing
	 * on this freed controller (use-after-free / double terminate): if this
	 * terminate was triggered inline (fatal error / admin close) or by the
	 * keep-alive path, the graceful-shutdown timer may still be pending.
	 * untimeout() of an already-fired id returns -1 harmlessly.
	 */
	mutex_enter(&ctrlr->ctrlr_lock);
	ka_timer = ctrlr->ctrlr_ka_timer;
	ctrlr->ctrlr_ka_timer = 0;
	term_timer = ctrlr->ctrlr_terminate_timer;
	ctrlr->ctrlr_terminate_timer = 0;
	mutex_exit(&ctrlr->ctrlr_lock);
	if (ka_timer != 0)
		(void) untimeout(ka_timer);
	if (term_timer != 0)
		(void) untimeout(term_timer);

	(void) nvmft_printf(ctrlr, "association terminated\n");
	nvmft_controller_free(ctrlr);
	nvmft_port_rele(np);
}

void
nvmft_controller_error(nvmft_controller_t *ctrlr, struct nvmft_qpair *qp,
    int error)
{
	nvme_reg_cc_t cc;
	nvme_reg_csts_t csts;
	timeout_id_t ka_timer;
	timeout_id_t term_timer;

	/*
	 * If a queue pair is closed, that isn't an error per se; it just means
	 * no more commands can be received on it.  Closing the admin queue
	 * while idle or shutting down terminates the association; closing an
	 * I/O queue is ignored.
	 */
	if (error == 0) {
		if (qp != ctrlr->ctrlr_admin)
			return;

		mutex_enter(&ctrlr->ctrlr_lock);
		if (ctrlr->ctrlr_shutdown) {
			ctrlr->ctrlr_admin_closed = B_TRUE;
			mutex_exit(&ctrlr->ctrlr_lock);
			return;
		}

		if (NVMFT_CC_EN(ctrlr->ctrlr_cc) == 0) {
			timeout_id_t old;

			ASSERT3U(ctrlr->ctrlr_num_io_queues, ==, 0);

			/*
			 * Safe to schedule terminate directly: no I/O queues to
			 * tear down.  Cannot call terminate inline here since
			 * this is invoked from the transport layer and freeing
			 * the admin qpair might deadlock.  Schedule via the
			 * timeout trampoline so the blocking terminate work runs
			 * on ns_taskq, not in callout context.
			 *
			 * Cancel any pending graceful-terminate timer and arm an
			 * immediate one, keeping ctrlr_terminate_timer consistent
			 * under ctrlr_lock so a still-armed callout is always
			 * cancelled before a new one is published (else two live
			 * callouts -> double terminate -> controller double-free).
			 * untimeout() must run without the lock the callout may
			 * block on, so snapshot-and-clear under the lock, drop it
			 * to untimeout, then re-arm under the lock.
			 */
			old = ctrlr->ctrlr_terminate_timer;
			ctrlr->ctrlr_terminate_timer = 0;
			mutex_exit(&ctrlr->ctrlr_lock);
			if (old != 0)
				(void) untimeout(old);
			mutex_enter(&ctrlr->ctrlr_lock);
			ctrlr->ctrlr_terminate_timer = timeout(
			    nvmft_controller_terminate_timeout, ctrlr, 0);
			mutex_exit(&ctrlr->ctrlr_lock);
			return;
		}

		ctrlr->ctrlr_admin_closed = B_TRUE;
	} else {
		mutex_enter(&ctrlr->ctrlr_lock);
	}

	/* Ignore transport errors while already shutting down. */
	if (ctrlr->ctrlr_shutdown) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}

	csts.r = ctrlr->ctrlr_csts;
	csts.b.csts_cfs = 1;			/* fatal status */
	ctrlr->ctrlr_csts = csts.r;
	cc.r = ctrlr->ctrlr_cc;
	cc.b.cc_en = 0;				/* disable */
	ctrlr->ctrlr_cc = cc.r;
	ctrlr->ctrlr_shutdown = B_TRUE;
	/*
	 * Snapshot and clear the keep-alive and any pending graceful-shutdown
	 * terminate id under ctrlr_lock so the cancels are synchronized against
	 * nvmft_keep_alive_timer() re-arming and against the delayed terminate
	 * firing.  Cancelling the terminate timer here is essential: this path
	 * is about to dispatch nvmft_controller_shutdown, which (because cfs is
	 * now set) terminates inline and frees the controller; a still-pending
	 * terminate timer would otherwise fire on freed memory.  Once
	 * ctrlr_shutdown is set neither timer routine reschedules.
	 */
	ka_timer = ctrlr->ctrlr_ka_timer;
	ctrlr->ctrlr_ka_timer = 0;
	term_timer = ctrlr->ctrlr_terminate_timer;
	ctrlr->ctrlr_terminate_timer = 0;
	mutex_exit(&ctrlr->ctrlr_lock);

	if (ka_timer != 0)
		(void) untimeout(ka_timer);
	if (term_timer != 0)
		(void) untimeout(term_timer);
	taskq_dispatch_ent(nvmft_global->ns_taskq, nvmft_controller_shutdown,
	    ctrlr, 0, &ctrlr->ctrlr_shutdown_task);
}

/*
 * Start the shutdown that update_cc() requested by setting ctrlr_shutdown,
 * which also keeps the keep-alive timer from rearming.
 */
void
nvmft_controller_start_shutdown(nvmft_controller_t *ctrlr)
{
	timeout_id_t ka_timer;
	timeout_id_t term_timer;

	mutex_enter(&ctrlr->ctrlr_lock);
	ka_timer = ctrlr->ctrlr_ka_timer;
	ctrlr->ctrlr_ka_timer = 0;
	term_timer = ctrlr->ctrlr_terminate_timer;
	ctrlr->ctrlr_terminate_timer = 0;
	mutex_exit(&ctrlr->ctrlr_lock);
	if (ka_timer != 0)
		(void) untimeout(ka_timer);
	if (term_timer != 0)
		(void) untimeout(term_timer);
	taskq_dispatch_ent(nvmft_global->ns_taskq, nvmft_controller_shutdown,
	    ctrlr, 0, &ctrlr->ctrlr_shutdown_task);
}

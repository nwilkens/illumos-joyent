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
 * queue handoff, keep-alive, Get Log Page / Identify / Set Features / property
 * get-set admin command handling, AER, controller enable/disable/shutdown.  The
 * protocol logic is ported faithfully; the data-movement helpers (mbuf-based
 * log-page replies) become mblk(9) chains, and the deferred-work primitives
 * (callout / taskqueue) become timeout(9F) and the shared nvmft taskq.
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

/*
 * Controller Configuration (CC) and Controller Status (CSTS) are stored as raw
 * uint32_t in nvmft_controller_t.  These accessors extract the spec-defined
 * fields without needing an lvalue of the nvme_reg_cc_t / nvme_reg_csts_t union.
 */
#define	NVMFT_CC_EN(cc)		((cc) & 0x1u)		/* Enable, bit 0 */
#define	NVMFT_CC_SHN(cc)	(((cc) >> 14) & 0x3u)	/* Shutdown Notify */
#define	NVMFT_CSTS_CFS(csts)	(((csts) >> 1) & 0x1u)	/* Fatal Status */

/*
 * NVM Verify command opcode (NVMe NVM command set).  io/nvme/nvme_reg.h does not
 * yet define this opcode; the spec fixes it at 0x0c.  (FreeBSD: NVME_OPC_VERIFY.)
 */
#define	NVMFT_OPC_NVM_VERIFY	0x0c

/*
 * Asynchronous Event Configuration enable bit for the Namespace Attribute
 * Notices event (Set Features FID 0x0b, cdw11 bit 8).  illumos's headers do not
 * define this AER-config mask.  (FreeBSD: NVME_ASYNC_EVENT_NS_ATTRIBUTE.)
 */
#define	NVMFT_AER_NS_ATTRIBUTE	(1u << 8)

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

	/*
	 * Clamp Identify Controller IOCCSZ to the transport's maximum in-capsule
	 * command size (FreeBSD nvmft_update_cdata).  The NVMe-oF transport
	 * region of nvme_identify_ctrl_t is exposed by <sys/nvme.h> as the opaque
	 * id_nvmof[] byte array; IOCCSZ is a little-endian uint32 at id_nvmof[0]
	 * expressed in 16-byte units (see nvmft_subr.c).  nvmft_max_ioccsz()
	 * returns the limit in bytes.
	 */
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
	nvmft_controller_t *ctrlr;
	struct nvmft_qpair *qp;
	uint32_t kato;
	id_t cntlid;

	if (cmd->nfcc_qid != LE_16(0))
		return (EINVAL);

	qp = nvmft_qpair_init(trtype, params, 0, "admin queue");
	if (qp == NULL) {
		NVMFT_DPRINTF_L1("Failed to setup admin queue from %.*s",
		    (int)sizeof (data->nfcd_hostnqn), data->nfcd_hostnqn);
		return (ENXIO);
	}

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
	ctrlr->ctrlr_trtype = trtype;
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
	nvmft_controller_t *ctrlr;
	struct nvmft_qpair *qp;
	char name[16];
	uint16_t cntlid, qid;

	qid = LE_16(cmd->nfcc_qid);
	if (qid == 0)
		return (EINVAL);
	cntlid = LE_16(data->nfcd_cntlid);

	(void) snprintf(name, sizeof (name), "I/O queue %u", qid);
	qp = nvmft_qpair_init(trtype, params, qid, name);
	if (qp == NULL) {
		NVMFT_DPRINTF_L1("Failed to setup I/O queue %u from %.*s", qid,
		    (int)sizeof (data->nfcd_hostnqn), data->nfcd_hostnqn);
		return (ENXIO);
	}

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
 * Allocate an mblk of len bytes with the leading region filled from src (todo
 * bytes) and the remainder zero-filled.  Replaces the FreeBSD m_getml /
 * m_copyback / m_zero pattern used by the log-page builders.  FreeBSD allocated
 * with M_WAITOK, so block (allocb_wait, STR_NOSIG) rather than fail; NULL is
 * only returned if the request is too large to ever satisfy, which the callers
 * turn into an Internal Error completion.
 */
static mblk_t *
nvmft_alloc_data(const void *src, size_t todo, size_t len)
{
	mblk_t *mp;
	int error = 0;

	/*
	 * STR_NOSIG forces a KM_SLEEP-backed allocation, so this does not return
	 * NULL in practice; callers still treat a NULL result as an Internal
	 * Error completion in case that ever changes.
	 */
	mp = allocb_wait(len, BPRI_MED, STR_NOSIG, &error);
	if (mp == NULL)
		return (NULL);
	if (todo > 0)
		(void) memcpy(mp->b_wptr, src, todo);
	if (len > todo)
		(void) bzero(mp->b_wptr + todo, len - todo);
	mp->b_wptr += len;
	return (mp);
}

/*
 * Allocate a fully zero-filled mblk of len bytes.  Used where the source must
 * be copied in under a lock that cannot be held across the blocking
 * allocb_wait(): the caller allocates here first, then memcpy's the leading
 * todo bytes into mp->b_rptr while holding the lock (FreeBSD allocates the mbuf
 * before mtx_lock for exactly this reason; see the NSCHANGE log page).
 */
static mblk_t *
nvmft_alloc_zeroed(size_t len)
{
	return (nvmft_alloc_data(NULL, 0, len));
}

static void
handle_get_log_page(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd)
{
	mblk_t *mp;
	uint64_t offset;
	uint32_t numd;
	size_t len, todo;
	uint_t status;
	uint8_t lid;
	boolean_t rae;

	lid = LE_32(cmd->sqe_cdw10) & 0xff;
	rae = (LE_32(cmd->sqe_cdw10) & (1U << 15)) != 0;
	numd = (LE_32(cmd->sqe_cdw10) >> 16) | (LE_32(cmd->sqe_cdw11) << 16);
	offset = LE_32(cmd->sqe_cdw12) |
	    ((uint64_t)LE_32(cmd->sqe_cdw13) << 32);

	/*
	 * The Get Log Page offset (LPOL/LPOU) must be DWORD aligned.  FreeBSD
	 * uses "offset % 3" here, which is a long-standing upstream bug (it
	 * accepts a misaligned offset of 6 and rejects an aligned offset of 8);
	 * the spec requires a multiple of 4.  DIVERGENCE: fixed here as % 4;
	 * worth reporting upstream so both trees converge.
	 */
	if (offset % 4 != 0) {
		status = NVME_CQE_SC_GEN_INV_FLD;
		goto done;
	}

	len = (numd + 1) * 4;

	/*
	 * NUMD is host-controlled; (numd+1)*4 can reach ~4 GiB and would drive
	 * an unbounded kmem allocation below.  Reject oversize requests; no
	 * supported log page approaches NVMFT_MAX_LOGPAGE_LEN.
	 */
	if (len > NVMFT_MAX_LOGPAGE_LEN) {
		status = NVME_CQE_SC_GEN_INV_FLD;
		goto done;
	}

	switch (lid) {
	case NVME_LOGPAGE_ERROR:
		mp = nvmft_alloc_zeroed(len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}
		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	case NVME_LOGPAGE_HEALTH: {
		nvme_health_log_t hip;

		if (offset >= sizeof (hip)) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			goto done;
		}
		todo = sizeof (hip) - offset;
		if (todo > len)
			todo = len;

		mutex_enter(&ctrlr->ctrlr_lock);
		hip = ctrlr->ctrlr_hip;
		/*
		 * FreeBSD reports Controller Busy Time in minutes and Power On
		 * Hours in hours.  ctrlr_busy_total and ctrlr_create_time are
		 * hrtime_t nanoseconds; the low 64 bits of the 128-bit fields are
		 * sufficient.
		 */
		hip.hl_ctrl_busy.lo = (ctrlr->ctrlr_busy_total / NANOSEC) / 60;
		hip.hl_power_on_hours.lo =
		    ((gethrtime() - ctrlr->ctrlr_create_time) / NANOSEC) / 3600;
		mutex_exit(&ctrlr->ctrlr_lock);

		mp = nvmft_alloc_data((char *)&hip + offset, todo, len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}
		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	case NVME_LOGPAGE_FWSLOT:
		if (offset >= sizeof (ctrlr->ctrlr_np->np_fp)) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			goto done;
		}
		todo = sizeof (ctrlr->ctrlr_np->np_fp) - offset;
		if (todo > len)
			todo = len;
		mp = nvmft_alloc_data((char *)&ctrlr->ctrlr_np->np_fp + offset,
		    todo, len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}
		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	case NVME_LOGPAGE_NSCHANGE:
		if (offset >= sizeof (*ctrlr->ctrlr_changed_ns)) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			goto done;
		}
		todo = sizeof (*ctrlr->ctrlr_changed_ns) - offset;
		if (todo > len)
			todo = len;

		/*
		 * Allocate the (zero-filled) reply mblk before taking
		 * ctrlr_lock: allocb_wait() can block and sleeping with a driver
		 * mutex held is illegal.  Under the lock we only do the
		 * non-blocking copy/bzero/flag update (FreeBSD likewise allocates
		 * the mbuf before mtx_lock here).
		 */
		mp = nvmft_alloc_zeroed(len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}

		mutex_enter(&ctrlr->ctrlr_lock);
		(void) memcpy(mp->b_rptr,
		    (char *)ctrlr->ctrlr_changed_ns + offset, todo);
		if (offset == 0 && len == sizeof (*ctrlr->ctrlr_changed_ns))
			(void) bzero(ctrlr->ctrlr_changed_ns,
			    sizeof (*ctrlr->ctrlr_changed_ns));
		if (!rae)
			ctrlr->ctrlr_changed_ns_reported = B_FALSE;
		mutex_exit(&ctrlr->ctrlr_lock);

		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	/*
	 * PORT-TODO (NVMEOF.md 9.4): add case NVME_LOGPAGE_ANA to serve the ANA
	 * log page from nvmft_ana_build_log_page().
	 */
	default:
		(void) nvmft_printf(ctrlr,
		    "Unsupported page 0x%x for GET_LOG_PAGE\n", lid);
		status = NVME_CQE_SC_GEN_INV_FLD;
		break;
	}

done:
	if (status == NVMF_SUCCESS_SENT)
		nvmft_command_completed(ctrlr->ctrlr_admin, nc);
	else
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc, status);
	nvmf_free_capsule(nc);
}

static void
handle_identify_command(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd)
{
	mblk_t *mp;
	size_t data_len;
	uint_t status;
	uint8_t cns;

	cns = LE_32(cmd->sqe_cdw10) & 0xFF;
	data_len = nvmf_capsule_data_len(nc);
	if (data_len != sizeof (ctrlr->ctrlr_cdata)) {
		(void) nvmft_printf(ctrlr,
		    "Invalid length %zu for IDENTIFY with CNS 0x%x\n", data_len,
		    cns);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		return;
	}

	switch (cns) {
	case 0: {	/* Identify Namespace. */
		nvme_identify_nsid_t *nsdata;
		uint32_t nsid = LE_32(cmd->sqe_nsid);

		/*
		 * Synthesize Identify Namespace from the STMF LU geometry
		 * (nvmft_stmf.c).  An inactive/unallocated NSID returns a
		 * zero-filled structure (the spec uses that, not an error, so
		 * the host can probe namespace ids).
		 */
		nsdata = kmem_zalloc(sizeof (*nsdata), KM_SLEEP);
		(void) nvmft_build_identify_nsid(ctrlr, nsid, nsdata);
		mp = nvmft_alloc_data(nsdata, sizeof (*nsdata),
		    sizeof (ctrlr->ctrlr_cdata));
		kmem_free(nsdata, sizeof (*nsdata));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp,
		    sizeof (ctrlr->ctrlr_cdata));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	case 3: {	/* Namespace Identification Descriptor list. */
		uint8_t *desc;
		uint32_t nsid = LE_32(cmd->sqe_nsid);

		desc = kmem_zalloc(sizeof (ctrlr->ctrlr_cdata), KM_SLEEP);
		(void) nvmft_build_nsid_desc(ctrlr, nsid, desc,
		    sizeof (ctrlr->ctrlr_cdata));
		mp = nvmft_alloc_data(desc, sizeof (ctrlr->ctrlr_cdata),
		    sizeof (ctrlr->ctrlr_cdata));
		kmem_free(desc, sizeof (ctrlr->ctrlr_cdata));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp,
		    sizeof (ctrlr->ctrlr_cdata));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	case 1:	/* Controller data. */
		mp = nvmft_alloc_data(&ctrlr->ctrlr_cdata,
		    sizeof (ctrlr->ctrlr_cdata), sizeof (ctrlr->ctrlr_cdata));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp,
		    sizeof (ctrlr->ctrlr_cdata));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	case 2: {	/* Active namespace list. */
		nvme_identify_nsid_list_t *nslist;
		uint32_t nsid;

		nsid = LE_32(cmd->sqe_nsid);
		if (nsid >= 0xfffffffe) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			break;
		}

		nslist = kmem_zalloc(sizeof (*nslist), KM_SLEEP);
		nvmft_build_active_nslist(ctrlr, nsid, nslist);
		mp = nvmft_alloc_data(nslist, sizeof (*nslist),
		    sizeof (*nslist));
		kmem_free(nslist, sizeof (*nslist));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp, sizeof (*nslist));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	default:
		(void) nvmft_printf(ctrlr,
		    "Unsupported CNS 0x%x for IDENTIFY\n", cns);
		status = NVME_CQE_SC_GEN_INV_FLD;
		break;
	}

	if (status == NVMF_SUCCESS_SENT)
		nvmft_command_completed(ctrlr->ctrlr_admin, nc);
	else
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc, status);
	nvmf_free_capsule(nc);
}

static void
handle_set_features(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd)
{
	nvme_cqe_t cqe;
	uint8_t fid;

	fid = LE_32(cmd->sqe_cdw10) & 0xff;
	switch (fid) {
	case NVME_FEAT_NQUEUES: {
		uint32_t num_queues;
		nvmft_io_qpair_t *io_qpairs;

		num_queues = LE_32(cmd->sqe_cdw11) & 0xffff;

		/* 5.12.1.7: 65535 is invalid. */
		if (num_queues == 65535)
			goto error;
		/* Fabrics requires the same number of SQs and CQs. */
		if ((LE_32(cmd->sqe_cdw11) >> 16) != num_queues)
			goto error;

		num_queues++;	/* convert to 1's based */

		io_qpairs = kmem_zalloc(num_queues * sizeof (*io_qpairs),
		    KM_SLEEP);

		mutex_enter(&ctrlr->ctrlr_lock);
		if (ctrlr->ctrlr_num_io_queues != 0) {
			mutex_exit(&ctrlr->ctrlr_lock);
			kmem_free(io_qpairs, num_queues * sizeof (*io_qpairs));
			(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
			    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
			nvmf_free_capsule(nc);
			return;
		}
		ctrlr->ctrlr_num_io_queues = num_queues;
		ctrlr->ctrlr_io_qpairs = io_qpairs;
		mutex_exit(&ctrlr->ctrlr_lock);

		nvmft_init_cqe(&cqe, nc, 0);
		cqe.cqe_dw0 = cmd->sqe_cdw11;
		(void) nvmft_send_response(ctrlr->ctrlr_admin, &cqe);
		nvmf_free_capsule(nc);
		return;
	}
	case NVME_FEAT_ASYNC_EVENT: {
		uint32_t aer_mask = LE_32(cmd->sqe_cdw11);

		if ((aer_mask & 0xffffc000) != 0)
			goto error;
		mutex_enter(&ctrlr->ctrlr_lock);
		ctrlr->ctrlr_aer_mask = aer_mask;
		mutex_exit(&ctrlr->ctrlr_lock);
		(void) nvmft_send_success(ctrlr->ctrlr_admin, nc);
		nvmf_free_capsule(nc);
		return;
	}
	default:
		(void) nvmft_printf(ctrlr,
		    "Unsupported feature ID %u for SET_FEATURES\n", fid);
		goto error;
	}

error:
	(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
	    NVME_CQE_SC_GEN_INV_FLD);
	nvmf_free_capsule(nc);
}

static boolean_t
update_cc(nvmft_controller_t *ctrlr, uint32_t new_cc, boolean_t *need_shutdown)
{
	nvmft_port_t *np = ctrlr->ctrlr_np;
	nvme_reg_cc_t cc;
	nvme_reg_csts_t csts;
	uint32_t changes;

	*need_shutdown = B_FALSE;

	mutex_enter(&ctrlr->ctrlr_lock);

	if (ctrlr->ctrlr_shutdown) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return (B_FALSE);
	}
	if (!_nvmf_validate_cc(np->np_max_io_qsize, np->np_cap, ctrlr->ctrlr_cc,
	    new_cc)) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return (B_FALSE);
	}

	changes = ctrlr->ctrlr_cc ^ new_cc;
	ctrlr->ctrlr_cc = new_cc;
	cc.r = new_cc;
	csts.r = ctrlr->ctrlr_csts;

	/* Handle shutdown requests (CC.SHN). */
	if (NVMFT_CC_SHN(changes) != 0 && cc.b.cc_shn != 0) {
		csts.b.csts_shst = NVME_CSTS_SHN_OCCURING;
		cc.b.cc_en = 0;
		ctrlr->ctrlr_cc = cc.r;
		ctrlr->ctrlr_shutdown = B_TRUE;
		*need_shutdown = B_TRUE;
		(void) nvmft_printf(ctrlr, "shutdown requested\n");
	}

	/* Handle enable/disable (CC.EN). */
	if ((changes & 0x1u) != 0) {
		if (cc.b.cc_en == 0) {
			(void) nvmft_printf(ctrlr, "reset requested\n");
			ctrlr->ctrlr_shutdown = B_TRUE;
			*need_shutdown = B_TRUE;
		} else {
			csts.b.csts_rdy = 1;
		}
	}
	ctrlr->ctrlr_csts = csts.r;
	mutex_exit(&ctrlr->ctrlr_lock);

	return (B_TRUE);
}

static void
handle_property_get(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvmf_fabric_prop_get_cmd_t *pget)
{
	nvmf_fabric_prop_get_rsp_t rsp;

	nvmft_init_cqe(&rsp, nc, 0);

	switch (LE_32(pget->nfpg_ofst)) {
	case NVMF_PROP_CAP:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_8)
			goto error;
		rsp.nfpr_value.u64 = LE_64(ctrlr->ctrlr_np->np_cap);
		break;
	case NVMF_PROP_VS:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = ctrlr->ctrlr_cdata.id_ver;
		break;
	case NVMF_PROP_CC:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = LE_32(ctrlr->ctrlr_cc);
		break;
	case NVMF_PROP_CSTS:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = LE_32(ctrlr->ctrlr_csts);
		break;
	default:
		goto error;
	}

	(void) nvmft_send_response(ctrlr->ctrlr_admin, &rsp);
	return;
error:
	(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
	    NVME_CQE_SC_GEN_INV_FLD);
}

static void
handle_property_set(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvmf_fabric_prop_set_cmd_t *pset)
{
	boolean_t need_shutdown = B_FALSE;

	switch (LE_32(pset->nfps_ofst)) {
	case NVMF_PROP_CC:
		if (pset->nfps_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		if (!update_cc(ctrlr, LE_32(pset->nfps_value.u32.low),
		    &need_shutdown))
			goto error;
		break;
	default:
		goto error;
	}

	(void) nvmft_send_success(ctrlr->ctrlr_admin, nc);
	if (need_shutdown) {
		timeout_id_t ka_timer;
		timeout_id_t term_timer;

		/*
		 * update_cc() set ctrlr_shutdown under ctrlr_lock, so the
		 * keep-alive timer routine will not re-arm.  Snapshot and clear
		 * both timer ids under the lock before cancelling them.  The
		 * terminate timer matters here too: a prior graceful shutdown may
		 * have armed the 2-minute terminate and then the controller been
		 * re-enabled; a fresh shutdown must cancel that stale callout so
		 * it cannot fire (and re-dispatch terminate) after this shutdown.
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
		taskq_dispatch_ent(nvmft_global->ns_taskq,
		    nvmft_controller_shutdown, ctrlr, 0,
		    &ctrlr->ctrlr_shutdown_task);
	}
	return;
error:
	(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
	    NVME_CQE_SC_GEN_INV_FLD);
}

static void
handle_admin_fabrics_command(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvmf_fabric_cmd_t *fc)
{
	switch (fc->nfc_fctype) {
	case NVMF_FCTYPE_PROPERTY_GET:
		handle_property_get(ctrlr, nc,
		    (const nvmf_fabric_prop_get_cmd_t *)fc);
		break;
	case NVMF_FCTYPE_PROPERTY_SET:
		handle_property_set(ctrlr, nc,
		    (const nvmf_fabric_prop_set_cmd_t *)fc);
		break;
	case NVMF_FCTYPE_CONNECT:
		(void) nvmft_printf(ctrlr,
		    "CONNECT command on connected admin queue\n");
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
		break;
	case NVMF_FCTYPE_DISCONNECT:
		(void) nvmft_printf(ctrlr, "DISCONNECT command on admin queue\n");
		(void) nvmft_send_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SCT_SPECIFIC, NVMF_FABRIC_SC_INVALID_QUEUE_TYPE);
		break;
	default:
		(void) nvmft_printf(ctrlr, "Unsupported fabrics command 0x%x\n",
		    fc->nfc_fctype);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_INV_OPC);
		break;
	}
	nvmf_free_capsule(nc);
}

void
nvmft_handle_admin_command(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc)
{
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);

	/* Only permit Fabrics commands while the controller is disabled. */
	if (NVMFT_CC_EN(ctrlr->ctrlr_cc) == 0 &&
	    cmd->sqe_opc != NVMFT_OPC_FABRICS) {
		(void) nvmft_printf(ctrlr,
		    "Unsupported admin opcode 0x%x while disabled\n",
		    cmd->sqe_opc);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
		nvmf_free_capsule(nc);
		return;
	}

	(void) atomic_swap_uint(&ctrlr->ctrlr_ka_active_traffic, 1);

	switch (cmd->sqe_opc) {
	case NVME_OPC_GET_LOG_PAGE:
		handle_get_log_page(ctrlr, nc, cmd);
		break;
	case NVME_OPC_IDENTIFY:
		handle_identify_command(ctrlr, nc, cmd);
		break;
	case NVME_OPC_SET_FEATURES:
		handle_set_features(ctrlr, nc, cmd);
		break;
	case NVME_OPC_ASYNC_EVENT:
		mutex_enter(&ctrlr->ctrlr_lock);
		if (ctrlr->ctrlr_aer_pending == NVMFT_NUM_AER) {
			mutex_exit(&ctrlr->ctrlr_lock);
			(void) nvmft_send_error(ctrlr->ctrlr_admin, nc,
			    NVME_CQE_SCT_SPECIFIC,
			    NVME_CQE_SC_SPC_ASYNC_EVREQ_EXC);
		} else {
			/* NB: store the CID without byte-swapping. */
			ctrlr->ctrlr_aer_cids[ctrlr->ctrlr_aer_pidx] =
			    cmd->sqe_cid;
			ctrlr->ctrlr_aer_pending++;
			ctrlr->ctrlr_aer_pidx =
			    (ctrlr->ctrlr_aer_pidx + 1) % NVMFT_NUM_AER;
			mutex_exit(&ctrlr->ctrlr_lock);
		}
		nvmf_free_capsule(nc);
		break;
	case NVME_OPC_KEEP_ALIVE:
		(void) nvmft_send_success(ctrlr->ctrlr_admin, nc);
		nvmf_free_capsule(nc);
		break;
	case NVMFT_OPC_FABRICS:
		handle_admin_fabrics_command(ctrlr, nc,
		    (const nvmf_fabric_cmd_t *)cmd);
		break;
	default:
		(void) nvmft_printf(ctrlr, "Unsupported admin opcode 0x%x\n",
		    cmd->sqe_opc);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		break;
	}
}

void
nvmft_handle_io_command(struct nvmft_qpair *qp, uint16_t qid,
    struct nvmf_capsule *nc)
{
	nvmft_controller_t *ctrlr = nvmft_qpair_ctrlr(qp);
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);

	_NOTE(ARGUNUSED(qid));

	(void) atomic_swap_uint(&ctrlr->ctrlr_ka_active_traffic, 1);

	switch (cmd->sqe_opc) {
	case NVME_OPC_NVM_FLUSH:
		if (cmd->sqe_nsid == LE_32(NVME_NSID_BCAST)) {
			(void) nvmft_send_generic_error(qp, nc,
			    NVME_CQE_SC_GEN_INV_NS);
			nvmf_free_capsule(nc);
			break;
		}
		/* FALLTHROUGH */
	case NVME_OPC_NVM_WRITE:
	case NVME_OPC_NVM_READ:
	case NVME_OPC_NVM_WRITE_UNC:
	case NVME_OPC_NVM_COMPARE:
	case NVME_OPC_NVM_WRITE_ZERO:
	case NVME_OPC_NVM_DSET_MGMT:
	case NVMFT_OPC_NVM_VERIFY:
	/*
	 * PORT-TODO (7.4): NVME_OPC_NVM_RESV_* (register/report/acquire/release)
	 * should route to nvmft_resv.c rather than the generic datamove path
	 * (FreeBSD nvmft_handle_io_command does not handle the reservation
	 * opcodes either; they are added here for the COMSTAR reservation work).
	 */
		nvmft_dispatch_command(qp, nc, B_FALSE);
		break;
	default:
		(void) nvmft_printf(ctrlr, "Unsupported I/O opcode 0x%x\n",
		    cmd->sqe_opc);
		(void) nvmft_send_generic_error(qp, nc, NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		break;
	}
}

static void
nvmft_report_aer(nvmft_controller_t *ctrlr, uint32_t aer_mask, uint_t type,
    uint8_t info, uint8_t log_page_id)
{
	nvme_cqe_t cpl;

	ASSERT3U(type, <=, 7);

	mutex_enter(&ctrlr->ctrlr_lock);
	if ((ctrlr->ctrlr_aer_mask & aer_mask) == 0) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	if (ctrlr->ctrlr_aer_pending == 0) {
		mutex_exit(&ctrlr->ctrlr_lock);
		(void) nvmft_printf(ctrlr,
		    "dropping AER type %u, info 0x%x, page 0x%x\n", type, info,
		    log_page_id);
		return;
	}

	(void) bzero(&cpl, sizeof (cpl));
	cpl.cqe_cid = ctrlr->ctrlr_aer_cids[ctrlr->ctrlr_aer_cidx];
	ctrlr->ctrlr_aer_pending--;
	ctrlr->ctrlr_aer_cidx = (ctrlr->ctrlr_aer_cidx + 1) % NVMFT_NUM_AER;
	mutex_exit(&ctrlr->ctrlr_lock);

	cpl.cqe_dw0 = LE_32(((uint32_t)type & 0x7) |
	    ((uint32_t)info << 8) | ((uint32_t)log_page_id << 16));

	(void) nvmft_send_response(ctrlr->ctrlr_admin, &cpl);
}

void
nvmft_controller_lun_changed(nvmft_controller_t *ctrlr, int lun_id)
{
	nvme_nschange_list_t *nslist;
	uint32_t new_nsid, nsid;
	uint_t i, nitems;

	new_nsid = lun_id + 1;
	nslist = ctrlr->ctrlr_changed_ns;
	nitems = sizeof (nslist->nscl_ns) / sizeof (nslist->nscl_ns[0]);

	mutex_enter(&ctrlr->ctrlr_lock);

	/* If the first entry is 0xffffffff, the list is already full. */
	if (nslist->nscl_ns[0] != 0xffffffff) {
		for (i = 0; i < nitems; i++) {
			nsid = LE_32(nslist->nscl_ns[i]);
			if (nsid == new_nsid) {
				mutex_exit(&ctrlr->ctrlr_lock);
				return;
			}
			if (nsid == 0 || nsid > new_nsid)
				break;
		}

		if (nslist->nscl_ns[nitems - 1] != LE_32(0)) {
			/* List is full. */
			(void) bzero(nslist, sizeof (*nslist));
			nslist->nscl_ns[0] = 0xffffffff;
		} else {
			/*
			 * Not full: nscl_ns[nitems-1] is zero, so the scan above
			 * must have broken (on a zero or a larger NSID) rather
			 * than running to completion; therefore i < nitems and
			 * is a valid in-range insertion point.  The insertion
			 * point may be the trailing-zero slot itself, in which
			 * case we just write there.
			 */
			VERIFY3U(i, <, nitems);
			if (nslist->nscl_ns[i] == LE_32(0)) {
				nslist->nscl_ns[i] = LE_32(new_nsid);
			} else {
				/*
				 * A nonzero slot that is not the trailing zero
				 * implies a free slot exists after it, so i is
				 * strictly below the last index and the shift
				 * stays in-bounds.
				 */
				VERIFY3U(i, <, nitems - 1);
				(void) memmove(&nslist->nscl_ns[i + 1],
				    &nslist->nscl_ns[i],
				    (nitems - i - 1) *
				    sizeof (nslist->nscl_ns[0]));
				nslist->nscl_ns[i] = LE_32(new_nsid);
			}
		}
	}

	if (ctrlr->ctrlr_changed_ns_reported) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	ctrlr->ctrlr_changed_ns_reported = B_TRUE;
	mutex_exit(&ctrlr->ctrlr_lock);

	nvmft_report_aer(ctrlr, NVMFT_AER_NS_ATTRIBUTE, NVME_ASYNC_TYPE_NOTICE,
	    NVME_ASYNC_NOTICE_NS_CHANGE, NVME_LOGPAGE_NSCHANGE);
}

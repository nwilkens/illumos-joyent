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
 * sys/dev/nvmf/controller/nvmft_var.h.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * This is the controller (target) side state for the NVMe-over-Fabrics COMSTAR
 * STMF port provider.  The protocol/controller state machine is ported faithfully
 * from FreeBSD; the OS-glue layer has been retargeted from FreeBSD CTL to illumos
 * STMF.  The field-level KPI substitutions are:
 *
 *   FreeBSD                          illumos
 *   -------                          -------
 *   struct ctl_port port             stmf_local_port_t *lport (registered with STMF)
 *   struct nvme_controller_data      nvme_identify_ctrl_t (sys/nvme.h)
 *   struct mtx lock                  kmutex_t lock
 *   struct callout ka_timer          timeout_id_t ka_timer (timeout(9F))
 *   struct task/timeout_task         taskq_dispatch / nvmft taskq
 *   struct unrhdr *ids               id_space_t *ids (cntlid allocator)
 *   TAILQ_*                          list_t / list_node_t (sys/list.h)
 *   refcount(9)                      uint_t refs guarded by lock
 *
 * The CTL-specific datamove queue (struct ctl_io_hdr) has no counterpart: STMF
 * drives data transfers inline through lport_xfer_data; see nvmft_stmf.c.
 */

#ifndef	_NVMFT_VAR_H
#define	_NVMFT_VAR_H

#include <sys/types.h>
#include <sys/time.h>		/* hrtime_t (needed by <sys/stmf.h>) */
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/taskq_impl.h>	/* taskq_ent_t, taskq_dispatch_ent */
#include <sys/id_space.h>
#include <sys/nvpair.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

/* Raw nvme_sqe_t/nvme_cqe_t and NVME_OPC_* live in the nvme(4D) private header. */
#include "../../../nvme/nvme_reg.h"

#include <sys/stmf.h>
#include <sys/stmf_ioctl.h>
#include <sys/portif.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvmf_capsule;
struct nvmft_controller;
struct nvmft_qpair;
struct nvmft_ana_group;

/* Maximum number of outstanding AER commands tracked per controller. */
#define	NVMFT_NUM_AER		16

/*
 * Upper bound on a Get Log Page transfer.  NUMD is fully host-controlled and
 * (numd+1)*4 can reach ~4 GiB, which would drive an unbounded KM_SLEEP
 * allocation.  No log page this controller serves approaches this; 64 KiB is
 * generous headroom (covers a large future ANA log page) while preventing a
 * bogus NUMD from exhausting kernel memory.
 */
#define	NVMFT_MAX_LOGPAGE_LEN	(64 * 1024)

/*
 * Fabrics command opcode for the admin SQE.  <sys/nvme.h> does not define a
 * Fabrics opcode constant (the local nvme(4D) driver has no Fabrics support);
 * the NVMe-oF spec fixes it at 0x7f.  (FreeBSD: NVME_OPC_FABRICS_COMMANDS.)
 */
#define	NVMFT_OPC_FABRICS	0x7f

/*
 * NVMe revision 1.4 packed as the Identify Controller VER field expects
 * (major << 16 | minor << 8).  <sys/nvme.h> exposes only comparison helpers,
 * not a packed 1.4 constant.  (FreeBSD: NVME_REV(1, 4).)
 */
#define	NVMFT_VER_1_4		0x00010400u

/*
 * nvmft_port - an exported NVMe subsystem (SubNQN) registered with STMF as a
 * single stmf_local_port_t.  This is the COMSTAR equivalent of FreeBSD's
 * "struct nvmft_port" which embedded a "struct ctl_port".
 *
 * (FreeBSD: struct nvmft_port)
 */
typedef struct nvmft_port {
	list_node_t		np_link;	/* nvmft_ports list */
	uint_t			np_refs;	/* refs, guarded by np_lock */

	/*
	 * The STMF local port and the SCSI device id descriptor used as the
	 * port identity.  FreeBSD embedded a struct ctl_port; here STMF owns the
	 * lport and we hang the nvmft_port off lport_port_private.
	 */
	stmf_local_port_t	*np_lport;
	scsi_devid_desc_t	*np_devid;

	/* Identify Controller template shared by all controllers on this port. */
	nvme_identify_ctrl_t	np_cdata;
	nvme_fwslot_log_t	np_fp;		/* Firmware slot log page */
	uint64_t		np_cap;		/* Controller CAP property */
	uint32_t		np_max_io_qsize;
	uint16_t		np_portid;
	boolean_t		np_online;

	kmutex_t		np_lock;

	/* cntlid allocator (FreeBSD: struct unrhdr *ids). */
	id_space_t		*np_ids;
	list_t			np_controllers;	/* nvmft_controller list */
	kcondvar_t		np_controllers_cv; /* np_controllers drain, np_lock */

	/*
	 * Sorted array of active namespace IDs (== STMF LUN id + 1).  Never
	 * populated today (always NULL/0); retained only because nvmft_ana.c
	 * still reads these fields.  Their removal is part of the deferred ANA
	 * decision and must land atomically with the nvmft_ana.c change.
	 */
	uint32_t		*np_active_ns;
	uint_t			np_num_ns;

	/*
	 * ANA group table (NEW relative to FreeBSD; see NVMEOF.md 9.4 and
	 * nvmft_ana.c).  Each namespace carries an ANAGRPID; the group owns the
	 * settable ANA state advertised through the ANA log page.
	 */
	struct nvmft_ana_group	*np_ana_groups;
	uint_t			np_num_ana_groups;
	uint64_t		np_ana_changecount;
} nvmft_port_t;

/*
 * Per-I/O-queue wrapper.  (FreeBSD: struct nvmft_io_qpair)
 */
typedef struct nvmft_io_qpair {
	struct nvmft_qpair	*nio_qp;
	boolean_t		nio_shutdown;
} nvmft_io_qpair_t;

/*
 * nvmft_controller - a single host association (Fabrics CONNECT) to a subsystem.
 * Owns one admin qpair and N I/O qpairs.  (FreeBSD: struct nvmft_controller)
 */
typedef struct nvmft_controller {
	struct nvmft_qpair	*ctrlr_admin;
	nvmft_io_qpair_t	*ctrlr_io_qpairs;
	uint_t			ctrlr_num_io_queues;
	boolean_t		ctrlr_shutdown;
	boolean_t		ctrlr_admin_closed;
	uint16_t		ctrlr_cntlid;
	uint32_t		ctrlr_cc;	/* Controller Configuration */
	uint32_t		ctrlr_csts;	/* Controller Status */

	nvmft_port_t		*ctrlr_np;
	stmf_scsi_session_t	*ctrlr_session;	/* per-controller STMF session */
	kmutex_t		ctrlr_lock;

	nvme_identify_ctrl_t	ctrlr_cdata;
	nvme_health_log_t	ctrlr_hip;	/* SMART/health log page */
	hrtime_t		ctrlr_create_time;
	hrtime_t		ctrlr_start_busy;
	hrtime_t		ctrlr_busy_total;
	uint16_t		ctrlr_partial_dur;
	uint16_t		ctrlr_partial_duw;

	uint8_t			ctrlr_hostid[16];
	uint8_t			ctrlr_hostnqn[NVMF_NQN_FIELD_SIZE];
	nvmf_trtype_t		ctrlr_trtype;

	list_node_t		ctrlr_link;	/* np_controllers list */

	/*
	 * Each queue can have at most UINT16_MAX commands, so the total across
	 * all queues fits in a uint32_t.  Guarded by ctrlr_lock; the shutdown
	 * path waits on ctrlr_pending_cv for this to drain.  ctrlr_pending_bytes
	 * is the in-flight command payload (admission control, nvmft_stmf.c).
	 */
	uint32_t		ctrlr_pending_commands;
	uint64_t		ctrlr_pending_bytes;
	kcondvar_t		ctrlr_pending_cv;

	/*
	 * Writeback backpressure: commands that arrive while ctrlr_pending_commands
	 * is at the in-flight cap are queued here (nvmft_deferred_cmd_t) instead of
	 * posted to STMF, and re-dispatched as in-flight commands complete.  This
	 * bounds the commands concurrently in flight so a fast writeback-cached
	 * initiator cannot overrun the backing store's drain rate; the initiator
	 * paces itself via SQ-credit backpressure (the host maps any non-zero CQE to
	 * EIO and will not retry).  Guarded by ctrlr_lock; drained on controller
	 * shutdown before the qpairs are freed.
	 */
	list_t			ctrlr_deferred;
	uint32_t		ctrlr_deferred_commands;

	/* Keep-alive (FreeBSD: callout ka_timer + ka_active_traffic). */
	volatile uint_t		ctrlr_ka_active_traffic;
	timeout_id_t		ctrlr_ka_timer;
	clock_t			ctrlr_ka_ticks;	/* KATO in ticks, 0 == off */

	/* AER state.  CIDs are stored without byte-swapping, per FreeBSD. */
	uint32_t		ctrlr_aer_mask;
	uint16_t		ctrlr_aer_cids[NVMFT_NUM_AER];
	uint8_t			ctrlr_aer_pending;
	uint8_t			ctrlr_aer_cidx;
	uint8_t			ctrlr_aer_pidx;

	/* Changed namespace IDs for the Changed Namespace List log page. */
	nvme_nschange_list_t	*ctrlr_changed_ns;
	boolean_t		ctrlr_changed_ns_reported;

	/*
	 * Deferred shutdown/terminate work.  Like FreeBSD's separate struct task
	 * and timeout_task, shutdown and terminate need INDEPENDENT taskq entries:
	 * a single taskq_ent_t is the queue's list node, and re-dispatching one
	 * that is still queued (e.g. a late error dispatching shutdown while the
	 * delayed terminate is queued) corrupts the single-threaded ns_taskq list.
	 */
	taskq_ent_t		ctrlr_shutdown_task;
	taskq_ent_t		ctrlr_terminate_task;
	timeout_id_t		ctrlr_terminate_timer;
	/*
	 * Set under ctrlr_lock while a terminate is queued or running, so the
	 * timeout trampoline never re-dispatches ctrlr_terminate_task while it is
	 * still on the taskq (which would corrupt the queue): untimeout() cancels
	 * the timer id but cannot recall an already-queued task.
	 */
	boolean_t		ctrlr_terminate_queued;
} nvmft_controller_t;

/*
 * Module-global controller-software context.  Holds the STMF port-provider
 * registration and the list of exported ports.  (FreeBSD: file-scope statics in
 * ctl_frontend_nvmf.c.)
 */
typedef struct nvmft_softc {
	dev_info_t		*ns_dip;
	stmf_port_provider_t	*ns_pp;		/* registered with STMF */

	/* Worker taskq for controller shutdown/terminate work. */
	taskq_t			*ns_taskq;

	kmutex_t		ns_lock;	/* protects ns_ports */
	list_t			ns_ports;	/* nvmft_port list */
} nvmft_softc_t;

extern nvmft_softc_t *nvmft_global;

/*
 * Logging.  Mirrors the srpt port-provider log levels.
 */
#define	NVMFT_LOG_L0	0
#define	NVMFT_LOG_L1	1
#define	NVMFT_LOG_L2	2
#define	NVMFT_LOG_L3	3

extern uint_t nvmft_errlevel;

#define	NVMFT_DPRINTF_L0(...)	cmn_err(CE_WARN, __VA_ARGS__)
#define	NVMFT_DPRINTF_L1(...)	cmn_err(CE_NOTE, __VA_ARGS__)
#define	NVMFT_DPRINTF_L2(...)	if (nvmft_errlevel >= NVMFT_LOG_L2) { \
					cmn_err(CE_NOTE, __VA_ARGS__); \
				}
#ifdef	DEBUG
#define	NVMFT_DPRINTF_L3(...)	if (nvmft_errlevel >= NVMFT_LOG_L3) { \
					cmn_err(CE_NOTE, __VA_ARGS__); \
				}
#else
#define	NVMFT_DPRINTF_L3(...)	(void)(0)
#endif

/*
 * nvmft_stmf.c - STMF binding (replaces ctl_frontend_nvmf.c).
 */
nvmft_port_t *nvmft_port_alloc(const char *subnqn, uint16_t portid,
	    const char *serial, uint32_t max_io_qsize, uint32_t enable_timeout,
	    uint32_t ioccsz, uint32_t iorcsz, uint32_t nn);
void	nvmft_port_free(nvmft_port_t *np);
int	nvmft_session_register(nvmft_controller_t *ctrlr);
void	nvmft_session_deregister(nvmft_controller_t *ctrlr);
void	nvmft_build_active_nslist(nvmft_controller_t *ctrlr, uint32_t start_nsid,
	    nvme_identify_nsid_list_t *nslist);
boolean_t nvmft_build_identify_nsid(nvmft_controller_t *ctrlr, uint32_t nsid,
	    nvme_identify_nsid_t *nsdata);
boolean_t nvmft_build_nsid_desc(nvmft_controller_t *ctrlr, uint32_t nsid,
	    uint8_t *buf, size_t buflen);
void	nvmft_dispatch_command(struct nvmft_qpair *qp,
	    struct nvmf_capsule *nc, boolean_t admin);
void	nvmft_terminate_commands(nvmft_controller_t *ctrlr);
void	nvmft_deferred_init(nvmft_controller_t *ctrlr);
void	nvmft_deferred_drain(nvmft_controller_t *ctrlr);
void	nvmft_deferred_fini(nvmft_controller_t *ctrlr);

/* nvmft_controller.c */
void	nvmft_controller_error(nvmft_controller_t *ctrlr,
	    struct nvmft_qpair *qp, int error);
void	nvmft_controller_lun_changed(nvmft_controller_t *ctrlr, int lun_id);
void	nvmft_handle_admin_command(nvmft_controller_t *ctrlr,
	    struct nvmf_capsule *nc);
void	nvmft_handle_io_command(struct nvmft_qpair *qp, uint16_t qid,
	    struct nvmf_capsule *nc);
int	nvmft_handoff_admin_queue(nvmft_port_t *np, nvmf_trtype_t trtype,
	    const nvlist_t *params, const nvmf_fabric_connect_cmd_t *cmd,
	    const nvmf_fabric_connect_data_t *data);
int	nvmft_handoff_io_queue(nvmft_port_t *np, nvmf_trtype_t trtype,
	    const nvlist_t *params, const nvmf_fabric_connect_cmd_t *cmd,
	    const nvmf_fabric_connect_data_t *data);

/* nvmft_qpair.c */
struct nvmft_qpair *nvmft_qpair_init(nvmf_trtype_t trtype,
	    const nvlist_t *params, uint16_t qid, const char *name);
void	nvmft_qpair_shutdown(struct nvmft_qpair *qp);
void	nvmft_qpair_destroy(struct nvmft_qpair *qp);
nvmft_controller_t *nvmft_qpair_ctrlr(struct nvmft_qpair *qp);
uint16_t nvmft_qpair_id(struct nvmft_qpair *qp);
const char *nvmft_qpair_name(struct nvmft_qpair *qp);
uint32_t nvmft_max_ioccsz(struct nvmft_qpair *qp);
struct nvmf_qpair *nvmft_qpair_data_hold(struct nvmft_qpair *qp);
void	nvmft_qpair_data_rele(struct nvmft_qpair *qp, struct nvmf_qpair *nq);
void	nvmft_command_completed(struct nvmft_qpair *qp,
	    struct nvmf_capsule *nc);
int	nvmft_send_response(struct nvmft_qpair *qp, const void *cqe);
void	nvmft_init_cqe(void *cqe, struct nvmf_capsule *nc, uint16_t status);
int	nvmft_send_error(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
	    uint8_t sc_type, uint8_t sc_status);
int	nvmft_send_generic_error(struct nvmft_qpair *qp,
	    struct nvmf_capsule *nc, uint8_t sc_status);
int	nvmft_send_success(struct nvmft_qpair *qp, struct nvmf_capsule *nc);
void	nvmft_connect_error(struct nvmft_qpair *qp,
	    const nvmf_fabric_connect_cmd_t *cmd, uint8_t sc_type,
	    uint8_t sc_status);
void	nvmft_connect_invalid_parameters(struct nvmft_qpair *qp,
	    const nvmf_fabric_connect_cmd_t *cmd, boolean_t data,
	    uint16_t offset);
int	nvmft_finish_accept(struct nvmft_qpair *qp,
	    const nvmf_fabric_connect_cmd_t *cmd, nvmft_controller_t *ctrlr);

/* nvmft_subr.c (portable helpers shared with userland). */
boolean_t nvmf_nqn_valid(const char *nqn);
uint64_t _nvmf_controller_cap(uint32_t max_io_qsize, uint8_t enable_timeout);
boolean_t _nvmf_validate_cc(uint32_t max_io_qsize, uint64_t cap,
	    uint32_t old_cc, uint32_t new_cc);
void	nvmf_controller_serial(char *buf, size_t len, ulong_t hostid);
void	nvmf_strpad(char *dst, const char *src, size_t len);
void	_nvmf_init_io_controller_data(uint16_t cntlid, uint32_t max_io_qsize,
	    const char *serial, const char *model, const char *firmware_version,
	    const char *subnqn, int nn, uint32_t ioccsz, uint32_t iorcsz,
	    nvme_identify_ctrl_t *cdata);

/* nvmft_controller.c logging helper. */
int	nvmft_printf(nvmft_controller_t *ctrlr, const char *fmt, ...);

static inline void
nvmft_port_ref(nvmft_port_t *np)
{
	mutex_enter(&np->np_lock);
	np->np_refs++;
	mutex_exit(&np->np_lock);
}

static inline void
nvmft_port_rele(nvmft_port_t *np)
{
	boolean_t free_it;

	mutex_enter(&np->np_lock);
	ASSERT3U(np->np_refs, >, 0);
	free_it = (--np->np_refs == 0);
	mutex_exit(&np->np_lock);
	if (free_it)
		nvmft_port_free(np);
}

#ifdef __cplusplus
}
#endif

#endif /* _NVMFT_VAR_H */

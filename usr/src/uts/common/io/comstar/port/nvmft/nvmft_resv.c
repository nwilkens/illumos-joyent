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
 * NEW file.  NVMe reservations -> SCSI-3 persistent reservation (PGR) bridge.
 *
 * No FreeBSD counterpart exists: FreeBSD's NVMe target does not implement
 * reservations (NVMEOF.md section 10).  illumos can get them nearly for free
 * because the target I/O path already runs NVMe -> SCSI -> stmf_sbd, and sbd
 * implements SCSI-3 persistent reservations.  This file is the translation seam
 * described in NVMEOF.md section 7.4: it maps the NVMe reservation commands onto
 * the existing sbd PGR machinery
 * (io/comstar/lu/stmf_sbd/sbd_pgr.c):
 *
 *   NVMe command (opcode)            SCSI / sbd_pgr action
 *   --------------------            ---------------------
 *   Reservation Register (0x0d)     PERSISTENT RESERVE OUT / REGISTER
 *                                     -> sbd_pgr_out_register()
 *   Reservation Acquire (0x11)      PERSISTENT RESERVE OUT / RESERVE|PREEMPT
 *                                     -> sbd_pgr_out_reserve()/preempt()
 *   Reservation Release (0x12)      PERSISTENT RESERVE OUT / RELEASE|CLEAR
 *                                     -> sbd_pgr_out_*()
 *   Reservation Report  (0x0e)      PERSISTENT RESERVE IN  / READ FULL STATUS
 *   (on every I/O)                  sbd_pgr_reservation_conflict()
 *
 * Caveats (NVMEOF.md 7.4 / R11):
 *   - This is cheap only for an sbd-over-zvol LU; sbd_pgr state hangs off
 *     sbd_lu_t and is persisted through sbd metadata.  A non-sbd LU does not
 *     inherit PGR and would need native nvmft reservation handling.
 *   - PGR enforces fencing only on the STMF/sbd target I/O path; the
 *     distribution layer must supply its own authoritative back-end fence.
 *   - The NVMe and SCSI-3 reservation models are close but not identical; the
 *     register/acquire/release type and the reservation-holder identity
 *     mappings must be verified with conformance tests (R3).
 *
 * The reservation-key <-> PGR-key identity mapping is the crux: an NVMe host is
 * identified by its 128-bit Host Identifier (from Fabrics CONNECT data) and a
 * 64-bit Reservation Key (RK) in the command; SCSI PGR keys are 64-bit and
 * scoped to an I_T nexus.  The mapping uses the controller's hostid to derive a
 * stable PGR transport id and the NVMe RK as the PGR key.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#include <sys/sunddi.h>		/* bzero */

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

#include <sys/time.h>		/* hrtime_t (sys/stmf.h needs it) */
#include <sys/stmf.h>

#include "nvmft_var.h"

/*
 * NVMe reservation command opcodes (NVM command set).  These mirror the
 * NVME_OPC_NVM_RESV_* definitions in io/nvme/nvme_reg.h.
 */
#define	NVMFT_RESV_REGISTER	0x0d
#define	NVMFT_RESV_REPORT	0x0e
#define	NVMFT_RESV_ACQUIRE	0x11
#define	NVMFT_RESV_RELEASE	0x12

/* Reservation Conflict generic status (NVMe). */
#define	NVMFT_SC_RSV_CONFLICT	NVME_CQE_SC_GEN_NVM_RSV_CNFLCT	/* 0x83 */

/*
 * cdw10 field accessors for the reservation commands (NVMe 1.4 NVM command
 * set).  Register/Acquire/Release share the low-nibble action and IEKEY bit;
 * Acquire/Release additionally carry RTYPE in bits 15:8.
 */
#define	NVMFT_RESV_ACTION(cdw10)	((cdw10) & 0x7)
#define	NVMFT_RESV_IEKEY(cdw10)		(((cdw10) >> 3) & 0x1)
#define	NVMFT_RESV_RTYPE(cdw10)		(((cdw10) >> 8) & 0xff)
#define	NVMFT_RESV_CPTPL(cdw10)		(((cdw10) >> 30) & 0x3)

/* RREGA (Register action). */
#define	NVMFT_RREGA_REGISTER		0x0
#define	NVMFT_RREGA_UNREGISTER		0x1
#define	NVMFT_RREGA_REPLACE		0x2

/* RACQA (Acquire action). */
#define	NVMFT_RACQA_ACQUIRE		0x0
#define	NVMFT_RACQA_PREEMPT		0x1
#define	NVMFT_RACQA_PREEMPT_ABORT	0x2

/* RRELA (Release action). */
#define	NVMFT_RRELA_RELEASE		0x0
#define	NVMFT_RRELA_CLEAR		0x1

/*
 * Entry point invoked by nvmft_handle_io_command() for the reservation
 * opcodes.  Returns B_TRUE if the command was consumed (a response/queueing was
 * issued), B_FALSE to fall through to the normal datamove dispatch.
 *
 * Status of the bridge (NVMEOF.md 7.4 / R11): the per-opcode NVMe fields are
 * decoded and validated here, but the actual PGR mutation is NOT yet dispatched
 * because the prerequisite does not exist: nvmft does not yet register a
 * per-controller stmf_scsi_session_t, and sbd_pgr requires task->task_session
 * (ss_rport / ss_lport->lport_id) and task->task_lu_itl_handle (sbd_it_data_t
 * with pgr_key_ptr).  Without those, building a PERSISTENT RESERVE OUT/IN CDB
 * and posting it would NULL-deref in stmf/sbd or, worse, record a bogus I_T
 * nexus as the reservation holder.  Until the session wiring lands, each
 * reservation command is rejected with a clean, spec-valid status so the host
 * never sees silent data corruption.
 *
 * PORT-TODO (NVMEOF.md 7.4 / R11): once nvmft registers a per-controller
 * stmf_scsi_session_t whose ss_rport carries the transport id derived from
 * nvmft_resv_host_key(), dispatch each decoded action by synthesizing the
 * matching PERSISTENT RESERVE OUT/IN CDB + parameter list on a scsi_task_t and
 * posting it (so sbd_pgr_out_register/reserve/release/clear/preempt and
 * sbd_handle_pgr_in_cmd run), or by factoring sbd_pgr into a reusable module and
 * calling sbd_pgr_out_*() directly.  Mapping:
 *   RREGA register/replace -> PR OUT REGISTER (+ IGNORE_EXISTING on replace);
 *   RREGA unregister       -> PR OUT REGISTER with service key 0;
 *   RACQA acquire          -> PR OUT RESERVE;
 *   RACQA preempt[/abort]  -> PR OUT PREEMPT[/PREEMPT_ABORT];
 *   RRELA release          -> PR OUT RELEASE;
 *   RRELA clear            -> PR OUT CLEAR;
 *   Report                 -> PR IN READ FULL STATUS, reformatted into the NVMe
 *                             Reservation Status data structure.
 * Honor IEKEY (ignore existing key) and CPTPL (-> APTPL) along the way; the
 * NVMe Reservation Key (RK) becomes the PGR key value.
 */
boolean_t
nvmft_resv_dispatch(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd)
{
	nvmft_controller_t *ctrlr = nvmft_qpair_ctrlr(qp);
	uint32_t cdw10 = LE_32(cmd->sqe_cdw10);
	uint8_t action = 0;
	boolean_t valid = B_FALSE;

	/*
	 * nvmft_qpair_ctrlr() returns NULL on a newborn qpair that is not yet
	 * associated with a controller.  A reservation capsule should never
	 * arrive on such a queue (CONNECT runs first), but a malformed/early
	 * capsule could; reject it with Invalid Opcode rather than dereferencing
	 * a NULL ctrlr below, matching how nvmft_receive_capsule() treats a
	 * capsule on a not-yet-associated queue.
	 */
	if (ctrlr == NULL) {
		(void) nvmft_send_generic_error(qp, nc, NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		return (B_TRUE);
	}

	switch (cmd->sqe_opc) {
	case NVMFT_RESV_REGISTER:
		action = NVMFT_RESV_ACTION(cdw10);
		valid = (action == NVMFT_RREGA_REGISTER ||
		    action == NVMFT_RREGA_UNREGISTER ||
		    action == NVMFT_RREGA_REPLACE);
		break;
	case NVMFT_RESV_ACQUIRE:
		action = NVMFT_RESV_ACTION(cdw10);
		valid = (action == NVMFT_RACQA_ACQUIRE ||
		    action == NVMFT_RACQA_PREEMPT ||
		    action == NVMFT_RACQA_PREEMPT_ABORT);
		break;
	case NVMFT_RESV_RELEASE:
		action = NVMFT_RESV_ACTION(cdw10);
		valid = (action == NVMFT_RRELA_RELEASE ||
		    action == NVMFT_RRELA_CLEAR);
		break;
	case NVMFT_RESV_REPORT:
		/* Report carries only a dword count in cdw10; nothing to gate. */
		valid = B_TRUE;
		break;
	default:
		return (B_FALSE);
	}

	if (!valid) {
		/* Reserved/invalid action code: Invalid Field in Command. */
		(void) nvmft_send_generic_error(qp, nc, NVME_CQE_SC_GEN_INV_FLD);
		nvmf_free_capsule(nc);
		return (B_TRUE);
	}

	/*
	 * Fields are well-formed but the PGR dispatch path is not yet wired (no
	 * per-controller STMF session).  Reject with Reservation Conflict: it is
	 * a spec-valid terminal status for a reservation command and is safer
	 * than appearing to succeed.
	 */
	(void) nvmft_printf(ctrlr,
	    "reservation opcode 0x%x action 0x%x rejected: PGR bridge pending "
	    "per-controller STMF session (NVMEOF.md 7.4)\n", cmd->sqe_opc,
	    NVMFT_RESV_ACTION(cdw10));
	(void) nvmft_send_generic_error(qp, nc, NVMFT_SC_RSV_CONFLICT);
	nvmf_free_capsule(nc);
	return (B_TRUE);
}

/*
 * Derive a stable persistent-reservation transport identity for a controller
 * from its 128-bit NVMe Host Identifier.  The result is the *initiator
 * identity* used to scope reservation ownership: NVMe reservation semantics make
 * the holder the host (HostID), not the individual controller/association, so
 * folding the HostID (rather than anything association-specific) means an
 * acquire/register on one path is honored across the host's other paths
 * (multipath).
 *
 * The fold is a simple, deterministic XOR of the two 64-bit halves of the
 * HostID.  This is used only as a transport-id discriminator for the SCSI PGR
 * key list; the NVMe Reservation Key (RK) from the command is what is stored as
 * the PGR key value (sbd_pgr_key_t.pgr_key).
 */
void
nvmft_resv_host_key(const nvmft_controller_t *ctrlr, uint8_t key_out[8])
{
	uint64_t lo, hi, fold;

	(void) bcopy(&ctrlr->ctrlr_hostid[0], &hi, sizeof (hi));
	(void) bcopy(&ctrlr->ctrlr_hostid[8], &lo, sizeof (lo));
	fold = hi ^ lo;

	(void) bcopy(&fold, key_out, 8);
}

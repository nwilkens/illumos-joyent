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
 * sys/dev/nvmf/controller/nvmft_qpair.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Per-queue-pair controller state and the response/error capsule helpers.  The
 * protocol logic ports directly.  KPI substitutions:
 *
 *   FreeBSD                       illumos
 *   -------                       -------
 *   struct mtx lock               kmutex_t lock
 *   refcount(9) qp_refs           uint_t qp_refs guarded by lock
 *   BITSET cidset (64K bits)      uint8_t bitmap (NUM_CIDS/8 bytes), BT_* macros
 *   nvlist_get_bool/_number       nvlist_lookup_boolean_value/_uint64
 *   le16toh/htole16               LE_16
 *
 * STMF drives data transfers inline through lport_xfer_data (nvmft_stmf.c), so
 * the FreeBSD per-qpair datamove queue (union ctl_io) has no counterpart here.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/kmem.h>
#include <sys/bitmap.h>
#include <sys/nvpair.h>
#include <sys/sunddi.h>		/* bzero/memcpy/strlcpy */

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "nvmft_var.h"

/*
 * A bitmap of in-flight command ID values, used to detect duplicate commands
 * with the same CID on a queue pair.  (FreeBSD: BITSET_DEFINE(cidset, ...).)
 */
#define	NUM_CIDS	(UINT16_MAX + 1)
#define	CIDSET_WORDS	BT_BITOUL(NUM_CIDS)

/*
 * The packed 16-bit NVMe status word places the phase bit at bit 0, the Status
 * Code at bits 8:1, and the Status Code Type at bits 11:9.  That is the exact
 * layout of nvme_cqe_sf_t on a little-endian host (sf_p:1, sf_sc:8, sf_sct:3).
 * These helpers pack/unpack that word; the phase bit is owned by the transport
 * and is always left clear here.
 */
#define	NVMFT_STATUS(sct, sc) \
	(((uint16_t)(sct) << 9) | ((uint16_t)(sc) << 1))
#define	NVMFT_STATUS_SC(status)		(((status) >> 1) & 0xff)
#define	NVMFT_STATUS_SCT(status)	(((status) >> 9) & 0x7)

struct nvmft_qpair {
	nvmft_controller_t	*qp_ctrlr;
	struct nvmf_qpair	*qp_qp;
	ulong_t			*qp_cids;	/* CIDSET_WORDS ulong_t */

	boolean_t		qp_admin;
	boolean_t		qp_sq_flow_control;
	uint16_t		qp_qid;
	uint_t			qp_qsize;
	uint16_t		qp_sqhd;
	volatile uint_t		qp_refs;	/* internal refs on qp_qp */

	kmutex_t		qp_lock;

	char			qp_name[16];
};

static int	_nvmft_send_generic_error(struct nvmft_qpair *qp,
    struct nvmf_capsule *nc, uint8_t sc_status);

static void
nvmft_qpair_error(void *arg, int error)
{
	struct nvmft_qpair *qp = arg;
	nvmft_controller_t *ctrlr = qp->qp_ctrlr;

	/*
	 * The Linux TCP initiator sends a RST immediately after the FIN, so
	 * treat ECONNRESET as a plain EOF to avoid spurious shutdown errors.
	 */
	if (error == ECONNRESET)
		error = 0;

	if (error != 0)
		(void) nvmft_printf(ctrlr, "error %d on %s\n", error,
		    qp->qp_name);
	nvmft_controller_error(ctrlr, qp, error);
}

static void
nvmft_receive_capsule(void *arg, struct nvmf_capsule *nc)
{
	struct nvmft_qpair *qp = arg;
	nvmft_controller_t *ctrlr = qp->qp_ctrlr;
	const nvme_sqe_t *cmd;
	uint8_t sc_status;

	cmd = nvmf_capsule_sqe(nc);
	if (ctrlr == NULL) {
		NVMFT_DPRINTF_L1("%s received CID %u opcode %u on newborn queue",
		    qp->qp_name, LE_16(cmd->sqe_cid), cmd->sqe_opc);
		nvmf_free_capsule(nc);
		return;
	}

	sc_status = nvmf_validate_command_capsule(nc);
	if (sc_status != 0) {
		(void) _nvmft_send_generic_error(qp, nc, sc_status);
		nvmf_free_capsule(nc);
		return;
	}

	/* Don't bother byte-swapping CID. */
	mutex_enter(&qp->qp_lock);
	if (BT_TEST(qp->qp_cids, cmd->sqe_cid)) {
		mutex_exit(&qp->qp_lock);
		(void) _nvmft_send_generic_error(qp, nc,
		    NVME_CQE_SC_GEN_ID_CNFL);
		nvmf_free_capsule(nc);
		return;
	}
	BT_SET(qp->qp_cids, cmd->sqe_cid);
	mutex_exit(&qp->qp_lock);

	if (qp->qp_admin)
		nvmft_handle_admin_command(ctrlr, nc);
	else
		nvmft_handle_io_command(qp, qp->qp_qid, nc);
}

struct nvmft_qpair *
nvmft_qpair_init(nvmf_trtype_t trtype, const nvlist_t *params, uint16_t qid,
    const char *name)
{
	struct nvmft_qpair *qp;
	boolean_t admin = B_FALSE, sqfc = B_FALSE;
	uint64_t qsize = 0, sqhd = 0;

	qp = kmem_zalloc(sizeof (*qp), KM_SLEEP);

	(void) nvlist_lookup_boolean_value((nvlist_t *)params, "admin", &admin);
	(void) nvlist_lookup_boolean_value((nvlist_t *)params,
	    "sq_flow_control", &sqfc);
	(void) nvlist_lookup_uint64((nvlist_t *)params, "qsize", &qsize);
	(void) nvlist_lookup_uint64((nvlist_t *)params, "sqhd", &sqhd);

	qp->qp_admin = admin;
	qp->qp_sq_flow_control = sqfc;
	qp->qp_qsize = (uint_t)qsize;
	qp->qp_qid = qid;
	qp->qp_sqhd = (uint16_t)sqhd;
	(void) strlcpy(qp->qp_name, name, sizeof (qp->qp_name));
	mutex_init(&qp->qp_lock, NULL, MUTEX_DRIVER, NULL);
	qp->qp_cids = kmem_zalloc(CIDSET_WORDS * sizeof (ulong_t), KM_SLEEP);

	qp->qp_qp = nvmf_allocate_qpair(trtype, B_TRUE, params,
	    nvmft_qpair_error, qp, nvmft_receive_capsule, qp);
	if (qp->qp_qp == NULL) {
		mutex_destroy(&qp->qp_lock);
		kmem_free(qp->qp_cids, CIDSET_WORDS * sizeof (ulong_t));
		kmem_free(qp, sizeof (*qp));
		return (NULL);
	}

	qp->qp_refs = 1;
	return (qp);
}

void
nvmft_qpair_shutdown(struct nvmft_qpair *qp)
{
	struct nvmf_qpair *nq;
	boolean_t free_it;

	mutex_enter(&qp->qp_lock);
	nq = qp->qp_qp;
	qp->qp_qp = NULL;
	free_it = (nq != NULL && --qp->qp_refs == 0);
	mutex_exit(&qp->qp_lock);

	if (free_it)
		nvmf_free_qpair(nq);
}

void
nvmft_qpair_destroy(struct nvmft_qpair *qp)
{
	nvmft_qpair_shutdown(qp);
	mutex_destroy(&qp->qp_lock);
	kmem_free(qp->qp_cids, CIDSET_WORDS * sizeof (ulong_t));
	kmem_free(qp, sizeof (*qp));
}

nvmft_controller_t *
nvmft_qpair_ctrlr(struct nvmft_qpair *qp)
{
	return (qp->qp_ctrlr);
}

uint16_t
nvmft_qpair_id(struct nvmft_qpair *qp)
{
	return (qp->qp_qid);
}

const char *
nvmft_qpair_name(struct nvmft_qpair *qp)
{
	return (qp->qp_name);
}

uint32_t
nvmft_max_ioccsz(struct nvmft_qpair *qp)
{
	return (nvmf_max_ioccsz(qp->qp_qp));
}

static int
_nvmft_send_response(struct nvmft_qpair *qp, const void *cqe)
{
	nvme_cqe_t cpl;
	struct nvmf_qpair *nq;
	struct nvmf_capsule *rc;
	boolean_t free_it;
	int error;

	(void) memcpy(&cpl, cqe, sizeof (cpl));
	mutex_enter(&qp->qp_lock);
	nq = qp->qp_qp;
	if (nq == NULL) {
		mutex_exit(&qp->qp_lock);
		return (ENOTCONN);
	}
	qp->qp_refs++;

	/* Set SQHD. */
	if (qp->qp_sq_flow_control) {
		qp->qp_sqhd = (qp->qp_sqhd + 1) % qp->qp_qsize;
		cpl.cqe_sqhd = LE_16(qp->qp_sqhd);
	} else {
		cpl.cqe_sqhd = 0;
	}
	mutex_exit(&qp->qp_lock);

	rc = nvmf_allocate_response(nq, &cpl, KM_SLEEP);
	error = nvmf_transmit_capsule(rc);
	nvmf_free_capsule(rc);

	mutex_enter(&qp->qp_lock);
	free_it = (--qp->qp_refs == 0);
	mutex_exit(&qp->qp_lock);
	if (free_it)
		nvmf_free_qpair(nq);
	return (error);
}

/*
 * Reference handshake for an STMF data transfer (nvmft_lport_xfer_data), which
 * runs on an STMF worker thread and dereferences the transport qpair while a
 * concurrent nvmft_qpair_shutdown() may free it.  _hold() returns the transport
 * qpair to use, or NULL if the qpair has already been shut down (the caller
 * must then fail the transfer); _rele() drops the reference once the transport
 * send/receive has been issued, freeing the transport qpair if it was the last
 * reference.  Same handshake as _nvmft_send_response(), exposed for nvmft_stmf.c
 * because struct nvmft_qpair is opaque there.
 */
struct nvmf_qpair *
nvmft_qpair_data_hold(struct nvmft_qpair *qp)
{
	struct nvmf_qpair *nq;

	mutex_enter(&qp->qp_lock);
	nq = qp->qp_qp;
	if (nq != NULL)
		qp->qp_refs++;
	mutex_exit(&qp->qp_lock);
	return (nq);
}

void
nvmft_qpair_data_rele(struct nvmft_qpair *qp, struct nvmf_qpair *nq)
{
	boolean_t free_it;

	mutex_enter(&qp->qp_lock);
	free_it = (--qp->qp_refs == 0);
	mutex_exit(&qp->qp_lock);
	if (free_it)
		nvmf_free_qpair(nq);
}

void
nvmft_command_completed(struct nvmft_qpair *qp, struct nvmf_capsule *nc)
{
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);

	mutex_enter(&qp->qp_lock);
	ASSERT(BT_TEST(qp->qp_cids, cmd->sqe_cid));
	BT_CLEAR(qp->qp_cids, cmd->sqe_cid);
	mutex_exit(&qp->qp_lock);
}

int
nvmft_send_response(struct nvmft_qpair *qp, const void *cqe)
{
	const nvme_cqe_t *cpl = cqe;

	mutex_enter(&qp->qp_lock);
	ASSERT(BT_TEST(qp->qp_cids, cpl->cqe_cid));
	BT_CLEAR(qp->qp_cids, cpl->cqe_cid);
	mutex_exit(&qp->qp_lock);
	return (_nvmft_send_response(qp, cqe));
}

void
nvmft_init_cqe(void *cqe, struct nvmf_capsule *nc, uint16_t status)
{
	nvme_cqe_t *cpl = cqe;
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);

	(void) bzero(cpl, sizeof (*cpl));
	cpl->cqe_cid = cmd->sqe_cid;
	cpl->cqe_sf.sf_sc = NVMFT_STATUS_SC(status);
	cpl->cqe_sf.sf_sct = NVMFT_STATUS_SCT(status);
}

int
nvmft_send_error(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
    uint8_t sc_type, uint8_t sc_status)
{
	nvme_cqe_t cpl;

	nvmft_init_cqe(&cpl, nc, NVMFT_STATUS(sc_type, sc_status));
	return (nvmft_send_response(qp, &cpl));
}

int
nvmft_send_generic_error(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
    uint8_t sc_status)
{
	return (nvmft_send_error(qp, nc, NVME_CQE_SCT_GENERIC, sc_status));
}

/*
 * Send a generic error without clearing the CID; used for errors raised before
 * the CID has been validated/recorded in qp_cids.
 */
static int
_nvmft_send_generic_error(struct nvmft_qpair *qp, struct nvmf_capsule *nc,
    uint8_t sc_status)
{
	nvme_cqe_t cpl;

	nvmft_init_cqe(&cpl, nc, NVMFT_STATUS(NVME_CQE_SCT_GENERIC, sc_status));
	return (_nvmft_send_response(qp, &cpl));
}

int
nvmft_send_success(struct nvmft_qpair *qp, struct nvmf_capsule *nc)
{
	return (nvmft_send_generic_error(qp, nc, NVME_CQE_SC_GEN_SUCCESS));
}

static void
nvmft_init_connect_rsp(nvmf_fabric_connect_rsp_t *rsp,
    const nvmf_fabric_connect_cmd_t *cmd, uint16_t status)
{
	(void) bzero(rsp, sizeof (*rsp));
	rsp->nfcr_cid = cmd->nfcc_cid;
	rsp->nfcr_status = LE_16(status);
}

static int
nvmft_send_connect_response(struct nvmft_qpair *qp,
    const nvmf_fabric_connect_rsp_t *rsp)
{
	struct nvmf_capsule *rc;
	struct nvmf_qpair *nq;
	boolean_t free_it;
	int error;

	mutex_enter(&qp->qp_lock);
	nq = qp->qp_qp;
	if (nq == NULL) {
		mutex_exit(&qp->qp_lock);
		return (ENOTCONN);
	}
	qp->qp_refs++;
	mutex_exit(&qp->qp_lock);

	rc = nvmf_allocate_response(nq, rsp, KM_SLEEP);
	error = nvmf_transmit_capsule(rc);
	nvmf_free_capsule(rc);

	mutex_enter(&qp->qp_lock);
	free_it = (--qp->qp_refs == 0);
	mutex_exit(&qp->qp_lock);
	if (free_it)
		nvmf_free_qpair(nq);
	return (error);
}

void
nvmft_connect_error(struct nvmft_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd, uint8_t sc_type, uint8_t sc_status)
{
	nvmf_fabric_connect_rsp_t rsp;

	nvmft_init_connect_rsp(&rsp, cmd, NVMFT_STATUS(sc_type, sc_status));
	(void) nvmft_send_connect_response(qp, &rsp);
}

void
nvmft_connect_invalid_parameters(struct nvmft_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd, boolean_t data, uint16_t offset)
{
	nvmf_fabric_connect_rsp_t rsp;

	nvmft_init_connect_rsp(&rsp, cmd,
	    NVMFT_STATUS(NVME_CQE_SCT_SPECIFIC, NVMF_FABRIC_SC_INVALID_PARAM));
	rsp.nfcr_status_code_specific.invalid.ipo = LE_16(offset);
	rsp.nfcr_status_code_specific.invalid.iattr = data ? 1 : 0;
	(void) nvmft_send_connect_response(qp, &rsp);
}

int
nvmft_finish_accept(struct nvmft_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd, nvmft_controller_t *ctrlr)
{
	nvmf_fabric_connect_rsp_t rsp;

	qp->qp_ctrlr = ctrlr;
	nvmft_init_connect_rsp(&rsp, cmd, 0);
	if (qp->qp_sq_flow_control)
		rsp.nfcr_sqhd = LE_16(qp->qp_sqhd);
	else
		rsp.nfcr_sqhd = LE_16(0xffff);
	rsp.nfcr_status_code_specific.success.cntlid = LE_16(ctrlr->ctrlr_cntlid);
	return (nvmft_send_connect_response(qp, &rsp));
}

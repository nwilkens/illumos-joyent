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
#include <sys/atomic.h>
#include <sys/taskq.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "nvmft_var.h"
#include "../../../nvmf/nvmf_core.h"

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

/*
 * A queue is NEW until its Connect is accepted.  A kernel-born queue moves
 * through CONNECT (work queued), DATA (Connect data in flight) and READY (data
 * arrived, work queued).  Any of these can become DYING on a transport error.
 */
typedef enum {
	NVMFT_QP_NEW = 0,
	NVMFT_QP_CONNECT,
	NVMFT_QP_DATA,
	NVMFT_QP_READY,
	NVMFT_QP_DYING,
	NVMFT_QP_CONNECTED
} nvmft_qp_state_t;

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
	nvmf_trtype_t		qp_trtype;
	uint32_t		qp_caps;

	kmutex_t		qp_lock;
	volatile nvmft_qp_state_t qp_state;
	boolean_t		qp_error_latched;
	int			qp_error;

	/* Kernel-born queues only: nvmft parses the Connect itself. */
	boolean_t		qp_kernel;
	uint16_t		qp_cm_sqsize;
	int			qp_data_error;
	struct nvmf_capsule	*qp_connect_nc;
	nvmf_fabric_connect_data_t *qp_connect_data;
	taskq_ent_t		qp_connect_task;

	char			qp_name[16];
};

static int	_nvmft_send_generic_error(struct nvmft_qpair *qp,
    struct nvmf_capsule *nc, uint8_t sc_status);
static void	nvmft_qpair_connect_task(void *arg);

/* Called with qp_lock held; the task entry is idle in these states. */
static void
nvmft_qpair_dispatch(struct nvmft_qpair *qp, nvmft_qp_state_t state)
{
	ASSERT(MUTEX_HELD(&qp->qp_lock));
	qp->qp_state = state;
	taskq_dispatch_ent(nvmft_global->ns_taskq, nvmft_qpair_connect_task,
	    qp, 0, &qp->qp_connect_task);
}

static void
nvmft_qpair_error(void *arg, int error)
{
	struct nvmft_qpair *qp = arg;
	nvmft_controller_t *ctrlr;

	/*
	 * The Linux TCP initiator sends a RST immediately after the FIN, so
	 * treat ECONNRESET as a plain EOF to avoid spurious shutdown errors.
	 */
	if (error == ECONNRESET)
		error = 0;

	if (qp->qp_state != NVMFT_QP_CONNECTED) {
		mutex_enter(&qp->qp_lock);
		switch (qp->qp_state) {
		case NVMFT_QP_CONNECTED:
			break;
		case NVMFT_QP_NEW:
		case NVMFT_QP_DATA:
			if (!qp->qp_error_latched) {
				qp->qp_error_latched = B_TRUE;
				qp->qp_error = error;
			}
			if (qp->qp_kernel)
				nvmft_qpair_dispatch(qp, NVMFT_QP_DYING);
			mutex_exit(&qp->qp_lock);
			return;
		default:
			/*
			 * Connect work is queued or running.  It sees DYING, or
			 * it connects the queue and delivers the latched error.
			 */
			if (!qp->qp_error_latched) {
				qp->qp_error_latched = B_TRUE;
				qp->qp_error = error;
			}
			qp->qp_state = NVMFT_QP_DYING;
			mutex_exit(&qp->qp_lock);
			return;
		}
		mutex_exit(&qp->qp_lock);
	}
	membar_consumer();
	ctrlr = qp->qp_ctrlr;

	if (error != 0)
		(void) nvmft_printf(ctrlr, "error %d on %s\n", error,
		    qp->qp_name);
	nvmft_controller_error(ctrlr, qp, error);
}

static void
nvmft_receive_capsule(void *arg, struct nvmf_capsule *nc)
{
	struct nvmft_qpair *qp = arg;
	nvmft_controller_t *ctrlr;
	const nvme_sqe_t *cmd;
	uint8_t sc_status;

	cmd = nvmf_capsule_sqe(nc);
	if (qp->qp_state != NVMFT_QP_CONNECTED) {
		mutex_enter(&qp->qp_lock);
		if (qp->qp_kernel && qp->qp_state == NVMFT_QP_NEW) {
			qp->qp_connect_nc = nc;
			nvmft_qpair_dispatch(qp, NVMFT_QP_CONNECT);
			mutex_exit(&qp->qp_lock);
			return;
		}
		if (qp->qp_state != NVMFT_QP_CONNECTED) {
			mutex_exit(&qp->qp_lock);
			NVMFT_DPRINTF_L2("%s received CID %u opcode %u on "
			    "newborn queue", qp->qp_name, LE_16(cmd->sqe_cid),
			    cmd->sqe_opc);
			nvmf_free_capsule(nc);
			return;
		}
		mutex_exit(&qp->qp_lock);
	}
	membar_consumer();
	ctrlr = qp->qp_ctrlr;

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

	if (!qp->qp_admin)
		nvmft_handle_io_command(qp, qp->qp_qid, nc);
	else if (qp->qp_caps & NVMF_QP_CAP_NOSLEEP_RECEIVE)
		nvmft_queue_admin_command(ctrlr, nc);
	else
		nvmft_handle_admin_command(ctrlr, nc);
}

static struct nvmft_qpair *
nvmft_qpair_alloc(nvmf_trtype_t trtype, uint16_t qid, const char *name,
    int kmflag)
{
	struct nvmft_qpair *qp;

	qp = kmem_zalloc(sizeof (*qp), kmflag);
	if (qp == NULL)
		return (NULL);
	qp->qp_cids = kmem_zalloc(CIDSET_WORDS * sizeof (ulong_t), kmflag);
	if (qp->qp_cids == NULL) {
		kmem_free(qp, sizeof (*qp));
		return (NULL);
	}
	qp->qp_trtype = trtype;
	qp->qp_qid = qid;
	(void) strlcpy(qp->qp_name, name, sizeof (qp->qp_name));
	mutex_init(&qp->qp_lock, NULL, MUTEX_DRIVER, NULL);
	return (qp);
}

static void
nvmft_qpair_free(struct nvmft_qpair *qp)
{
	boolean_t kernel = qp->qp_kernel;

	if (qp->qp_connect_data != NULL)
		kmem_free(qp->qp_connect_data, sizeof (*qp->qp_connect_data));
	mutex_destroy(&qp->qp_lock);
	kmem_free(qp->qp_cids, CIDSET_WORDS * sizeof (ulong_t));
	kmem_free(qp, sizeof (*qp));

	if (kernel) {
		mutex_enter(&nvmft_global->ns_lock);
		ASSERT3U(nvmft_global->ns_kernel_qpairs, >, 0);
		nvmft_global->ns_kernel_qpairs--;
		mutex_exit(&nvmft_global->ns_lock);
	}
}

struct nvmft_qpair *
nvmft_qpair_init(nvmf_trtype_t trtype, const nvlist_t *params, uint16_t qid,
    const char *name)
{
	struct nvmft_qpair *qp;
	boolean_t admin = B_FALSE, sqfc = B_FALSE;
	uint64_t qsize = 0, sqhd = 0;

	qp = nvmft_qpair_alloc(trtype, qid, name, KM_SLEEP);

	(void) nvlist_lookup_boolean_value((nvlist_t *)params, "admin", &admin);
	(void) nvlist_lookup_boolean_value((nvlist_t *)params,
	    "sq_flow_control", &sqfc);
	(void) nvlist_lookup_uint64((nvlist_t *)params, "qsize", &qsize);
	(void) nvlist_lookup_uint64((nvlist_t *)params, "sqhd", &sqhd);

	qp->qp_admin = admin;
	qp->qp_sq_flow_control = sqfc;
	qp->qp_qsize = (uint_t)qsize;
	qp->qp_sqhd = (uint16_t)sqhd;

	qp->qp_qp = nvmf_allocate_qpair(trtype, B_TRUE, params,
	    nvmft_qpair_error, qp, nvmft_receive_capsule, qp);
	if (qp->qp_qp == NULL) {
		nvmft_qpair_free(qp);
		return (NULL);
	}

	qp->qp_caps = nvmf_qpair_caps(qp->qp_qp);
	qp->qp_refs = 1;
	return (qp);
}

/*
 * Take ownership of a controller qpair that a transport created in the kernel.
 * qid and sqsize are the values from the transport's connection setup; the
 * Connect command must repeat them.  This does not sleep.  On failure the
 * transport keeps nq.
 */
int
nvmft_adopt_qpair(struct nvmf_transport_ops *ops, struct nvmf_qpair *nq,
    uint16_t qid, uint16_t sqsize)
{
	struct nvmft_qpair *qp;
	char name[16];
	int error;

	if (qid == 0)
		(void) strlcpy(name, "admin queue", sizeof (name));
	else
		(void) snprintf(name, sizeof (name), "I/O queue %u", qid);
	qp = nvmft_qpair_alloc(ops->trtype, qid, name, KM_NOSLEEP);
	if (qp == NULL)
		return (ENOMEM);
	qp->qp_connect_data = kmem_zalloc(sizeof (*qp->qp_connect_data),
	    KM_NOSLEEP);
	if (qp->qp_connect_data == NULL) {
		nvmft_qpair_free(qp);
		return (ENOMEM);
	}
	qp->qp_admin = (qid == 0);
	qp->qp_cm_sqsize = sqsize;

	mutex_enter(&nvmft_global->ns_lock);
	if (nvmft_global->ns_closing) {
		mutex_exit(&nvmft_global->ns_lock);
		nvmft_qpair_free(qp);
		return (ENXIO);
	}
	nvmft_global->ns_kernel_qpairs++;
	qp->qp_kernel = B_TRUE;
	mutex_exit(&nvmft_global->ns_lock);

	qp->qp_qp = nq;
	qp->qp_refs = 1;
	qp->qp_caps = ops->caps != NULL ? ops->caps(nq) : 0;
	error = nvmf_adopt_qpair(ops, nq, B_TRUE, qp->qp_admin,
	    nvmft_qpair_error, qp, nvmft_receive_capsule, qp);
	if (error != 0)
		nvmft_qpair_free(qp);
	return (error);
}

nvmf_trtype_t
nvmft_qpair_trtype(struct nvmft_qpair *qp)
{
	return (qp->qp_trtype);
}

uint32_t
nvmft_qpair_caps(struct nvmft_qpair *qp)
{
	return (qp->qp_caps);
}

static void
nvmft_connect_reject(struct nvmft_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd, const nvmft_connect_status_t *st)
{
	if (st->ncs_invalid) {
		nvmft_connect_invalid_parameters(qp, cmd, st->ncs_iattr,
		    st->ncs_ipo);
	} else {
		nvmft_connect_error(qp, cmd, st->ncs_sct, st->ncs_sc);
	}
}

static void
nvmft_connect_data_done(void *arg, size_t xfered, int error)
{
	struct nvmft_qpair *qp = arg;

	if (error == 0 && xfered != sizeof (nvmf_fabric_connect_data_t))
		error = EIO;
	mutex_enter(&qp->qp_lock);
	if (qp->qp_state == NVMFT_QP_DATA) {
		qp->qp_data_error = error;
		nvmft_qpair_dispatch(qp, NVMFT_QP_READY);
	}
	mutex_exit(&qp->qp_lock);
}

/* Check the Connect command and start the transfer of its data. */
static boolean_t
nvmft_connect_start(struct nvmft_qpair *qp)
{
	struct nvmf_capsule *nc = qp->qp_connect_nc;
	const nvmf_fabric_connect_cmd_t *cmd = nvmf_capsule_sqe(nc);
	nvmft_connect_status_t st;
	nvmf_memdesc_t mem;
	uint8_t sc;
	int error;

	sc = nvmf_validate_command_capsule(nc);
	if (sc != NVME_CQE_SC_GEN_SUCCESS) {
		(void) _nvmft_send_generic_error(qp, nc, sc);
		return (B_FALSE);
	}
	if (!nvmft_connect_cmd_valid(cmd, nvmf_capsule_data_len(nc),
	    qp->qp_qid, qp->qp_cm_sqsize, NVME_MAX_ADMIN_ENTRIES,
	    NVMF_MAX_IO_ENTRIES, &st)) {
		nvmft_connect_reject(qp, cmd, &st);
		return (B_FALSE);
	}

	mem.nmd_type = NVMF_MEMDESC_VADDR;
	mem.nmd_len = sizeof (*qp->qp_connect_data);
	mem.nmd_u.nmd_vaddr = qp->qp_connect_data;
	mutex_enter(&qp->qp_lock);
	if (qp->qp_state == NVMFT_QP_DYING) {
		mutex_exit(&qp->qp_lock);
		return (B_FALSE);
	}
	qp->qp_state = NVMFT_QP_DATA;
	mutex_exit(&qp->qp_lock);

	error = nvmf_receive_controller_data(nc, 0, &mem, mem.nmd_len,
	    nvmft_connect_data_done, qp);
	if (error != 0) {
		mutex_enter(&qp->qp_lock);
		if (qp->qp_state == NVMFT_QP_DATA) {
			qp->qp_data_error = error;
			nvmft_qpair_dispatch(qp, NVMFT_QP_READY);
		}
		mutex_exit(&qp->qp_lock);
	}
	return (B_TRUE);
}

/*
 * Check the Connect data and join or create the association.  Returns B_FALSE
 * if qp is still ours to destroy.
 */
static boolean_t
nvmft_connect_finish(struct nvmft_qpair *qp)
{
	const nvmf_fabric_connect_cmd_t *cmd = nvmf_capsule_sqe(
	    qp->qp_connect_nc);
	const nvmf_fabric_connect_data_t *data = qp->qp_connect_data;
	char subnqn[NVMF_NQN_FIELD_SIZE + 1];
	nvmft_connect_status_t st;
	nvmft_port_t *np;
	nvmf_fabric_connect_cmd_t cmdc;
	int error;

	if (qp->qp_data_error != 0) {
		nvmft_connect_error(qp, cmd, NVME_CQE_SCT_GENERIC,
		    NVME_CQE_SC_GEN_DATA_XFR_ERR);
		return (B_FALSE);
	}
	if (!nvmft_connect_data_valid(cmd, data, &st)) {
		nvmft_connect_reject(qp, cmd, &st);
		return (B_FALSE);
	}

	(void) memcpy(subnqn, data->nfcd_subnqn, sizeof (data->nfcd_subnqn));
	subnqn[sizeof (data->nfcd_subnqn)] = '\0';
	np = nvmft_port_find(subnqn);
	if (np == NULL) {
		nvmft_connect_invalid_parameters(qp, cmd, B_TRUE,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_subnqn));
		return (B_FALSE);
	}
	if (!qp->qp_admin && (uint32_t)LE_16(cmd->nfcc_sqsize) + 1 >
	    np->np_max_io_qsize) {
		nvmft_port_rele(np);
		nvmft_connect_invalid_parameters(qp, cmd, B_FALSE,
		    offsetof(nvmf_fabric_connect_cmd_t, nfcc_sqsize));
		return (B_FALSE);
	}

	/* SQ flow control is always on, as nvmfd requires by default. */
	qp->qp_qsize = (uint_t)LE_16(cmd->nfcc_sqsize) + 1;
	qp->qp_sq_flow_control = B_TRUE;
	qp->qp_sqhd = 0;

	/* The capsule goes back to the transport before the queue is live. */
	(void) memcpy(&cmdc, cmd, sizeof (cmdc));
	nvmf_free_capsule(qp->qp_connect_nc);
	qp->qp_connect_nc = NULL;

	/* On failure these destroy qp. */
	if (qp->qp_admin)
		(void) nvmft_connect_admin_queue(np, qp, &cmdc, data);
	else
		(void) nvmft_connect_io_queue(np, qp, &cmdc, data);
	nvmft_port_rele(np);
	return (B_TRUE);
}

static void
nvmft_qpair_connect_task(void *arg)
{
	struct nvmft_qpair *qp = arg;
	nvmft_qp_state_t state;
	boolean_t keep;

	mutex_enter(&qp->qp_lock);
	state = qp->qp_state;
	mutex_exit(&qp->qp_lock);

	switch (state) {
	case NVMFT_QP_CONNECT:
		keep = nvmft_connect_start(qp);
		break;
	case NVMFT_QP_READY:
		keep = nvmft_connect_finish(qp);
		break;
	default:
		ASSERT3U(state, ==, NVMFT_QP_DYING);
		keep = B_FALSE;
		break;
	}
	if (keep)
		return;

	/*
	 * Freeing the transport qpair completes any Connect data transfer,
	 * whose callback sees DYING and does nothing.  Only then can the
	 * capsule go, which its transport qpair outlives.
	 */
	mutex_enter(&qp->qp_lock);
	qp->qp_state = NVMFT_QP_DYING;
	mutex_exit(&qp->qp_lock);
	nvmft_qpair_shutdown(qp);
	if (qp->qp_connect_nc != NULL)
		nvmf_free_capsule(qp->qp_connect_nc);
	nvmft_qpair_free(qp);
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
	nvmft_qpair_free(qp);
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

/* Called with qp_lock held. */
static void
nvmft_stamp_sqhd(struct nvmft_qpair *qp, nvme_cqe_t *cpl)
{
	ASSERT(MUTEX_HELD(&qp->qp_lock));
	if (qp->qp_sq_flow_control) {
		qp->qp_sqhd = (qp->qp_sqhd + 1) % qp->qp_qsize;
		cpl->cqe_sqhd = LE_16(qp->qp_sqhd);
	} else {
		cpl->cqe_sqhd = 0;
	}
}

static int
nvmft_transmit_cqe(struct nvmft_qpair *qp, const void *cqe, boolean_t stamp)
{
	nvme_cqe_t cpl;
	struct nvmf_qpair *nq;
	struct nvmf_capsule *rc;
	boolean_t free_it;
	int error, kmflag;

	(void) memcpy(&cpl, cqe, sizeof (cpl));
	mutex_enter(&qp->qp_lock);
	nq = qp->qp_qp;
	if (nq == NULL) {
		mutex_exit(&qp->qp_lock);
		return (ENOTCONN);
	}
	qp->qp_refs++;
	if (stamp)
		nvmft_stamp_sqhd(qp, &cpl);
	mutex_exit(&qp->qp_lock);

	kmflag = (qp->qp_caps & NVMF_QP_CAP_NOSLEEP_RECEIVE) ? KM_NOSLEEP :
	    KM_SLEEP;
	rc = nvmf_allocate_response(nq, &cpl, kmflag);
	if (rc != NULL) {
		error = nvmf_transmit_capsule(rc);
		nvmf_free_capsule(rc);
	} else {
		error = ENOMEM;
	}

	mutex_enter(&qp->qp_lock);
	free_it = (--qp->qp_refs == 0);
	mutex_exit(&qp->qp_lock);
	if (free_it)
		nvmf_free_qpair(nq);
	return (error);
}

static int
_nvmft_send_response(struct nvmft_qpair *qp, const void *cqe)
{
	return (nvmft_transmit_cqe(qp, cqe, B_TRUE));
}

/*
 * Retire the CID of a response and stamp its SQHD, for a response that the
 * transport sends after the command's data.  The caller must send it once,
 * with nvmft_transmit_response() if the transport did not take it.
 */
void
nvmft_prepare_response(struct nvmft_qpair *qp, void *cqe)
{
	nvme_cqe_t *cpl = cqe;

	mutex_enter(&qp->qp_lock);
	ASSERT(BT_TEST(qp->qp_cids, cpl->cqe_cid));
	BT_CLEAR(qp->qp_cids, cpl->cqe_cid);
	nvmft_stamp_sqhd(qp, cpl);
	mutex_exit(&qp->qp_lock);
}

int
nvmft_transmit_response(struct nvmft_qpair *qp, const void *cqe)
{
	return (nvmft_transmit_cqe(qp, cqe, B_FALSE));
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
	boolean_t latched;
	int error, qerror;

	mutex_enter(&qp->qp_lock);
	qp->qp_ctrlr = ctrlr;
	membar_producer();
	qp->qp_state = NVMFT_QP_CONNECTED;
	latched = qp->qp_error_latched;
	qerror = qp->qp_error;
	qp->qp_error_latched = B_FALSE;
	mutex_exit(&qp->qp_lock);

	nvmft_init_connect_rsp(&rsp, cmd, 0);
	if (qp->qp_sq_flow_control)
		rsp.nfcr_sqhd = LE_16(qp->qp_sqhd);
	else
		rsp.nfcr_sqhd = LE_16(0xffff);
	rsp.nfcr_status_code_specific.success.cntlid = LE_16(ctrlr->ctrlr_cntlid);
	error = nvmft_send_connect_response(qp, &rsp);

	/* An error that arrived before the queue was connected. */
	if (latched)
		nvmft_qpair_error(qp, qerror);
	return (error);
}

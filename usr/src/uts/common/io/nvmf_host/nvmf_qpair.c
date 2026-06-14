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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/host/nvmf_qpair.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Host queue-pair management: command-slot (cid) allocation, request
 * submission with submission-queue flow control, and completion dispatch.
 * This is core protocol logic and ports closely; OS-glue substitutions:
 *
 *   FreeBSD                 illumos
 *   -------                 -------
 *   struct mtx + cv via     kmutex_t + kcondvar_t
 *     mtx_sleep/wakeup
 *   TAILQ/STAILQ            list_t (free_commands, pending_requests)
 *   malloc(M_NVMF)          kmem_zalloc / kmem_alloc
 *   atomic_store_int        the keep-alive traffic flags are touched with
 *                           atomic_swap_uint here, matching FreeBSD's atomics
 *                           on an int and the atomic load in nvmf_host.c.
 *   le16toh(cqe->sqhd)      illumos is little-endian only
 *   sysctl per-queue stats  TODO (see PORT-TODO below)
 *
 * The active_commands array is indexed by cid.  As in FreeBSD, the cid in the
 * SQE/CQE is deliberately NOT byte-swapped so the receive path does not have
 * to swap it back.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/atomic.h>
#include <sys/errno.h>
#include <sys/nvpair.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"

typedef struct nvmf_host_command {
	nvmf_request_t	*req;
	list_node_t	link;
	uint16_t	cid;
} nvmf_host_command_t;

typedef struct nvmf_host_qpair {
	nvmf_softc_t		*sc;
	struct nvmf_qpair	*qp;

	boolean_t		sq_flow_control;
	boolean_t		shutting_down;
	uint_t			allocating;
	uint_t			num_commands;
	uint16_t		sqhd;
	uint16_t		sqtail;
	uint64_t		submitted;

	kmutex_t		lock;
	kcondvar_t		cv;

	list_t			free_commands;		/* nvmf_host_command_t */
	list_t			pending_requests;	/* nvmf_request_t */

	/* Indexed by cid. */
	nvmf_host_command_t	**active_commands;

	char			name[16];
} nvmf_host_qpair_t;

nvmf_request_t *
nvmf_allocate_request(nvmf_host_qpair_t *qp, void *sqe,
    nvmf_request_complete_t *cb, void *cb_arg, int how)
{
	nvmf_request_t *req;
	struct nvmf_qpair *nq;

	ASSERT(how == KM_SLEEP || how == KM_NOSLEEP);

	req = kmem_zalloc(sizeof (*req), how);
	if (req == NULL)
		return (NULL);

	mutex_enter(&qp->lock);
	nq = qp->qp;
	if (nq == NULL) {
		mutex_exit(&qp->lock);
		kmem_free(req, sizeof (*req));
		return (NULL);
	}
	qp->allocating++;
	ASSERT(qp->allocating != 0);
	mutex_exit(&qp->lock);

	req->qp = qp;
	req->cb = cb;
	req->cb_arg = cb_arg;
	req->nc = nvmf_allocate_command(nq, sqe, how);
	if (req->nc == NULL) {
		kmem_free(req, sizeof (*req));
		req = NULL;
	}

	mutex_enter(&qp->lock);
	qp->allocating--;
	if (qp->allocating == 0 && qp->shutting_down)
		cv_broadcast(&qp->cv);
	mutex_exit(&qp->lock);

	return (req);
}

static void
nvmf_abort_request(nvmf_request_t *req, uint16_t cid)
{
	nvme_cqe_t cqe;

	bzero(&cqe, sizeof (cqe));
	cqe.cqe_cid = cid;
	cqe.cqe_sf.sf_sct = NVME_CQE_SCT_PATH;
	cqe.cqe_sf.sf_sc = NVME_CQE_SC_PATH_HOST_ABRT;
	req->cb(req->cb_arg, &cqe);
}

void
nvmf_free_request(nvmf_request_t *req)
{
	if (req->nc != NULL)
		nvmf_free_capsule(req->nc);
	kmem_free(req, sizeof (*req));
}

static void
nvmf_dispatch_command(nvmf_host_qpair_t *qp, nvmf_host_command_t *cmd)
{
	nvmf_softc_t *sc = qp->sc;
	nvme_sqe_t *sqe;
	struct nvmf_capsule *nc;
	uint16_t new_sqtail;
	int error;

	ASSERT(MUTEX_HELD(&qp->lock));

	qp->submitted++;

	/*
	 * Update flow control tracking.  Since num_commands == qsize - 1,
	 * there can never be too many commands in flight.
	 */
	new_sqtail = (qp->sqtail + 1) % (qp->num_commands + 1);
	ASSERT(new_sqtail != qp->sqhd);
	qp->sqtail = new_sqtail;
	mutex_exit(&qp->lock);

	nc = cmd->req->nc;
	sqe = nvmf_capsule_sqe(nc);

	/*
	 * NB: Don't bother byte-swapping the cid so that receive doesn't have
	 * to swap.
	 */
	sqe->sqe_cid = cmd->cid;

	error = nvmf_transmit_capsule(nc);
	if (error != 0) {
		dev_err(sc->dip, CE_WARN,
		    "!failed to transmit capsule: %d, disconnecting", error);
		nvmf_disconnect(sc);
		return;
	}

	if (sc->ka_traffic)
		(void) atomic_swap_uint(&sc->ka_active_tx_traffic, 1);
}

static void
nvmf_qp_error(void *arg, int error)
{
	nvmf_host_qpair_t *qp = arg;
	nvmf_softc_t *sc = qp->sc;

	/* Ignore simple close of queue pairs during shutdown. */
	if (!(sc->detaching && error == 0)) {
		dev_err(sc->dip, CE_WARN, "!error %d on %s, disconnecting",
		    error, qp->name);
	}
	nvmf_disconnect(sc);
}

static void
nvmf_receive_capsule(void *arg, struct nvmf_capsule *nc)
{
	nvmf_host_qpair_t *qp = arg;
	nvmf_softc_t *sc = qp->sc;
	nvmf_host_command_t *cmd;
	nvmf_request_t *req;
	const nvme_cqe_t *cqe;
	uint16_t cid;

	cqe = nvmf_capsule_cqe(nc);

	if (sc->ka_traffic)
		(void) atomic_swap_uint(&sc->ka_active_rx_traffic, 1);

	/*
	 * NB: Don't bother byte-swapping the cid as transmit doesn't swap
	 * either.
	 */
	cid = cqe->cqe_cid;

	if (cid > qp->num_commands) {
		dev_err(sc->dip, CE_WARN,
		    "!received invalid CID %u, disconnecting", cid);
		nvmf_disconnect(sc);
		nvmf_free_capsule(nc);
		return;
	}

	/* Update flow control tracking. */
	mutex_enter(&qp->lock);
	if (qp->sq_flow_control) {
		if (nvmf_sqhd_valid(nc))
			qp->sqhd = cqe->cqe_sqhd;
	} else {
		/*
		 * If SQ FC is disabled, just advance the head for each
		 * response capsule received.
		 */
		qp->sqhd = (qp->sqhd + 1) % (qp->num_commands + 1);
	}

	/*
	 * If the queue has been shutdown due to an error, silently drop the
	 * response.
	 */
	if (qp->qp == NULL) {
		dev_err(sc->dip, CE_NOTE,
		    "!received completion for CID %u on shutdown %s", cid,
		    qp->name);
		mutex_exit(&qp->lock);
		nvmf_free_capsule(nc);
		return;
	}

	cmd = qp->active_commands[cid];
	if (cmd == NULL) {
		mutex_exit(&qp->lock);
		dev_err(sc->dip, CE_WARN,
		    "!received completion for inactive CID %u, disconnecting",
		    cid);
		nvmf_disconnect(sc);
		nvmf_free_capsule(nc);
		return;
	}

	ASSERT(cmd->cid == cid);
	req = cmd->req;
	cmd->req = NULL;
	if (list_is_empty(&qp->pending_requests)) {
		qp->active_commands[cid] = NULL;
		list_insert_tail(&qp->free_commands, cmd);
		mutex_exit(&qp->lock);
	} else {
		cmd->req = list_remove_head(&qp->pending_requests);
		nvmf_dispatch_command(qp, cmd);
	}

	req->cb(req->cb_arg, cqe);
	nvmf_free_capsule(nc);
	nvmf_free_request(req);
}

/*
 * PORT-TODO (FreeBSD nvmf_qpair.c nvmf_sysctls_qp): FreeBSD exports per-queue
 * stats (num_entries, sq_head, sq_tail, num_cmds) through sysctl.  illumos
 * should expose the equivalent through kstats hung off the softc; not yet
 * implemented.
 */

nvmf_host_qpair_t *
nvmf_init_qp(nvmf_softc_t *sc, nvmf_trtype_t trtype, const nvlist_t *nvl,
    const char *name, uint_t qid)
{
	nvmf_host_command_t *cmd;
	nvmf_host_qpair_t *qp;
	uint_t i;
	boolean_t admin = B_FALSE;
	boolean_t sq_fc = B_FALSE;
	uint64_t v64 = 0;
	nvlist_t *m = (nvlist_t *)nvl;

	_NOTE(ARGUNUSED(qid));

	(void) nvlist_lookup_boolean_value(m, "admin", &admin);
	qp = kmem_zalloc(sizeof (*qp), KM_SLEEP);
	qp->sc = sc;
	(void) nvlist_lookup_boolean_value(m, "sq_flow_control", &sq_fc);
	qp->sq_flow_control = sq_fc;
	(void) nvlist_lookup_uint64(m, "sqhd", &v64);
	qp->sqhd = (uint16_t)v64;
	v64 = 0;
	(void) nvlist_lookup_uint64(m, "sqtail", &v64);
	qp->sqtail = (uint16_t)v64;
	(void) strlcpy(qp->name, name, sizeof (qp->name));
	mutex_init(&qp->lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&qp->cv, NULL, CV_DRIVER, NULL);

	/*
	 * Allocate a spare command slot for each pending AER command on the
	 * admin queue.
	 */
	v64 = 0;
	(void) nvlist_lookup_uint64(m, "qsize", &v64);
	qp->num_commands = (uint_t)v64 - 1;
	if (admin)
		qp->num_commands += sc->num_aer;

	qp->active_commands = kmem_zalloc(sizeof (*qp->active_commands) *
	    qp->num_commands, KM_SLEEP);
	list_create(&qp->free_commands, sizeof (nvmf_host_command_t),
	    offsetof(nvmf_host_command_t, link));
	list_create(&qp->pending_requests, sizeof (nvmf_request_t),
	    offsetof(nvmf_request_t, link));
	for (i = 0; i < qp->num_commands; i++) {
		cmd = kmem_zalloc(sizeof (*cmd), KM_SLEEP);
		cmd->cid = i;
		list_insert_tail(&qp->free_commands, cmd);
	}

	qp->qp = nvmf_allocate_qpair(trtype, B_FALSE, nvl, nvmf_qp_error, qp,
	    nvmf_receive_capsule, qp);
	if (qp->qp == NULL) {
		while ((cmd = list_remove_head(&qp->free_commands)) != NULL)
			kmem_free(cmd, sizeof (*cmd));
		list_destroy(&qp->free_commands);
		list_destroy(&qp->pending_requests);
		kmem_free(qp->active_commands,
		    sizeof (*qp->active_commands) * qp->num_commands);
		cv_destroy(&qp->cv);
		mutex_destroy(&qp->lock);
		kmem_free(qp, sizeof (*qp));
		return (NULL);
	}

	/* PORT-TODO: nvmf_sysctls_qp() equivalent kstats. */

	return (qp);
}

void
nvmf_shutdown_qp(nvmf_host_qpair_t *qp)
{
	nvmf_host_command_t *cmd;
	nvmf_request_t *req;
	struct nvmf_qpair *nq;
	uint_t i;

	mutex_enter(&qp->lock);
	nq = qp->qp;
	qp->qp = NULL;

	if (nq == NULL) {
		while (qp->shutting_down)
			cv_wait(&qp->cv, &qp->lock);
		mutex_exit(&qp->lock);
		return;
	}
	qp->shutting_down = B_TRUE;
	while (qp->allocating != 0)
		cv_wait(&qp->cv, &qp->lock);
	mutex_exit(&qp->lock);

	nvmf_free_qpair(nq);

	/*
	 * Abort outstanding requests.  Active requests will have their I/O
	 * completions invoked and associated capsules freed by the transport
	 * layer via nvmf_free_qpair.  Pending requests must have their I/O
	 * completion invoked via nvmf_abort_capsule_data.
	 */
	for (i = 0; i < qp->num_commands; i++) {
		cmd = qp->active_commands[i];
		if (cmd != NULL) {
			if (!cmd->req->aer) {
				cmn_err(CE_NOTE,
				    "!nvmf: aborted active command (CID %u)",
				    cmd->cid);
			}

			/* This was freed by nvmf_free_qpair. */
			cmd->req->nc = NULL;
			nvmf_abort_request(cmd->req, cmd->cid);
			nvmf_free_request(cmd->req);
			kmem_free(cmd, sizeof (*cmd));
			qp->active_commands[i] = NULL;
		}
	}
	while ((req = list_remove_head(&qp->pending_requests)) != NULL) {
		if (!req->aer)
			cmn_err(CE_NOTE, "!nvmf: aborted pending command");
		nvmf_abort_capsule_data(req->nc, ECONNABORTED);
		nvmf_abort_request(req, 0);
		nvmf_free_request(req);
	}

	mutex_enter(&qp->lock);
	qp->shutting_down = B_FALSE;
	cv_broadcast(&qp->cv);
	mutex_exit(&qp->lock);
}

void
nvmf_destroy_qp(nvmf_host_qpair_t *qp)
{
	nvmf_host_command_t *cmd;

	nvmf_shutdown_qp(qp);

	while ((cmd = list_remove_head(&qp->free_commands)) != NULL)
		kmem_free(cmd, sizeof (*cmd));
	list_destroy(&qp->free_commands);
	list_destroy(&qp->pending_requests);
	kmem_free(qp->active_commands,
	    sizeof (*qp->active_commands) * qp->num_commands);
	cv_destroy(&qp->cv);
	mutex_destroy(&qp->lock);
	kmem_free(qp, sizeof (*qp));
}

uint64_t
nvmf_max_xfer_size_qp(nvmf_host_qpair_t *qp)
{
	return (nvmf_max_xfer_size(qp->qp));
}

void
nvmf_submit_request(nvmf_request_t *req)
{
	nvmf_host_qpair_t *qp;
	nvmf_host_command_t *cmd;

	qp = req->qp;
	mutex_enter(&qp->lock);
	if (qp->qp == NULL) {
		mutex_exit(&qp->lock);
		cmn_err(CE_NOTE, "!nvmf: aborted pending command");
		nvmf_abort_capsule_data(req->nc, ECONNABORTED);
		nvmf_abort_request(req, 0);
		nvmf_free_request(req);
		return;
	}
	cmd = list_remove_head(&qp->free_commands);
	if (cmd == NULL) {
		/*
		 * Queue this request.  Will be sent after enough in-flight
		 * requests have completed.
		 */
		list_insert_tail(&qp->pending_requests, req);
		mutex_exit(&qp->lock);
		return;
	}

	ASSERT(qp->active_commands[cmd->cid] == NULL);
	qp->active_commands[cmd->cid] = cmd;
	cmd->req = req;
	nvmf_dispatch_command(qp, cmd);
}

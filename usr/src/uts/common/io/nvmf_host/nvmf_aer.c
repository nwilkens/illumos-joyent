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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/host/nvmf_aer.c.
 *
 * Original: Copyright (c) 2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Asynchronous Event Request handling.  When an AER completes, the host reads
 * the log page the controller names on the namespace worker and then submits
 * the AER again.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/errno.h>
#include <sys/nvme.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"

/*
 * One AER slot.  active is set from the submission of the AER command until
 * the chain of completions and tasks that follows it ends, so a slot never
 * has two commands outstanding and its taskq entry is never queued twice.
 */
typedef struct nvmf_aer {
	nvmf_softc_t	*sc;
	uint8_t		log_page_id;
	uint8_t		info;
	uint8_t		type;

	uint_t		page_len;
	void		*page;

	kmutex_t	lock;
	boolean_t	active;
	int		pending;
	boolean_t	failed;
	boolean_t	aborted;
	taskq_ent_t	ent;
} nvmf_aer_t;

#define	MAX_LOG_PAGE_SIZE	4096

static void	nvmf_complete_aer(void *arg, const nvme_cqe_t *cqe);

static void
nvmf_aer_idle(nvmf_aer_t *aer)
{
	mutex_enter(&aer->lock);
	aer->active = B_FALSE;
	mutex_exit(&aer->lock);
}

/* Submit the AER command of a slot that the caller has marked active. */
static void
nvmf_submit_aer(nvmf_softc_t *sc, nvmf_aer_t *aer)
{
	struct nvmf_host_qpair *qp;
	nvmf_request_t *req;
	nvme_sqe_t cmd;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_ASYNC_EVENT;

	qp = nvmf_admin_hold(sc);
	if (qp == NULL) {
		nvmf_aer_idle(aer);
		return;
	}
	req = nvmf_allocate_request(qp, &cmd, nvmf_complete_aer, aer,
	    KM_SLEEP);
	if (req == NULL) {
		nvmf_qp_rele(sc);
		nvmf_aer_idle(aer);
		return;
	}
	req->aer = B_TRUE;
	nvmf_submit_request(req);
	nvmf_qp_rele(sc);
}

static void
nvmf_handle_changed_namespaces(nvmf_softc_t *sc,
    nvme_identify_nsid_list_t *ns_list)
{
	uint_t i, n;

	/* More than 1024 namespaces changed, so look at all of them. */
	if (ns_list->nl_nsid[0] == NVME_NSID_BCAST) {
		nvmf_rescan_all_ns(sc);
		return;
	}

	n = sizeof (ns_list->nl_nsid) / sizeof (ns_list->nl_nsid[0]);
	for (i = 0; i < n; i++) {
		if (ns_list->nl_nsid[i] == 0)
			break;
		nvmf_rescan_ns(sc, ns_list->nl_nsid[i]);
	}
}

static void
nvmf_finish_aer_page_task(void *arg)
{
	nvmf_aer_t *aer = arg;
	nvmf_softc_t *sc = aer->sc;

	if (aer->log_page_id == NVME_LOGPAGE_NSCHANGE)
		nvmf_handle_changed_namespaces(sc, aer->page);

	nvmf_submit_aer(sc, aer);
}

/* Both completions of the log page read have arrived. */
static void
nvmf_finish_aer_page(nvmf_softc_t *sc, nvmf_aer_t *aer, boolean_t failed)
{
	/* A failed read on a live association must not cost the slot. */
	if (failed) {
		if (aer->aborted)
			nvmf_aer_idle(aer);
		else
			nvmf_submit_aer(sc, aer);
		return;
	}

	taskq_dispatch_ent(sc->ns_tq, nvmf_finish_aer_page_task, aer, 0,
	    &aer->ent);
}

static void
nvmf_aer_page_done(nvmf_aer_t *aer, boolean_t failed, boolean_t aborted)
{
	boolean_t last;

	mutex_enter(&aer->lock);
	if (failed)
		aer->failed = B_TRUE;
	if (aborted)
		aer->aborted = B_TRUE;
	last = --aer->pending == 0;
	failed = aer->failed;
	mutex_exit(&aer->lock);

	if (last)
		nvmf_finish_aer_page(aer->sc, aer, failed);
}

static void
nvmf_io_complete_aer_page(void *arg, size_t xfered, int error)
{
	nvmf_aer_t *aer = arg;

	nvmf_aer_page_done(aer, error != 0 || xfered != aer->page_len,
	    error == ECONNABORTED);
}

static void
nvmf_complete_aer_page(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_aer_page_done(arg, nvmf_cqe_failed(cqe), nvmf_cqe_aborted(cqe));
}

static uint_t
nvmf_log_page_size(nvmf_softc_t *sc, uint8_t log_page_id)
{
	switch (log_page_id) {
	case NVME_LOGPAGE_ERROR:
		return ((sc->cdata->id_elpe + 1) *
		    sizeof (nvme_error_log_entry_t));
	case NVME_LOGPAGE_NSCHANGE:
		return (sizeof (nvme_identify_nsid_list_t));
	default:
		return (0);
	}
}

static void
nvmf_complete_aer_task(void *arg)
{
	nvmf_aer_t *aer = arg;
	nvmf_softc_t *sc = aer->sc;

	aer->page_len = MIN(nvmf_log_page_size(sc, aer->log_page_id),
	    MAX_LOG_PAGE_SIZE);
	if (aer->page_len == 0) {
		nvmf_submit_aer(sc, aer);
		return;
	}

	/* Reading the log page also clears the event on the controller. */
	bzero(aer->page, MAX_LOG_PAGE_SIZE);
	mutex_enter(&aer->lock);
	aer->pending = 2;
	aer->failed = B_FALSE;
	aer->aborted = B_FALSE;
	mutex_exit(&aer->lock);

	if (!nvmf_cmd_get_log_page(sc, NVME_NSID_BCAST, aer->log_page_id, 0,
	    aer->page, aer->page_len, nvmf_complete_aer_page, aer,
	    nvmf_io_complete_aer_page, aer, KM_SLEEP))
		nvmf_aer_idle(aer);
}

static void
nvmf_complete_aer(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_aer_t *aer = arg;
	nvmf_softc_t *sc = aer->sc;
	uint32_t cdw0;

	/*
	 * The only error defined for AER is an abort for too many AER
	 * commands, and the host aborts pending AERs when it shuts the queue
	 * down.  In both cases the slot is left idle.
	 */
	if (nvmf_cqe_failed(cqe)) {
		if (!nvmf_cqe_aborted(cqe)) {
			dev_err(sc->dip, CE_NOTE,
			    "!Ignoring error 0x%x for AER",
			    *(const uint16_t *)&cqe->cqe_sf);
		}
		nvmf_aer_idle(aer);
		return;
	}

	cdw0 = cqe->cqe_dw0;
	aer->log_page_id = (cdw0 >> 16) & 0xff;
	aer->info = (cdw0 >> 8) & 0xff;
	aer->type = cdw0 & 0x7;

	dev_err(sc->dip, CE_NOTE, "!AER type %u, info 0x%x, page 0x%x",
	    aer->type, aer->info, aer->log_page_id);

	taskq_dispatch_ent(sc->ns_tq, nvmf_complete_aer_task, aer, 0,
	    &aer->ent);
}

void
nvmf_init_aer(nvmf_softc_t *sc)
{
	uint_t i;

	/* 8 matches NVME_MAX_ASYNC_EVENTS */
	sc->num_aer = MIN(8, sc->cdata->id_aerl + 1);
	sc->aer = kmem_zalloc(sc->num_aer * sizeof (nvmf_aer_t), KM_SLEEP);
	for (i = 0; i < sc->num_aer; i++) {
		sc->aer[i].sc = sc;
		sc->aer[i].page = kmem_zalloc(MAX_LOG_PAGE_SIZE, KM_SLEEP);
		mutex_init(&sc->aer[i].lock, NULL, MUTEX_DRIVER, NULL);
	}
}

int
nvmf_start_aer(nvmf_softc_t *sc)
{
	nvme_sqe_t cmd;
	nvmf_completion_status_t status;
	struct nvmf_host_qpair *qp;
	nvmf_request_t *req;
	nvme_async_event_conf_t aec;
	nvmf_aer_t *aer;
	uint16_t ver_major, ver_minor;
	boolean_t submit;
	uint_t i;

	bzero(&aec, sizeof (aec));
	aec.b.aec_avail = 1;
	aec.b.aec_reliab = 1;
	aec.b.aec_readonly = 1;
	aec.b.aec_volatile = 1;

	/* The Namespace Attribute Notice exists from NVMe 1.2 onward. */
	ver_major = (uint16_t)(sc->cdata->id_ver >> 16);
	ver_minor = (uint16_t)((sc->cdata->id_ver >> 8) & 0xff);
	if ((ver_major > 1 || (ver_major == 1 && ver_minor >= 2)) &&
	    sc->cdata->id_oaes.oaes_nsan != 0)
		aec.b.aec_nsan = 1;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_SET_FEATURES;
	cmd.sqe_cdw10 = NVME_FEAT_ASYNC_EVENT;
	cmd.sqe_cdw11 = aec.r;

	nvmf_status_init(&status);
	qp = nvmf_admin_hold(sc);
	if (qp == NULL)
		return (ECONNABORTED);
	req = nvmf_allocate_request(qp, &cmd, nvmf_complete, &status,
	    KM_SLEEP);
	if (req == NULL) {
		nvmf_qp_rele(sc);
		return (ECONNABORTED);
	}
	nvmf_submit_request(req);
	nvmf_qp_rele(sc);
	nvmf_wait_for_reply(sc, &status, nvmf_admin_timeout);

	if (nvmf_cqe_failed(&status.cqe)) {
		dev_err(sc->dip, CE_WARN,
		    "!SET_FEATURES (ASYNC_EVENT_CONFIGURATION) failed");
		return (nvmf_cqe_aborted(&status.cqe) ? ECONNABORTED : EIO);
	}

	/*
	 * A slot can still be active from the previous association while its
	 * task waits on the namespace worker.  That task submits the AER
	 * itself when it ends.
	 */
	for (i = 0; i < sc->num_aer; i++) {
		aer = &sc->aer[i];
		mutex_enter(&aer->lock);
		submit = !aer->active;
		aer->active = B_TRUE;
		mutex_exit(&aer->lock);
		if (submit)
			nvmf_submit_aer(sc, aer);
	}

	return (0);
}

/*
 * Free the AER slots.  The caller has destroyed the admin queue pair and
 * drained the namespace worker, so no slot is active.
 */
void
nvmf_destroy_aer(nvmf_softc_t *sc)
{
	uint_t i;

	if (sc->aer == NULL)
		return;

	ASSERT3P(sc->admin, ==, NULL);
	for (i = 0; i < sc->num_aer; i++) {
		ASSERT(!sc->aer[i].active);
		kmem_free(sc->aer[i].page, MAX_LOG_PAGE_SIZE);
		mutex_destroy(&sc->aer[i].lock);
	}
	kmem_free(sc->aer, sc->num_aer * sizeof (nvmf_aer_t));
	sc->aer = NULL;
	sc->num_aer = 0;
}

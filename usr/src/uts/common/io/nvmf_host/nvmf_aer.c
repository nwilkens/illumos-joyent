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
 * Asynchronous Event Request handling.  When an AER completes, the controller
 * reports an event type/info/log-page in CDW0; the host reads the associated
 * log page (changed-namespace list, error log) on a background task and then
 * resubmits the AER.  The protocol state machine ports closely; OS-glue:
 *
 *   FreeBSD                 illumos
 *   -------                 -------
 *   struct task +           ddi_taskq_t (one per softc, supplied by nvmf_host.c
 *     taskqueue_thread        as nvmf_aer_taskq(sc)); each AER queues two
 *                             dispatch functions
 *   mtx_pool_find/mtx        a single kmutex per AER
 *   le16toh/le32toh          illumos is little-endian only
 *   NVMEV(field, val)        bit extraction is open-coded here
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/nvme.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"

typedef struct nvmf_aer {
	nvmf_softc_t	*sc;
	uint8_t		log_page_id;
	uint8_t		info;
	uint8_t		type;

	uint_t		page_len;
	void		*page;

	int		error;
	uint16_t	status;
	int		pending;
	kmutex_t	lock;
	/*
	 * FreeBSD uses two struct task instances dispatched to taskqueue_thread.
	 * illumos dispatches the same two functions to the softc's taskq.
	 */
} nvmf_aer_t;

#define	MAX_LOG_PAGE_SIZE	4096

/*
 * PORT-TODO (FreeBSD nvmf_aer.c uses taskqueue_thread): nvmf_host.c owns a
 * ddi_taskq_t for the softc; this accessor returns it.  Declared here and
 * defined in nvmf_host.c.
 */
extern ddi_taskq_t *nvmf_aer_taskq(nvmf_softc_t *sc);

static void	nvmf_complete_aer(void *arg, const nvme_cqe_t *cqe);

static void
nvmf_submit_aer(nvmf_softc_t *sc, nvmf_aer_t *aer)
{
	nvmf_request_t *req;
	nvme_sqe_t cmd;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_ASYNC_EVENT;

	req = nvmf_allocate_request(sc->admin, &cmd, nvmf_complete_aer, aer,
	    KM_SLEEP);
	if (req == NULL)
		return;
	req->aer = B_TRUE;
	nvmf_submit_request(req);
}

static void
nvmf_handle_changed_namespaces(nvmf_softc_t *sc,
    nvme_identify_nsid_list_t *ns_list)
{
	uint32_t nsid;
	uint_t i, n;

	/*
	 * If more than 1024 namespaces have changed, we should probably just
	 * rescan the entire set of namespaces.
	 */
	n = sizeof (ns_list->nl_nsid) / sizeof (ns_list->nl_nsid[0]);
	if (ns_list->nl_nsid[0] == 0xffffffff) {
		nvmf_rescan_all_ns(sc);
		return;
	}

	for (i = 0; i < n; i++) {
		if (ns_list->nl_nsid[i] == 0)
			break;

		nsid = ns_list->nl_nsid[i];
		nvmf_rescan_ns(sc, nsid);
	}
}

static void
nvmf_finish_aer_page_task(void *arg)
{
	nvmf_aer_t *aer = arg;
	nvmf_softc_t *sc = aer->sc;

	switch (aer->log_page_id) {
	case NVME_LOGPAGE_ERROR:
		/* TODO: Should we log these? */
		break;
	case NVME_LOGPAGE_NSCHANGE:
		nvmf_handle_changed_namespaces(sc, aer->page);
		break;
	}

	/* Resubmit this AER command. */
	nvmf_submit_aer(sc, aer);
}

static void
nvmf_finish_aer_page(nvmf_softc_t *sc, nvmf_aer_t *aer)
{
	/* If an error occurred fetching the page, just bail. */
	if (aer->error != 0 || aer->status != 0)
		return;

	(void) ddi_taskq_dispatch(nvmf_aer_taskq(sc), nvmf_finish_aer_page_task,
	    aer, DDI_SLEEP);
}

static void
nvmf_io_complete_aer_page(void *arg, size_t xfered, int error)
{
	nvmf_aer_t *aer = arg;
	nvmf_softc_t *sc = aer->sc;

	_NOTE(ARGUNUSED(xfered));

	mutex_enter(&aer->lock);
	aer->error = error;
	aer->pending--;
	if (aer->pending == 0) {
		mutex_exit(&aer->lock);
		nvmf_finish_aer_page(sc, aer);
	} else {
		mutex_exit(&aer->lock);
	}
}

static void
nvmf_complete_aer_page(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_aer_t *aer = arg;
	nvmf_softc_t *sc = aer->sc;

	mutex_enter(&aer->lock);
	/*
	 * Record only the error-bearing portion of the status field.  The raw
	 * 16-bit status word also carries the Phase Tag (sf_p), More (sf_m),
	 * and Do Not Retry (sf_dnr) bits; folding those into the error test
	 * (a plain "status != 0" in nvmf_finish_aer_page()) would spuriously
	 * treat a successful GET_LOG_PAGE as failed, skipping the
	 * changed-namespace rescan and permanently losing the AER slot.  Match
	 * nvmf_cqe_failed().
	 */
	aer->status = (cqe->cqe_sf.sf_sc != 0 || cqe->cqe_sf.sf_sct != 0);
	aer->pending--;
	if (aer->pending == 0) {
		mutex_exit(&aer->lock);
		nvmf_finish_aer_page(sc, aer);
	} else {
		mutex_exit(&aer->lock);
	}
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

	if (aer->page_len != 0) {
		/* Read the associated log page. */
		aer->page_len = MIN(aer->page_len, MAX_LOG_PAGE_SIZE);
		aer->pending = 2;
		(void) nvmf_cmd_get_log_page(sc, NVME_NSID_BCAST,
		    aer->log_page_id, 0, aer->page, aer->page_len,
		    nvmf_complete_aer_page, aer, nvmf_io_complete_aer_page,
		    aer, KM_SLEEP);
	} else {
		/* Resubmit this AER command. */
		nvmf_submit_aer(sc, aer);
	}
}

static void
nvmf_complete_aer(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_aer_t *aer = arg;
	nvmf_softc_t *sc = aer->sc;
	uint32_t cdw0;

	/*
	 * The only error defined for AER is an abort due to submitting too
	 * many AER commands.  Just discard this AER without resubmitting if we
	 * get an error.
	 *
	 * NB: Pending AER commands are aborted during controller shutdown, so
	 * discard aborted commands silently.
	 */
	if (cqe->cqe_sf.sf_sc != 0 || cqe->cqe_sf.sf_sct != 0) {
		if (!nvmf_cqe_aborted(cqe)) {
			dev_err(sc->dip, CE_NOTE,
			    "!Ignoring error 0x%x for AER",
			    *(const uint16_t *)&cqe->cqe_sf);
		}
		return;
	}

	cdw0 = cqe->cqe_dw0;
	aer->log_page_id = (cdw0 >> 16) & 0xff;
	aer->info = (cdw0 >> 8) & 0xff;
	aer->type = cdw0 & 0x7;

	dev_err(sc->dip, CE_NOTE, "!AER type %u, info 0x%x, page 0x%x",
	    aer->type, aer->info, aer->log_page_id);

	aer->page_len = nvmf_log_page_size(sc, aer->log_page_id);
	(void) ddi_taskq_dispatch(nvmf_aer_taskq(sc), nvmf_complete_aer_task,
	    aer, DDI_SLEEP);
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
		sc->aer[i].page = kmem_alloc(MAX_LOG_PAGE_SIZE, KM_SLEEP);
		mutex_init(&sc->aer[i].lock, NULL, MUTEX_DRIVER, NULL);
	}
}

int
nvmf_start_aer(nvmf_softc_t *sc)
{
	nvme_sqe_t cmd;
	nvmf_completion_status_t status;
	nvmf_request_t *req;
	nvme_async_event_conf_t aec;
	uint16_t ver_major, ver_minor;
	uint_t i;

	/*
	 * Enable the SMART/health critical-warning notices that FreeBSD's
	 * nvmf_set_async_event_config() requests: available spare, device
	 * reliability, read-only media, and volatile-memory-backup failure.
	 */
	bzero(&aec, sizeof (aec));
	aec.b.aec_avail = 1;
	aec.b.aec_reliab = 1;
	aec.b.aec_readonly = 1;
	aec.b.aec_volatile = 1;

	/*
	 * From NVMe 1.2 onward the controller advertises optional async event
	 * support in OAES; enable the Namespace Attribute Notice if the
	 * controller supports it (FreeBSD: cdata->ver >= NVME_REV(1,2) and
	 * cdata->oaes & NVME_ASYNC_EVENT_NS_ATTRIBUTE).  id_ver is encoded as
	 * [31:16] major, [15:8] minor.
	 */
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
	req = nvmf_allocate_request(sc->admin, &cmd, nvmf_complete, &status,
	    KM_SLEEP);
	if (req == NULL) {
		dev_err(sc->dip, CE_WARN,
		    "!failed to allocate SET_FEATURES "
		    "(ASYNC_EVENT_CONFIGURATION) command");
		return (ECONNABORTED);
	}
	nvmf_submit_request(req);
	nvmf_wait_for_reply(&status);

	if (status.cqe.cqe_sf.sf_sc != 0 || status.cqe.cqe_sf.sf_sct != 0) {
		dev_err(sc->dip, CE_WARN,
		    "!SET_FEATURES (ASYNC_EVENT_CONFIGURATION) failed");
		return (EIO);
	}

	for (i = 0; i < sc->num_aer; i++)
		nvmf_submit_aer(sc, &sc->aer[i]);

	return (0);
}

void
nvmf_destroy_aer(nvmf_softc_t *sc)
{
	uint_t i;

	/*
	 * FreeBSD drains the two per-AER tasks here, after all qpairs
	 * (including admin) have been destroyed.  illumos dispatches the same
	 * two functions to the shared softc taskq; the caller MUST have
	 * destroyed the admin qpair first so that no further AER CQE can arrive
	 * and dispatch a task.  We then drain the taskq so no
	 * nvmf_complete_aer_task / nvmf_finish_aer_page_task is in flight or
	 * queued before aer->page and aer->lock are freed (use-after-free or
	 * destroyed-mutex otherwise).
	 */
	ASSERT(sc->admin == NULL);
	ddi_taskq_wait(nvmf_aer_taskq(sc));

	for (i = 0; i < sc->num_aer; i++) {
		kmem_free(sc->aer[i].page, MAX_LOG_PAGE_SIZE);
		mutex_destroy(&sc->aer[i].lock);
	}
	kmem_free(sc->aer, sc->num_aer * sizeof (nvmf_aer_t));
}

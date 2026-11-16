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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/host/nvmf.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Namespace discovery for the NVMe over Fabrics host, and the namespace
 * worker.  Everything here may wait for the controller, so none of it runs on
 * the connection worker.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/nvme.h>

#include "nvmf_var.h"

/* The most namespaces one association may present, and one scan may visit. */
uint_t nvmf_max_namespaces = 1024;

#define	NVMF_NSLIST_ENTRIES	\
	(sizeof (((nvme_identify_nsid_list_t *)0)->nl_nsid) / sizeof (uint32_t))

/*
 * Count the entries of an active namespace list that was requested for the
 * NSIDs above prev.  Returns -1 unless they are valid and strictly ascending,
 * which is what bounds a scan.
 */
int
nvmf_nslist_count(const nvme_identify_nsid_list_t *nslist, uint32_t prev)
{
	uint32_t nsid;
	uint_t i;

	for (i = 0; i < NVMF_NSLIST_ENTRIES; i++) {
		nsid = nslist->nl_nsid[i];
		if (nsid == 0)
			break;
		if (nsid <= prev || nsid == NVME_NSID_BCAST)
			return (-1);
		prev = nsid;
	}
	return ((int)i);
}

static boolean_t
nvmf_identify_nslist(nvmf_softc_t *sc, uint32_t prev,
    nvme_identify_nsid_list_t *nslist)
{
	nvmf_completion_status_t status;

	bzero(nslist, sizeof (*nslist));
	nvmf_status_init(&status);
	nvmf_status_wait_io(&status);
	if (!nvmf_cmd_identify_active_namespaces(sc, prev, nslist,
	    nvmf_complete, &status, nvmf_io_complete, &status, KM_SLEEP))
		return (B_FALSE);
	nvmf_wait_for_reply(sc, &status, nvmf_admin_timeout);

	if (!nvmf_status_ok(&status, sizeof (*nslist))) {
		dev_err(sc->dip, CE_WARN,
		    "!IDENTIFY active namespaces failed");
		return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * Identify one namespace.  Returns 0, EIO if the controller refused the
 * command, or ECONNABORTED if the command could not be sent or was aborted.
 */
static int
nvmf_identify_ns(nvmf_softc_t *sc, uint32_t nsid, nvme_identify_nsid_t *data)
{
	nvmf_completion_status_t status;

	bzero(data, sizeof (*data));
	nvmf_status_init(&status);
	nvmf_status_wait_io(&status);
	if (!nvmf_cmd_identify_namespace(sc, nsid, data, nvmf_complete,
	    &status, nvmf_io_complete, &status, KM_SLEEP))
		return (ECONNABORTED);
	nvmf_wait_for_reply(sc, &status, nvmf_admin_timeout);

	if (nvmf_cqe_aborted(&status.cqe))
		return (ECONNABORTED);
	if (!nvmf_status_ok(&status, sizeof (*data)))
		return (EIO);
	return (0);
}

/*
 * Make the namespaces match the controller's active namespace list.  A scan
 * that fails part way leaves the namespaces it did not reach as they were.
 */
static boolean_t
nvmf_scan_namespaces(nvmf_softc_t *sc)
{
	nvme_identify_nsid_list_t *nslist;
	nvme_identify_nsid_t *data;
	uint32_t nsid, prev = 0;
	uint_t visited = 0;
	boolean_t ok = B_FALSE;
	int i, n, error;

	nslist = kmem_alloc(sizeof (*nslist), KM_SLEEP);
	data = kmem_alloc(sizeof (*data), KM_SLEEP);

	for (;;) {
		if (!nvmf_identify_nslist(sc, prev, nslist))
			goto out;

		n = nvmf_nslist_count(nslist, prev);
		if (n < 0) {
			dev_err(sc->dip, CE_WARN,
			    "!active namespace list is not ascending");
			goto out;
		}

		for (i = 0; i < n; i++) {
			nsid = nslist->nl_nsid[i];
			if (++visited > nvmf_max_namespaces) {
				dev_err(sc->dip, CE_WARN, "!more than %u "
				    "active namespaces, ignoring the rest",
				    nvmf_max_namespaces);
				ok = B_TRUE;
				goto out;
			}
			/* A namespace that cannot be identified is absent. */
			error = nvmf_identify_ns(sc, nsid, data);
			if (error == ECONNABORTED)
				goto out;
			if (error != 0) {
				dev_err(sc->dip, CE_WARN,
				    "!IDENTIFY namespace %u failed", nsid);
				bzero(data, sizeof (*data));
			}

			nvmf_ns_purge(sc, prev + 1, nsid);
			nvmf_ns_reconcile(sc, nsid, data);
			prev = nsid;
		}

		if ((uint_t)n < NVMF_NSLIST_ENTRIES ||
		    prev >= NVME_NSID_BCAST - 1)
			break;
	}

	nvmf_ns_purge(sc, prev + 1, NVME_NSID_BCAST);
	ok = B_TRUE;
out:
	kmem_free(data, sizeof (*data));
	kmem_free(nslist, sizeof (*nslist));
	return (ok);
}

boolean_t
nvmf_add_namespaces(nvmf_softc_t *sc)
{
	return (nvmf_scan_namespaces(sc));
}

void
nvmf_rescan_all_ns(nvmf_softc_t *sc)
{
	(void) nvmf_scan_namespaces(sc);
}

void
nvmf_rescan_ns(nvmf_softc_t *sc, uint32_t nsid)
{
	nvme_identify_nsid_t *data;

	if (nsid == NVME_NSID_BCAST)
		return;

	data = kmem_alloc(sizeof (*data), KM_SLEEP);
	if (nvmf_identify_ns(sc, nsid, data) == 0) {
		nvmf_ns_reconcile(sc, nsid, data);
	} else {
		dev_err(sc->dip, CE_WARN, "!IDENTIFY namespace %u failed",
		    nsid);
	}
	kmem_free(data, sizeof (*data));
}

/*
 * Identify again every namespace that existed before the association was
 * lost, so that I/O waiting for the reconnect is not sent to a namespace that
 * was replaced meanwhile.  Returns B_FALSE if the new association failed.
 */
boolean_t
nvmf_revalidate_namespaces(nvmf_softc_t *sc)
{
	nvme_identify_nsid_t *data;
	nvmf_ns_info_t *info;
	uint_t i, count, max;
	boolean_t ok = B_TRUE;
	int error;

	max = nvmf_ns_snapshot(sc, NULL, 0);
	if (max == 0)
		return (B_TRUE);

	info = kmem_zalloc(max * sizeof (*info), KM_SLEEP);
	data = kmem_alloc(sizeof (*data), KM_SLEEP);
	count = MIN(nvmf_ns_snapshot(sc, info, max), max);

	for (i = 0; i < count; i++) {
		error = nvmf_identify_ns(sc, info[i].ni_nsid, data);
		if (error == ECONNABORTED) {
			ok = B_FALSE;
			break;
		}
		nvmf_ns_revalidate(sc, info[i].ni_nsid,
		    error == 0 ? data : NULL);
	}

	kmem_free(data, sizeof (*data));
	kmem_free(info, max * sizeof (*info));
	return (ok);
}

/*
 * Detach every disk on behalf of the disconnect ioctl, which waits for the
 * answer.  If no disk is open the softc is handed to that ioctl in
 * NVMF_SC_DISCONNECTING.
 */
static void
nvmf_ns_detach(nvmf_softc_t *sc)
{
	boolean_t busy;

	mutex_enter(&sc->lock);
	if (!sc->detach_req || sc->state != NVMF_SC_CONNECTED) {
		mutex_exit(&sc->lock);
		return;
	}
	mutex_exit(&sc->lock);

	busy = !nvmf_mpath_detach_all(sc);

	mutex_enter(&sc->lock);
	if (sc->detach_req) {
		sc->detach_busy = busy;
		sc->detach_done = B_TRUE;
		if (!busy && sc->state == NVMF_SC_CONNECTED) {
			sc->state = NVMF_SC_DISCONNECTING;
			sc->io_enabled = B_FALSE;
			sc->detach_owned = B_TRUE;
		}
		cv_broadcast(&sc->cv);
	}
	mutex_exit(&sc->lock);
}

/* The namespace worker's own events.  AER tasks share its thread. */
void
nvmf_ns_task(void *arg)
{
	nvmf_softc_t *sc = arg;
	nvmf_sc_state_t state;
	uint_t events;

	mutex_enter(&sc->lock);
	while ((events = sc->ns_events) != 0) {
		sc->ns_events = 0;
		state = sc->state;
		mutex_exit(&sc->lock);

		if ((events & NVMF_NS_EV_DETACH) != 0)
			nvmf_ns_detach(sc);
		if ((events & NVMF_NS_EV_PURGE) != 0 &&
		    state == NVMF_SC_FAILED)
			nvmf_ns_remove_all(sc);
		if ((events & NVMF_NS_EV_RESCAN) != 0 &&
		    state == NVMF_SC_CONNECTED)
			nvmf_rescan_all_ns(sc);

		mutex_enter(&sc->lock);
	}
	sc->ns_queued = B_FALSE;
	mutex_exit(&sc->lock);
}

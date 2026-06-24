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
 * Provenance: re-bound to illumos from FreeBSD sys/dev/nvmf/host/nvmf_ns.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * FreeBSD's nvmf_ns.c is a full character/disk device (cdev + bio strategy)
 * with its own per-namespace I/O queue and disconnect/reconnect bio plumbing.
 * On illumos the I/O strategy and disk presentation move to blkdev
 * (nvmf_blkdev.c via bd_ops_t) and the multipath layer (nvmf_mpath.c), so the
 * per-namespace object here is thinner: it parses Identify Namespace data,
 * registers/refreshes this association's PATH for the namespace head, and holds
 * the namespace's cached geometry/flags.
 *
 * What ports from FreeBSD: the Identify-Namespace validation (data protection,
 * LBA format index, LBA data size) in nvmf_init_ns / nvmf_update_ns.  What does
 * NOT port: the cdev (struct cdevsw), the bio strategy/biodone path, and the
 * pending-bio requeue list -- those are replaced by blkdev + the path selector.
 *
 * The disconnect/reconnect/shutdown entry points are kept (the core calls them
 * per association) but now defer to the multipath layer: a disconnect marks the
 * association's path down rather than quiescing a per-namespace bio queue
 * (NVMEOF.md 9.3 keep-alive-to-path seam).
 *
 * Locking invariant: unlike FreeBSD, the per-namespace object here keeps no
 * active-I/O refcount, so it does not drain in-flight requests before freeing.
 * nvmf_init_ns / nvmf_update_ns / nvmf_disconnect_ns / nvmf_shutdown_ns /
 * nvmf_destroy_ns therefore REQUIRE the caller to serialize every access to a
 * given softc's sc->ns[] slot by holding sc->connection_lock as a writer; the
 * qpair teardown done under that same writer lock (nvmf_host.c) is the lifetime
 * barrier that guarantees no in-flight I/O still references a path when its
 * namespace is freed.  The disconnect and controller-loss tasks honor this.
 *
 * PORT-TODO (caller-side, nvmf_host.c): the AER-driven rescan path
 * (nvmf_finish_aer_page_task -> nvmf_handle_changed_namespaces ->
 * nvmf_rescan_ns -> nvmf_rescan_ns_1) drives init/update/destroy but runs on
 * the AER taskq WITHOUT holding connection_lock, so a rescan concurrent with a
 * disconnect on the same softc can free a namespace under a concurrent
 * update/disconnect.  The fix belongs in the caller (take connection_lock as a
 * writer across nvmf_rescan_ns_1, or dispatch the rescan through the same
 * serialization token as the disconnect task); it cannot be enforced here with
 * an assertion because that path would trip it today.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/nvme.h>

#include "nvmf_var.h"

/*
 * (FreeBSD: struct nvmf_namespace) Per-association, per-namespace state.  In
 * the illumos model this is the binding between a namespace as seen on THIS
 * association and the namespace HEAD that owns the blkdev disk.
 */
typedef struct nvmf_namespace {
	nvmf_softc_t		*sc;
	uint64_t		size;		/* in bytes */
	uint32_t		id;		/* NSID on this controller */
	uint32_t		lba_size;
	boolean_t		disconnected;
	boolean_t		shutdown;

	/* Cached Identify data so the head can answer media/drive info. */
	nvme_identify_nsid_t	data;

	/* This association's path for the namespace head. */
	struct nvmf_path	*path;

	kmutex_t		lock;
} nvmf_namespace_t;

/*
 * Validate Identify Namespace data and derive block geometry: number of blocks
 * and block size in bytes.  Ports the FreeBSD checks verbatim (no end-to-end
 * data protection, valid LBA format index, no metadata, non-zero LBA data
 * size).  Returns B_FALSE for a format we cannot present, leaving *nblksp /
 * *blksizep untouched.
 *
 * Single source of truth for the geometry parse: nvmf_mpath.c calls it (dip ==
 * NULL, no warnings) at add_path time to seed head geometry, and nvmf_ns.c
 * calls it (dip set) for init/update so the disk size and I/O block size cannot
 * desync.  Any change here (accepting metadata, honoring the NVMe 1.4 lba_fidxu
 * upper format-index bits, etc.) therefore applies to both call sites.  The
 * format index is taken from id_flbas.lba_format only, exactly as both callers
 * did before this was hoisted.
 */
boolean_t
nvmf_ns_fmt(const nvme_identify_nsid_t *data, uint64_t *nblksp,
    uint32_t *blksizep, dev_info_t *dip, uint32_t nsid)
{
	uint8_t lbaf, lbads;

	if (data->id_dps.dp_pinfo != 0) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: End-to-end data protection not supported",
			    nsid);
		return (B_FALSE);
	}

	lbaf = data->id_flbas.lba_format;
	if (lbaf > data->id_nlbaf) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: Invalid LBA format index", nsid);
		return (B_FALSE);
	}

	if (data->id_lbaf[lbaf].lbaf_ms != 0) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: Namespaces with metadata are not supported",
			    nsid);
		return (B_FALSE);
	}

	lbads = data->id_lbaf[lbaf].lbaf_lbads;
	if (lbads == 0) {
		if (dip != NULL)
			dev_err(dip, CE_WARN,
			    "!ns%u: Invalid LBA format index", nsid);
		return (B_FALSE);
	}

	*blksizep = 1U << lbads;
	*nblksp = data->id_nsize;
	return (B_TRUE);
}

struct nvmf_namespace *
nvmf_init_ns(nvmf_softc_t *sc, uint32_t id, const nvme_identify_nsid_t *data)
{
	nvmf_namespace_t *ns;
	struct nvmf_ns_head *head;
	uint64_t nblks;
	uint32_t lba_size;

	ns = kmem_zalloc(sizeof (*ns), KM_SLEEP);
	ns->sc = sc;
	ns->id = id;
	ns->data = *data;
	mutex_init(&ns->lock, NULL, MUTEX_DRIVER, NULL);

	if (!nvmf_ns_fmt(data, &nblks, &lba_size, sc->dip, id))
		goto fail;

	ns->lba_size = lba_size;
	ns->size = nblks * lba_size;

	/*
	 * FreeBSD records per-namespace DEALLOCATE/FLUSH support here (from
	 * id_oncs.on_dset_mgmt and id_vwc.vwc_present) and gates BIO_DELETE /
	 * BIO_FLUSH on it.  In the illumos model the I/O strategy lives in the
	 * blkdev binding, so that gating moves there: nvmf_bd_sync_cache
	 * completes a FLUSH as a no-op when the controller advertises no
	 * volatile write cache, and nvmf_bd_free_space returns ENOTSUP when
	 * Dataset Management is not supported.  Both consult sc->cdata directly,
	 * so no per-namespace flag is carried here.
	 */

	/*
	 * Register this association's path for the namespace head.  If this is
	 * the first path for the head's identity, the multipath layer creates
	 * the head (seeding its geometry from this same Identify data) and the
	 * blkdev binding attaches the bd_handle (NVMEOF.md 9.3 additional-path
	 * handoff).
	 */
	ns->path = nvmf_mpath_add_path(sc, id, data);
	if (ns->path == NULL)
		goto fail;

	/*
	 * nvmf_mpath_add_path already seeded the head geometry from this data
	 * on first appearance, so on the create path there is nothing more to
	 * do.  When this path joined a pre-existing head, refresh its geometry
	 * best-effort: the head is looked up by namespace identity, which is
	 * only possible for namespaces that expose an NGUID/EUI64/UUID.  An
	 * identity-less namespace has its own per-path head whose geometry was
	 * already set by add_path, so the lookup returning NULL is harmless.
	 */
	head = nvmf_mpath_find_head(data);
	if (head != NULL)
		nvmf_mpath_head_set_geometry(head, data->id_nsize, ns->lba_size);

	return (ns);
fail:
	mutex_destroy(&ns->lock);
	kmem_free(ns, sizeof (*ns));
	return (NULL);
}

void
nvmf_disconnect_ns(struct nvmf_namespace *ns)
{
	mutex_enter(&ns->lock);
	ns->disconnected = B_TRUE;
	mutex_exit(&ns->lock);

	/*
	 * Route disconnect to the path layer, not to disk teardown: mark this
	 * association's path down so the head's bd_handle survives if another
	 * association still provides a path (NVMEOF.md 9.3).
	 */
	if (ns->path != NULL)
		nvmf_mpath_path_down(ns->path);
}

void
nvmf_reconnect_ns(struct nvmf_namespace *ns)
{
	mutex_enter(&ns->lock);
	ns->disconnected = B_FALSE;
	mutex_exit(&ns->lock);

	if (ns->path != NULL)
		nvmf_mpath_path_up(ns->path);
}

void
nvmf_shutdown_ns(struct nvmf_namespace *ns)
{
	mutex_enter(&ns->lock);
	ns->shutdown = B_TRUE;
	mutex_exit(&ns->lock);

	/*
	 * FreeBSD fails all pending bios with ECONNABORTED here so filesystems
	 * can unmount.  In the illumos model the pending I/O lives in blkdev's
	 * queue, not a per-namespace bio list; marking this association's path
	 * down makes the selector stop handing it out so newly dispatched
	 * bd_xfer_t requests fail with ENXIO instead of hanging on a dead
	 * association.  In-flight requests error out via their CQE/I/O
	 * completion when the qpair is torn down.
	 */
	if (ns->path != NULL)
		nvmf_mpath_path_down(ns->path);
}

void
nvmf_destroy_ns(struct nvmf_namespace *ns)
{
	/*
	 * Remove this association's path.  When the last path of a head is
	 * removed, the multipath layer destroys the head and (PORT-TODO)
	 * detaches+frees the bd_handle.
	 */
	if (ns->path != NULL)
		nvmf_mpath_remove_path(ns->path);

	mutex_destroy(&ns->lock);
	kmem_free(ns, sizeof (*ns));
}

boolean_t
nvmf_update_ns(struct nvmf_namespace *ns, const nvme_identify_nsid_t *data)
{
	struct nvmf_ns_head *head;
	uint64_t nblks;
	uint32_t lba_size;

	if (!nvmf_ns_fmt(data, &nblks, &lba_size, ns->sc->dip, ns->id))
		return (B_FALSE);

	mutex_enter(&ns->lock);
	ns->data = *data;
	ns->lba_size = lba_size;
	ns->size = nblks * lba_size;
	mutex_exit(&ns->lock);

	/*
	 * Propagate the new geometry to the head so the blkdev media_info
	 * reflects the resize/reformat.  nvmf_mpath_head_set_geometry issues the
	 * bd_state_change when the geometry actually changes.
	 *
	 * The head is reached by namespace identity (nvmf_mpath_find_head),
	 * which only resolves for namespaces that expose an NGUID/EUI64/UUID.
	 * An identity-less namespace keys a per-path head that the multipath
	 * layer does not expose any accessor for, so its resize cannot be
	 * refreshed from here; that gap closes once nvmf_mpath.c grows a
	 * path->head accessor (PORT-TODO).
	 */
	head = nvmf_mpath_find_head(data);
	if (head != NULL)
		nvmf_mpath_head_set_geometry(head, data->id_nsize, lba_size);
	return (B_TRUE);
}

/*
 * Read-only snapshot of a namespace for NVMF_LIST_CONTROLLER.  The struct is
 * private to this file, so nvmf_host.c reads it through this accessor and builds
 * the nvlist there.  ns->lock is intentionally not taken: the caller already
 * holds sc->connection_lock (so the ns cannot be freed underneath us), and
 * taking ns->lock under connection_lock would invert the lock order used by the
 * I/O and AEN paths.  The geometry/identity fields are stable after init except
 * across a rare resize/reformat AEN; a slightly stale display value is harmless.
 */
void
nvmf_ns_get_info(struct nvmf_namespace *ns, uint32_t *nsidp, uint64_t *sizep,
    uint32_t *blksizep, uint8_t *nguid, uint8_t *eui64, boolean_t *connectedp)
{
	*nsidp = ns->id;
	*sizep = ns->size;
	*blksizep = ns->lba_size;
	(void) memcpy(nguid, ns->data.id_nguid, sizeof (ns->data.id_nguid));
	(void) memcpy(eui64, ns->data.id_eui64, sizeof (ns->data.id_eui64));
	*connectedp = !ns->disconnected;
}

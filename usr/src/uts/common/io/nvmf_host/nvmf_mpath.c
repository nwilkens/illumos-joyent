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
 * Namespace heads and paths for the NVMe over Fabrics host.
 *
 * A head is one blkdev disk.  It is keyed by subsystem NQN and namespace
 * identity, so its devid does not depend on the association.  A path binds a
 * head to one namespace of the current association.  A head has at most one
 * path.  A head whose disk is still open when its path goes away stays behind
 * with no path, fails I/O, and is given a path again if the same namespace
 * returns.
 *
 * head_lock protects head_path, head_nblks and the reference count of the
 * path.  It is never held together with nvmf_softc_t.lock, and neither is held
 * across a call into blkdev.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/blkdev.h>
#include <sys/debug.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

#include "nvmf_var.h"

/* A namespace with neither NGUID nor EUI64 is told apart by its NSID. */
typedef struct nvmf_ns_id {
	uint8_t		nid_subnqn[NVMF_NQN_FIELD_SIZE];
	uint8_t		nid_nguid[16];
	uint8_t		nid_eui64[8];
	uint32_t	nid_nsid;
	boolean_t	nid_have_nguid;
	boolean_t	nid_have_eui64;
} nvmf_ns_id_t;

typedef struct nvmf_path {
	struct nvmf_ns_head	*path_head;
	nvmf_softc_t		*path_sc;
	uint32_t		path_nsid;
	boolean_t		path_gone;	/* under path_sc->lock */
	uint_t			path_refs;
	kcondvar_t		path_cv;
} nvmf_path_t;

typedef struct nvmf_ns_head {
	list_node_t		head_link;	/* in nvmf_softc_t.heads */
	nvmf_softc_t		*head_sc;

	/* Fixed for the life of the head. */
	nvmf_ns_id_t		head_id;
	uint32_t		head_blksize;
	uint64_t		head_maxxfer;
	bd_handle_t		head_bdh;

	boolean_t		head_attached;

	kmutex_t		head_lock;
	nvmf_path_t		*head_path;
	uint64_t		head_nblks;
} nvmf_ns_head_t;

CTASSERT(sizeof (((nvme_identify_ctrl_t *)0)->id_subnqn) ==
    NVMF_NQN_FIELD_SIZE);

void
nvmf_mpath_init(nvmf_softc_t *sc)
{
	list_create(&sc->heads, sizeof (nvmf_ns_head_t),
	    offsetof(nvmf_ns_head_t, head_link));
}

void
nvmf_mpath_fini(nvmf_softc_t *sc)
{
	list_destroy(&sc->heads);
}

static boolean_t
nvmf_nonzero(const uint8_t *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (p[i] != 0)
			return (B_TRUE);
	}
	return (B_FALSE);
}

static void
nvmf_ns_id_init(nvmf_ns_id_t *id, nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data)
{
	bzero(id, sizeof (*id));
	bcopy(sc->cdata->id_subnqn, id->nid_subnqn, sizeof (id->nid_subnqn));

	if (nvmf_nonzero(data->id_nguid, sizeof (data->id_nguid))) {
		bcopy(data->id_nguid, id->nid_nguid, sizeof (id->nid_nguid));
		id->nid_have_nguid = B_TRUE;
	}
	if (nvmf_nonzero(data->id_eui64, sizeof (data->id_eui64))) {
		bcopy(data->id_eui64, id->nid_eui64, sizeof (id->nid_eui64));
		id->nid_have_eui64 = B_TRUE;
	}
	if (!id->nid_have_nguid && !id->nid_have_eui64)
		id->nid_nsid = nsid;
}

static nvmf_ns_head_t *
nvmf_head_find(nvmf_softc_t *sc, const nvmf_ns_id_t *id)
{
	nvmf_ns_head_t *head;

	for (head = list_head(&sc->heads); head != NULL;
	    head = list_next(&sc->heads, head)) {
		if (bcmp(&head->head_id, id, sizeof (*id)) == 0)
			return (head);
	}
	return (NULL);
}

static void
nvmf_head_free(nvmf_ns_head_t *head)
{
	nvmf_softc_t *sc = head->head_sc;

	ASSERT(!head->head_attached);
	ASSERT3P(head->head_path, ==, NULL);

	mutex_enter(&sc->lock);
	list_remove(&sc->heads, head);
	mutex_exit(&sc->lock);

	bd_free_handle(head->head_bdh);
	mutex_destroy(&head->head_lock);
	kmem_free(head, sizeof (*head));
}

/*
 * Give up a head that has no path.  If its disk is still open the head stays,
 * and blkdev is told so that it sees the media as gone.
 */
static boolean_t
nvmf_head_dispose(nvmf_ns_head_t *head)
{
	if (head->head_attached) {
		if (bd_detach_handle(head->head_bdh) != DDI_SUCCESS) {
			bd_state_change(head->head_bdh);
			return (B_FALSE);
		}
		head->head_attached = B_FALSE;
	}
	nvmf_head_free(head);
	return (B_TRUE);
}

/*
 * Bind a namespace of this association to a disk.  Returns NULL, having
 * logged why, if the namespace cannot be presented.
 */
struct nvmf_path *
nvmf_mpath_add_path(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data, uint64_t nblks, uint32_t blksize)
{
	nvmf_ns_id_t id;
	nvmf_ns_head_t *head;
	nvmf_path_t *path;
	uint64_t maxxfer = sc->max_xfer_size;
	boolean_t reused = B_FALSE;

	/*
	 * blkdev sizes a transfer in its own copy of the block size, and it
	 * keeps a stale one if it rejects the media.  Refuse here whatever
	 * bd_update_state() would refuse.
	 */
	if (maxxfer > UINT32_MAX || blksize > maxxfer ||
	    P2PHASE(maxxfer, blksize) != 0) {
		dev_err(sc->dip, CE_WARN, "!ns%u: block size %u does not fit "
		    "the transfer limit %llu", nsid, blksize,
		    (u_longlong_t)maxxfer);
		return (NULL);
	}

	nvmf_ns_id_init(&id, sc, nsid, data);
	head = nvmf_head_find(sc, &id);
	if (head != NULL && head->head_path != NULL) {
		dev_err(sc->dip, CE_WARN,
		    "!ns%u: ignored, its identity duplicates ns%u", nsid,
		    head->head_path->path_nsid);
		return (NULL);
	}
	if (head != NULL && (head->head_blksize != blksize ||
	    head->head_maxxfer > maxxfer)) {
		if (!nvmf_head_dispose(head)) {
			dev_err(sc->dip, CE_WARN, "!ns%u: ignored, its disk "
			    "is still open with another format", nsid);
			return (NULL);
		}
		head = NULL;
	}

	if (head == NULL) {
		head = kmem_zalloc(sizeof (*head), KM_SLEEP);
		head->head_sc = sc;
		head->head_id = id;
		head->head_blksize = blksize;
		head->head_maxxfer = maxxfer;
		head->head_nblks = nblks;
		mutex_init(&head->head_lock, NULL, MUTEX_DRIVER, NULL);
		head->head_bdh = nvmf_bd_alloc_handle(head);

		mutex_enter(&sc->lock);
		list_insert_tail(&sc->heads, head);
		mutex_exit(&sc->lock);
	} else {
		reused = head->head_attached;
	}

	path = kmem_zalloc(sizeof (*path), KM_SLEEP);
	path->path_head = head;
	path->path_sc = sc;
	path->path_nsid = nsid;
	cv_init(&path->path_cv, NULL, CV_DRIVER, NULL);

	/* Attaching reads the label, so the path must be in place first. */
	mutex_enter(&head->head_lock);
	head->head_nblks = nblks;
	head->head_path = path;
	mutex_exit(&head->head_lock);

	if (bd_attach_handle(sc->dip, head->head_bdh) != DDI_SUCCESS) {
		dev_err(sc->dip, CE_WARN, "!ns%u: failed to attach blkdev",
		    nsid);
		nvmf_mpath_remove_path(path);
		return (NULL);
	}
	head->head_attached = B_TRUE;
	if (reused)
		bd_state_change(head->head_bdh);

	return (path);
}

/*
 * Take a path away from its head.  I/O that waits for a reconnect on this
 * path fails, and I/O in flight is waited for.  That wait ends when the
 * controller answers or when the queue pairs are shut down.
 */
void
nvmf_mpath_remove_path(struct nvmf_path *path)
{
	nvmf_ns_head_t *head = path->path_head;

	mutex_enter(&head->head_lock);
	head->head_path = NULL;
	mutex_exit(&head->head_lock);

	nvmf_bd_path_gone(path->path_sc, path);

	mutex_enter(&head->head_lock);
	while (path->path_refs != 0)
		cv_wait(&path->path_cv, &head->head_lock);
	mutex_exit(&head->head_lock);

	cv_destroy(&path->path_cv);
	kmem_free(path, sizeof (*path));

	(void) nvmf_head_dispose(head);
}

boolean_t
nvmf_mpath_path_gone(struct nvmf_path *path)
{
	ASSERT(MUTEX_HELD(&path->path_sc->lock));
	return (path->path_gone);
}

void
nvmf_mpath_path_set_gone(struct nvmf_path *path)
{
	ASSERT(MUTEX_HELD(&path->path_sc->lock));
	path->path_gone = B_TRUE;
}

/* True if data describes the namespace this path was created for. */
boolean_t
nvmf_mpath_path_matches(struct nvmf_path *path,
    const nvme_identify_nsid_t *data)
{
	nvmf_ns_id_t id;

	nvmf_ns_id_init(&id, path->path_sc, path->path_nsid, data);
	return (bcmp(&id, &path->path_head->head_id, sizeof (id)) == 0);
}

/*
 * Apply fresh Identify data to a path.  Only the size may change.  Returns
 * B_FALSE if the path no longer stands for this namespace.
 */
boolean_t
nvmf_mpath_path_update(struct nvmf_path *path,
    const nvme_identify_nsid_t *data, uint64_t nblks, uint32_t blksize)
{
	nvmf_ns_head_t *head = path->path_head;
	nvmf_softc_t *sc = path->path_sc;
	boolean_t gone, resized;

	mutex_enter(&sc->lock);
	gone = path->path_gone;
	mutex_exit(&sc->lock);

	if (gone || blksize != head->head_blksize ||
	    !nvmf_mpath_path_matches(path, data))
		return (B_FALSE);

	mutex_enter(&head->head_lock);
	resized = head->head_nblks != nblks;
	head->head_nblks = nblks;
	mutex_exit(&head->head_lock);

	/* A disk that could not be attached again earlier gets another try. */
	if (!head->head_attached) {
		if (bd_attach_handle(sc->dip, head->head_bdh) == DDI_SUCCESS)
			head->head_attached = B_TRUE;
	} else if (resized) {
		bd_state_change(head->head_bdh);
	}
	return (B_TRUE);
}

/* Return the head's path with a reference, or NULL if it has none. */
struct nvmf_path *
nvmf_mpath_select(struct nvmf_ns_head *head)
{
	nvmf_path_t *path;

	mutex_enter(&head->head_lock);
	path = head->head_path;
	if (path != NULL)
		path->path_refs++;
	mutex_exit(&head->head_lock);
	return (path);
}

void
nvmf_mpath_select_rele(struct nvmf_path *path)
{
	nvmf_ns_head_t *head = path->path_head;

	mutex_enter(&head->head_lock);
	ASSERT3U(path->path_refs, >, 0);
	if (--path->path_refs == 0)
		cv_broadcast(&path->path_cv);
	mutex_exit(&head->head_lock);
}

/* Attach again each disk that nvmf_mpath_detach_all() detached. */
void
nvmf_mpath_attach_all(nvmf_softc_t *sc)
{
	nvmf_ns_head_t *head;

	for (head = list_head(&sc->heads); head != NULL;
	    head = list_next(&sc->heads, head)) {
		if (head->head_path == NULL || head->head_attached)
			continue;
		if (bd_attach_handle(sc->dip, head->head_bdh) != DDI_SUCCESS) {
			dev_err(sc->dip, CE_WARN, "!ns%u: failed to attach "
			    "blkdev again", head->head_path->path_nsid);
			continue;
		}
		head->head_attached = B_TRUE;
	}
}

/*
 * Detach every disk of the association, or none.  Returns B_FALSE if a disk
 * is open, after attaching again those already detached.
 */
boolean_t
nvmf_mpath_detach_all(nvmf_softc_t *sc)
{
	nvmf_ns_head_t *head;

	for (head = list_head(&sc->heads); head != NULL;
	    head = list_next(&sc->heads, head)) {
		if (head->head_path == NULL || !head->head_attached)
			continue;
		if (bd_detach_handle(head->head_bdh) != DDI_SUCCESS) {
			nvmf_mpath_attach_all(sc);
			return (B_FALSE);
		}
		head->head_attached = B_FALSE;
	}
	return (B_TRUE);
}

/* Free the heads that have no path and whose disk is no longer open. */
void
nvmf_mpath_reap(nvmf_softc_t *sc)
{
	nvmf_ns_head_t *head, *next;

	for (head = list_head(&sc->heads); head != NULL; head = next) {
		next = list_next(&sc->heads, head);
		if (head->head_path == NULL)
			(void) nvmf_head_dispose(head);
	}
}

boolean_t
nvmf_mpath_busy(nvmf_softc_t *sc)
{
	boolean_t busy;

	mutex_enter(&sc->lock);
	busy = !list_is_empty(&sc->heads);
	mutex_exit(&sc->lock);
	return (busy);
}

uint32_t
nvmf_mpath_head_blksize(struct nvmf_ns_head *head)
{
	return (head->head_blksize);
}

uint64_t
nvmf_mpath_head_maxxfer(struct nvmf_ns_head *head)
{
	return (head->head_maxxfer);
}

/* The media of a head, or EIO while it has no path. */
int
nvmf_mpath_head_media(struct nvmf_ns_head *head, uint64_t *nblksp,
    uint32_t *blksizep)
{
	int error = 0;

	mutex_enter(&head->head_lock);
	if (head->head_path == NULL)
		error = EIO;
	*nblksp = head->head_nblks;
	*blksizep = head->head_blksize;
	mutex_exit(&head->head_lock);
	return (error);
}

void
nvmf_mpath_head_ids(struct nvmf_ns_head *head, uint8_t *eui64, uint8_t *guid)
{
	if (head->head_id.nid_have_eui64) {
		bcopy(head->head_id.nid_eui64, eui64,
		    sizeof (head->head_id.nid_eui64));
	}
	if (head->head_id.nid_have_nguid) {
		bcopy(head->head_id.nid_nguid, guid,
		    sizeof (head->head_id.nid_nguid));
	}
}

int
nvmf_mpath_head_devid_init(struct nvmf_ns_head *head, dev_info_t *dip,
    ddi_devid_t *devid)
{
	nvmf_ns_id_t *id = &head->head_id;

	if (id->nid_have_nguid) {
		return (ddi_devid_init(dip, DEVID_NVME_NGUID,
		    sizeof (id->nid_nguid), id->nid_nguid, devid));
	} else if (id->nid_have_eui64) {
		return (ddi_devid_init(dip, DEVID_NVME_EUI64,
		    sizeof (id->nid_eui64), id->nid_eui64, devid));
	}

	return (DDI_FAILURE);
}

uint32_t
nvmf_mpath_path_nsid(struct nvmf_path *path)
{
	return (path->path_nsid);
}

nvmf_softc_t *
nvmf_mpath_path_sc(struct nvmf_path *path)
{
	return (path->path_sc);
}

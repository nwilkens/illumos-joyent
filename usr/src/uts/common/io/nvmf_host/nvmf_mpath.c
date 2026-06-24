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
 * NVMe over Fabrics host native multipath (the namespace-head / path /
 * selector model of NVMEOF.md sections 9.2-9.3).
 *
 * This file is NEW; it has no FreeBSD counterpart.  FreeBSD presents one
 * cam_sim per association and relies on nvme_xpt to enumerate namespaces, so it
 * gets no multipath structure for free.  illumos has neither scsi_vhci (SCSI
 * only) nor a multipath-aware blkdev, so we build the structure that Linux's
 * nvme-core has: a subsystem owns namespace heads; a head is keyed by namespace
 * identity (NGUID/EUI64/UUID) and owns the single bd_handle; each head owns a
 * set of paths; a path is (controller association, that controller's NSID for
 * this head, current ANA state).
 *
 * Why this is structural and must exist in v1 (NVMEOF.md 9.6): device identity
 * (o_devid_init, the disk label) is derived from the HEAD and is
 * path-independent, so failover does not change the disk; and keep-alive death
 * marks a path down rather than tearing down the bd_handle.  Retrofitting this
 * after shipping a one-disk-per-association model is a rewrite.
 *
 * v1 populates one or more paths per head and the selector prefers an
 * Optimized path; until the ANA log page is read every group is treated as
 * Optimized.  The ANA-state log-page logic, AEN-driven re-read, and
 * error-triggered failover are deferred with PORT-TODO markers (the mechanism
 * lands by Phase 3.5).
 *
 * The object model, locking, entry points, and the head's bd_handle lifecycle
 * (attach on first path, detach on last path; keep-alive death only marks paths
 * down) are real.  What remains deferred: NS-UUID extraction from the CNS 0x03
 * descriptor list (only NGUID/EUI64 are taken from Identify Namespace here) and
 * the Phase-3.5 ANA mechanism.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/blkdev.h>
#include <sys/sdt.h>
#include <sys/nvme.h>

#include "nvmf_var.h"

/*
 * Seam to the blkdev binding (nvmf_blkdev.c).  The head owns its bd_handle, but
 * the bd_ops_t table and DMA attributes are private to nvmf_blkdev.c, so the
 * head/path layer drives the bd_handle lifecycle through these two helpers:
 * attach on first-path creation, detach on last-path removal.  Keep-alive death
 * never reaches these (it routes to nvmf_mpath_path_down), so the disk survives
 * as long as any path remains (NVMEOF.md 9.3).
 *
 * nvmf_blkdev_attach_head() allocates a bd_handle for the head (passing the
 * head itself as the bd_ops "driver private" so the callbacks resolve geometry
 * and NSID through it) and bd_attach_handle()s it to dip; it returns the
 * handle, or NULL on failure.  nvmf_blkdev_detach_head() bd_detach_handle()s
 * and bd_free_handle()s a handle.  The head retains ownership of the handle
 * storage; these helpers only translate between the head and blkdev's ops.
 * Both are declared in nvmf_var.h.
 */

/*
 * Path lifetime: nvmf_mpath_select() hands a path to the blkdev binding with an
 * in-flight reference held; the binding MUST release it via this entry point on
 * every post-select code path (normal completion in nvmf_bd_finish AND every
 * early return after a successful select: sync-cache no-op, request/range
 * allocation failure, unsupported DSM, and submit/append failure).  A missed
 * release stalls nvmf_mpath_remove_path()'s drain.
 *
 * PORT-TODO (integrator): add the prototype to nvmf_var.h next to the other
 * nvmf_mpath.c accessors, then wire the rele calls into nvmf_blkdev.c.
 */
void nvmf_mpath_select_rele(struct nvmf_path *path);

/*
 * ANA (Asymmetric Namespace Access) state for a path.  Values match the NVMe
 * ANA log-page state values so the selector can compare directly.
 */
typedef enum nvmf_ana_state {
	NVMF_ANA_OPTIMIZED	= 0x1,
	NVMF_ANA_NONOPTIMIZED	= 0x2,
	NVMF_ANA_INACCESSIBLE	= 0x3,
	NVMF_ANA_PERSISTENT_LOSS = 0x4,
	NVMF_ANA_CHANGE		= 0xf
} nvmf_ana_state_t;

/*
 * Namespace identity used to key a head.  Per NVMEOF.md 9.1/9.3 the host
 * recognizes the *same* namespace across controllers by NGUID, then EUI64, then
 * NS UUID (from the Identify Namespace ID-descriptor list).
 */
typedef struct nvmf_ns_id {
	uint8_t		nid_nguid[16];
	uint8_t		nid_eui64[8];
	uint8_t		nid_uuid[16];
	boolean_t	nid_have_nguid;
	boolean_t	nid_have_eui64;
	boolean_t	nid_have_uuid;
} nvmf_ns_id_t;

/*
 * A path: this association's view of one head.  Owned by both a head
 * (head_link) and the providing softc (sc_link, see nvmf_softc_t.paths).
 */
typedef struct nvmf_path {
	struct nvmf_ns_head	*path_head;
	nvmf_softc_t		*path_sc;
	uint32_t		path_nsid;	/* this controller's NSID */
	/*
	 * ANA Group ID for this namespace on this controller (Identify Namespace
	 * id_anagrpid).  The group is the failover unit (NVMEOF.md 9.5); the
	 * Phase-3.5 ANA log-page handler keys path_ana_state off this.  Captured
	 * in v1 as required structural state even though the selector does not
	 * yet consume it.
	 */
	uint32_t		path_anagrpid;
	nvmf_ana_state_t	path_ana_state;
	boolean_t		path_down;

	/*
	 * In-flight I/O lifetime guard (adversarial review: nvmf_path_t UAF).
	 * nvmf_mpath_select() takes a reference under head_lock before returning
	 * a path; the blkdev binding releases it via nvmf_mpath_select_rele()
	 * when the command finishes.  nvmf_mpath_remove_path() retires the path
	 * (path_removing) so no new reference can be taken, then drains
	 * path_io_refs to 0 before freeing.  path_io_cv (associated with the
	 * head_lock that protects path_io_refs) carries the drain wakeup.
	 */
	uint_t			path_io_refs;
	boolean_t		path_removing;
	kcondvar_t		path_io_cv;

	list_node_t		head_link;	/* in head->paths */
	list_node_t		sc_link;	/* in sc->paths */
} nvmf_path_t;

/*
 * A namespace head: one blkdev disk, identity-keyed, owning N paths.  The
 * bd_handle survives as long as any path remains (NVMEOF.md 9.3).
 */
typedef struct nvmf_ns_head {
	nvmf_ns_id_t		head_id;
	uint64_t		head_nblks;
	uint32_t		head_blksize;

	bd_handle_t		head_bdh;	/* the single blkdev handle */

	kmutex_t		head_lock;
	list_t			paths;		/* nvmf_path_t by head_link */
	uint_t			head_npaths;

	list_node_t		subsys_link;	/* in nvmf_heads */
} nvmf_ns_head_t;

/*
 * Subsystem-wide head registry.  PORT-TODO: in a full implementation heads hang
 * off a per-subsystem object keyed by subnqn; v1 uses one global list guarded
 * by nvmf_heads_lock since there is a single subsystem per attach in practice.
 */
static kmutex_t nvmf_heads_lock;
static list_t nvmf_heads;
static boolean_t nvmf_mpath_inited;

void
nvmf_mpath_init(void)
{
	if (nvmf_mpath_inited)
		return;
	mutex_init(&nvmf_heads_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&nvmf_heads, sizeof (nvmf_ns_head_t),
	    offsetof(nvmf_ns_head_t, subsys_link));
	nvmf_mpath_inited = B_TRUE;
}

void
nvmf_mpath_fini(void)
{
	if (!nvmf_mpath_inited)
		return;
	list_destroy(&nvmf_heads);
	mutex_destroy(&nvmf_heads_lock);
	nvmf_mpath_inited = B_FALSE;
}

/*
 * Per-softc path bookkeeping.  The sc->paths list links nvmf_path_t by
 * sc_link; since nvmf_path_t is private to this file, the list lifecycle and
 * iteration live here rather than in nvmf_blkdev.c.
 */
void
nvmf_mpath_softc_init(nvmf_softc_t *sc)
{
	nvmf_mpath_init();
	mutex_init(&sc->mpath_mtx, NULL, MUTEX_DRIVER, NULL);
	list_create(&sc->paths, sizeof (nvmf_path_t),
	    offsetof(nvmf_path_t, sc_link));
}

void
nvmf_mpath_softc_fini(nvmf_softc_t *sc)
{
	list_destroy(&sc->paths);
	mutex_destroy(&sc->mpath_mtx);
}

/* Mark every path this association provides down (keep-alive death). */
void
nvmf_mpath_softc_down(nvmf_softc_t *sc)
{
	nvmf_path_t *path;

	mutex_enter(&sc->mpath_mtx);
	for (path = list_head(&sc->paths); path != NULL;
	    path = list_next(&sc->paths, path)) {
		nvmf_mpath_path_down(path);
	}
	mutex_exit(&sc->mpath_mtx);
}

void
nvmf_mpath_softc_up(nvmf_softc_t *sc)
{
	nvmf_path_t *path;

	mutex_enter(&sc->mpath_mtx);
	for (path = list_head(&sc->paths); path != NULL;
	    path = list_next(&sc->paths, path)) {
		nvmf_mpath_path_up(path);
	}
	mutex_exit(&sc->mpath_mtx);
}

/* Remove all paths this association provides (association teardown). */
void
nvmf_mpath_softc_remove_all(nvmf_softc_t *sc)
{
	nvmf_path_t *path;

	for (;;) {
		mutex_enter(&sc->mpath_mtx);
		path = list_head(&sc->paths);
		mutex_exit(&sc->mpath_mtx);
		if (path == NULL)
			break;
		nvmf_mpath_remove_path(path);
	}
}

/*
 * Extract a namespace identity from Identify Namespace data.
 *
 * PORT-TODO: the NS UUID comes from the Namespace Identification Descriptor
 * list (Identify CNS 0x03), not from Identify Namespace; fetch and parse that
 * separately.  Here we take NGUID and EUI64 directly from the nsid data.
 */
static void
nvmf_ns_id_from_data(nvmf_ns_id_t *id, const nvme_identify_nsid_t *data)
{
	uint_t i;
	boolean_t nz;

	bzero(id, sizeof (*id));

	nz = B_FALSE;
	for (i = 0; i < sizeof (data->id_nguid); i++) {
		if (data->id_nguid[i] != 0) {
			nz = B_TRUE;
			break;
		}
	}
	if (nz) {
		bcopy(data->id_nguid, id->nid_nguid, sizeof (id->nid_nguid));
		id->nid_have_nguid = B_TRUE;
	}

	nz = B_FALSE;
	for (i = 0; i < sizeof (data->id_eui64); i++) {
		if (data->id_eui64[i] != 0) {
			nz = B_TRUE;
			break;
		}
	}
	if (nz) {
		bcopy(data->id_eui64, id->nid_eui64, sizeof (id->nid_eui64));
		id->nid_have_eui64 = B_TRUE;
	}
}

static boolean_t
nvmf_ns_id_equal(const nvmf_ns_id_t *a, const nvmf_ns_id_t *b)
{
	if (a->nid_have_nguid && b->nid_have_nguid)
		return (bcmp(a->nid_nguid, b->nid_nguid,
		    sizeof (a->nid_nguid)) == 0);
	if (a->nid_have_eui64 && b->nid_have_eui64)
		return (bcmp(a->nid_eui64, b->nid_eui64,
		    sizeof (a->nid_eui64)) == 0);
	if (a->nid_have_uuid && b->nid_have_uuid)
		return (bcmp(a->nid_uuid, b->nid_uuid,
		    sizeof (a->nid_uuid)) == 0);
	return (B_FALSE);
}

/*
 * Find the head matching a namespace identity, or NULL.  Caller must hold
 * nvmf_heads_lock.
 */
static nvmf_ns_head_t *
nvmf_mpath_find_head_locked(const nvmf_ns_id_t *id)
{
	nvmf_ns_head_t *head;

	ASSERT(MUTEX_HELD(&nvmf_heads_lock));

	for (head = list_head(&nvmf_heads); head != NULL;
	    head = list_next(&nvmf_heads, head)) {
		if (nvmf_ns_id_equal(&head->head_id, id))
			return (head);
	}
	return (NULL);
}

struct nvmf_ns_head *
nvmf_mpath_find_head(const nvme_identify_nsid_t *data)
{
	nvmf_ns_id_t id;
	nvmf_ns_head_t *head;

	nvmf_ns_id_from_data(&id, data);
	mutex_enter(&nvmf_heads_lock);
	head = nvmf_mpath_find_head_locked(&id);
	mutex_exit(&nvmf_heads_lock);
	return (head);
}

/*
 * Add (or join) a path for the namespace described by data, provided by this
 * association.  Returns the path so the blkdev binding can drive I/O on it.
 *
 * If no head exists for the identity, one is created and its blkdev handle is
 * attached (via nvmf_blkdev_attach_head) the first time the head appears.  On a
 * subsequent association reaching the same namespace, only a new path is added
 * and the existing bd_handle is reused -> this is the "additional path handoff"
 * seam (NVMEOF.md 9.3).
 */
struct nvmf_path *
nvmf_mpath_add_path(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data)
{
	nvmf_ns_id_t id;
	nvmf_ns_head_t *head;
	nvmf_path_t *path;
	uint64_t nblks = 0;
	uint32_t blksize = 0;
	boolean_t new_head = B_FALSE;
	bd_handle_t bdh;

	nvmf_ns_id_from_data(&id, data);
	(void) nvmf_ns_fmt(data, &nblks, &blksize, NULL, 0);

	mutex_enter(&nvmf_heads_lock);
	head = nvmf_mpath_find_head_locked(&id);
	if (head == NULL) {
		head = kmem_zalloc(sizeof (*head), KM_SLEEP);
		head->head_id = id;
		head->head_nblks = nblks;
		head->head_blksize = blksize;
		mutex_init(&head->head_lock, NULL, MUTEX_DRIVER, NULL);
		list_create(&head->paths, sizeof (nvmf_path_t),
		    offsetof(nvmf_path_t, head_link));
		list_insert_tail(&nvmf_heads, head);
		new_head = B_TRUE;
	}
	mutex_exit(&nvmf_heads_lock);

	path = kmem_zalloc(sizeof (*path), KM_SLEEP);
	path->path_head = head;
	path->path_sc = sc;
	path->path_nsid = nsid;
	path->path_anagrpid = data->id_anagrpid;
	/*
	 * v1: every group is Optimized; the ANA log page is not yet read, so a
	 * path's real state is unknown.  Treat it as Optimized so the selector
	 * uses it; Phase 3.5 replaces this with the log-page-driven state.
	 */
	path->path_ana_state = NVMF_ANA_OPTIMIZED;
	path->path_down = B_FALSE;
	path->path_io_refs = 0;
	path->path_removing = B_FALSE;
	cv_init(&path->path_io_cv, NULL, CV_DRIVER, NULL);

	mutex_enter(&head->head_lock);
	list_insert_tail(&head->paths, path);
	head->head_npaths++;
	mutex_exit(&head->head_lock);

	mutex_enter(&sc->mpath_mtx);
	list_insert_tail(&sc->paths, path);
	mutex_exit(&sc->mpath_mtx);

	/*
	 * On first appearance of a head, attach its blkdev disk.  The bd_handle
	 * is owned by the head and survives keep-alive death of individual paths
	 * (NVMEOF.md 9.3); only last-path removal detaches it.  bd_attach_handle
	 * may call back into our bd_ops, so it must run without head_lock held.
	 *
	 * On a subsequent association reaching the same namespace we fall through
	 * here with new_head == B_FALSE: only the new path was added and the
	 * existing bd_handle is reused (the additional-path handoff seam).
	 *
	 * Storing head_bdh after dropping nvmf_heads_lock is safe: our path is
	 * already linked, so head_npaths >= 1 and nvmf_mpath_remove_path cannot
	 * tear this head down until that path is removed.  The store and any
	 * concurrent last-path read of head_bdh are both under head_lock.
	 */
	if (new_head) {
		DTRACE_PROBE1(nvmf__mpath__new__head, nvmf_ns_head_t *, head);
		bdh = nvmf_blkdev_attach_head(head, sc->dip);
		mutex_enter(&head->head_lock);
		head->head_bdh = bdh;
		mutex_exit(&head->head_lock);
	}

	return (path);
}

/*
 * Bound (in nanoseconds) on how long nvmf_mpath_remove_path() waits for a
 * retiring path's in-flight I/O to drain.  In the common case the references
 * are dropped by the blkdev binding within microseconds; the bound only exists
 * so a missed release (e.g. an integration where nvmf_blkdev.c does not yet call
 * nvmf_mpath_select_rele() on every path) degrades to a logged, deliberate leak
 * instead of an unbounded hang during teardown.
 */
#define	NVMF_MPATH_DRAIN_TIMEOUT_SECS	30

/*
 * Wait under head_lock for path->path_io_refs to fall to zero.  Returns B_TRUE
 * if it drained, B_FALSE if the bound elapsed first (the path still has live
 * references and must NOT be freed).  Caller holds head_lock; this function
 * keeps holding it across the wait via cv_timedwait().
 */
static boolean_t
nvmf_mpath_drain_path(nvmf_path_t *path)
{
	clock_t deadline;

	ASSERT(MUTEX_HELD(&path->path_head->head_lock));

	deadline = ddi_get_lbolt() +
	    drv_usectohz((clock_t)NVMF_MPATH_DRAIN_TIMEOUT_SECS * MICROSEC);
	while (path->path_io_refs != 0) {
		if (cv_timedwait(&path->path_io_cv, &path->path_head->head_lock,
		    deadline) == -1)
			break;
	}
	return (path->path_io_refs == 0);
}

/*
 * Remove a path.  When the last path of a head goes away, the head (and its
 * bd_handle) is destroyed.  A path going *down* (keep-alive death) does NOT
 * call this; it calls nvmf_mpath_path_down so the bd_handle survives.
 *
 * Lifetime (adversarial review fix): a path selected for I/O carries an
 * in-flight reference (nvmf_mpath_select), so this function cannot free it while
 * a command is still outstanding -- doing so was a use-after-free reachable from
 * the lock-free AER rescan path.  The path is first retired (path_removing, and
 * unlinked from head->paths) so the selector stops handing it out, then its
 * in-flight references are drained before it is freed.
 */
void
nvmf_mpath_remove_path(struct nvmf_path *path)
{
	nvmf_ns_head_t *head = path->path_head;
	nvmf_softc_t *sc = path->path_sc;
	boolean_t last;
	boolean_t drained;
	bd_handle_t bdh;

	mutex_enter(&sc->mpath_mtx);
	list_remove(&sc->paths, path);
	mutex_exit(&sc->mpath_mtx);

	/*
	 * Retire the path AND decide whether it was the last, unlinking the head
	 * from the registry atomically under nvmf_heads_lock.  nvmf_mpath_add_path
	 * looks up / inserts heads under the same lock, so holding it across the
	 * last-path decision prevents a concurrent add_path from joining a head
	 * that is about to be destroyed.  Setting path_removing and unlinking the
	 * path from head->paths here makes the selector skip it, so no NEW
	 * in-flight reference can be taken after this point.  Lock order is
	 * nvmf_heads_lock -> head_lock.
	 */
	mutex_enter(&nvmf_heads_lock);
	mutex_enter(&head->head_lock);
	path->path_removing = B_TRUE;
	list_remove(&head->paths, path);
	head->head_npaths--;
	last = (head->head_npaths == 0);
	bdh = last ? head->head_bdh : NULL;
	if (last) {
		head->head_bdh = NULL;
		list_remove(&nvmf_heads, head);
	}
	mutex_exit(&nvmf_heads_lock);

	/*
	 * Drain any references taken by a selector that ran before the path was
	 * retired.  cv_timedwait keeps head_lock across the wait; the releaser
	 * (nvmf_mpath_select_rele) takes the same lock to decrement and signal.
	 * nvmf_heads_lock is already dropped so unrelated heads are unaffected.
	 */
	drained = nvmf_mpath_drain_path(path);
	mutex_exit(&head->head_lock);

	if (!drained) {
		/*
		 * The bound elapsed with I/O still referencing the path.  Freeing
		 * it now would be a use-after-free, and freeing the head (or
		 * detaching its bd_handle) would dangle path->path_head or race
		 * I/O still reaching blkdev through this head.  The only safe
		 * action is to deliberately leak the path -- and, if it was the
		 * last path, the head and its still-attached bd_handle too -- and
		 * warn.  This only happens if a selected path's reference was
		 * never released (a wiring gap in nvmf_blkdev.c, see the seam
		 * comment); a correctly wired data path drains in microseconds.
		 */
		DTRACE_PROBE2(nvmf__mpath__drain__timeout, nvmf_path_t *, path,
		    nvmf_ns_head_t *, head);
		cmn_err(CE_WARN, "!nvmf: path I/O did not drain on removal; "
		    "leaking path to avoid use-after-free");
		return;
	}

	cv_destroy(&path->path_io_cv);
	kmem_free(path, sizeof (*path));

	if (!last)
		return;

	/*
	 * Last path of this head is gone and fully drained: tear the disk down.
	 * The head is no longer reachable from the registry, so detach the
	 * bd_handle (which may re-enter our bd_ops, hence no lock held) and free
	 * the head.
	 */
	if (bdh != NULL)
		nvmf_blkdev_detach_head(bdh);

	list_destroy(&head->paths);
	mutex_destroy(&head->head_lock);
	kmem_free(head, sizeof (*head));
}

/*
 * Mark a path down without tearing it down.  Keep-alive death / disconnect
 * routes here (NVMEOF.md 9.3): the association's paths are marked down and I/O
 * reroutes; the bd_handle survives as long as any path remains.
 */
void
nvmf_mpath_path_down(struct nvmf_path *path)
{
	nvmf_ns_head_t *head = path->path_head;

	mutex_enter(&head->head_lock);
	path->path_down = B_TRUE;
	mutex_exit(&head->head_lock);
}

void
nvmf_mpath_path_up(struct nvmf_path *path)
{
	nvmf_ns_head_t *head = path->path_head;

	mutex_enter(&head->head_lock);
	path->path_down = B_FALSE;
	mutex_exit(&head->head_lock);
}

/*
 * Accessors so the blkdev binding (nvmf_blkdev.c) can read head geometry and
 * the path's NSID without the head/path structs being public.  Geometry is set
 * at head creation from the first path's Identify data (nvmf_ns_fmt) and
 * refreshed via nvmf_mpath_head_set_geometry on a namespace-resize rescan.
 */
uint32_t
nvmf_mpath_head_blksize(struct nvmf_ns_head *head)
{
	return (head->head_blksize);
}

uint64_t
nvmf_mpath_head_nblks(struct nvmf_ns_head *head)
{
	return (head->head_nblks);
}

void
nvmf_mpath_head_set_geometry(struct nvmf_ns_head *head, uint64_t nblks,
    uint32_t blksize)
{
	boolean_t changed;
	bd_handle_t bdh;

	mutex_enter(&head->head_lock);
	changed = (head->head_nblks != nblks || head->head_blksize != blksize);
	head->head_nblks = nblks;
	head->head_blksize = blksize;
	bdh = head->head_bdh;
	mutex_exit(&head->head_lock);

	/*
	 * A resized/reformatted namespace must be reflected in the disk.  Notify
	 * blkdev to re-read media info; bd_state_change re-enters our bd_ops, so
	 * it must run with head_lock dropped.
	 */
	if (changed && bdh != NULL)
		bd_state_change(bdh);
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

/*
 * Return a representative path for the head, or NULL if it currently has none.
 * Used by the blkdev binding to reach a providing association for the
 * controller-level Identify data when filling out bd_drive_t.  Any path serves:
 * all paths to one head are in the same subsystem and report identical
 * controller strings.  Prefer an up path so kstats reflect a live controller.
 */
struct nvmf_path *
nvmf_mpath_head_path(struct nvmf_ns_head *head)
{
	nvmf_path_t *path, *any = NULL;

	mutex_enter(&head->head_lock);
	for (path = list_head(&head->paths); path != NULL;
	    path = list_next(&head->paths, path)) {
		if (any == NULL)
			any = path;
		if (!path->path_down) {
			any = path;
			break;
		}
	}
	mutex_exit(&head->head_lock);
	return (any);
}

/*
 * Copy the head's EUI64 / NGUID into the caller's buffer if present, returning
 * B_TRUE when the identity exists (else the buffer is left as-is).  These feed
 * bd_drive_t.d_eui64 / d_guid and are path-independent (NVMEOF.md 9.3).
 */
boolean_t
nvmf_mpath_head_eui64(struct nvmf_ns_head *head, uint8_t *eui64)
{
	if (!head->head_id.nid_have_eui64)
		return (B_FALSE);
	bcopy(head->head_id.nid_eui64, eui64, sizeof (head->head_id.nid_eui64));
	return (B_TRUE);
}

boolean_t
nvmf_mpath_head_guid(struct nvmf_ns_head *head, uint8_t *guid)
{
	if (!head->head_id.nid_have_nguid)
		return (B_FALSE);
	bcopy(head->head_id.nid_nguid, guid, sizeof (head->head_id.nid_nguid));
	return (B_TRUE);
}

/*
 * Build a path-independent devid for the head (NVMEOF.md 7.3 o_devid_init).
 * NGUID is preferred over EUI64, matching nvme(4D) nvme_bd_devid.  A namespace
 * with neither has no stable cross-controller identity, so we decline rather
 * than fabricate an NSID-based devid (the NSID can differ per controller, which
 * would make the devid path-dependent and break failover).
 */
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

/*
 * Path selector.  Choose a path for one bd_xfer_t: prefer an Optimized path,
 * else the first usable Non-Optimized path; skip paths that are down,
 * Inaccessible, in Persistent-Loss, or being retired (path_removing).  Returns
 * NULL if no path is usable, in which case the blkdev binding fails the
 * transfer (ENXIO).
 *
 * Lifetime (adversarial review fix): the selected path is dereferenced and
 * submitted on by the caller AFTER head_lock is dropped, and the I/O may still
 * be in flight when an asynchronous rescan (nvmf_host.c nvmf_rescan_ns_1, which
 * runs on the AER taskq WITHOUT connection_lock) calls nvmf_destroy_ns ->
 * nvmf_mpath_remove_path.  The old code assumed qpair teardown always preceded
 * path removal; that ordering does not hold on the AER path, so the path could
 * be freed under live I/O (use-after-free of nvmf_path_t).
 *
 * To make the hand-off safe the selector takes an in-flight reference on the
 * chosen path (path_io_refs, under head_lock).  nvmf_mpath_remove_path() retires
 * the path and drains that reference to zero before freeing.  The blkdev binding
 * MUST drop the reference with nvmf_mpath_select_rele() on every post-select
 * code path (see the seam comment near the top of this file).
 *
 * PORT-TODO (NVMEOF.md 9.3 / 9.6, by Phase 3.5): round-robin within a state
 * class for balancing, and let the I/O completion path retry on an alternate
 * path (nvmf_blkdev.c nvmf_bd_finish) before calling bd_xfer_done.
 */
struct nvmf_path *
nvmf_mpath_select(struct nvmf_ns_head *head, bd_xfer_t *xfer)
{
	nvmf_path_t *path, *fallback = NULL;

	_NOTE(ARGUNUSED(xfer));

	mutex_enter(&head->head_lock);
	for (path = list_head(&head->paths); path != NULL;
	    path = list_next(&head->paths, path)) {
		if (path->path_down || path->path_removing)
			continue;
		if (path->path_ana_state == NVMF_ANA_INACCESSIBLE ||
		    path->path_ana_state == NVMF_ANA_PERSISTENT_LOSS)
			continue;
		if (path->path_ana_state == NVMF_ANA_OPTIMIZED) {
			path->path_io_refs++;
			mutex_exit(&head->head_lock);
			return (path);
		}
		if (fallback == NULL)
			fallback = path;
	}
	if (fallback != NULL)
		fallback->path_io_refs++;
	mutex_exit(&head->head_lock);
	return (fallback);
}

/*
 * Release the in-flight reference taken by nvmf_mpath_select().  The blkdev
 * binding calls this exactly once per successful select once the command has
 * finished (or could not be submitted).  When the count reaches zero a thread
 * draining the path in nvmf_mpath_remove_path() is woken.
 */
void
nvmf_mpath_select_rele(struct nvmf_path *path)
{
	nvmf_ns_head_t *head = path->path_head;

	mutex_enter(&head->head_lock);
	ASSERT3U(path->path_io_refs, >, 0);
	if (--path->path_io_refs == 0 && path->path_removing)
		cv_broadcast(&path->path_io_cv);
	mutex_exit(&head->head_lock);
}

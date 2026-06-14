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
 * NVMe over Fabrics host blkdev binding (NVMEOF.md section 7.3).
 *
 * This file is NEW; it REPLACES FreeBSD's CAM SIM glue (host/nvmf_sim.c).
 * FreeBSD maps NVMe onto CAM's nvmeio CCBs and lets nvme_xpt build disks;
 * illumos has no CAM, so we present each namespace HEAD as one blkdev(4D) disk
 * via bd_ops_t and route I/O through the multipath layer (nvmf_mpath.c).
 *
 * The bd_ops_t entry points map to NVMe commands as follows (NVMEOF.md 7.3):
 *
 *   o_drive_info  -> queue count/size + EUI64/GUID/serial from Identify
 *   o_media_info  -> m_nblks / m_blksize from Identify Namespace
 *   o_read        -> NVMe READ  (opcode 0x2)
 *   o_write       -> NVMe WRITE (opcode 0x1)
 *   o_sync_cache  -> NVMe FLUSH (opcode 0x0)
 *   o_free_space  -> NVMe DATASET MANAGEMENT / deallocate (opcode 0x9)
 *   o_devid_init  -> devid from the namespace identity (path-independent!)
 *
 * Each bd_xfer_t is satisfied by: select a path (nvmf_mpath_select), build the
 * NVMe SQE, attach the transfer's data buffer to the command capsule as a
 * memdesc, submit via that path's I/O qpair, and on completion call
 * bd_xfer_done().
 *
 * Data path (NVMEOF.md 7.3 / 8): the head's bd_handle is allocated with NO DMA
 * attributes (bd_alloc_handle(..., NULL, ...)).  blkdev therefore maps each I/O
 * buffer into the kernel (bp_mapin) and hands us a single virtually-contiguous
 * kernel VA in bd_xfer_t.x_kaddr, exactly the shape the TCP transport's copy
 * path consumes via NVMF_MEMDESC_VADDR.  The DMA-cookie form (x_dmac/x_ndmac)
 * is intentionally not used in this (copy) phase; an RDMA transport (Phase 4)
 * will supply DMA attributes and a cookie-vector memdesc instead.
 *
 * The completion bookkeeping mirrors FreeBSD host/nvmf_sim.c
 * (nvmf_ccb_complete / nvmf_ccb_io_complete with the spriv refcount) and
 * host/nvmf_ns.c (nvmf_ns_submit_bio command-build idiom, BIO_DELETE for DSM).
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ksynch.h>
#include <sys/atomic.h>
#include <sys/list.h>
#include <sys/blkdev.h>
#include <sys/dkio.h>
#include <sys/errno.h>
#include <sys/sdt.h>
#include <sys/nvme.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"
#include "../nvmf/nvmf_transport_internal.h"

/*
 * Minimum per-queue depth reported to blkdev.  nvme(4D) uses its private
 * NVME_MIN_IO_QUEUE_LEN (16) from nvme_var.h; the Fabrics host does not pull in
 * that driver-private header, so the same floor is defined locally.
 */
#define	NVMF_BD_MIN_QSIZE	16

/*
 * Head-identity accessors implemented in nvmf_mpath.c.  The namespace-head and
 * its identity (NGUID/EUI64/UUID) are private to the multipath layer; these
 * narrow helpers let the blkdev binding populate bd_drive_t and build the
 * path-independent devid without exposing the struct.
 *
 * PORT-TODO (integrator): move these prototypes into nvmf_var.h alongside the
 * other nvmf_mpath.c accessors.  They are declared here so this file compiles
 * without an implicit declaration before the header is updated.
 */
extern struct nvmf_path *nvmf_mpath_head_path(struct nvmf_ns_head *head);
extern boolean_t nvmf_mpath_head_eui64(struct nvmf_ns_head *head,
    uint8_t *eui64);
extern boolean_t nvmf_mpath_head_guid(struct nvmf_ns_head *head, uint8_t *guid);
extern int nvmf_mpath_head_devid_init(struct nvmf_ns_head *head,
    dev_info_t *dip, ddi_devid_t *devid);

/*
 * Per-xfer state carried alongside a bd_xfer_t while a command is in flight.
 * FreeBSD abuses CCB spriv fields as a refcount (nvmf_sim.c ccb_refs); we keep
 * an explicit small object since blkdev gives us no private slot on bd_xfer_t.
 *
 * The completion may fire twice: once for the response CQE
 * (nvmf_request_complete_t) and once for the data I/O completion
 * (nvmf_io_complete_t), so a refcount gates the final bd_xfer_done().
 *
 * bc_dsm/bc_dsm_len, when non-NULL, is the DATASET MANAGEMENT range buffer that
 * was attached to the capsule as a memdesc and must be freed when the command
 * completes (FreeBSD nvmf_ns_delete_complete frees bio_driver2).
 */
typedef struct nvmf_bd_cmd {
	bd_xfer_t	*bc_xfer;
	struct nvmf_ns_head *bc_head;
	struct nvmf_path *bc_path;
	uint_t		bc_refs;
	int		bc_error;
	boolean_t	bc_cqe_aborted;
	void		*bc_dsm;
	size_t		bc_dsm_len;
	/*
	 * Expected data-transfer length for the command's data phase, captured
	 * when the memdesc is built.  The data completion (nvmf_bd_io_complete)
	 * compares the transport's xfered count against it so a short transfer
	 * with no CQE error is surfaced as EIO rather than silently treated as a
	 * full success (FreeBSD nvmf_ns_io_complete asserts xfered ==
	 * dxfer_len).  Zero means no data phase.
	 */
	size_t		bc_data_len;
} nvmf_bd_cmd_t;

/*
 * The blkdev "driver private" is the namespace head.  Each bd_ops callback
 * receives this as its first void * argument (set up by bd_alloc_handle).
 */

static void
nvmf_bd_drive_info(void *arg, bd_drive_t *drive)
{
	struct nvmf_ns_head *head = arg;
	struct nvmf_path *path;
	nvmf_softc_t *sc;

	bzero(drive, sizeof (*drive));

	/*
	 * The head is reachable through any of its paths; use a representative
	 * path's providing association for the controller-level Identify data and
	 * this controller's NSID.  If no path exists the disk is between
	 * failovers; report conservative defaults rather than dereferencing a
	 * NULL controller.  A single representative-path lookup avoids racing two
	 * separate accessors against path teardown.
	 */
	path = nvmf_mpath_head_path(head);
	sc = (path != NULL) ? nvmf_mpath_path_sc(path) : NULL;
	if (sc == NULL) {
		drive->d_qcount = 1;
		drive->d_qsize = NVMF_BD_MIN_QSIZE;
		drive->d_removable = B_FALSE;
		drive->d_hotpluggable = B_FALSE;
		return;
	}

	/*
	 * One blkdev queue per I/O submission queue, mirroring nvme(4D)
	 * nvme_bd_driveinfo so blkdev spreads I/O across the controller's
	 * queues.  d_qsize bounds the outstanding I/Os blkdev will submit per
	 * queue before holding the rest in its waitq.
	 */
	drive->d_qcount = sc->num_io_queues;
	drive->d_qsize = sc->max_pending_io / MAX(1, sc->num_io_queues);
	drive->d_qsize = MAX(drive->d_qsize, NVMF_BD_MIN_QSIZE);

	/*
	 * d_maxxfer caps a single transfer.  With no DMA attributes blkdev
	 * would default this to 1MiB; clamp it to the negotiated Fabrics
	 * transfer size so we never build a command larger than the transport
	 * (or controller MDTS) allows.  d_maxxfer is 32-bit; the Fabrics limit
	 * is well within that, but clamp defensively.
	 */
	if (sc->max_xfer_size != 0) {
		drive->d_maxxfer =
		    (uint32_t)MIN(sc->max_xfer_size, 0xffffffffULL);
	}

	drive->d_removable = B_FALSE;
	drive->d_hotpluggable = B_FALSE;

	/*
	 * Path-independent identity: EUI64/GUID come from the HEAD, not from
	 * this particular association (NVMEOF.md 9.3).  d_target is the head's
	 * NSID on the representative path purely for kstat/topology display.
	 */
	(void) nvmf_mpath_head_eui64(head, drive->d_eui64);
	(void) nvmf_mpath_head_guid(head, drive->d_guid);
	drive->d_target = nvmf_mpath_path_nsid(path);
	drive->d_lun = 0;

	/*
	 * Controller strings live in the cached Identify Controller data.  These
	 * are fixed-width, space-padded NVMe fields; report the full field width
	 * (blkdev/kstat trims trailing space) as nvme(4D) does.
	 */
	if (sc->cdata != NULL) {
		drive->d_model = sc->cdata->id_model;
		drive->d_model_len = sizeof (sc->cdata->id_model);
		drive->d_serial = sc->cdata->id_serial;
		drive->d_serial_len = sizeof (sc->cdata->id_serial);
		drive->d_revision = sc->cdata->id_fwrev;
		drive->d_revision_len = sizeof (sc->cdata->id_fwrev);

		/*
		 * If the controller supports Dataset Management the only limit
		 * on a free_space (deallocate) request is the maximum number of
		 * ranges per command (NVMEOF.md 7.3 o_free_space).
		 */
		if (sc->cdata->id_oncs.on_dset_mgmt != 0)
			drive->d_max_free_seg = NVME_DSET_MGMT_MAX_RANGES;
	}
}

static int
nvmf_bd_media_info(void *arg, bd_media_t *media)
{
	struct nvmf_ns_head *head = arg;
	uint64_t nblks;
	uint32_t blksize;

	nblks = nvmf_mpath_head_nblks(head);
	blksize = nvmf_mpath_head_blksize(head);

	/*
	 * The head geometry is cached from Identify Namespace (set by
	 * nvmf_init_ns / nvmf_update_ns via nvmf_mpath_head_set_geometry).  A
	 * zero block size means the head has not been populated yet, which
	 * blkdev would reject; report EIO so the media is treated as not ready.
	 */
	if (blksize == 0)
		return (EIO);

	bzero(media, sizeof (*media));
	media->m_nblks = nblks;
	media->m_blksize = blksize;
	media->m_readonly = B_FALSE;
	media->m_solidstate = B_TRUE;
	media->m_pblksize = blksize;
	return (0);
}

static int
nvmf_bd_devid_init(void *arg, dev_info_t *dip, ddi_devid_t *devid)
{
	struct nvmf_ns_head *head = arg;

	/*
	 * Path-independent identity (NVMEOF.md 7.3 o_devid_init, 9.3): the devid
	 * is built from the namespace identity carried by the HEAD (NGUID, then
	 * EUI64), never from a single association, so it survives failover.  The
	 * actual ddi_devid_init() call lives in nvmf_mpath.c where the head's
	 * identity struct is private.
	 */
	return (nvmf_mpath_head_devid_init(head, dip, devid));
}

/*
 * Completion plumbing.  A command may complete via the response CQE and/or the
 * data I/O completion; the last one in calls bd_xfer_done.
 *
 * Ordering (codex review): snapshot xfer/error, free our private state, THEN
 * call bd_xfer_done().  bc and xfer must not be touched afterwards.
 */
static void
nvmf_bd_finish(nvmf_bd_cmd_t *bc)
{
	bd_xfer_t *xfer;
	int error;

	if (atomic_dec_uint_nv(&bc->bc_refs) != 0)
		return;

	xfer = bc->bc_xfer;
	error = bc->bc_error;

	/*
	 * PORT-TODO (NVMEOF.md 9.3 error-triggered failover): if the command
	 * failed because the path went away (bc_cqe_aborted) and another path
	 * is usable, retry on the alternate path BEFORE completing.  v1 just
	 * completes with the mapped error.
	 */
	if (bc->bc_cqe_aborted && error == 0)
		error = EIO;

	if (bc->bc_dsm != NULL)
		kmem_free(bc->bc_dsm, bc->bc_dsm_len);

	kmem_free(bc, sizeof (*bc));
	bd_xfer_done(xfer, error);
}

static void
nvmf_bd_cqe_complete(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_bd_cmd_t *bc = arg;

	if (nvmf_cqe_aborted(cqe)) {
		bc->bc_cqe_aborted = B_TRUE;
		if (bc->bc_error == 0)
			bc->bc_error = EIO;
	} else if (cqe->cqe_sf.sf_sc != 0 || cqe->cqe_sf.sf_sct != 0) {
		if (bc->bc_error == 0)
			bc->bc_error = EIO;
	}
	nvmf_bd_finish(bc);
}

static void
nvmf_bd_io_complete(void *arg, size_t xfered, int error)
{
	nvmf_bd_cmd_t *bc = arg;

	/*
	 * A transport (data movement) error takes precedence over the generic
	 * EIO synthesised from a CQE status, matching FreeBSD's bio_driver2
	 * precedence in nvmf_ns_io_complete.  A zero-length, error-free data
	 * completion is the "CQE carried the error, no data moved" case and is
	 * left for nvmf_bd_cqe_complete to report.
	 *
	 * A non-zero-length but short transfer with no transport error has no
	 * representation in blkdev (which has no resid) and would otherwise be
	 * reported as a full success, returning stale bytes for the untransferred
	 * tail.  Surface it as EIO -- the same defensive position as FreeBSD's
	 * INVARIANTS assert that xfered == dxfer_len (nvmf_ns_io_complete).
	 */
	if (error != 0)
		bc->bc_error = error;
	else if (xfered != 0 && xfered != bc->bc_data_len && bc->bc_error == 0)
		bc->bc_error = EIO;
	nvmf_bd_finish(bc);
}

/*
 * Common submission path for read/write.  is_write selects WRITE vs READ.
 *
 * The data buffer is the blkdev-provided kernel VA (x_kaddr); the head's
 * bd_handle is allocated without DMA attributes so this is always a single
 * virtually-contiguous buffer of exactly x_nblks * blksize bytes.  It is
 * attached to the command capsule as an NVMF_MEMDESC_VADDR memdesc, which the
 * transport copies to/from (TCP) -- the FreeBSD memdesc_bio() equivalent.
 */
static int
nvmf_bd_rw(struct nvmf_ns_head *head, bd_xfer_t *xfer, boolean_t is_write)
{
	struct nvmf_path *path;
	struct nvmf_host_qpair *qp;
	nvmf_bd_cmd_t *bc;
	nvmf_softc_t *sc;
	nvmf_request_t *req;
	nvme_sqe_t cmd;
	nvmf_memdesc_t mem;
	uint64_t lba;
	uint64_t nlb;
	uint32_t blksize;
	size_t data_len;
	int how;

	lba = (uint64_t)xfer->x_blkno;
	nlb = (uint64_t)xfer->x_nblks;

	/*
	 * NVM READ/WRITE encode the block count zero-based in a 16-bit field;
	 * the bd_drive_t d_maxxfer keeps a single transfer within that bound.
	 */
	if (nlb == 0 || nlb > 0x10000)
		return (EINVAL);

	/*
	 * Read the head block size once and use it both to validate the request
	 * and to size the data memdesc.  Reading it twice (once implicitly via
	 * blkdev's d_blkshift, once here) could disagree if a namespace
	 * resize/reformat AEN ran nvmf_mpath_head_set_geometry between blkdev
	 * sizing the xfer and this point; using a single snapshot keeps the
	 * transport length consistent with the LBA range we encode.
	 */
	blksize = nvmf_mpath_head_blksize(head);
	if (blksize == 0)
		return (EIO);
	data_len = (size_t)nlb * blksize;

	path = nvmf_mpath_select(head, xfer);
	if (path == NULL)
		return (ENXIO);
	sc = nvmf_mpath_path_sc(path);

	/*
	 * blkdev only sets BD_XFER_POLL when dumping; we cannot block then.  The
	 * Fabrics transport never polls, so a dump while disconnected simply
	 * fails the I/O.
	 */
	how = (xfer->x_flags & BD_XFER_POLL) ? KM_NOSLEEP : KM_SLEEP;

	bc = kmem_zalloc(sizeof (*bc), how);
	if (bc == NULL)
		return (ENOMEM);
	bc->bc_xfer = xfer;
	bc->bc_head = head;
	bc->bc_path = path;
	bc->bc_refs = 2;	/* CQE + data I/O completion */
	bc->bc_data_len = data_len;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = is_write ? NVME_OPC_NVM_WRITE : NVME_OPC_NVM_READ;
	cmd.sqe_nsid = nvmf_mpath_path_nsid(path);
	cmd.sqe_cdw10 = (uint32_t)(lba & 0xffffffffu);
	cmd.sqe_cdw11 = (uint32_t)(lba >> 32);
	cmd.sqe_cdw12 = (uint32_t)(nlb - 1);	/* zero-based block count */

	/*
	 * Hold connection_lock as a reader across the queue selection, request
	 * allocation, AND submission.  nvmf_disconnect_task tears down sc->io
	 * under the same lock as a writer: it nvmf_destroy_qp()s each I/O qpair,
	 * frees the sc->io array, NULLs it, and zeroes num_io_queues.  Two
	 * hazards require the reader to span the whole sequence:
	 *
	 *   - the unlocked nvmf_select_io_queue() divides by num_io_queues and
	 *     indexes sc->io[], so a concurrent teardown is a divide-by-zero or a
	 *     use-after-free of a freed sc->io;
	 *   - a request returned by nvmf_allocate_request() is not yet linked on
	 *     its qpair (that happens in nvmf_submit_request), so dropping the
	 *     lock before submit would let nvmf_destroy_qp() free the host qpair
	 *     out from under req->qp.
	 *
	 * This mirrors FreeBSD's sim_mtx scoping in nvmf_sim_io.  An rwlock
	 * reader may block on the KM_SLEEP allocation; the writer (run from a
	 * taskq, not interrupt context) simply waits.  Holding through submit is
	 * safe against recursive reader re-entry (bd_xfer_done can synchronously
	 * re-dispatch the next window into this function) only because the
	 * transport's submit path never completes synchronously on this thread --
	 * it queues transmit work, and nvmf_disconnect() merely dispatches a
	 * taskq -- so no completion can re-enter nvmf_bd_rw under this lock.
	 */
	rw_enter(&sc->connection_lock, RW_READER);
	if (sc->io == NULL || sc->num_io_queues == 0) {
		rw_exit(&sc->connection_lock);
		kmem_free(bc, sizeof (*bc));
		return (ENXIO);
	}
	qp = nvmf_select_io_queue(sc);

	req = nvmf_allocate_request(qp, &cmd, nvmf_bd_cqe_complete, bc, how);
	if (req == NULL) {
		rw_exit(&sc->connection_lock);
		kmem_free(bc, sizeof (*bc));
		return (ENOMEM);
	}

	mem.nmd_type = NVMF_MEMDESC_VADDR;
	mem.nmd_len = data_len;
	mem.nmd_u.nmd_vaddr = xfer->x_kaddr;
	if (nvmf_capsule_append_data(req->nc, &mem, mem.nmd_len, is_write,
	    nvmf_bd_io_complete, bc) != 0) {
		rw_exit(&sc->connection_lock);
		nvmf_free_request(req);
		kmem_free(bc, sizeof (*bc));
		return (EIO);
	}

	nvmf_submit_request(req);
	rw_exit(&sc->connection_lock);
	return (0);
}

static int
nvmf_bd_read(void *arg, bd_xfer_t *xfer)
{
	return (nvmf_bd_rw(arg, xfer, B_FALSE));
}

static int
nvmf_bd_write(void *arg, bd_xfer_t *xfer)
{
	return (nvmf_bd_rw(arg, xfer, B_TRUE));
}

static int
nvmf_bd_sync_cache(void *arg, bd_xfer_t *xfer)
{
	struct nvmf_ns_head *head = arg;
	struct nvmf_path *path;
	struct nvmf_host_qpair *qp;
	nvmf_bd_cmd_t *bc;
	nvmf_softc_t *sc;
	nvmf_request_t *req;
	nvme_sqe_t cmd;
	int how;

	path = nvmf_mpath_select(head, xfer);
	if (path == NULL)
		return (ENXIO);
	sc = nvmf_mpath_path_sc(path);

	/*
	 * If the controller has no volatile write cache the FLUSH is a no-op;
	 * complete immediately rather than round-tripping (nvme(4D) nvme_bd_sync).
	 */
	if (sc->cdata != NULL && sc->cdata->id_vwc.vwc_present == 0) {
		bd_xfer_done(xfer, 0);
		return (0);
	}

	how = (xfer->x_flags & BD_XFER_POLL) ? KM_NOSLEEP : KM_SLEEP;

	bc = kmem_zalloc(sizeof (*bc), how);
	if (bc == NULL)
		return (ENOMEM);
	bc->bc_xfer = xfer;
	bc->bc_head = head;
	bc->bc_path = path;
	bc->bc_refs = 1;	/* no data phase */

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_NVM_FLUSH;
	cmd.sqe_nsid = nvmf_mpath_path_nsid(path);

	/*
	 * Serialize queue selection + submission against association teardown;
	 * see the comment in nvmf_bd_rw.
	 */
	rw_enter(&sc->connection_lock, RW_READER);
	if (sc->io == NULL || sc->num_io_queues == 0) {
		rw_exit(&sc->connection_lock);
		kmem_free(bc, sizeof (*bc));
		return (ENXIO);
	}
	qp = nvmf_select_io_queue(sc);

	req = nvmf_allocate_request(qp, &cmd, nvmf_bd_cqe_complete, bc, how);
	if (req == NULL) {
		rw_exit(&sc->connection_lock);
		kmem_free(bc, sizeof (*bc));
		return (ENOMEM);
	}
	nvmf_submit_request(req);
	rw_exit(&sc->connection_lock);
	return (0);
}

/*
 * o_free_space -> NVMe DATASET MANAGEMENT (deallocate).  The illumos free list
 * (x_dfl) may carry many ranges; each becomes an nvme_range_t in a small data
 * buffer attached to the capsule as a memdesc (FreeBSD nvmf_ns.c BIO_DELETE
 * handles only the single-range form, but the structure is identical).  The
 * range buffer is freed on completion (bc_dsm), mirroring nvmf_ns_delete_complete.
 */
static int
nvmf_bd_free_space(void *arg, bd_xfer_t *xfer)
{
	struct nvmf_ns_head *head = arg;
	const dkioc_free_list_t *dfl = xfer->x_dfl;
	const dkioc_free_list_ext_t *exts;
	struct nvmf_path *path;
	struct nvmf_host_qpair *qp;
	nvmf_bd_cmd_t *bc;
	nvmf_softc_t *sc;
	nvmf_request_t *req;
	nvme_sqe_t cmd;
	nvmf_memdesc_t mem;
	nvme_range_t *ranges;
	size_t ranges_len;
	uint64_t blksize;
	uint64_t i;
	int how;

	if (dfl == NULL)
		return (EINVAL);

	/*
	 * Derive the allocation mode from BD_XFER_POLL exactly as the read/write
	 * and sync paths do, and use it consistently for every allocation in
	 * this function.  blkdev does not currently dispatch free_space during a
	 * crash dump, but a KM_SLEEP allocation in a poll/dump context must never
	 * block, so do not diverge from the documented poll contract.
	 */
	how = (xfer->x_flags & BD_XFER_POLL) ? KM_NOSLEEP : KM_SLEEP;

	path = nvmf_mpath_select(head, xfer);
	if (path == NULL)
		return (ENXIO);
	sc = nvmf_mpath_path_sc(path);

	if (sc->cdata == NULL || sc->cdata->id_oncs.on_dset_mgmt == 0)
		return (ENOTSUP);

	/*
	 * The number of ranges is zero-based in CDW10; blkdev bounds the request
	 * to d_max_free_seg (NVME_DSET_MGMT_MAX_RANGES) reported by drive_info.
	 */
	if (dfl->dfl_num_exts == 0 ||
	    dfl->dfl_num_exts > NVME_DSET_MGMT_MAX_RANGES)
		return (EINVAL);

	blksize = nvmf_mpath_head_blksize(head);
	if (blksize == 0)
		return (EIO);

	bc = kmem_zalloc(sizeof (*bc), how);
	if (bc == NULL)
		return (ENOMEM);
	bc->bc_xfer = xfer;
	bc->bc_head = head;
	bc->bc_path = path;
	bc->bc_refs = 2;	/* CQE + data I/O completion */

	ranges_len = dfl->dfl_num_exts * sizeof (nvme_range_t);
	ranges = kmem_zalloc(ranges_len, how);
	if (ranges == NULL) {
		kmem_free(bc, sizeof (*bc));
		return (ENOMEM);
	}
	bc->bc_dsm = ranges;
	bc->bc_dsm_len = ranges_len;
	bc->bc_data_len = ranges_len;

	exts = dfl->dfl_exts;
	for (i = 0; i < dfl->dfl_num_exts; i++) {
		ranges[i].nr_ctxattr = 0;
		ranges[i].nr_lba =
		    (dfl->dfl_offset + exts[i].dfle_start) / blksize;
		ranges[i].nr_len = exts[i].dfle_length / blksize;
	}

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_NVM_DSET_MGMT;
	cmd.sqe_nsid = nvmf_mpath_path_nsid(path);
	cmd.sqe_cdw10 = (uint32_t)(dfl->dfl_num_exts - 1) & 0xff;
	cmd.sqe_cdw11 = NVME_DSET_MGMT_ATTR_DEALLOCATE;

	/*
	 * Serialize queue selection + submission against association teardown;
	 * see the comment in nvmf_bd_rw.
	 */
	rw_enter(&sc->connection_lock, RW_READER);
	if (sc->io == NULL || sc->num_io_queues == 0) {
		rw_exit(&sc->connection_lock);
		kmem_free(ranges, ranges_len);
		kmem_free(bc, sizeof (*bc));
		return (ENXIO);
	}
	qp = nvmf_select_io_queue(sc);

	req = nvmf_allocate_request(qp, &cmd, nvmf_bd_cqe_complete, bc, how);
	if (req == NULL) {
		rw_exit(&sc->connection_lock);
		kmem_free(ranges, ranges_len);
		kmem_free(bc, sizeof (*bc));
		return (ENOMEM);
	}

	mem.nmd_type = NVMF_MEMDESC_VADDR;
	mem.nmd_len = ranges_len;
	mem.nmd_u.nmd_vaddr = ranges;
	if (nvmf_capsule_append_data(req->nc, &mem, ranges_len, B_TRUE,
	    nvmf_bd_io_complete, bc) != 0) {
		rw_exit(&sc->connection_lock);
		nvmf_free_request(req);
		kmem_free(ranges, ranges_len);
		kmem_free(bc, sizeof (*bc));
		return (EIO);
	}

	nvmf_submit_request(req);
	rw_exit(&sc->connection_lock);
	return (0);
}

static bd_ops_t nvmf_bd_ops = {
	.o_version =		BD_OPS_CURRENT_VERSION,
	.o_drive_info =		nvmf_bd_drive_info,
	.o_media_info =		nvmf_bd_media_info,
	.o_devid_init =		nvmf_bd_devid_init,
	.o_sync_cache =		nvmf_bd_sync_cache,
	.o_read =		nvmf_bd_read,
	.o_write =		nvmf_bd_write,
	.o_free_space =		nvmf_bd_free_space
};

/*
 * Allocate and attach the single blkdev disk for a namespace head.  Called by
 * the multipath layer (nvmf_mpath_add_path) the first time a head appears.
 *
 * No DMA attributes are supplied (the NULL argument to bd_alloc_handle): blkdev
 * maps each I/O buffer in and hands us a contiguous kernel VA (x_kaddr), which
 * is what the copy-based transport consumes (see nvmf_bd_rw).  The head itself
 * is the bd_ops "driver private" so every callback resolves geometry, identity,
 * and NSID through it.
 *
 * Returns the bd_handle (owned by the head) or NULL on failure.
 */
bd_handle_t
nvmf_blkdev_attach_head(struct nvmf_ns_head *head, dev_info_t *dip)
{
	bd_handle_t h;

	h = bd_alloc_handle(head, &nvmf_bd_ops, NULL, KM_SLEEP);
	if (h == NULL)
		return (NULL);

	if (bd_attach_handle(dip, h) != DDI_SUCCESS) {
		bd_free_handle(h);
		return (NULL);
	}

	return (h);
}

/*
 * Detach and free a head's blkdev disk.  Called by the multipath layer when the
 * last path to a head goes away (nvmf_mpath_remove_path).
 */
void
nvmf_blkdev_detach_head(bd_handle_t bdh)
{
	if (bdh == NULL)
		return;
	(void) bd_detach_handle(bdh);
	bd_free_handle(bdh);
}

/*
 * ----------------------------------------------------------------------------
 * Association lifecycle hooks (called by nvmf_host.c, mirroring the SIM hooks).
 *
 * These bridge the per-association softc to the head/path multipath layer.
 * Where FreeBSD freezes/releases a cam_sim queue, illumos marks this
 * association's paths up/down so the bd_handle survives (NVMEOF.md 9.3).
 * ----------------------------------------------------------------------------
 */

int
nvmf_init_bd(nvmf_softc_t *sc)
{
	nvmf_mpath_softc_init(sc);
	return (0);
}

void
nvmf_disconnect_bd(nvmf_softc_t *sc)
{
	/*
	 * Mark every path this association provides as down.  The bd_handle on
	 * each affected head survives as long as another association still
	 * provides a path (NVMEOF.md 9.3 keep-alive-to-path seam).
	 */
	nvmf_mpath_softc_down(sc);
}

void
nvmf_reconnect_bd(nvmf_softc_t *sc)
{
	nvmf_mpath_softc_up(sc);
}

void
nvmf_shutdown_bd(nvmf_softc_t *sc)
{
	mutex_enter(&sc->mpath_mtx);
	sc->mpath_shutdown = B_TRUE;
	mutex_exit(&sc->mpath_mtx);
}

void
nvmf_destroy_bd(nvmf_softc_t *sc)
{
	nvmf_mpath_softc_remove_all(sc);
	nvmf_mpath_softc_fini(sc);
}

/*
 * Downstream notification that a namespace appeared, changed, or went away on
 * this association.  Called by nvmf_host.c (nvmf_add_ns / nvmf_rescan_ns_1 /
 * nvmf_purge_namespaces) AFTER the per-namespace state object (nvmf_ns.c) has
 * already (re)built its path: nvmf_init_ns -> nvmf_mpath_add_path attaches the
 * disk on first head appearance, and nvmf_destroy_ns -> nvmf_mpath_remove_path
 * detaches it on last-path loss.  The disk lifecycle is therefore complete by
 * the time we are called, so unlike FreeBSD's nvmf_sim_rescan_ns (which had to
 * kick a CAM rescan to materialise the disk) this hook only needs to observe
 * the event.  It MUST NOT call nvmf_rescan_ns(): that issues IDENTIFY and calls
 * back here, which would recurse.
 */
void
nvmf_bd_rescan_ns(nvmf_softc_t *sc, uint32_t id)
{
	DTRACE_PROBE2(nvmf__bd__rescan__ns, nvmf_softc_t *, sc, uint32_t, id);
}

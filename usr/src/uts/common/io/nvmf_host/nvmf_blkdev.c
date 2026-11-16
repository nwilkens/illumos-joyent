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
 * blkdev binding for the NVMe over Fabrics host.  Each namespace head is one
 * blkdev(4D) disk.  The handle has no DMA attributes, so blkdev maps each
 * buffer and passes one kernel address in x_kaddr, which the transport copies.
 *
 * A command pins its path from submission to completion.  While the
 * association is lost a command waits on nvmf_softc_t.held instead of
 * failing.  A blkdev entry point may be entered again from bd_xfer_done(), so
 * holding means queueing, never sleeping, and no lock is held across
 * bd_xfer_done().
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
#include <sys/nvme.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"
#include "../nvmf/nvmf_transport_internal.h"

/* nvme(4D) uses the same floor, NVME_MIN_IO_QUEUE_LEN. */
#define	NVMF_BD_MIN_QSIZE	16

typedef struct nvmf_bd_cmd {
	list_node_t		bc_link;	/* in nvmf_softc_t.held */
	bd_xfer_t		*bc_xfer;
	struct nvmf_path	*bc_path;
	nvme_sqe_t		bc_sqe;
	void			*bc_data;
	size_t			bc_data_len;
	boolean_t		bc_write;
	void			*bc_dsm;
	size_t			bc_dsm_len;

	/*
	 * The response and the data completion can run on two threads, so
	 * each writes only its own result.
	 */
	uint_t			bc_refs;
	int			bc_io_error;
	int			bc_cqe_error;
	boolean_t		bc_aborted;
} nvmf_bd_cmd_t;

void
nvmf_bd_init(nvmf_softc_t *sc)
{
	list_create(&sc->held, sizeof (nvmf_bd_cmd_t),
	    offsetof(nvmf_bd_cmd_t, bc_link));
}

void
nvmf_bd_fini(nvmf_softc_t *sc)
{
	list_destroy(&sc->held);
}

static void
nvmf_bd_drive_info(void *arg, bd_drive_t *drive)
{
	struct nvmf_ns_head *head = arg;
	struct nvmf_path *path;
	nvmf_softc_t *sc;

	bzero(drive, sizeof (*drive));

	/* blkdev rejects a free-space capable drive with a zero alignment. */
	drive->d_free_align = 1;
	drive->d_qcount = 1;
	drive->d_qsize = NVMF_BD_MIN_QSIZE;
	drive->d_maxxfer = (uint32_t)nvmf_mpath_head_maxxfer(head);
	drive->d_removable = B_FALSE;
	drive->d_hotpluggable = B_FALSE;
	drive->d_vendor = "NVMf";
	drive->d_vendor_len = strlen("NVMf");
	drive->d_lun = 0;
	nvmf_mpath_head_ids(head, drive->d_eui64, drive->d_guid);

	path = nvmf_mpath_select(head);
	if (path == NULL)
		return;
	sc = nvmf_mpath_path_sc(path);

	/*
	 * A quarter of the submission queue is kept back, so blkdev cannot
	 * fill every queue to its negotiated depth.
	 */
	mutex_enter(&sc->lock);
	drive->d_qcount = MAX(1, sc->num_io_queues);
	drive->d_qsize = (sc->max_pending_io * 3 / 4) / drive->d_qcount;
	drive->d_qsize = MAX(drive->d_qsize, NVMF_BD_MIN_QSIZE);
	if (sc->dsm_supported)
		drive->d_max_free_seg = NVME_DSET_MGMT_MAX_RANGES;
	mutex_exit(&sc->lock);

	drive->d_target = nvmf_mpath_path_nsid(path);

	/* The controller data buffer outlives every head. */
	drive->d_model = sc->cdata->id_model;
	drive->d_model_len = sizeof (sc->cdata->id_model);
	drive->d_product = sc->cdata->id_model;
	drive->d_product_len = sizeof (sc->cdata->id_model);
	drive->d_serial = sc->cdata->id_serial;
	drive->d_serial_len = sizeof (sc->cdata->id_serial);
	drive->d_revision = sc->cdata->id_fwrev;
	drive->d_revision_len = sizeof (sc->cdata->id_fwrev);

	nvmf_mpath_select_rele(path);
}

static int
nvmf_bd_media_info(void *arg, bd_media_t *media)
{
	struct nvmf_ns_head *head = arg;
	uint64_t nblks;
	uint32_t blksize;
	int error;

	error = nvmf_mpath_head_media(head, &nblks, &blksize);
	if (error != 0)
		return (error);

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
	return (nvmf_mpath_head_devid_init(arg, dip, devid));
}

static void
nvmf_bd_free(nvmf_bd_cmd_t *bc)
{
	if (bc->bc_dsm != NULL)
		kmem_free(bc->bc_dsm, bc->bc_dsm_len);
	nvmf_mpath_select_rele(bc->bc_path);
	kmem_free(bc, sizeof (*bc));
}

static void
nvmf_bd_done(nvmf_bd_cmd_t *bc, int error)
{
	bd_xfer_t *xfer = bc->bc_xfer;

	nvmf_bd_free(bc);
	bd_xfer_done(xfer, error);
}

/*
 * A command that the host aborted during a teardown waits for the next
 * association if the policy says so.  It is never sent again from here.
 */
static void
nvmf_bd_requeue(nvmf_bd_cmd_t *bc)
{
	nvmf_softc_t *sc = nvmf_mpath_path_sc(bc->bc_path);

	mutex_enter(&sc->lock);
	if (nvmf_io_policy(sc->state, B_FALSE,
	    nvmf_mpath_path_gone(bc->bc_path), nvmf_fail_disconnect) ==
	    NVMF_IO_HOLD) {
		list_insert_tail(&sc->held, bc);
		mutex_exit(&sc->lock);
		return;
	}
	mutex_exit(&sc->lock);

	nvmf_bd_done(bc, EIO);
}

static void
nvmf_bd_finish(nvmf_bd_cmd_t *bc)
{
	if (atomic_dec_uint_nv(&bc->bc_refs) != 0)
		return;

	if (bc->bc_aborted)
		nvmf_bd_requeue(bc);
	else if (bc->bc_io_error != 0)
		nvmf_bd_done(bc, bc->bc_io_error);
	else
		nvmf_bd_done(bc, bc->bc_cqe_error);
}

static void
nvmf_bd_cqe_complete(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_bd_cmd_t *bc = arg;

	if (nvmf_cqe_aborted(cqe))
		bc->bc_aborted = B_TRUE;
	else if (nvmf_cqe_failed(cqe))
		bc->bc_cqe_error = EIO;
	nvmf_bd_finish(bc);
}

static void
nvmf_bd_io_complete(void *arg, size_t xfered, int error)
{
	nvmf_bd_cmd_t *bc = arg;

	/* blkdev has no residual count, so a short transfer is an error. */
	if (error == 0 && xfered != bc->bc_data_len)
		error = EIO;
	bc->bc_io_error = error;
	nvmf_bd_finish(bc);
}

/*
 * Send a command, or queue it until the association returns.  Returns 0 if
 * the command was taken, in which case it may already have completed.
 * Otherwise the caller still owns bc.
 */
static int
nvmf_bd_dispatch(nvmf_bd_cmd_t *bc)
{
	nvmf_softc_t *sc = nvmf_mpath_path_sc(bc->bc_path);
	struct nvmf_host_qpair *qp;
	nvmf_request_t *req;
	nvmf_memdesc_t mem;
	int how;

	mutex_enter(&sc->lock);
	switch (nvmf_io_policy(sc->state, sc->io_enabled,
	    nvmf_mpath_path_gone(bc->bc_path), nvmf_fail_disconnect)) {
	case NVMF_IO_SUBMIT:
		break;
	case NVMF_IO_HOLD:
		list_insert_tail(&sc->held, bc);
		mutex_exit(&sc->lock);
		return (0);
	default:
		mutex_exit(&sc->lock);
		return (ENXIO);
	}
	qp = sc->io[CPU->cpu_seqid % sc->num_io_queues];
	sc->qp_users++;
	mutex_exit(&sc->lock);

	how = (bc->bc_xfer->x_flags & BD_XFER_POLL) != 0 ?
	    KM_NOSLEEP : KM_SLEEP;

	req = nvmf_allocate_request(qp, &bc->bc_sqe, nvmf_bd_cqe_complete, bc,
	    how);
	if (req == NULL) {
		nvmf_qp_rele(sc);
		return (how == KM_NOSLEEP ? ENOMEM : ENXIO);
	}

	bc->bc_io_error = 0;
	bc->bc_cqe_error = 0;
	bc->bc_aborted = B_FALSE;
	bc->bc_refs = 1;
	if (bc->bc_data_len != 0) {
		bc->bc_refs = 2;
		mem.nmd_type = NVMF_MEMDESC_VADDR;
		mem.nmd_len = bc->bc_data_len;
		mem.nmd_u.nmd_vaddr = bc->bc_data;
		VERIFY0(nvmf_capsule_append_data(req->nc, &mem,
		    bc->bc_data_len, bc->bc_write, nvmf_bd_io_complete, bc));
	}

	nvmf_submit_request(req);
	nvmf_qp_rele(sc);
	return (0);
}

/* Send the commands that waited for this association. */
void
nvmf_bd_resume(nvmf_softc_t *sc)
{
	nvmf_bd_cmd_t *bc;
	list_t list;
	int error;

	list_create(&list, sizeof (nvmf_bd_cmd_t),
	    offsetof(nvmf_bd_cmd_t, bc_link));

	mutex_enter(&sc->lock);
	list_move_tail(&list, &sc->held);
	mutex_exit(&sc->lock);

	while ((bc = list_remove_head(&list)) != NULL) {
		error = nvmf_bd_dispatch(bc);
		if (error != 0)
			nvmf_bd_done(bc, error);
	}
	list_destroy(&list);
}

/* Fail the waiting commands of one path, or of every path if path is NULL. */
static void
nvmf_bd_fail(nvmf_softc_t *sc, struct nvmf_path *path, int error)
{
	nvmf_bd_cmd_t *bc, *next;
	list_t list;

	list_create(&list, sizeof (nvmf_bd_cmd_t),
	    offsetof(nvmf_bd_cmd_t, bc_link));

	mutex_enter(&sc->lock);
	if (path != NULL)
		nvmf_mpath_path_set_gone(path);
	for (bc = list_head(&sc->held); bc != NULL; bc = next) {
		next = list_next(&sc->held, bc);
		if (path == NULL || bc->bc_path == path) {
			list_remove(&sc->held, bc);
			list_insert_tail(&list, bc);
		}
	}
	mutex_exit(&sc->lock);

	while ((bc = list_remove_head(&list)) != NULL)
		nvmf_bd_done(bc, error);
	list_destroy(&list);
}

void
nvmf_bd_fail_held(nvmf_softc_t *sc, int error)
{
	nvmf_bd_fail(sc, NULL, error);
}

void
nvmf_bd_path_gone(nvmf_softc_t *sc, struct nvmf_path *path)
{
	nvmf_bd_fail(sc, path, ENXIO);
}

/*
 * Start a command for a blkdev transfer.  A panic dump cannot be served, as
 * the transport does not poll.
 */
static int
nvmf_bd_alloc(struct nvmf_ns_head *head, bd_xfer_t *xfer, uint8_t opc,
    nvmf_bd_cmd_t **bcp)
{
	struct nvmf_path *path;
	nvmf_bd_cmd_t *bc;

	if (ddi_in_panic())
		return (EIO);

	path = nvmf_mpath_select(head);
	if (path == NULL)
		return (ENXIO);

	bc = kmem_zalloc(sizeof (*bc),
	    (xfer->x_flags & BD_XFER_POLL) != 0 ? KM_NOSLEEP : KM_SLEEP);
	if (bc == NULL) {
		nvmf_mpath_select_rele(path);
		return (ENOMEM);
	}
	bc->bc_xfer = xfer;
	bc->bc_path = path;
	bc->bc_sqe.sqe_opc = opc;
	bc->bc_sqe.sqe_nsid = nvmf_mpath_path_nsid(path);
	*bcp = bc;
	return (0);
}

static int
nvmf_bd_start(nvmf_bd_cmd_t *bc)
{
	int error;

	error = nvmf_bd_dispatch(bc);
	if (error != 0)
		nvmf_bd_free(bc);
	return (error);
}

static int
nvmf_bd_rw(struct nvmf_ns_head *head, bd_xfer_t *xfer, boolean_t is_write)
{
	nvmf_bd_cmd_t *bc;
	uint64_t lba = xfer->x_blkno;
	uint64_t nlb = xfer->x_nblks;
	uint64_t len;
	int error;

	/*
	 * The block size of a head never changes, so this is the length that
	 * blkdev mapped at x_kaddr.  READ and WRITE carry a zero-based 16-bit
	 * block count.
	 */
	if (nlb == 0 || nlb > 0x10000)
		return (EINVAL);
	len = nlb * nvmf_mpath_head_blksize(head);
	if (len > nvmf_mpath_head_maxxfer(head))
		return (EINVAL);

	error = nvmf_bd_alloc(head, xfer,
	    is_write ? NVME_OPC_NVM_WRITE : NVME_OPC_NVM_READ, &bc);
	if (error != 0)
		return (error);

	bc->bc_sqe.sqe_cdw10 = (uint32_t)lba;
	bc->bc_sqe.sqe_cdw11 = (uint32_t)(lba >> 32);
	bc->bc_sqe.sqe_cdw12 = (uint32_t)(nlb - 1);
	bc->bc_data = xfer->x_kaddr;
	bc->bc_data_len = (size_t)len;
	bc->bc_write = is_write;

	return (nvmf_bd_start(bc));
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
	nvmf_bd_cmd_t *bc;
	nvmf_softc_t *sc;
	boolean_t vwc;
	int error;

	error = nvmf_bd_alloc(arg, xfer, NVME_OPC_NVM_FLUSH, &bc);
	if (error != 0)
		return (error);

	sc = nvmf_mpath_path_sc(bc->bc_path);
	mutex_enter(&sc->lock);
	vwc = sc->vwc_present;
	mutex_exit(&sc->lock);

	/* With no volatile write cache there is nothing to flush. */
	if (!vwc) {
		nvmf_bd_done(bc, 0);
		return (0);
	}

	return (nvmf_bd_start(bc));
}

static int
nvmf_bd_free_space(void *arg, bd_xfer_t *xfer)
{
	struct nvmf_ns_head *head = arg;
	const dkioc_free_list_t *dfl = xfer->x_dfl;
	const dkioc_free_list_ext_t *exts;
	nvmf_bd_cmd_t *bc;
	nvmf_softc_t *sc;
	nvme_range_t *ranges;
	size_t ranges_len;
	uint32_t blksize = nvmf_mpath_head_blksize(head);
	boolean_t dsm;
	uint64_t i;
	int error;

	if (dfl == NULL || dfl->dfl_num_exts == 0 ||
	    dfl->dfl_num_exts > NVME_DSET_MGMT_MAX_RANGES)
		return (EINVAL);

	error = nvmf_bd_alloc(head, xfer, NVME_OPC_NVM_DSET_MGMT, &bc);
	if (error != 0)
		return (error);

	sc = nvmf_mpath_path_sc(bc->bc_path);
	mutex_enter(&sc->lock);
	dsm = sc->dsm_supported;
	mutex_exit(&sc->lock);
	if (!dsm) {
		nvmf_bd_free(bc);
		return (ENOTSUP);
	}

	ranges_len = dfl->dfl_num_exts * sizeof (nvme_range_t);
	ranges = kmem_zalloc(ranges_len,
	    (xfer->x_flags & BD_XFER_POLL) != 0 ? KM_NOSLEEP : KM_SLEEP);
	if (ranges == NULL) {
		nvmf_bd_free(bc);
		return (ENOMEM);
	}
	bc->bc_dsm = ranges;
	bc->bc_dsm_len = ranges_len;

	exts = dfl->dfl_exts;
	for (i = 0; i < dfl->dfl_num_exts; i++) {
		uint64_t nblks = exts[i].dfle_length / blksize;

		if (nblks > UINT32_MAX) {
			nvmf_bd_free(bc);
			return (EINVAL);
		}
		ranges[i].nr_lba =
		    (dfl->dfl_offset + exts[i].dfle_start) / blksize;
		ranges[i].nr_len = (uint32_t)nblks;
	}

	bc->bc_sqe.sqe_cdw10 = (uint32_t)(dfl->dfl_num_exts - 1) & 0xff;
	bc->bc_sqe.sqe_cdw11 = NVME_DSET_MGMT_ATTR_DEALLOCATE;
	bc->bc_data = ranges;
	bc->bc_data_len = ranges_len;
	bc->bc_write = B_TRUE;

	return (nvmf_bd_start(bc));
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

bd_handle_t
nvmf_bd_alloc_handle(struct nvmf_ns_head *head)
{
	return (bd_alloc_handle(head, &nvmf_bd_ops, NULL, KM_SLEEP));
}

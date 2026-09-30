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
 * sys/dev/nvmf/nvmf_transport_internal.h.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Interface between the transport-independent APIs in nvmf_transport.c and the
 * individual transports (TCP, RDMA).  The vtable (nvmf_transport_ops), queue
 * pair (nvmf_qpair), capsule (nvmf_capsule), and I/O request (nvmf_io_request)
 * structures are preserved field-for-field from the FreeBSD source.
 *
 * The single OS-specific substitution is the data-movement seam: FreeBSD uses
 * "struct memdesc" to describe the controller-side receive buffer and
 * "struct mbuf" to describe the controller-side send chain.  illumos has no
 * memdesc; nvmf_memdesc_t below is a minimal, equivalent descriptor and
 * "struct mbuf" becomes mblk_t.  Crucially, the vtable shape is kept IDENTICAL
 * so an RDMA transport can be slotted in without changing this contract.
 */

#ifndef	_NVMF_TRANSPORT_INTERNAL_H
#define	_NVMF_TRANSPORT_INTERNAL_H

#include <sys/types.h>
#include <sys/nvpair.h>
#include <sys/stream.h>
#include <sys/ddidmareq.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

/*
 * io/nvme/nvme_reg.h supplies the concrete generic 64-byte SQE (nvme_sqe_t) and
 * 16-byte CQE (nvme_cqe_t).  Transports live under io/nvmf and are permitted to
 * include the driver-private register layout that the public consumer header
 * (sys/nvme/nvmf_transport.h) only forward declares.
 */
#include "../nvme/nvme_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Port-local NVMe constants that exist in FreeBSD's <dev/nvme/nvme.h> but not
 * in the illumos public <sys/nvme.h>.
 *
 * NVME_PSDT_SGL is the PRP-or-SGL-for-Data-Transfer selector value meaning
 * "use SGLs".  The queue-entry bounds are the admin/IO submission queue size
 * limits used by nvmf_validate_qpair_nvlist().  Values are taken verbatim
 * from the FreeBSD source so the validation semantics match exactly.
 */
#ifndef	NVME_PSDT_SGL
#define	NVME_PSDT_SGL		0x1
#endif

/*
 * PSDT is bits 7:6 of an SQE's second byte; the sqe_psdt field of the
 * illumos nvme_sqe_t is bit 7 alone.
 */
#define	NVMF_SQE_PSDT(sqe)	(((const uint8_t *)(sqe))[1] >> 6)
#define	NVMF_SQE_SET_PSDT(sqe, v)	(((uint8_t *)(sqe))[1] = \
	(((uint8_t *)(sqe))[1] & 0x3f) | (uint8_t)((v) << 6))

#ifndef	NVMF_FABRICS_OPC
#define	NVMF_FABRICS_OPC	0x7f
#endif
#ifndef	NVME_MIN_ADMIN_ENTRIES
#define	NVME_MIN_ADMIN_ENTRIES	2
#endif
#ifndef	NVME_MAX_ADMIN_ENTRIES
#define	NVME_MAX_ADMIN_ENTRIES	4096
#endif
#ifndef	NVME_MIN_IO_ENTRIES
#define	NVME_MIN_IO_ENTRIES	2
#endif
#ifndef	NVME_MAX_IO_ENTRIES
#define	NVME_MAX_IO_ENTRIES	65536
#endif

struct nvmf_io_request;

/*
 * Memory descriptor for transport data buffers.
 *
 * FreeBSD's <sys/memdesc.h> "struct memdesc" is a tagged union over a kernel
 * VA, a bus_dma vlist/sglist, a struct mbuf, a uio, or a struct bio, used so a
 * transport can DMA or copy into/out of any of those backing stores.  illumos
 * has no single equivalent; the in-tree consumers of this transport (the
 * STMF-backed controller and the bd(9)-backed host) hand the transport either a
 * flat kernel buffer or an mblk chain.  nvmf_memdesc_t captures exactly those
 * two cases.  It is intentionally small and copyable by value, matching how
 * FreeBSD copies "struct memdesc" into nvmf_io_request and nvmf_capsule.
 */
typedef enum {
	NVMF_MEMDESC_VADDR = 1,		/* nmd_vaddr / nmd_len kernel buffer */
	NVMF_MEMDESC_MBLK,		/* nmd_mp mblk_t chain */
	NVMF_MEMDESC_SGL		/* nmd_sgl segment array */
} nvmf_memdesc_type_t;

/* One kernel buffer segment.  The layout matches stmf_sglist_ent_t. */
typedef struct nvmf_seg {
	uint32_t	nsg_len;
	uint8_t		*nsg_addr;
} nvmf_seg_t;

typedef struct nvmf_memdesc {
	nvmf_memdesc_type_t	nmd_type;
	size_t			nmd_len;
	union {
		void	*nmd_vaddr;
		mblk_t	*nmd_mp;
		struct {
			const nvmf_seg_t	*nmd_segs;
			uint_t			nmd_nsegs;
			/* DMA addresses of the segments, if bound. */
			const ddi_dma_cookie_t	*nmd_cookies;
			uint_t			nmd_ncookies;
		} nmd_sgl;
	} nmd_u;
} nvmf_memdesc_t;

/*
 * Copy len bytes at offset off into (copyin) or out of (copyout) a memdesc.
 * The range must lie within nmd_len; nothing outside nmd_len or the backing
 * store is touched.
 */
void	nvmf_memdesc_copyin(const nvmf_memdesc_t *md, size_t off,
    const void *src, size_t len);
void	nvmf_memdesc_copyout(const nvmf_memdesc_t *md, size_t off, void *dst,
    size_t len);

/*
 * SGL Data Block descriptors a transport can accept in SGL1: in-capsule data
 * at an offset (subtype 1), or a keyed remote buffer (type 4).
 */
#define	NVMF_SGL_DATA_BLOCK	0x0
#define	NVMF_SGL_KEYED_DATA_BLOCK	0x4
#define	NVMF_SGL_SUBTYPE_ADDRESS	0x0
#define	NVMF_SGL_SUBTYPE_OFFSET		0x1

typedef struct nvmf_sgl {
	boolean_t	nsl_keyed;	/* else in-capsule data */
	boolean_t	nsl_invalidate;	/* the host asks for SEND_WITH_INV */
	uint32_t	nsl_len;
	uint32_t	nsl_key;
	uint64_t	nsl_addr;	/* remote address, or capsule offset */
} nvmf_sgl_t;

uint8_t	nvmf_sgl_decode(const nvme_sqe_t *sqe, size_t icd_len,
    uint64_t max_len, nvmf_sgl_t *sgl);

/*
 * A data buffer that a transport made ready for DMA: from its pool, or LU
 * memory that it mapped.  It holds the transport, not the qpair, so it may
 * outlive the qpair.
 */
typedef struct nvmf_databuf {
	void			*ndb_addr;	/* pool buffers only */
	size_t			ndb_len;
	const ddi_dma_cookie_t	*ndb_cookies;
	uint_t			ndb_ncookies;
	void			*ndb_priv;	/* the transport's */
	struct nvmf_transport	*ndb_transport;	/* the core's */
} nvmf_databuf_t;

/*
 * Neither allocation sleeps.  A pool buffer has between min_len and len
 * bytes.  ENOTSUP means the transport has no such buffers.
 */
int	nvmf_alloc_data_buf(struct nvmf_qpair *qp, size_t len, size_t min_len,
    nvmf_databuf_t *db);
int	nvmf_map_data_buf(struct nvmf_qpair *qp, const nvmf_seg_t *segs,
    uint_t nsegs, nvmf_databuf_t *db);
void	nvmf_free_data_buf(nvmf_databuf_t *db);

/* Data for a controller-to-host transfer, as a transport receives it. */
struct nvmf_send_request {
	nvmf_memdesc_t		nsr_mem;
	size_t			nsr_len;
	nvmf_send_complete_t	*nsr_complete;
	void			*nsr_complete_arg;
};

struct nvmf_transport_ops {
	/* Queue pair management. */
	struct nvmf_qpair *(*allocate_qpair)(boolean_t controller,
	    const nvlist_t *nvl);
	void (*free_qpair)(struct nvmf_qpair *qp);

	/* Limit on I/O command capsule size. */
	uint32_t (*max_ioccsz)(struct nvmf_qpair *qp);

	/* Limit on transfer size. */
	uint64_t (*max_xfer_size)(struct nvmf_qpair *qp);

	/* Capsule operations. */
	struct nvmf_capsule *(*allocate_capsule)(struct nvmf_qpair *qp,
	    int how);
	void (*free_capsule)(struct nvmf_capsule *nc);
	int (*transmit_capsule)(struct nvmf_capsule *nc);
	uint8_t (*validate_command_capsule)(struct nvmf_capsule *nc);

	/* Transferring controller data. */
	size_t (*capsule_data_len)(const struct nvmf_capsule *nc);
	int (*receive_controller_data)(struct nvmf_capsule *nc,
	    uint32_t data_offset, struct nvmf_io_request *io);
	uint_t (*send_controller_data)(struct nvmf_capsule *nc,
	    uint32_t data_offset, mblk_t *mp, size_t len);

	/*
	 * Optional.  Send data and, if final_cqe is not NULL, the response
	 * after it; see nvmf_send_controller_data_io().  The transport copies
	 * *req and *final_cqe.  Without this op the core copies the data into
	 * an mblk and calls send_controller_data().
	 */
	int (*send_controller_data_io)(struct nvmf_capsule *nc,
	    uint32_t data_offset, const struct nvmf_send_request *req,
	    const nvme_cqe_t *final_cqe);

	/* Optional.  NVMF_QP_CAP_* for the qpair; fixed for its life. */
	uint32_t (*caps)(struct nvmf_qpair *qp);

	/*
	 * Optional: NVMF_QP_CAP_DATA_BUF needs alloc_data_buf, and
	 * NVMF_QP_CAP_LU_DBUF needs map_data_buf.  free_data_buf releases
	 * either kind and may run after free_qpair.
	 */
	int (*alloc_data_buf)(struct nvmf_qpair *qp, size_t len,
	    size_t min_len, nvmf_databuf_t *db);
	int (*map_data_buf)(struct nvmf_qpair *qp, const nvmf_seg_t *segs,
	    uint_t nsegs, nvmf_databuf_t *db);
	void (*free_data_buf)(nvmf_databuf_t *db);

	nvmf_trtype_t trtype;
	int priority;
};

/* Either an Admin or I/O Submission/Completion Queue pair. */
struct nvmf_qpair {
	struct nvmf_transport *nq_transport;
	struct nvmf_transport_ops *nq_ops;
	boolean_t nq_controller;

	/* Callback to invoke for a received capsule. */
	nvmf_capsule_receive_t *nq_receive;
	void *nq_receive_arg;

	/* Callback to invoke for an error. */
	nvmf_qpair_error_t *nq_error;
	void *nq_error_arg;

	boolean_t nq_admin;
};

struct nvmf_io_request {
	/*
	 * Data buffer contains io_len bytes in the backing store
	 * described by mem.
	 */
	nvmf_memdesc_t	io_mem;
	size_t	io_len;
	nvmf_io_complete_t *io_complete;
	void	*io_complete_arg;
};

#define	NVMF_CAPSULE_CONSUMER_WORDS	12

/*
 * Fabrics Command and Response Capsules.  The Fabrics host
 * (initiator) and controller (target) drivers work with capsules that
 * are transmitted and received by a specific transport.
 */
struct nvmf_capsule {
	struct nvmf_qpair *nc_qpair;

	/* Either a SQE or CQE. */
	union {
		nvme_sqe_t nc_sqe;
		nvme_cqe_t nc_cqe;
	};
	int	nc_qe_len;

	/*
	 * Is SQHD in received capsule valid?  B_FALSE for locally-
	 * synthesized responses.
	 */
	boolean_t	nc_sqhd_valid;

	boolean_t	nc_send_data;
	struct nvmf_io_request nc_data;

	/* The consumer answers this command after it frees the capsule. */
	boolean_t	nc_deferred;

	/* Per-command state of the consumer, so it need not allocate. */
	uint64_t	nc_consumer[NVMF_CAPSULE_CONSUMER_WORDS];
};

#define	NVMF_CAPSULE_CONSUMER(nc)	((void *)(nc)->nc_consumer)

/*
 * Register a qpair that the transport created in the kernel, as the handoff
 * path does for nvmf_allocate_qpair().  The callbacks are fixed for the life
 * of the qpair, and the transport must not deliver events before this returns
 * 0.  On success nvmf_free_qpair() releases the qpair; on failure the caller
 * still owns it.
 *
 * As with TCP, free_qpair must complete every transfer still registered and
 * return only after the last callback has returned, and a capsule keeps its
 * transport qpair valid until the capsule is freed.
 */
int	nvmf_adopt_qpair(struct nvmf_transport_ops *ops,
    struct nvmf_qpair *qp, boolean_t controller, boolean_t admin,
    nvmf_qpair_error_t *error_cb, void *error_cb_arg,
    nvmf_capsule_receive_t *receive_cb, void *receive_cb_arg);

static inline void
nvmf_qpair_error(struct nvmf_qpair *nq, int error)
{
	nq->nq_error(nq->nq_error_arg, error);
}

static inline void
nvmf_capsule_received(struct nvmf_qpair *nq, struct nvmf_capsule *nc)
{
	nq->nq_receive(nq->nq_receive_arg, nc);
}

static inline void
nvmf_complete_io_request(struct nvmf_io_request *io, size_t xfered, int error)
{
	io->io_complete(io->io_complete_arg, xfered, error);
}

#ifdef __cplusplus
}
#endif

#endif /* _NVMF_TRANSPORT_INTERNAL_H */

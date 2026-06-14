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
 * "use SGLs"; the illumos SQE carries it in the single sqe_psdt bit.  The
 * queue-entry bounds are the admin/IO submission queue size limits used by
 * nvmf_validate_qpair_nvlist().  Values are taken verbatim from the FreeBSD
 * source so the validation semantics match exactly.
 */
#ifndef	NVME_PSDT_SGL
#define	NVME_PSDT_SGL		0x1
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
	NVMF_MEMDESC_MBLK		/* nmd_mp mblk_t chain */
} nvmf_memdesc_type_t;

typedef struct nvmf_memdesc {
	nvmf_memdesc_type_t	nmd_type;
	size_t			nmd_len;
	union {
		void	*nmd_vaddr;
		mblk_t	*nmd_mp;
	} nmd_u;
} nvmf_memdesc_t;

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
};

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

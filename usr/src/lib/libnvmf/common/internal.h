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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/internal.h.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Library-private definitions: the transport provider vtable
 * (nvmf_transport_ops) and the transport-independent association, queue pair,
 * and capsule structures.  Field-for-field from the FreeBSD source; the only
 * substitutions are the NVMe SQE/CQE types and refcount glue:
 *
 *   FreeBSD                   illumos
 *   -------                   -------
 *   struct nvme_command       nvme_sqe_t (io/nvme/nvme_reg.h)
 *   struct nvme_completion     nvme_cqe_t (io/nvme/nvme_reg.h)
 *   refcount(9)               plain u_int under a single-threaded library
 *   nvlist_t (sys/_nv.h)      nvlist_t (libnvpair)
 *
 * Like FreeBSD's libnvmf, this library is not internally thread safe; a caller
 * that shares an association or qpair across threads must serialize access.
 * The association reference count is therefore a plain counter rather than the
 * atomic refcount(9) primitive used by the FreeBSD original.
 */

#ifndef	_LIBNVMF_INTERNAL_H
#define	_LIBNVMF_INTERNAL_H

#include <sys/queue.h>
#include <libnvpair.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

/*
 * The concrete generic 64-byte SQE (nvme_sqe_t), 16-byte CQE (nvme_cqe_t), the
 * on-wire SGL descriptor (nvme_sgl_t), and the admin opcodes live in the
 * driver-private register header.  The kernel transport includes it the same
 * way; the library build adds -I$(SRC)/uts/common/io/nvme so the in-capsule
 * SQE/CQE layout matches the kernel exactly.
 */
#include "nvme_reg.h"

/*
 * Worst-case time (CAP.TO) for CC.EN -> CSTS.RDY, in 500ms units.  (FreeBSD:
 * NVMF_CC_EN_TIMEOUT in sys/dev/nvmf/nvmf.h.)
 */
#define	NVMF_CC_EN_TIMEOUT	15

#include "libnvmf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queue-entry bounds present in FreeBSD's <dev/nvme/nvme.h> but not in the
 * illumos public <sys/nvme.h>.  Values are taken verbatim so the CONNECT
 * validation semantics match the kernel's nvmf_validate_qpair_nvlist().
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

/*
 * The Fabrics command opcode.  FreeBSD's <dev/nvme/nvme.h> calls this
 * NVME_OPC_FABRICS_COMMANDS; the illumos register header does not yet carry a
 * Fabrics opcode constant, so define it here.  (NVMe base spec: Fabrics
 * command opcode is 0x7f.)
 */
#ifndef	NVME_OPC_FABRICS_COMMANDS
#define	NVME_OPC_FABRICS_COMMANDS	0x7f
#endif

/*
 * NVMe-oF Discovery Log Page identifier (LID).  FreeBSD: NVME_LOG_DISCOVERY.
 */
#ifndef	NVMF_LOG_DISCOVERY
#define	NVMF_LOG_DISCOVERY		0x70
#endif

/*
 * "Number of Queues" feature identifier for SET_FEATURES (FreeBSD:
 * NVME_FEAT_NUMBER_OF_QUEUES).  illumos <sys/nvme.h> spells this
 * NVME_FEAT_NQUEUES.
 */
#ifndef	NVME_FEAT_NUMBER_OF_QUEUES
#define	NVME_FEAT_NUMBER_OF_QUEUES	NVME_FEAT_NQUEUES
#endif

/*
 * Identify Controller "I/O Controller" version constant (NVMe 1.4), used for
 * the discovery controller data.  FreeBSD encodes this with NVME_REV(1, 4).
 */
#ifndef	NVMF_NVME_REV_1_4
#define	NVMF_NVME_REV_1_4		0x00010400u
#endif

struct nvmf_transport_ops {
	/* Association management. */
	struct nvmf_association *(*allocate_association)(bool controller,
	    const nvmf_association_params_t *params);
	void (*update_association)(struct nvmf_association *na,
	    const nvme_identify_ctrl_t *cdata);
	void (*free_association)(struct nvmf_association *na);

	/* Queue pair management. */
	struct nvmf_qpair *(*allocate_qpair)(struct nvmf_association *na,
	    const nvmf_qpair_params_t *params);
	void (*free_qpair)(struct nvmf_qpair *qp);

	/* Add params for kernel handoff. */
	void (*kernel_handoff_params)(struct nvmf_qpair *qp, nvlist_t *nvl);
	int (*populate_dle)(struct nvmf_qpair *qp,
	    nvmf_discovery_log_page_entry_t *dle);

	/* Capsule operations. */
	struct nvmf_capsule *(*allocate_capsule)(struct nvmf_qpair *qp);
	void (*free_capsule)(struct nvmf_capsule *nc);
	int (*transmit_capsule)(struct nvmf_capsule *nc);
	int (*receive_capsule)(struct nvmf_qpair *qp,
	    struct nvmf_capsule **ncp);
	uint8_t (*validate_command_capsule)(const struct nvmf_capsule *nc);

	/* Transferring controller data. */
	size_t (*capsule_data_len)(const struct nvmf_capsule *nc);
	int (*receive_controller_data)(const struct nvmf_capsule *nc,
	    uint32_t data_offset, void *buf, size_t len);
	int (*send_controller_data)(const struct nvmf_capsule *nc,
	    const void *buf, size_t len);
};

struct nvmf_association {
	struct nvmf_transport_ops *na_ops;
	nvmf_trtype_t na_trtype;
	bool na_controller;

	nvmf_association_params_t na_params;

	/* Each qpair holds a reference on an association. */
	u_int na_refs;

	char *na_last_error;
};

struct nvmf_qpair {
	struct nvmf_association *nq_association;
	bool nq_admin;

	uint16_t nq_cid;	/* host only */

	/*
	 * Queue sizes.  This assumes the same size for both the
	 * completion and submission queues within a pair.
	 */
	u_int	nq_qsize;

	/* Flow control management for submission queues. */
	bool nq_flow_control;
	uint16_t nq_sqhd;
	uint16_t nq_sqtail;	/* host only */

	/* Value in response to/from CONNECT. */
	uint16_t nq_cntlid;

	uint32_t nq_kato;	/* valid on admin queue only */

	TAILQ_HEAD(, nvmf_capsule) nq_rx_capsules;
};

struct nvmf_capsule {
	struct nvmf_qpair *nc_qpair;

	/* Either a SQE or CQE. */
	union {
		nvme_sqe_t nc_sqe;
		nvme_cqe_t nc_cqe;
	};
	int	nc_qe_len;

	/*
	 * Is SQHD in received capsule valid?  False for locally-
	 * synthesized responses.
	 */
	bool	nc_sqhd_valid;

	/* Data buffer. */
	bool	nc_send_data;
	void	*nc_data;
	size_t	nc_data_len;

	TAILQ_ENTRY(nvmf_capsule) nc_link;
};

extern struct nvmf_transport_ops tcp_ops;

extern void na_clear_error(struct nvmf_association *na);
extern void na_error(struct nvmf_association *na, const char *fmt, ...);

extern int nvmf_kernel_handoff_params(struct nvmf_qpair *qp, nvlist_t **nvlp);
extern int nvmf_populate_dle(struct nvmf_qpair *qp,
    nvmf_discovery_log_page_entry_t *dle);
extern int nvmf_pack_ioc_nvlist(struct nvmf_ioc_nv *nv, nvlist_t *nvl);

/*
 * CRC-32C (Castagnoli) used by the NVMe/TCP header and data digests.  Backed by
 * the generic <sys/crc32.h> CRC32() macro over a CRC-32C table built on first
 * use (nvmf_crc32c.c).  This mirrors the kernel transport's nvmf_tcp_crc32c().
 */
extern uint32_t nvmf_crc32c(uint32_t crc, const void *buf, size_t len);

/*
 * Status-field helpers.  FreeBSD writes the 16-bit completion status as a
 * packed value through NVMEF(NVME_STATUS_SCT/SC, ...) and reads it with
 * NVMEV().  illumos splits the status field into the nvme_cqe_sf_t bit-field
 * (sf_sct/sf_sc) inside nvme_cqe_t.  These helpers bridge the two so the ported
 * controller/host code can keep its "uint16_t status" idioms: bits 1..8 are the
 * status code (SC) and bits 9..11 the status code type (SCT), matching the CQE
 * DW3 layout (phase bit 0 is excluded).
 */
#define	NVMF_SC_TYPE(sct, sc)	\
	((uint16_t)((((sc) & 0xff) << 1) | (((sct) & 0x7) << 9)))
#define	NVMF_SC_GET_SCT(status)	(((status) >> 9) & 0x7)
#define	NVMF_SC_GET_SC(status)	(((status) >> 1) & 0xff)

/*
 * Host-side kernel control ioctls (/dev/nvmf, /dev/nvmeX).  These keep the
 * FreeBSD ordinals (200-205); the matching kernel-side numbers live in the
 * driver-private io/nvmf/nvmf_core.h.  They are duplicated here (rather than
 * pulled from a shared header) because the Fabrics host kernel driver's ioctl
 * surface is still settling; see the PORT-TODO in nvmf_host.c.
 */
#ifndef	NVMF_IOC
#define	NVMF_IOC		(('n' << 8))
#endif
#define	NVMF_HANDOFF_HOST	(NVMF_IOC | 200)
#define	NVMF_DISCONNECT_HOST	(NVMF_IOC | 201)
#define	NVMF_DISCONNECT_ALL	(NVMF_IOC | 202)
#define	NVMF_RECONNECT_PARAMS	(NVMF_IOC | 203)
#define	NVMF_RECONNECT_HOST	(NVMF_IOC | 204)
#define	NVMF_CONNECTION_STATUS	(NVMF_IOC | 205)

/* Shared lower-level controller helpers (nvmf_subr.c). */
extern uint64_t _nvmf_controller_cap(uint32_t max_io_qsize,
    uint8_t enable_timeout);
extern bool _nvmf_validate_cc(uint32_t max_io_qsize, uint64_t cap,
    uint32_t old_cc, uint32_t new_cc);
extern void nvmf_strpad(char *dst, const char *src, size_t len);
extern void _nvmf_init_io_controller_data(uint16_t cntlid,
    uint32_t max_io_qsize, const char *serial, const char *model,
    const char *firmware_version, const char *subnqn, int nn, uint32_t ioccsz,
    uint32_t iorcsz, nvme_identify_ctrl_t *cdata);

#ifdef __cplusplus
}
#endif

#endif /* _LIBNVMF_INTERNAL_H */

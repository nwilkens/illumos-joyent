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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/nvmf_transport.h.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * This is the transport-independent API used by the NVMe over Fabrics host
 * (initiator) and controller (target) to allocate queue pairs and to send and
 * receive capsules and their associated data.  It is consumed both by the
 * Fabrics host/controller drivers (consumers) and is the contract that
 * individual transports (TCP, RDMA) implement through nvmf_transport_internal.h
 * (providers).  The function signatures and semantics are preserved exactly
 * from the FreeBSD source; only the OS-glue primitives differ:
 *
 *   FreeBSD                 illumos
 *   -------                 -------
 *   nvlist_t (sys/_nv.h)    nvlist_t (sys/nvpair.h)
 *   struct mbuf             mblk_t (the RDMA/data-movement seam)
 *   struct memdesc          nvmf_memdesc_t (see nvmf_transport_internal.h)
 *   struct nvme_command     nvmf_sqe_t
 *   struct nvme_completion  nvmf_cqe_t
 *   int how (M_WAITOK/...)  int kmflag (KM_SLEEP/KM_NOSLEEP)
 *
 * The mbuf->mblk_t substitution in nvmf_send_controller_data() and the
 * provider vtable is the RDMA seam: the data-movement shape is kept identical
 * so a future RDMA transport can be plugged in unchanged.
 */

#ifndef	_SYS_NVME_NVMF_TRANSPORT_H
#define	_SYS_NVME_NVMF_TRANSPORT_H

/*
 * Interface used by the Fabrics host (initiator) and controller
 * (target) to send and receive capsules and associated data.
 */

#include <sys/types.h>
#include <sys/nvpair.h>
#include <sys/stream.h>
#include <sys/nvme/nvmf.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvmf_capsule;
struct nvmf_qpair;
struct nvmf_ioc_nv;
struct nvmf_memdesc;

/*
 * The on-wire NVMe submission and completion queue entries used by the Fabrics
 * transport are FreeBSD's "struct nvme_command" (64 bytes) and
 * "struct nvme_completion" (16 bytes) from <dev/nvme/nvme.h>.  illumos keeps
 * the equivalent generic SQE and CQE layouts in the driver-private
 * io/nvme/nvme_reg.h as nvme_sqe_t/nvme_cqe_t.  The consumer API below passes
 * those entries through "const void *" (nvmf_allocate_command/response) and
 * returns them through "void *" (nvmf_capsule_sqe/cqe), so this header does not
 * need the concrete register layout; transport providers under io/nvmf include
 * nvme_reg.h directly via nvmf_transport_internal.h.
 */

/*
 * Callback to invoke when an error occurs on a qpair.  The last
 * parameter is an error value.  If the error value is zero, the qpair
 * has been closed at the transport level rather than a transport
 * error occuring.
 */
typedef void nvmf_qpair_error_t(void *, int);

/* Callback to invoke when a capsule is received. */
typedef void nvmf_capsule_receive_t(void *, struct nvmf_capsule *);

/*
 * Callback to invoke when an I/O request has completed.  The second
 * parameter is the amount of data transferred.  The last parameter is
 * an error value which is non-zero if the request did not complete
 * successfully.  A request with an error may complete partially.
 */
typedef void nvmf_io_complete_t(void *, size_t, int);

/*
 * A queue pair represents either an Admin or I/O
 * submission/completion queue pair.  The params contains negotiated
 * values passed in from userland.
 *
 * Unlike libnvmf in userland, the kernel transport interface does not
 * have any notion of an association.  Instead, qpairs are
 * independent.
 */
struct nvmf_qpair *nvmf_allocate_qpair(nvmf_trtype_t trtype,
    boolean_t controller, const nvlist_t *params,
    nvmf_qpair_error_t *error_cb, void *error_cb_arg,
    nvmf_capsule_receive_t *receive_cb, void *receive_cb_arg);
void	nvmf_free_qpair(struct nvmf_qpair *qp);

/*
 * Capsules are either commands (host -> controller) or responses
 * (controller -> host).  A data buffer may be associated with a
 * command capsule.  Transmitted data is not copied by this API but
 * instead must be preserved until the completion callback is invoked
 * to indicate capsule transmission has completed.
 */
struct nvmf_capsule *nvmf_allocate_command(struct nvmf_qpair *qp,
    const void *sqe, int how);
struct nvmf_capsule *nvmf_allocate_response(struct nvmf_qpair *qp,
    const void *cqe, int how);
void	nvmf_free_capsule(struct nvmf_capsule *nc);
int	nvmf_capsule_append_data(struct nvmf_capsule *nc,
    struct nvmf_memdesc *mem, size_t len, boolean_t send,
    nvmf_io_complete_t *complete_cb, void *cb_arg);
int	nvmf_transmit_capsule(struct nvmf_capsule *nc);
void	nvmf_abort_capsule_data(struct nvmf_capsule *nc, int error);
void	*nvmf_capsule_sqe(struct nvmf_capsule *nc);
void	*nvmf_capsule_cqe(struct nvmf_capsule *nc);
boolean_t nvmf_sqhd_valid(struct nvmf_capsule *nc);

/* Host-specific APIs. */

/*
 * Largest I/O request size for a single command supported by the
 * transport.  If the transport does not have a limit, returns 0.
 */
uint64_t nvmf_max_xfer_size(struct nvmf_qpair *qp);

/* Controller-specific APIs. */

/*
 * Largest I/O command capsule size (IOCCSZ) supported by the
 * transport.  If the transport does not have a limit, returns 0.
 */
uint32_t nvmf_max_ioccsz(struct nvmf_qpair *qp);

/*
 * A controller calls this function to check for any
 * transport-specific errors (invalid fields) in a received command
 * capsule.  The callback returns a generic command status value:
 * NVME_CQE_SC_GEN_SUCCESS if no error is found.
 */
uint8_t	nvmf_validate_command_capsule(struct nvmf_capsule *nc);

/*
 * A controller calls this function to query the amount of data
 * associated with a command capsule.
 */
size_t	nvmf_capsule_data_len(const struct nvmf_capsule *cc);

/*
 * A controller calls this function to receive data associated with a
 * command capsule (e.g. the data for a WRITE command).  This can
 * either return in-capsule data or fetch data from the host
 * (e.g. using a R2T PDU over TCP).  The received command capsule
 * should be passed in 'nc'.  The received data is stored in 'mem'.
 * If this function returns success, then the callback will be invoked
 * once the operation has completed.  Note that the callback might be
 * invoked before this function returns.
 */
int	nvmf_receive_controller_data(struct nvmf_capsule *nc,
    uint32_t data_offset, struct nvmf_memdesc *mem, size_t len,
    nvmf_io_complete_t *complete_cb, void *cb_arg);

/*
 * A controller calls this function to send data in response to a
 * command prior to sending a response capsule.  If an error occurs,
 * the function returns a generic status completion code to be sent in
 * the following CQE.  Note that the transfer might send a subset of
 * the data requested by nc.  If the transfer succeeds, this function
 * can return one of the following values:
 *
 * - NVME_CQE_SC_GEN_SUCCESS: The transfer has completed successfully
 *   and the caller should send a success CQE in a response capsule.
 *
 * - NVMF_SUCCESS_SENT: The transfer has completed successfully and
 *   the transport layer has sent an implicit success CQE to the
 *   remote host (e.g. the SUCCESS flag for TCP).  The caller should
 *   not send a response capsule.
 *
 * - NVMF_MORE: The transfer has completed successfully, but the
 *   transfer did not complete the data buffer.
 *
 * The mblk chain in 'mp' is consumed by this function even if an error
 * is returned.  (FreeBSD passes a struct mbuf chain here; mblk_t is the
 * illumos equivalent and is the RDMA/data-movement seam.)
 */
uint_t	nvmf_send_controller_data(struct nvmf_capsule *nc,
    uint32_t data_offset, mblk_t *mp, size_t len);

#define	NVMF_SUCCESS_SENT	0x100
#define	NVMF_MORE		0x101

/* Helper APIs for nvlists used in ioctls. */

/*
 * Pack the nvlist nvl and copyout to the buffer described by nv.
 */
int	nvmf_pack_ioc_nvlist(nvlist_t *nvl, struct nvmf_ioc_nv *nv);

/*
 * Copyin and unpack an nvlist described by nv.  The unpacked nvlist
 * is returned in *nvlp on success.
 */
int	nvmf_unpack_ioc_nvlist(const struct nvmf_ioc_nv *nv, nvlist_t **nvlp);

/*
 * Returns B_TRUE if a qpair handoff nvlist has all the required
 * transport-independent values.
 */
boolean_t nvmf_validate_qpair_nvlist(const nvlist_t *nvl, boolean_t controller);

/*
 * Transport registration interface.
 *
 * FreeBSD registers transports declaratively through the NVMF_TRANSPORT()
 * macro, which hangs a moduledata_t off the kernel's module system and routes
 * MOD_LOAD/MOD_UNLOAD/MOD_QUIESCE to nvmf_transport_module_handler().  illumos
 * does not have an equivalent per-subsystem module event, so a transport
 * provider (a misc module, e.g. nvmf_tcp) instead calls these registration
 * functions explicitly from its own _init()/_fini() entry points.  The vtable
 * (struct nvmf_transport_ops) is defined in nvmf_transport_internal.h and is
 * identical in shape to the FreeBSD version.
 */
struct nvmf_transport_ops;
int	nvmf_transport_register(struct nvmf_transport_ops *ops);
int	nvmf_transport_unregister(struct nvmf_transport_ops *ops);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NVME_NVMF_TRANSPORT_H */

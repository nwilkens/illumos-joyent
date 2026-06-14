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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/libnvmf.h.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * libnvmf is the userland NVMe over Fabrics library used by the target daemon
 * (nvmfd) and the host/discovery tools.  It provides three layers:
 *
 *   - a transport-independent association / queue-pair / capsule API,
 *   - a TCP transport (userland sockets + NVMe/TCP PDU framing + CRC32C
 *     digests), and
 *   - controller-side (target) and host-side (initiator) Fabrics helpers.
 *
 * The public API keeps the shape of FreeBSD's libnvmf.h so the daemon ports
 * with minimal change.  The OS-glue substitutions are:
 *
 *   FreeBSD                              illumos
 *   -------                              -------
 *   enum nvmf_trtype                     nvmf_trtype_t (<sys/nvme/nvmf.h>)
 *   struct nvme_controller_data          nvme_identify_ctrl_t (<sys/nvme.h>)
 *   struct nvme_namespace_data           nvme_identify_nsid_t (<sys/nvme.h>)
 *   struct nvme_command                  void * (in-capsule SQE; nvme_sqe_t)
 *   struct nvme_completion               void * (in-capsule CQE; nvme_cqe_t)
 *   struct nvmf_fabric_connect_cmd       nvmf_fabric_connect_cmd_t
 *   struct nvmf_fabric_connect_data      nvmf_fabric_connect_data_t
 *   struct nvme_discovery_log[_entry]    nvmf_discovery_log_page[_entry]_t
 *   struct nvmf_ioc_nv                   struct nvmf_ioc_nv (<sys/nvme/nvmf_ioctl.h>)
 *   nvlist_t (sys/_nv.h)                 nvlist_t (libnvpair)
 */

#ifndef	_LIBNVMF_H
#define	_LIBNVMF_H

#include <sys/types.h>
#include <sys/uio.h>
#include <stdbool.h>
#include <stddef.h>
#include <libnvpair.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvmf_capsule;
struct nvmf_association;
struct nvmf_qpair;

/*
 * Parameters shared by all queue-pairs of an association.  Note that
 * this contains the requested values used to initiate transport
 * negotiation.
 */
typedef struct nvmf_association_params {
	bool nap_sq_flow_control;	/* SQ flow control required. */
	bool nap_dynamic_controller_model;	/* Controller only */
	uint16_t nap_max_admin_qsize;	/* Controller only */
	uint32_t nap_max_io_qsize;	/* Controller only, 0 for discovery */
	union {
		struct {
			uint8_t pda;		/* Tx-side PDA. */
			bool header_digests;
			bool data_digests;
			uint32_t maxr2t;	/* Host only */
			uint32_t maxh2cdata;	/* Controller only */
		} nap_tcp;
	};
} nvmf_association_params_t;

/* Parameters specific to a single queue pair of an association. */
typedef struct nvmf_qpair_params {
	bool nqp_admin;			/* Host only */
	union {
		struct {
			int fd;
		} nqp_tcp;
	};
} nvmf_qpair_params_t;

/* Transport-independent APIs. */

/*
 * A host should allocate a new association for each association with
 * a controller.  After the admin queue has been allocated and the
 * controller's data has been fetched, it should be passed to
 * nvmf_update_association to update internal transport-specific
 * parameters before allocating I/O queues.
 *
 * A controller uses a single association to manage all incoming
 * queues since it is not known until after parsing the CONNECT
 * command which transport queues are admin vs I/O and which
 * controller they are created against.
 */
extern struct nvmf_association *nvmf_allocate_association(nvmf_trtype_t trtype,
    bool controller, const nvmf_association_params_t *params);
extern void nvmf_update_association(struct nvmf_association *na,
    const nvme_identify_ctrl_t *cdata);
extern void nvmf_free_association(struct nvmf_association *na);

/* The most recent association-wide error message. */
extern const char *nvmf_association_error(const struct nvmf_association *na);

/*
 * A queue pair represents either an Admin or I/O
 * submission/completion queue pair.
 *
 * Each open qpair holds a reference on its association.  Once queue
 * pairs are allocated, callers can safely free the association to
 * ease bookkeeping.
 *
 * If nvmf_allocate_qpair fails, a detailed error message can be obtained
 * from nvmf_association_error.
 */
extern struct nvmf_qpair *nvmf_allocate_qpair(struct nvmf_association *na,
    const nvmf_qpair_params_t *params);
extern void nvmf_free_qpair(struct nvmf_qpair *qp);

/*
 * Capsules are either commands (host -> controller) or responses
 * (controller -> host).  A single data buffer segment may be
 * associated with a command capsule.  Transmitted data is not copied
 * by this API but instead must be preserved until the capsule is
 * transmitted and freed.
 *
 * sqe points at a 64-byte NVMe submission queue entry (nvme_sqe_t) and
 * cqe at a 16-byte completion queue entry (nvme_cqe_t).
 */
extern struct nvmf_capsule *nvmf_allocate_command(struct nvmf_qpair *qp,
    const void *sqe);
extern struct nvmf_capsule *nvmf_allocate_response(struct nvmf_qpair *qp,
    const void *cqe);
extern void nvmf_free_capsule(struct nvmf_capsule *nc);
extern int nvmf_capsule_append_data(struct nvmf_capsule *nc,
    void *buf, size_t len, bool send);
extern int nvmf_transmit_capsule(struct nvmf_capsule *nc);
extern int nvmf_receive_capsule(struct nvmf_qpair *qp,
    struct nvmf_capsule **ncp);
extern const void *nvmf_capsule_sqe(const struct nvmf_capsule *nc);
extern const void *nvmf_capsule_cqe(const struct nvmf_capsule *nc);

/* Return a string name for a transport type. */
extern const char *nvmf_transport_type(uint8_t trtype);

/*
 * Validate a NVMe Qualified Name.  The second version enforces
 * stricter checks inline with the specification.  The first version
 * enforces more minimal checks.
 */
extern bool nvmf_nqn_valid(const char *nqn);
extern bool nvmf_nqn_valid_strict(const char *nqn);

/* Controller-specific APIs. */

/*
 * A controller calls this function to check for any
 * transport-specific errors (invalid fields) in a received command
 * capsule.  The callback returns a generic command status value:
 * NVME_CQE_SC_GEN_SUCCESS if no error is found.
 */
extern uint8_t nvmf_validate_command_capsule(const struct nvmf_capsule *nc);

/*
 * A controller calls this function to query the amount of data
 * associated with a command capsule.
 */
extern size_t nvmf_capsule_data_len(const struct nvmf_capsule *cc);

/*
 * A controller calls this function to receive data associated with a
 * command capsule (e.g. the data for a WRITE command).  This can
 * either return in-capsule data or fetch data from the host
 * (e.g. using a R2T PDU over TCP).  The received command capsule
 * should be passed in 'nc'.  The received data is stored in '*buf'.
 */
extern int nvmf_receive_controller_data(const struct nvmf_capsule *nc,
    uint32_t data_offset, void *buf, size_t len);

/*
 * A controller calls this function to send data in response to a
 * command along with a response capsule.  If the data transfer
 * succeeds, a success response is sent.  If the data transfer fails,
 * an appropriate error status capsule is sent.  Regardless, a
 * response capsule is always sent.
 */
extern int nvmf_send_controller_data(const struct nvmf_capsule *nc,
    const void *buf, size_t len);

/*
 * Construct a CQE for a reply to a command capsule in 'nc' with the
 * completion status 'status'.  This is useful when additional CQE
 * info is required beyond the completion status.
 */
extern void nvmf_init_cqe(void *cqe, const struct nvmf_capsule *nc,
    uint16_t status);

/*
 * Construct and send a response capsule to a command capsule with
 * the supplied CQE.
 */
extern int nvmf_send_response(const struct nvmf_capsule *nc, const void *cqe);

/*
 * Wait for a single command capsule and return it in *ncp.  This can
 * fail if an invalid capsule is received or an I/O error occurs.
 */
extern int nvmf_controller_receive_capsule(struct nvmf_qpair *qp,
    struct nvmf_capsule **ncp);

/* Send a response capsule from a controller. */
extern int nvmf_controller_transmit_response(struct nvmf_capsule *nc);

/* Construct and send an error response capsule. */
extern int nvmf_send_error(const struct nvmf_capsule *cc, uint8_t sc_type,
    uint8_t sc_status);

/*
 * Construct and send an error response capsule using a generic status
 * code.
 */
extern int nvmf_send_generic_error(const struct nvmf_capsule *nc,
    uint8_t sc_status);

/* Construct and send a simple success response capsule. */
extern int nvmf_send_success(const struct nvmf_capsule *nc);

/*
 * Allocate a new queue pair and wait for the CONNECT command capsule.
 * If this fails, a detailed error message can be obtained from
 * nvmf_association_error.  On success, the command capsule is saved
 * in '*ccp' and the connect data is saved in 'data'.  The caller
 * must send an explicit response and free the the command capsule.
 */
extern struct nvmf_qpair *nvmf_accept(struct nvmf_association *na,
    const nvmf_qpair_params_t *params, struct nvmf_capsule **ccp,
    nvmf_fabric_connect_data_t *data);

/*
 * Construct and send a response capsule with the Fabrics CONNECT
 * invalid parameters error status.  If data is true the offset is
 * relative to the CONNECT data structure, otherwise the offset is
 * relative to the SQE.
 */
extern void nvmf_connect_invalid_parameters(const struct nvmf_capsule *cc,
    bool data, uint16_t offset);

/* Construct and send a response capsule for a successful CONNECT. */
extern int nvmf_finish_accept(const struct nvmf_capsule *cc, uint16_t cntlid);

/* Compute the initial state of CAP for a controller. */
extern uint64_t nvmf_controller_cap(struct nvmf_qpair *qp);

/* Generate a serial number string from a host ID. */
extern void nvmf_controller_serial(char *buf, size_t len, ulong_t hostid);

/*
 * Populate an Identify Controller data structure for a Discovery
 * controller.
 */
extern void nvmf_init_discovery_controller_data(struct nvmf_qpair *qp,
    nvme_identify_ctrl_t *cdata);

/*
 * Populate an Identify Controller data structure for an I/O
 * controller.
 */
extern void nvmf_init_io_controller_data(struct nvmf_qpair *qp,
    const char *serial, const char *subnqn, int nn, uint32_t ioccsz,
    nvme_identify_ctrl_t *cdata);

/*
 * Validate if a new value for CC is legal given the existing values of
 * CAP and CC.
 */
extern bool nvmf_validate_cc(struct nvmf_qpair *qp, uint64_t cap,
    uint32_t old_cc, uint32_t new_cc);

/* Return the log page id (LID) of a GET_LOG_PAGE command. */
extern uint8_t nvmf_get_log_page_id(const void *sqe);

/* Return the requested data length of a GET_LOG_PAGE command. */
extern uint64_t nvmf_get_log_page_length(const void *sqe);

/* Return the requested data offset of a GET_LOG_PAGE command. */
extern uint64_t nvmf_get_log_page_offset(const void *sqe);

/* Prepare to handoff a controller qpair. */
extern int nvmf_handoff_controller_qpair(struct nvmf_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data, struct nvmf_ioc_nv *nv);

/* Host-specific APIs. */

/*
 * Connect to an admin or I/O queue.  If this fails, a detailed error
 * message can be obtained from nvmf_association_error.
 */
extern struct nvmf_qpair *nvmf_connect(struct nvmf_association *na,
    const nvmf_qpair_params_t *params, uint16_t qid, u_int queue_size,
    const uint8_t hostid[16], uint16_t cntlid, const char *subnqn,
    const char *hostnqn, uint32_t kato);

/* Return the CNTLID for a queue returned from CONNECT. */
extern uint16_t nvmf_cntlid(struct nvmf_qpair *qp);

/*
 * Send a command to the controller.  This can fail with EBUSY if the
 * submission queue is full.
 */
extern int nvmf_host_transmit_command(struct nvmf_capsule *nc);

/*
 * Wait for a response to a command.  If there are no outstanding
 * commands in the SQ, fails with EWOULDBLOCK.
 */
extern int nvmf_host_receive_response(struct nvmf_qpair *qp,
    struct nvmf_capsule **rcp);

/*
 * Wait for a response to a specific command.  The command must have been
 * succesfully sent previously.
 */
extern int nvmf_host_wait_for_response(struct nvmf_capsule *cc,
    struct nvmf_capsule **rcp);

/* Build a KeepAlive command. */
extern struct nvmf_capsule *nvmf_keepalive(struct nvmf_qpair *qp);

/* Read a controller property. */
extern int nvmf_read_property(struct nvmf_qpair *qp, uint32_t offset,
    uint8_t size, uint64_t *value);

/* Write a controller property. */
extern int nvmf_write_property(struct nvmf_qpair *qp, uint32_t offset,
    uint8_t size, uint64_t value);

/* Construct a 16-byte HostId from the system host uuid. */
extern int nvmf_hostid_from_hostuuid(uint8_t hostid[16]);

/* Construct a NQN from the system host uuid. */
extern int nvmf_nqn_from_hostuuid(char nqn[NVMF_NQN_MAX_LEN]);

/* Fetch controller data via IDENTIFY. */
extern int nvmf_host_identify_controller(struct nvmf_qpair *qp,
    nvme_identify_ctrl_t *data);

/* Fetch namespace data via IDENTIFY. */
extern int nvmf_host_identify_namespace(struct nvmf_qpair *qp, uint32_t nsid,
    nvme_identify_nsid_t *nsdata);

/*
 * Fetch discovery log page.  The memory for the log page is allocated
 * by malloc() and returned in *logp.  The caller must free the
 * memory.
 */
extern int nvmf_host_fetch_discovery_log_page(struct nvmf_qpair *qp,
    nvmf_discovery_log_page_t **logp);

/*
 * Construct a discovery log page entry that describes the connection
 * used by a host association's admin queue pair.
 */
extern int nvmf_init_dle_from_admin_qp(struct nvmf_qpair *qp,
    const nvme_identify_ctrl_t *cdata,
    nvmf_discovery_log_page_entry_t *dle);

/*
 * Request a desired number of I/O queues via SET_FEATURES.  The
 * number of actual I/O queues available is returned in *actual on
 * success.
 */
extern int nvmf_host_request_queues(struct nvmf_qpair *qp, u_int requested,
    u_int *actual);

/*
 * Handoff active host association to the kernel.  This frees the
 * qpairs (even on error).
 */
extern int nvmf_handoff_host(const nvmf_discovery_log_page_entry_t *dle,
    const char *hostnqn, struct nvmf_qpair *admin_qp, u_int num_queues,
    struct nvmf_qpair **io_queues, const nvme_identify_ctrl_t *cdata,
    uint32_t reconnect_delay, uint32_t controller_loss_timeout);

/*
 * Disconnect an active host association previously handed off to the
 * kernel.  *name is either the name of the device (nvmeX) for this
 * association or the remote subsystem NQN.
 */
extern int nvmf_disconnect_host(const char *host);

/*
 * Disconnect all active host associations previously handed off to
 * the kernel.
 */
extern int nvmf_disconnect_all(void);

/*
 * Fetch reconnect parameters from an existing kernel host to use for
 * establishing a new association.  The caller must destroy the
 * returned nvlist.
 */
extern int nvmf_reconnect_params(int fd, nvlist_t **nvlp);

/*
 * Handoff active host association to an existing host in the kernel.
 * This frees the qpairs (even on error).
 */
extern int nvmf_reconnect_host(int fd,
    const nvmf_discovery_log_page_entry_t *dle, const char *hostnqn,
    struct nvmf_qpair *admin_qp, u_int num_queues,
    struct nvmf_qpair **io_queues, const nvme_identify_ctrl_t *cdata,
    uint32_t reconnect_delay, uint32_t controller_loss_timeout);

/*
 * Fetch connection status from an existing kernel host.
 */
extern int nvmf_connection_status(int fd, nvlist_t **nvlp);

#ifdef __cplusplus
}
#endif

#endif /* _LIBNVMF_H */

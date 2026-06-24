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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/host/nvmf_var.h.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Private state for the NVMe over Fabrics host (initiator).  The
 * transport-independent protocol state (softc, qpair, request, completion
 * status) is preserved field-for-field from the FreeBSD source; only the OS
 * glue differs:
 *
 *   FreeBSD                 illumos
 *   -------                 -------
 *   device_t dev            dev_info_t *dip
 *   struct cam_sim / path   bd_handle_t (blkdev) via the head/path model
 *   struct mtx              kmutex_t
 *   struct sx               krwlock_t
 *   struct callout          (k)timeout_id / cyclic (TODO)
 *   struct task / taskqueue ddi_taskq_t
 *   eventhandler_tag        (no equivalent; quiesce(9E)/detach paths)
 *   M_WAITOK / M_NOWAIT     KM_SLEEP / KM_NOSLEEP
 *
 * The largest structural divergence is the replacement of FreeBSD's CAM SIM
 * (nvmf_sim.c) with an illumos blkdev binding plus a native multipath layer
 * (NVMEOF.md sections 7.3 and 9.3).  Where FreeBSD presents one cam_sim per
 * association and lets nvme_xpt enumerate namespaces, illumos presents one
 * blkdev disk per *namespace head* (keyed by NGUID/EUI64/UUID), and each head
 * owns a set of paths.  An association (this softc) contributes one path per
 * head.  Keep-alive death marks paths down rather than tearing down the disk.
 */

#ifndef	_NVMF_VAR_H
#define	_NVMF_VAR_H

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/cpuvar.h>
#include <sys/blkdev.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "../nvme/nvme_reg.h"
#include "../nvmf/nvmf_core.h"
#include "../nvmf/nvmf_transport_internal.h"	/* nvmf_memdesc_t */

#ifdef __cplusplus
extern "C" {
#endif

struct nvmf_aer;
struct nvmf_capsule;
struct nvmf_host_qpair;
struct nvmf_namespace;
struct nvmf_ns_head;
struct nvmf_path;

/*
 * (FreeBSD: nvmf_request_complete_t) Per-request completion callback.  The
 * FreeBSD type takes "const struct nvme_completion *"; the illumos generic CQE
 * is nvme_cqe_t.
 */
typedef void nvmf_request_complete_t(void *, const nvme_cqe_t *);

/*
 * (FreeBSD: struct nvmf_softc) One Fabrics association == one controller
 * attachment.  In the illumos multipath model this softc is a *path provider*:
 * it contributes one nvmf_path to each namespace head it can reach.
 */
typedef struct nvmf_softc {
	dev_info_t		*dip;		/* FreeBSD: device_t dev */

	struct nvmf_host_qpair	*admin;
	struct nvmf_host_qpair	**io;
	uint_t			num_io_queues;
	nvmf_trtype_t		trtype;

	/*
	 * FreeBSD keeps cam_sim/cam_path/sim_mtx here.  illumos replaces them
	 * with the head/path multipath layer (nvmf_mpath.c); this softc holds
	 * the list of paths it currently provides.  The path selector consults
	 * per-path state (nvmf_path_t path_down), not a softc-wide flag.
	 */
	kmutex_t		mpath_mtx;
	list_t			paths;		/* nvmf_path_t by this assoc */

	struct nvmf_namespace	**ns;

	nvme_identify_ctrl_t	*cdata;
	uint64_t		cap;
	uint32_t		vs;
	uint_t			max_pending_io;
	uint64_t		max_xfer_size;

	/*
	 * Keep Alive support.  A tx timer sends KeepAlive at half the timeout
	 * and an rx timer detects an actual timeout; ka_active_*_traffic are
	 * touched with atomics.  FreeBSD's callout becomes a timeout(9F) id
	 * here; the scheduling lives in nvmf_host.c (nvmf_arm_ka_timers).
	 */
	boolean_t		ka_traffic;	/* Using TKAS? */
	volatile uint_t		ka_active_tx_traffic;
	volatile uint_t		ka_active_rx_traffic;
	hrtime_t		ka_rx_interval;	/* FreeBSD: ka_rx_sbt */
	hrtime_t		ka_tx_interval;	/* FreeBSD: ka_tx_sbt */
	timeout_id_t		ka_rx_timer;
	timeout_id_t		ka_tx_timer;

	uint32_t		reconnect_delay;
	uint32_t		controller_loss_timeout;

	/*
	 * Disconnect / reconnect / controller-loss state machine.  FreeBSD
	 * uses TIMEOUT_TASK on nvmf_tq for the reconnect-request and
	 * controller-loss tasks; illumos uses timeout(9F) ids cancelled under
	 * connection_lock.  cdev_attached replaces FreeBSD's sc->cdev != NULL
	 * check (gates whether namespaces/blkdev have been brought up and may
	 * be torn down).
	 */
	timeout_id_t		reconnect_timer;
	timeout_id_t		loss_timer;
	boolean_t		cdev_attached;

	/*
	 * FreeBSD: sx connection_lock + task disconnect_task + timeout tasks.
	 * illumos: krwlock + a per-driver taskq (nvmf_tq) shared across softcs.
	 */
	krwlock_t		connection_lock;
	boolean_t		detaching;
	boolean_t		controller_timedout;

	uint_t			num_aer;
	struct nvmf_aer		*aer;

	nvlist_t		*rparams;	/* reconnect parameters */

	timespec_t		last_disconnect;
} nvmf_softc_t;

/*
 * (FreeBSD: struct nvmf_request) An in-flight (or pending) command on a host
 * qpair, with its completion callback.
 */
typedef struct nvmf_request {
	struct nvmf_host_qpair	*qp;
	struct nvmf_capsule	*nc;
	nvmf_request_complete_t	*cb;
	void			*cb_arg;
	boolean_t		aer;

	list_node_t		link;		/* FreeBSD: STAILQ_ENTRY */
} nvmf_request_t;

/*
 * (FreeBSD: struct nvmf_completion_status) Synchronous command helper: a CQE
 * plus done/io_done flags poked by the completion callbacks and waited on by
 * nvmf_wait_for_reply().
 */
typedef struct nvmf_completion_status {
	nvme_cqe_t		cqe;
	boolean_t		done;
	boolean_t		io_done;
	int			io_error;
} nvmf_completion_status_t;

/*
 * (FreeBSD: nvmf_select_io_queue) Pick an I/O qpair for a command.  FreeBSD
 * indexes by curcpu; illumos uses CPU() / cpu_seqid against num_io_queues.
 */
static inline struct nvmf_host_qpair *
nvmf_select_io_queue(nvmf_softc_t *sc)
{
	uint_t idx;

	/*
	 * nvmf_disconnect_task zeroes num_io_queues (and frees sc->io) under
	 * connection_lock as a writer while the softc lives.  A caller racing
	 * that teardown must see no queue rather than divide by zero or index a
	 * freed array; return NULL and let the caller fail the I/O with ENXIO.
	 */
	if (sc->io == NULL || sc->num_io_queues == 0)
		return (NULL);
	idx = (uint_t)(CPU->cpu_seqid) % sc->num_io_queues;
	return (sc->io[idx]);
}

/*
 * (FreeBSD: nvmf_cqe_aborted) True if a CQE indicates the command was aborted
 * by the host due to a path/association teardown (Path Related status, Command
 * Aborted By Host).  Used by both the blkdev binding and multipath to decide
 * whether to retry on another path.
 */
static inline boolean_t
nvmf_cqe_aborted(const nvme_cqe_t *cqe)
{
	return (cqe->cqe_sf.sf_sct == NVME_CQE_SCT_PATH &&
	    cqe->cqe_sf.sf_sc == NVME_CQE_SC_PATH_HOST_ABRT);
}

static inline void
nvmf_status_init(nvmf_completion_status_t *status)
{
	status->done = B_FALSE;
	status->io_done = B_TRUE;
	status->io_error = 0;
}

static inline void
nvmf_status_wait_io(nvmf_completion_status_t *status)
{
	status->io_done = B_FALSE;
}

/* If B_TRUE, I/O requests fail while the host is disconnected. */
extern boolean_t nvmf_fail_disconnect;

/* nvmf_host.c */
extern void nvmf_complete(void *arg, const nvme_cqe_t *cqe);
extern void nvmf_io_complete(void *arg, size_t xfered, int error);
extern void nvmf_wait_for_reply(nvmf_completion_status_t *status);
extern int nvmf_copyin_handoff(const struct nvmf_ioc_nv *nv, nvlist_t **nvlp);
extern void nvmf_disconnect(nvmf_softc_t *sc);
extern void nvmf_rescan_ns(nvmf_softc_t *sc, uint32_t nsid);
extern void nvmf_rescan_all_ns(nvmf_softc_t *sc);
extern ddi_taskq_t *nvmf_aer_taskq(nvmf_softc_t *sc);

/* nvmf_aer.c */
extern void nvmf_init_aer(nvmf_softc_t *sc);
extern int nvmf_start_aer(nvmf_softc_t *sc);
extern void nvmf_destroy_aer(nvmf_softc_t *sc);

/* nvmf_cmd.c */
extern boolean_t nvmf_cmd_get_property(nvmf_softc_t *sc, uint32_t offset,
    uint8_t size, nvmf_request_complete_t *cb, void *cb_arg, int how);
extern boolean_t nvmf_cmd_set_property(nvmf_softc_t *sc, uint32_t offset,
    uint8_t size, uint64_t value, nvmf_request_complete_t *cb, void *cb_arg,
    int how);
extern boolean_t nvmf_cmd_keep_alive(nvmf_softc_t *sc,
    nvmf_request_complete_t *cb, void *cb_arg, int how);
extern boolean_t nvmf_cmd_identify_active_namespaces(nvmf_softc_t *sc,
    uint32_t id, nvme_identify_nsid_list_t *nslist,
    nvmf_request_complete_t *req_cb, void *req_cb_arg,
    nvmf_io_complete_t *io_cb, void *io_cb_arg, int how);
extern boolean_t nvmf_cmd_identify_namespace(nvmf_softc_t *sc, uint32_t id,
    nvme_identify_nsid_t *nsdata, nvmf_request_complete_t *req_cb,
    void *req_cb_arg, nvmf_io_complete_t *io_cb, void *io_cb_arg, int how);
extern boolean_t nvmf_cmd_get_log_page(nvmf_softc_t *sc, uint32_t nsid,
    uint8_t lid, uint64_t offset, void *buf, size_t len,
    nvmf_request_complete_t *req_cb, void *req_cb_arg,
    nvmf_io_complete_t *io_cb, void *io_cb_arg, int how);

/* nvmf_ns.c */
extern boolean_t nvmf_ns_fmt(const nvme_identify_nsid_t *data, uint64_t *nblksp,
    uint32_t *blksizep, dev_info_t *dip, uint32_t nsid);
extern struct nvmf_namespace *nvmf_init_ns(nvmf_softc_t *sc, uint32_t id,
    const nvme_identify_nsid_t *data);
extern void nvmf_disconnect_ns(struct nvmf_namespace *ns);
extern void nvmf_reconnect_ns(struct nvmf_namespace *ns);
extern void nvmf_shutdown_ns(struct nvmf_namespace *ns);
extern void nvmf_destroy_ns(struct nvmf_namespace *ns);
extern boolean_t nvmf_update_ns(struct nvmf_namespace *ns,
    const nvme_identify_nsid_t *data);
extern void nvmf_ns_get_info(struct nvmf_namespace *ns, uint32_t *nsidp,
    uint64_t *sizep, uint32_t *blksizep, uint8_t *nguid, uint8_t *eui64,
    boolean_t *connectedp);

/* nvmf_qpair.c */
extern struct nvmf_host_qpair *nvmf_init_qp(nvmf_softc_t *sc,
    nvmf_trtype_t trtype, const nvlist_t *nvl, const char *name, uint_t qid);
extern void nvmf_shutdown_qp(struct nvmf_host_qpair *qp);
extern void nvmf_destroy_qp(struct nvmf_host_qpair *qp);
extern uint64_t nvmf_max_xfer_size_qp(struct nvmf_host_qpair *qp);
extern nvmf_request_t *nvmf_allocate_request(struct nvmf_host_qpair *qp,
    void *sqe, nvmf_request_complete_t *cb, void *cb_arg, int how);
extern void nvmf_submit_request(nvmf_request_t *req);
extern void nvmf_free_request(nvmf_request_t *req);

/*
 * nvmf_blkdev.c (NEW - replaces FreeBSD nvmf_sim.c).  Per-association binding
 * to the blkdev framework via the namespace-head abstraction.  These mirror
 * the lifecycle entry points the FreeBSD core (nvmf.c) calls on the SIM.
 */
extern int nvmf_init_bd(nvmf_softc_t *sc);
extern void nvmf_disconnect_bd(nvmf_softc_t *sc);
extern void nvmf_reconnect_bd(nvmf_softc_t *sc);
extern void nvmf_shutdown_bd(nvmf_softc_t *sc);
extern void nvmf_destroy_bd(nvmf_softc_t *sc);
extern void nvmf_bd_rescan_ns(nvmf_softc_t *sc, uint32_t id);

/*
 * Head <-> blkdev handle translation, consumed by nvmf_mpath.c: the head
 * carries the bd_ops "driver private" so the callbacks resolve geometry/NSID
 * through it.
 */
extern bd_handle_t nvmf_blkdev_attach_head(struct nvmf_ns_head *head,
    dev_info_t *dip);
extern void nvmf_blkdev_detach_head(bd_handle_t bdh);

/*
 * nvmf_mpath.c (NEW - the namespace-head / path / selector model of
 * NVMEOF.md 9.3).  These are consumed by nvmf_blkdev.c and by the core.
 */
extern void nvmf_mpath_init(void);
extern void nvmf_mpath_fini(void);
extern void nvmf_mpath_softc_init(nvmf_softc_t *sc);
extern void nvmf_mpath_softc_fini(nvmf_softc_t *sc);
extern void nvmf_mpath_softc_down(nvmf_softc_t *sc);
extern void nvmf_mpath_softc_up(nvmf_softc_t *sc);
extern void nvmf_mpath_softc_remove_all(nvmf_softc_t *sc);
struct nvmf_path *nvmf_mpath_add_path(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data);
void nvmf_mpath_remove_path(struct nvmf_path *path);
struct nvmf_ns_head *nvmf_mpath_find_head(const nvme_identify_nsid_t *data);
struct nvmf_path *nvmf_mpath_select(struct nvmf_ns_head *head, bd_xfer_t *xfer);
void nvmf_mpath_select_rele(struct nvmf_path *path);
void nvmf_mpath_path_down(struct nvmf_path *path);
void nvmf_mpath_path_up(struct nvmf_path *path);
uint32_t nvmf_mpath_head_blksize(struct nvmf_ns_head *head);
uint64_t nvmf_mpath_head_nblks(struct nvmf_ns_head *head);
void nvmf_mpath_head_set_geometry(struct nvmf_ns_head *head, uint64_t nblks,
    uint32_t blksize);
uint32_t nvmf_mpath_path_nsid(struct nvmf_path *path);
nvmf_softc_t *nvmf_mpath_path_sc(struct nvmf_path *path);

/*
 * Head identity/devid accessors consumed by nvmf_blkdev.c to populate
 * bd_drive_t and build the path-independent devid without exposing the head
 * struct.
 */
struct nvmf_path *nvmf_mpath_head_path(struct nvmf_ns_head *head);
boolean_t nvmf_mpath_head_eui64(struct nvmf_ns_head *head, uint8_t *eui64);
boolean_t nvmf_mpath_head_guid(struct nvmf_ns_head *head, uint8_t *guid);
int nvmf_mpath_head_devid_init(struct nvmf_ns_head *head, dev_info_t *dip,
    ddi_devid_t *devid);

#ifdef __cplusplus
}
#endif

#endif /* _NVMF_VAR_H */

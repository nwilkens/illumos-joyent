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
 * Private state for the NVMe over Fabrics host (initiator).
 */

#ifndef	_NVMF_VAR_H
#define	_NVMF_VAR_H

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/list.h>
#include <sys/avl.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/taskq_impl.h>
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
struct nvmf_ns_head;
struct nvmf_path;

typedef void nvmf_request_complete_t(void *, const nvme_cqe_t *);

/*
 * Association state, changed only under nvmf_softc_t.lock.
 *
 * ATTACHING, RECONNECTING, DISCONNECTING and DETACHING each belong to the one
 * ioctl or detach thread that entered the state.  RESETTING belongs to the
 * connection worker.  A softc leaves one of those states only by the hand of
 * its owner, so two lifecycle operations never run together.
 */
typedef enum nvmf_sc_state {
	NVMF_SC_IDLE,
	NVMF_SC_ATTACHING,
	NVMF_SC_CONNECTED,
	NVMF_SC_RESETTING,
	NVMF_SC_LOST,
	NVMF_SC_RECONNECTING,
	NVMF_SC_FAILED,
	NVMF_SC_DISCONNECTING,
	NVMF_SC_DETACHING
} nvmf_sc_state_t;

typedef enum nvmf_io_action {
	NVMF_IO_SUBMIT,
	NVMF_IO_HOLD,
	NVMF_IO_FAIL
} nvmf_io_action_t;

/* Connection worker events. */
#define	NVMF_EV_KA_TX		0x01
#define	NVMF_EV_KA_RX		0x02
#define	NVMF_EV_LOSS		0x04
#define	NVMF_EV_RECONNECT	0x08
#define	NVMF_EV_DISCONNECT	0x10

/* Namespace worker events. */
#define	NVMF_NS_EV_RESCAN	0x01
#define	NVMF_NS_EV_PURGE	0x02
#define	NVMF_NS_EV_DETACH	0x04

/*
 * Sysevents posted with ddi_log_sysevent() under vendor DDI_VENDOR_SUNW.  Each
 * carries the int32 "instance" and the string "subnqn".
 */
#define	EC_NVMF			"EC_nvmf"
#define	ESC_NVMF_HOST_LOST	"ESC_nvmf_host_lost"
#define	ESC_NVMF_HOST_RECONNECT	"ESC_nvmf_host_reconnect"
#define	ESC_NVMF_HOST_FAILED	"ESC_nvmf_host_failed"

typedef struct nvmf_softc {
	dev_info_t		*dip;

	kmutex_t		lock;
	kcondvar_t		cv;		/* state changes */
	kcondvar_t		qp_cv;		/* qp_users reached zero */
	nvmf_sc_state_t		state;
	boolean_t		io_enabled;
	boolean_t		conn_error;

	/*
	 * admin and io are written under lock by the owner of the current
	 * state.  Any other thread reads them under lock and holds qp_users
	 * for as long as it uses a queue pair.
	 */
	struct nvmf_host_qpair	*admin;
	struct nvmf_host_qpair	**io;
	uint_t			num_io_queues;
	uint_t			qp_users;
	nvmf_trtype_t		trtype;

	list_t			held;		/* I/O awaiting a reconnect */

	taskq_t			*conn_tq;
	taskq_ent_t		conn_ent;
	uint_t			conn_events;
	boolean_t		conn_queued;

	taskq_t			*ns_tq;
	taskq_ent_t		ns_ent;
	uint_t			ns_events;
	boolean_t		ns_queued;

	/*
	 * Namespaces and heads are added and removed only by the namespace
	 * worker, or by a state owner after it has drained that worker.  lock
	 * protects the containers for every other reader.
	 */
	avl_tree_t		ns_tree;
	list_t			heads;

	boolean_t		detach_req;
	boolean_t		detach_done;
	boolean_t		detach_busy;
	boolean_t		detach_owned;

	nvme_identify_ctrl_t	*cdata;
	boolean_t		vwc_present;
	boolean_t		dsm_supported;
	uint64_t		cap;
	uint32_t		vs;
	uint_t			max_pending_io;
	uint64_t		max_xfer_size;

	boolean_t		ka_enabled;
	boolean_t		ka_traffic;	/* Using TKAS? */
	volatile uint_t		ka_active_tx_traffic;
	volatile uint_t		ka_active_rx_traffic;
	clock_t			ka_rx_ticks;
	clock_t			ka_tx_ticks;
	timeout_id_t		ka_rx_timer;
	timeout_id_t		ka_tx_timer;

	uint32_t		reconnect_delay;
	uint32_t		controller_loss_timeout;
	timeout_id_t		reconnect_timer;
	timeout_id_t		loss_timer;
	clock_t			loss_deadline;

	uint_t			num_aer;
	struct nvmf_aer		*aer;

	nvlist_t		*rparams;	/* reconnect parameters */

	timespec_t		last_disconnect;
} nvmf_softc_t;

typedef struct nvmf_request {
	struct nvmf_host_qpair	*qp;
	struct nvmf_capsule	*nc;
	nvmf_request_complete_t	*cb;
	void			*cb_arg;
	boolean_t		aer;

	list_node_t		link;
} nvmf_request_t;

/* State of one synchronous command, waited on by nvmf_wait_for_reply(). */
typedef struct nvmf_completion_status {
	nvme_cqe_t		cqe;
	boolean_t		done;
	boolean_t		io_done;
	int			io_error;
	size_t			io_xfered;
} nvmf_completion_status_t;

typedef struct nvmf_ns_info {
	uint32_t		ni_nsid;
	uint32_t		ni_blksize;
	uint64_t		ni_size;
	uint8_t			ni_nguid[16];
	uint8_t			ni_eui64[8];
} nvmf_ns_info_t;

/* True if a CQE reports that the host aborted the command during teardown. */
static inline boolean_t
nvmf_cqe_aborted(const nvme_cqe_t *cqe)
{
	return (cqe->cqe_sf.sf_sct == NVME_CQE_SCT_PATH &&
	    cqe->cqe_sf.sf_sc == NVME_CQE_SC_PATH_HOST_ABRT);
}

static inline boolean_t
nvmf_cqe_failed(const nvme_cqe_t *cqe)
{
	return (cqe->cqe_sf.sf_sc != 0 || cqe->cqe_sf.sf_sct != 0);
}

static inline void
nvmf_status_init(nvmf_completion_status_t *status)
{
	status->done = B_FALSE;
	status->io_done = B_TRUE;
	status->io_error = 0;
	status->io_xfered = 0;
}

static inline void
nvmf_status_wait_io(nvmf_completion_status_t *status)
{
	status->io_done = B_FALSE;
}

/* If B_TRUE, I/O fails at once while the association is lost. */
extern boolean_t nvmf_fail_disconnect;
extern uint_t nvmf_admin_timeout;
extern uint_t nvmf_shutdown_timeout;
extern uint_t nvmf_max_namespaces;

/* nvmf_host.c */
extern void *nvmf_state;

/* nvmf_conn.c */
extern nvmf_io_action_t nvmf_io_policy(nvmf_sc_state_t state,
    boolean_t enabled, boolean_t gone, boolean_t fail_fast);
extern uint64_t nvmf_max_xfer(uint64_t cap, uint8_t mdts, uint64_t transport,
    uint64_t limit);
extern struct nvmf_host_qpair *nvmf_admin_hold(nvmf_softc_t *sc);
extern void nvmf_qp_rele(nvmf_softc_t *sc);
extern void nvmf_shutdown_admin(nvmf_softc_t *sc);
extern void nvmf_disconnect(nvmf_softc_t *sc);
extern void nvmf_ns_post(nvmf_softc_t *sc, uint_t events);
extern int nvmf_attach_association(nvmf_softc_t *sc, nvlist_t *nvl);
extern int nvmf_reconnect_association(nvmf_softc_t *sc, nvlist_t *nvl);
extern void nvmf_teardown(nvmf_softc_t *sc, boolean_t graceful);
extern const char *nvmf_state_name(nvmf_sc_state_t state);

/* nvmf_ioctl.c */
extern int nvmf_ioctl(dev_t dev, int cmd, intptr_t arg, int mode,
    cred_t *credp, int *rvalp);

/* nvmf_scan.c */
extern int nvmf_nslist_count(const nvme_identify_nsid_list_t *nslist,
    uint32_t prev);
extern boolean_t nvmf_add_namespaces(nvmf_softc_t *sc);
extern boolean_t nvmf_revalidate_namespaces(nvmf_softc_t *sc);
extern void nvmf_rescan_ns(nvmf_softc_t *sc, uint32_t nsid);
extern void nvmf_rescan_all_ns(nvmf_softc_t *sc);
extern void nvmf_ns_task(void *arg);

/* nvmf_aer.c */
extern void nvmf_init_aer(nvmf_softc_t *sc);
extern int nvmf_start_aer(nvmf_softc_t *sc);
extern void nvmf_destroy_aer(nvmf_softc_t *sc);

/* nvmf_cmd.c */
extern void nvmf_cmd_init(void);
extern void nvmf_cmd_fini(void);
extern void nvmf_complete(void *arg, const nvme_cqe_t *cqe);
extern void nvmf_io_complete(void *arg, size_t xfered, int error);
extern void nvmf_wait_for_reply(nvmf_softc_t *sc,
    nvmf_completion_status_t *status, uint_t secs);
extern boolean_t nvmf_status_ok(const nvmf_completion_status_t *status,
    size_t len);
extern int nvmf_read_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    uint64_t *value, uint_t secs);
extern void nvmf_shutdown_controller(nvmf_softc_t *sc);
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
    uint8_t lid, uint64_t offset, void *page, size_t len,
    nvmf_request_complete_t *req_cb, void *req_cb_arg,
    nvmf_io_complete_t *io_cb, void *io_cb_arg, int how);

/* nvmf_ns.c */
extern void nvmf_ns_init(nvmf_softc_t *sc);
extern void nvmf_ns_fini(nvmf_softc_t *sc);
extern boolean_t nvmf_ns_fmt(const nvme_identify_nsid_t *data, uint64_t *nblksp,
    uint32_t *blksizep, dev_info_t *dip, uint32_t nsid);
extern void nvmf_ns_reconcile(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data);
extern void nvmf_ns_purge(nvmf_softc_t *sc, uint32_t first, uint32_t end);
extern void nvmf_ns_remove_all(nvmf_softc_t *sc);
extern void nvmf_ns_revalidate(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data);
extern uint_t nvmf_ns_snapshot(nvmf_softc_t *sc, nvmf_ns_info_t *info,
    uint_t max);

/* nvmf_qpair.c */
extern struct nvmf_host_qpair *nvmf_init_qp(nvmf_softc_t *sc,
    nvmf_trtype_t trtype, const nvlist_t *nvl, const char *name);
extern void nvmf_shutdown_qp(struct nvmf_host_qpair *qp);
extern void nvmf_destroy_qp(struct nvmf_host_qpair *qp);
extern uint64_t nvmf_max_xfer_size_qp(struct nvmf_host_qpair *qp);
extern nvmf_request_t *nvmf_allocate_request(struct nvmf_host_qpair *qp,
    void *sqe, nvmf_request_complete_t *cb, void *cb_arg, int how);
extern void nvmf_submit_request(nvmf_request_t *req);
extern void nvmf_free_request(nvmf_request_t *req);

/* nvmf_blkdev.c */
extern void nvmf_bd_init(nvmf_softc_t *sc);
extern void nvmf_bd_fini(nvmf_softc_t *sc);
extern void nvmf_bd_resume(nvmf_softc_t *sc);
extern void nvmf_bd_fail_held(nvmf_softc_t *sc, int error);
extern void nvmf_bd_path_gone(nvmf_softc_t *sc, struct nvmf_path *path);
extern bd_handle_t nvmf_bd_alloc_handle(struct nvmf_ns_head *head);

/* nvmf_mpath.c */
extern void nvmf_mpath_init(nvmf_softc_t *sc);
extern void nvmf_mpath_fini(nvmf_softc_t *sc);
extern struct nvmf_path *nvmf_mpath_add_path(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data, uint64_t nblks, uint32_t blksize);
extern void nvmf_mpath_remove_path(struct nvmf_path *path);
extern boolean_t nvmf_mpath_path_update(struct nvmf_path *path,
    const nvme_identify_nsid_t *data, uint64_t nblks, uint32_t blksize);
extern boolean_t nvmf_mpath_path_matches(struct nvmf_path *path,
    const nvme_identify_nsid_t *data);
extern boolean_t nvmf_mpath_path_gone(struct nvmf_path *path);
extern void nvmf_mpath_path_set_gone(struct nvmf_path *path);
extern struct nvmf_path *nvmf_mpath_select(struct nvmf_ns_head *head);
extern void nvmf_mpath_select_rele(struct nvmf_path *path);
extern boolean_t nvmf_mpath_detach_all(nvmf_softc_t *sc);
extern void nvmf_mpath_reap(nvmf_softc_t *sc);
extern boolean_t nvmf_mpath_busy(nvmf_softc_t *sc);
extern uint32_t nvmf_mpath_head_blksize(struct nvmf_ns_head *head);
extern uint64_t nvmf_mpath_head_maxxfer(struct nvmf_ns_head *head);
extern int nvmf_mpath_head_media(struct nvmf_ns_head *head, uint64_t *nblksp,
    uint32_t *blksizep);
extern void nvmf_mpath_head_ids(struct nvmf_ns_head *head, uint8_t *eui64,
    uint8_t *guid);
extern int nvmf_mpath_head_devid_init(struct nvmf_ns_head *head,
    dev_info_t *dip, ddi_devid_t *devid);
extern uint32_t nvmf_mpath_path_nsid(struct nvmf_path *path);
extern nvmf_softc_t *nvmf_mpath_path_sc(struct nvmf_path *path);

#ifdef __cplusplus
}
#endif

#endif /* _NVMF_VAR_H */

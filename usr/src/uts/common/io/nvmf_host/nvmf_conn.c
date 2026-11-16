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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/host/nvmf.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Association lifecycle for the NVMe over Fabrics host: establishment,
 * KeepAlive, connection loss, reconnect and teardown.
 *
 * Lock order: nvmf_softc_t.lock is a leaf apart from the taskq and callout
 * locks.  No lock of this driver is held across a call into blkdev, across
 * nvmf_shutdown_qp() or nvmf_destroy_qp(), or across untimeout(9F).
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/atomic.h>
#include <sys/errno.h>
#include <sys/nvpair.h>
#include <sys/sysevent.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"

/* The memory page size is 2^(12 + CAP.MPSMIN). */
#define	NVME_MPS_SHIFT	12

boolean_t nvmf_fail_disconnect = B_FALSE;
uint_t nvmf_admin_timeout = 30;
uint_t nvmf_shutdown_timeout = 5;

static void	nvmf_conn_task(void *arg);
static void	nvmf_ka_rx_timer(void *arg);
static void	nvmf_ka_tx_timer(void *arg);
static void	nvmf_loss_timer(void *arg);
static void	nvmf_reconnect_timer(void *arg);

const char *
nvmf_state_name(nvmf_sc_state_t state)
{
	switch (state) {
	case NVMF_SC_IDLE:
		return ("idle");
	case NVMF_SC_ATTACHING:
		return ("attaching");
	case NVMF_SC_CONNECTED:
		return ("connected");
	case NVMF_SC_RESETTING:
		return ("resetting");
	case NVMF_SC_LOST:
		return ("lost");
	case NVMF_SC_RECONNECTING:
		return ("reconnecting");
	case NVMF_SC_FAILED:
		return ("failed");
	case NVMF_SC_DISCONNECTING:
		return ("disconnecting");
	case NVMF_SC_DETACHING:
		return ("detaching");
	}
	return ("unknown");
}

/*
 * What to do with one block I/O request.  enabled says the I/O queue pairs
 * take submissions, gone says the namespace behind the request went away.
 */
nvmf_io_action_t
nvmf_io_policy(nvmf_sc_state_t state, boolean_t enabled, boolean_t gone,
    boolean_t fail_fast)
{
	if (gone)
		return (NVMF_IO_FAIL);

	switch (state) {
	case NVMF_SC_ATTACHING:
	case NVMF_SC_CONNECTED:
		return (enabled ? NVMF_IO_SUBMIT : NVMF_IO_FAIL);
	case NVMF_SC_RESETTING:
	case NVMF_SC_LOST:
	case NVMF_SC_RECONNECTING:
		return (fail_fast ? NVMF_IO_FAIL : NVMF_IO_HOLD);
	default:
		return (NVMF_IO_FAIL);
	}
}

/*
 * The largest transfer in bytes, given CAP, MDTS, the transport's limit and
 * the system's limit.  An MDTS that allows 4 GiB or more is no limit here.
 */
uint64_t
nvmf_max_xfer(uint64_t cap, uint8_t mdts, uint64_t transport, uint64_t limit)
{
	nvme_reg_cap_t reg;
	uint64_t mps;
	uint_t shift;

	reg.r = cap;
	shift = NVME_MPS_SHIFT + reg.b.cap_mpsmin;
	mps = (uint64_t)1 << shift;

	if (mdts != 0 && shift + mdts < 32)
		limit = MIN(limit, (uint64_t)1 << (shift + mdts));
	if (transport >= mps)
		limit = MIN(limit, transport);
	if (limit >= mps)
		limit = P2ALIGN(limit, mps);
	return (limit);
}

struct nvmf_host_qpair *
nvmf_admin_hold(nvmf_softc_t *sc)
{
	struct nvmf_host_qpair *qp;

	mutex_enter(&sc->lock);
	qp = sc->admin;
	if (qp != NULL)
		sc->qp_users++;
	mutex_exit(&sc->lock);
	return (qp);
}

void
nvmf_qp_rele(nvmf_softc_t *sc)
{
	mutex_enter(&sc->lock);
	ASSERT3U(sc->qp_users, >, 0);
	if (--sc->qp_users == 0)
		cv_broadcast(&sc->qp_cv);
	mutex_exit(&sc->lock);
}

/*
 * Shut down every published queue pair but leave it allocated, so that each
 * outstanding command is aborted and later submissions fail.
 */
static void
nvmf_shutdown_qpairs(nvmf_softc_t *sc)
{
	struct nvmf_host_qpair *admin, **io;
	uint_t i, n;

	mutex_enter(&sc->lock);
	admin = sc->admin;
	io = sc->io;
	n = sc->num_io_queues;
	sc->qp_users++;
	mutex_exit(&sc->lock);

	for (i = 0; i < n; i++)
		nvmf_shutdown_qp(io[i]);
	if (admin != NULL)
		nvmf_shutdown_qp(admin);

	nvmf_qp_rele(sc);
}

void
nvmf_shutdown_admin(nvmf_softc_t *sc)
{
	struct nvmf_host_qpair *admin;

	admin = nvmf_admin_hold(sc);
	if (admin == NULL)
		return;
	nvmf_shutdown_qp(admin);
	nvmf_qp_rele(sc);
}

/* Unpublish and free the queue pairs.  Only the owner of the state calls. */
static void
nvmf_destroy_qpairs(nvmf_softc_t *sc)
{
	struct nvmf_host_qpair *admin, **io;
	uint_t i, n;

	mutex_enter(&sc->lock);
	admin = sc->admin;
	io = sc->io;
	n = sc->num_io_queues;
	sc->admin = NULL;
	sc->io = NULL;
	sc->num_io_queues = 0;
	sc->io_enabled = B_FALSE;
	while (sc->qp_users != 0)
		cv_wait(&sc->qp_cv, &sc->lock);
	mutex_exit(&sc->lock);

	for (i = 0; i < n; i++)
		nvmf_destroy_qp(io[i]);
	if (io != NULL)
		kmem_free(io, n * sizeof (*io));
	if (admin != NULL)
		nvmf_destroy_qp(admin);
}

static void
nvmf_conn_post_locked(nvmf_softc_t *sc, uint_t events)
{
	ASSERT(MUTEX_HELD(&sc->lock));

	sc->conn_events |= events;
	if (!sc->conn_queued) {
		sc->conn_queued = B_TRUE;
		taskq_dispatch_ent(sc->conn_tq, nvmf_conn_task, sc, 0,
		    &sc->conn_ent);
	}
}

void
nvmf_disconnect(nvmf_softc_t *sc)
{
	mutex_enter(&sc->lock);
	nvmf_conn_post_locked(sc, NVMF_EV_DISCONNECT);
	mutex_exit(&sc->lock);
}

void
nvmf_ns_post(nvmf_softc_t *sc, uint_t events)
{
	mutex_enter(&sc->lock);
	sc->ns_events |= events;
	if (!sc->ns_queued) {
		sc->ns_queued = B_TRUE;
		taskq_dispatch_ent(sc->ns_tq, nvmf_ns_task, sc, 0,
		    &sc->ns_ent);
	}
	mutex_exit(&sc->lock);
}

static void
nvmf_sysevent(nvmf_softc_t *sc, char *subclass)
{
	char subnqn[sizeof (sc->cdata->id_subnqn) + 1];
	sysevent_id_t eid;
	nvlist_t *attr;

	mutex_enter(&sc->lock);
	bcopy(sc->cdata->id_subnqn, subnqn, sizeof (sc->cdata->id_subnqn));
	mutex_exit(&sc->lock);
	subnqn[sizeof (subnqn) - 1] = '\0';

	if (nvlist_alloc(&attr, NV_UNIQUE_NAME_TYPE, KM_NOSLEEP) != 0)
		return;
	if (nvlist_add_int32(attr, "instance",
	    ddi_get_instance(sc->dip)) == 0 &&
	    nvlist_add_string(attr, "subnqn", subnqn) == 0) {
		(void) ddi_log_sysevent(sc->dip, DDI_VENDOR_SUNW, EC_NVMF,
		    subclass, attr, &eid, DDI_NOSLEEP);
	}
	nvlist_free(attr);
}

/* A timeout(9F) handler only hands its event to the connection worker. */
static void
nvmf_timer_post(nvmf_softc_t *sc, timeout_id_t *idp, uint_t event)
{
	mutex_enter(&sc->lock);
	if (*idp != 0) {
		*idp = 0;
		nvmf_conn_post_locked(sc, event);
	}
	mutex_exit(&sc->lock);
}

static void
nvmf_ka_rx_timer(void *arg)
{
	nvmf_softc_t *sc = arg;

	nvmf_timer_post(sc, &sc->ka_rx_timer, NVMF_EV_KA_RX);
}

static void
nvmf_ka_tx_timer(void *arg)
{
	nvmf_softc_t *sc = arg;

	nvmf_timer_post(sc, &sc->ka_tx_timer, NVMF_EV_KA_TX);
}

static void
nvmf_loss_timer(void *arg)
{
	nvmf_softc_t *sc = arg;

	nvmf_timer_post(sc, &sc->loss_timer, NVMF_EV_LOSS);
}

static void
nvmf_reconnect_timer(void *arg)
{
	nvmf_softc_t *sc = arg;

	nvmf_timer_post(sc, &sc->reconnect_timer, NVMF_EV_RECONNECT);
}

static void
nvmf_ka_start(nvmf_softc_t *sc)
{
	mutex_enter(&sc->lock);
	if (sc->ka_rx_ticks != 0) {
		/* An event left from an earlier association must not expire. */
		sc->ka_active_rx_traffic = 1;
		sc->ka_enabled = B_TRUE;
		sc->ka_rx_timer = timeout(nvmf_ka_rx_timer, sc,
		    sc->ka_rx_ticks);
		sc->ka_tx_timer = timeout(nvmf_ka_tx_timer, sc,
		    sc->ka_tx_ticks);
	}
	mutex_exit(&sc->lock);
}

static void
nvmf_ka_stop(nvmf_softc_t *sc)
{
	timeout_id_t rx, tx;

	mutex_enter(&sc->lock);
	sc->ka_enabled = B_FALSE;
	sc->ka_traffic = B_FALSE;
	rx = sc->ka_rx_timer;
	tx = sc->ka_tx_timer;
	sc->ka_rx_timer = 0;
	sc->ka_tx_timer = 0;
	mutex_exit(&sc->lock);

	if (rx != 0)
		(void) untimeout(rx);
	if (tx != 0)
		(void) untimeout(tx);

	mutex_enter(&sc->lock);
	sc->conn_events &= ~(NVMF_EV_KA_RX | NVMF_EV_KA_TX);
	mutex_exit(&sc->lock);
}

static void
nvmf_keep_alive_complete(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_softc_t *sc = arg;

	(void) atomic_swap_uint(&sc->ka_active_rx_traffic, 1);
	if (nvmf_cqe_failed(cqe) && !nvmf_cqe_aborted(cqe)) {
		dev_err(sc->dip, CE_WARN,
		    "!KeepAlive response reported error status");
	}
}

/* Returns B_TRUE if the controller sent nothing for a whole interval. */
static boolean_t
nvmf_ka_rx(nvmf_softc_t *sc)
{
	uint_t traffic;

	traffic = atomic_swap_uint(&sc->ka_active_rx_traffic, 0);

	mutex_enter(&sc->lock);
	if (!sc->ka_enabled) {
		mutex_exit(&sc->lock);
		return (B_FALSE);
	}
	if (traffic == 0) {
		mutex_exit(&sc->lock);
		dev_err(sc->dip, CE_WARN,
		    "!disconnecting due to KeepAlive timeout");
		return (B_TRUE);
	}
	if (sc->ka_rx_timer == 0) {
		sc->ka_rx_timer = timeout(nvmf_ka_rx_timer, sc,
		    sc->ka_rx_ticks);
	}
	mutex_exit(&sc->lock);
	return (B_FALSE);
}

static void
nvmf_ka_tx(nvmf_softc_t *sc)
{
	mutex_enter(&sc->lock);
	if (!sc->ka_enabled) {
		mutex_exit(&sc->lock);
		return;
	}
	mutex_exit(&sc->lock);

	/*
	 * With TKAS, a command sent during the interval stands in for the
	 * KeepAlive.
	 */
	if (atomic_swap_uint(&sc->ka_active_tx_traffic, 0) == 0 &&
	    !nvmf_cmd_keep_alive(sc, nvmf_keep_alive_complete, sc,
	    KM_NOSLEEP)) {
		dev_err(sc->dip, CE_WARN, "!failed to send KeepAlive command");
	}
	(void) atomic_swap_uint(&sc->ka_active_tx_traffic, 0);

	mutex_enter(&sc->lock);
	if (sc->ka_enabled && sc->ka_tx_timer == 0) {
		sc->ka_tx_timer = timeout(nvmf_ka_tx_timer, sc,
		    sc->ka_tx_ticks);
	}
	mutex_exit(&sc->lock);
}

/* Cancel the reconnect and loss timers and drop their pending events. */
static void
nvmf_cancel_loss_timers(nvmf_softc_t *sc)
{
	timeout_id_t reconnect, loss;

	mutex_enter(&sc->lock);
	reconnect = sc->reconnect_timer;
	loss = sc->loss_timer;
	sc->reconnect_timer = 0;
	sc->loss_timer = 0;
	mutex_exit(&sc->lock);

	if (reconnect != 0)
		(void) untimeout(reconnect);
	if (loss != 0)
		(void) untimeout(loss);

	mutex_enter(&sc->lock);
	sc->conn_events &= ~(NVMF_EV_RECONNECT | NVMF_EV_LOSS);
	mutex_exit(&sc->lock);
}

static void
nvmf_arm_loss_timers(nvmf_softc_t *sc)
{
	clock_t left;

	ASSERT(MUTEX_HELD(&sc->lock));
	ASSERT3S(sc->state, ==, NVMF_SC_LOST);

	if (sc->reconnect_delay != 0 && sc->reconnect_timer == 0) {
		sc->reconnect_timer = timeout(nvmf_reconnect_timer, sc,
		    drv_usectohz((clock_t)sc->reconnect_delay * MICROSEC));
	}
	if (sc->controller_loss_timeout != 0 && sc->loss_timer == 0) {
		left = sc->loss_deadline - ddi_get_lbolt();
		sc->loss_timer = timeout(nvmf_loss_timer, sc, MAX(left, 1));
	}
}

/*
 * A transport error, a protocol error or a KeepAlive timeout.  When an ioctl
 * owns the association the queue pairs are only shut down, which fails the
 * owner's commands and leaves the cleanup to it.
 */
static void
nvmf_conn_lost(nvmf_softc_t *sc)
{
	boolean_t fail_fast;

	mutex_enter(&sc->lock);
	switch (sc->state) {
	case NVMF_SC_CONNECTED:
		break;
	case NVMF_SC_ATTACHING:
	case NVMF_SC_RECONNECTING:
	case NVMF_SC_DISCONNECTING:
		sc->conn_error = B_TRUE;
		mutex_exit(&sc->lock);
		nvmf_shutdown_qpairs(sc);
		return;
	default:
		mutex_exit(&sc->lock);
		return;
	}
	sc->state = NVMF_SC_RESETTING;
	sc->io_enabled = B_FALSE;
	gethrestime(&sc->last_disconnect);
	mutex_exit(&sc->lock);

	nvmf_ka_stop(sc);
	nvmf_destroy_qpairs(sc);

	fail_fast = nvmf_fail_disconnect;

	mutex_enter(&sc->lock);
	sc->conn_events &= ~NVMF_EV_DISCONNECT;
	sc->state = NVMF_SC_LOST;
	sc->loss_deadline = ddi_get_lbolt() +
	    drv_usectohz((clock_t)sc->controller_loss_timeout * MICROSEC);
	nvmf_arm_loss_timers(sc);
	cv_broadcast(&sc->cv);
	mutex_exit(&sc->lock);

	if (fail_fast)
		nvmf_bd_fail_held(sc, EIO);

	dev_err(sc->dip, CE_WARN, "!association lost");
	nvmf_sysevent(sc, ESC_NVMF_HOST_LOST);
}

static void
nvmf_conn_loss_expired(nvmf_softc_t *sc)
{
	timeout_id_t reconnect;

	mutex_enter(&sc->lock);
	if (sc->state != NVMF_SC_LOST) {
		mutex_exit(&sc->lock);
		return;
	}
	sc->state = NVMF_SC_FAILED;
	reconnect = sc->reconnect_timer;
	sc->reconnect_timer = 0;
	cv_broadcast(&sc->cv);
	mutex_exit(&sc->lock);

	if (reconnect != 0)
		(void) untimeout(reconnect);

	dev_err(sc->dip, CE_WARN,
	    "!association not re-established within %u seconds, failing I/O",
	    sc->controller_loss_timeout);
	nvmf_bd_fail_held(sc, EIO);
	nvmf_ns_post(sc, NVMF_NS_EV_PURGE);
	nvmf_sysevent(sc, ESC_NVMF_HOST_FAILED);
}

static void
nvmf_conn_request_reconnect(nvmf_softc_t *sc)
{
	mutex_enter(&sc->lock);
	if (sc->state != NVMF_SC_LOST) {
		mutex_exit(&sc->lock);
		return;
	}
	nvmf_arm_loss_timers(sc);
	mutex_exit(&sc->lock);

	nvmf_sysevent(sc, ESC_NVMF_HOST_RECONNECT);
}

/*
 * The connection worker.  It never waits for the controller, so a hung target
 * cannot keep it from tearing the association down.  The loss event is
 * handled before a disconnect so that it applies to the loss it was armed for.
 */
static void
nvmf_conn_task(void *arg)
{
	nvmf_softc_t *sc = arg;
	uint_t events;

	mutex_enter(&sc->lock);
	while ((events = sc->conn_events) != 0) {
		sc->conn_events = 0;
		mutex_exit(&sc->lock);

		if ((events & NVMF_EV_KA_TX) != 0)
			nvmf_ka_tx(sc);
		if ((events & NVMF_EV_KA_RX) != 0 && nvmf_ka_rx(sc))
			events |= NVMF_EV_DISCONNECT;
		if ((events & NVMF_EV_LOSS) != 0)
			nvmf_conn_loss_expired(sc);
		if ((events & NVMF_EV_RECONNECT) != 0)
			nvmf_conn_request_reconnect(sc);
		if ((events & NVMF_EV_DISCONNECT) != 0)
			nvmf_conn_lost(sc);

		mutex_enter(&sc->lock);
	}
	sc->conn_queued = B_FALSE;
	mutex_exit(&sc->lock);
}

/*
 * Create the queue pairs described by a handoff and publish them.  cdata is
 * the controller data that came with the same handoff.
 */
static int
nvmf_establish_connection(nvmf_softc_t *sc, nvlist_t *nvl,
    const nvme_identify_ctrl_t *cdata)
{
	struct nvmf_host_qpair *admin, **io;
	nvlist_t *admin_nvl, **io_nvl;
	uint64_t kato = 0;
	uint_t i, n;
	char name[16];

	VERIFY0(nvlist_lookup_nvlist(nvl, "admin", &admin_nvl));
	VERIFY0(nvlist_lookup_nvlist_array(nvl, "io", &io_nvl, &n));
	(void) nvlist_lookup_uint64(nvl, "kato", &kato);

	admin = nvmf_init_qp(sc, sc->trtype, admin_nvl, "admin queue");
	if (admin == NULL) {
		dev_err(sc->dip, CE_WARN, "!Failed to setup admin queue");
		return (ENXIO);
	}

	io = kmem_zalloc(n * sizeof (*io), KM_SLEEP);
	for (i = 0; i < n; i++) {
		(void) snprintf(name, sizeof (name), "I/O queue %u", i);
		io[i] = nvmf_init_qp(sc, sc->trtype, io_nvl[i], name);
		if (io[i] == NULL) {
			dev_err(sc->dip, CE_WARN,
			    "!Failed to setup I/O queue %u", i);
			goto fail;
		}
	}

	mutex_enter(&sc->lock);
	if (sc->conn_error) {
		mutex_exit(&sc->lock);
		goto fail;
	}
	sc->admin = admin;
	sc->io = io;
	sc->num_io_queues = n;
	if (kato != 0) {
		sc->ka_traffic = cdata->id_ctratt.ctrat_tbkas != 0;
		sc->ka_rx_ticks = MAX(drv_usectohz((clock_t)kato * MILLISEC),
		    2);
		sc->ka_tx_ticks = sc->ka_rx_ticks / 2;
	} else {
		sc->ka_traffic = B_FALSE;
		sc->ka_rx_ticks = 0;
		sc->ka_tx_ticks = 0;
	}
	mutex_exit(&sc->lock);

	nvmf_ka_start(sc);
	return (0);

fail:
	while (i > 0)
		nvmf_destroy_qp(io[--i]);
	kmem_free(io, n * sizeof (*io));
	nvmf_destroy_qp(admin);
	return (ENXIO);
}

static void
nvmf_set_cdata(nvmf_softc_t *sc, const nvme_identify_ctrl_t *cdata)
{
	ASSERT(MUTEX_HELD(&sc->lock));

	bcopy(cdata, sc->cdata, sizeof (*sc->cdata));
	sc->vwc_present = cdata->id_vwc.vwc_present != 0;
	sc->dsm_supported = cdata->id_oncs.on_dset_mgmt != 0;
}

static void
nvmf_set_timeouts(nvmf_softc_t *sc, nvlist_t *nvl)
{
	uint64_t v64;

	ASSERT(MUTEX_HELD(&sc->lock));

	v64 = 0;
	(void) nvlist_lookup_uint64(nvl, "reconnect_delay", &v64);
	sc->reconnect_delay = (uint32_t)v64;
	v64 = 0;
	(void) nvlist_lookup_uint64(nvl, "controller_loss_timeout", &v64);
	sc->controller_loss_timeout = (uint32_t)v64;
}

/*
 * Bring up the first association of a softc in NVMF_SC_ATTACHING.  nvl has
 * passed the handoff checks.  On failure the caller runs nvmf_teardown().
 */
int
nvmf_attach_association(nvmf_softc_t *sc, nvlist_t *nvl)
{
	nvlist_t **io, *rparams, *dup;
	uchar_t *cdata;
	uint_t cdlen, num_io_queues;
	uint64_t v64 = 0, cap = 0, vs = 0, max_xfer, qsize = 0;
	int error;

	VERIFY0(nvlist_lookup_byte_array(nvl, "cdata", &cdata, &cdlen));
	VERIFY0(nvlist_lookup_nvlist_array(nvl, "io", &io, &num_io_queues));
	VERIFY0(nvlist_lookup_nvlist(nvl, "rparams", &rparams));
	VERIFY0(nvlist_lookup_uint64(nvl, "trtype", &v64));
	(void) nvlist_lookup_uint64(io[0], "qsize", &qsize);

	mutex_enter(&sc->lock);
	nvmf_set_cdata(sc, (const nvme_identify_ctrl_t *)cdata);
	nvmf_set_timeouts(sc, nvl);
	sc->trtype = (nvmf_trtype_t)v64;
	mutex_exit(&sc->lock);

	nvmf_init_aer(sc);

	error = nvmf_establish_connection(sc, nvl, sc->cdata);
	if (error != 0)
		return (error);

	if (nvmf_read_property(sc, NVMF_PROP_CAP, 8, &cap,
	    nvmf_admin_timeout) != 0 ||
	    nvmf_read_property(sc, NVMF_PROP_VS, 4, &vs,
	    nvmf_admin_timeout) != 0) {
		dev_err(sc->dip, CE_WARN, "!Failed to fetch CAP and VS");
		return (ENXIO);
	}

	max_xfer = nvmf_max_xfer(cap, sc->cdata->id_mdts,
	    nvmf_max_xfer_size_qp(sc->io[0]), (uint64_t)maxphys);

	mutex_enter(&sc->lock);
	if (sc->conn_error) {
		mutex_exit(&sc->lock);
		return (ECONNABORTED);
	}
	sc->cap = cap;
	sc->vs = (uint32_t)vs;
	sc->max_xfer_size = max_xfer;
	sc->max_pending_io = (uint_t)qsize * num_io_queues;
	sc->io_enabled = B_TRUE;
	mutex_exit(&sc->lock);

	if (!nvmf_add_namespaces(sc))
		return (ENXIO);

	error = nvmf_start_aer(sc);
	if (error != 0)
		return (error);

	dup = fnvlist_dup(rparams);

	mutex_enter(&sc->lock);
	if (sc->conn_error) {
		mutex_exit(&sc->lock);
		fnvlist_free(dup);
		return (ECONNABORTED);
	}
	sc->rparams = dup;
	sc->state = NVMF_SC_CONNECTED;
	cv_broadcast(&sc->cv);
	mutex_exit(&sc->lock);
	return (0);
}

/*
 * Re-establish a lost association in NVMF_SC_RECONNECTING.  Held I/O is
 * released only after every namespace it could reach has been identified
 * again.  On failure the softc is back in NVMF_SC_LOST.
 */
int
nvmf_reconnect_association(nvmf_softc_t *sc, nvlist_t *nvl)
{
	const nvme_identify_ctrl_t *cdata;
	nvlist_t *rparams, *dup, *old;
	uchar_t *bytes;
	uint_t len;
	uint64_t cap = 0, max_xfer;
	int error;

	VERIFY0(nvlist_lookup_byte_array(nvl, "cdata", &bytes, &len));
	VERIFY0(nvlist_lookup_nvlist(nvl, "rparams", &rparams));
	cdata = (const nvme_identify_ctrl_t *)bytes;

	nvmf_cancel_loss_timers(sc);

	error = nvmf_establish_connection(sc, nvl, cdata);
	if (error != 0)
		goto fail;

	if (nvmf_read_property(sc, NVMF_PROP_CAP, 8, &cap,
	    nvmf_admin_timeout) != 0) {
		error = ENXIO;
		goto fail;
	}

	/* blkdev read the transfer limit once, when each disk attached. */
	max_xfer = nvmf_max_xfer(cap, cdata->id_mdts,
	    nvmf_max_xfer_size_qp(sc->io[0]), (uint64_t)maxphys);
	if (max_xfer < sc->max_xfer_size) {
		dev_err(sc->dip, CE_WARN, "!new association limits a transfer "
		    "to %llu bytes, less than the %llu in use",
		    (u_longlong_t)max_xfer, (u_longlong_t)sc->max_xfer_size);
		error = EINVAL;
		goto fail;
	}

	/* The subsystem is the same, so this stands even after a failure. */
	mutex_enter(&sc->lock);
	nvmf_set_cdata(sc, cdata);
	sc->cap = cap;
	mutex_exit(&sc->lock);

	if (!nvmf_revalidate_namespaces(sc)) {
		error = ECONNABORTED;
		goto fail;
	}

	error = nvmf_start_aer(sc);
	if (error != 0)
		goto fail;

	dup = fnvlist_dup(rparams);

	mutex_enter(&sc->lock);
	if (sc->conn_error) {
		mutex_exit(&sc->lock);
		fnvlist_free(dup);
		error = ECONNABORTED;
		goto fail;
	}
	nvmf_set_timeouts(sc, nvl);
	old = sc->rparams;
	sc->rparams = dup;
	sc->state = NVMF_SC_CONNECTED;
	sc->io_enabled = B_TRUE;
	cv_broadcast(&sc->cv);
	mutex_exit(&sc->lock);
	nvlist_free(old);

	dev_err(sc->dip, CE_NOTE,
	    "!established new association with %u I/O queues",
	    sc->num_io_queues);

	nvmf_bd_resume(sc);
	nvmf_ns_post(sc, NVMF_NS_EV_RESCAN);
	return (0);

fail:
	nvmf_ka_stop(sc);
	nvmf_destroy_qpairs(sc);

	mutex_enter(&sc->lock);
	sc->conn_events &= ~NVMF_EV_DISCONNECT;
	sc->conn_error = B_FALSE;
	sc->state = NVMF_SC_LOST;
	nvmf_arm_loss_timers(sc);
	cv_broadcast(&sc->cv);
	mutex_exit(&sc->lock);
	return (error);
}

/*
 * Tear an association down to nothing.  The caller owns the state, which must
 * be one that fails I/O.  The queue pairs are stopped before anything waits
 * for the namespace worker or for a path to drain.
 */
void
nvmf_teardown(nvmf_softc_t *sc, boolean_t graceful)
{
	nvlist_t *rparams;

	mutex_enter(&sc->lock);
	sc->io_enabled = B_FALSE;
	mutex_exit(&sc->lock);

	nvmf_cancel_loss_timers(sc);
	nvmf_ka_stop(sc);
	nvmf_bd_fail_held(sc, ENXIO);

	if (graceful)
		nvmf_shutdown_controller(sc);
	nvmf_shutdown_qpairs(sc);

	taskq_wait(sc->ns_tq);
	nvmf_ns_remove_all(sc);

	nvmf_destroy_qpairs(sc);
	nvmf_destroy_aer(sc);
	nvmf_mpath_reap(sc);

	mutex_enter(&sc->lock);
	rparams = sc->rparams;
	sc->rparams = NULL;
	sc->conn_events &= ~NVMF_EV_DISCONNECT;
	sc->conn_error = B_FALSE;
	mutex_exit(&sc->lock);
	nvlist_free(rparams);
}

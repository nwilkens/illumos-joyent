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
 * Core of the NVMe over Fabrics host (initiator).  This file ports the
 * protocol-bearing logic of FreeBSD's nvmf.c directly:
 *
 *   - synchronous command helpers (nvmf_complete / nvmf_io_complete /
 *     nvmf_wait_for_reply) used by the property and IDENTIFY flows;
 *   - controller property read/write and graceful shutdown;
 *   - Keep-Alive tx/rx timer logic;
 *   - association establishment (admin + I/O qpairs, CAP/VS, MDTS) and the
 *     handoff-nvlist validation (nvmf_copyin_handoff);
 *   - active-namespace scanning and rescan;
 *   - the disconnect / reconnect / controller-loss state machine.
 *
 * The device-model glue is rebound rather than ported.  FreeBSD's newbus
 * (device_t, DRIVER_MODULE, make_dev, cdevsw, eventhandler) becomes illumos
 * DDI (dev_info_t, dev_ops/cb_ops, attach/detach/ioctl); those entry points and
 * the protocol routines they call are fully implemented here.
 *
 * OS-glue substitutions used throughout:
 *
 *   FreeBSD                 illumos
 *   -------                 -------
 *   device_t / device_printf dev_info_t / dev_err
 *   struct sx               krwlock_t (connection_lock)
 *   struct callout          timeout(9F) id (keep-alive)
 *   taskqueue / timeout_task ddi_taskq_t (nvmf_tq) + timeout(9F)
 *   mtx_pool_find/mtx_sleep  one kmutex + kcondvar on the completion status
 *   nvlist (sys/_nv.h)      nvlist (sys/nvpair.h), fnvlist_* helpers
 *   le16toh/le32toh/le64toh  illumos is little-endian only
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/autoconf.h>
#include <sys/mkdev.h>
#include <sys/modctl.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>
#include <sys/atomic.h>
#include <sys/errno.h>
#include <sys/policy.h>
#include <sys/stat.h>
#include <sys/open.h>
#include <sys/mkdev.h>
#include <sys/nvpair.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"

/*
 * Minimum NVMe memory page size shift: the page size is 2^(12 + CAP.MPSMIN).
 * FreeBSD spells this NVME_MPS_SHIFT; illumos has no equivalent in <sys/nvme.h>.
 */
#define	NVME_MPS_SHIFT	12

boolean_t nvmf_fail_disconnect = B_FALSE;

void	nvmf_disconnect_task(void *arg);
void	nvmf_check_keep_alive(void *arg);
void	nvmf_send_keep_alive(void *arg);

static void	nvmf_controller_loss_task(void *arg);
static void	nvmf_request_reconnect_task(void *arg);
static int	nvmf_establish_connection(nvmf_softc_t *sc, nvlist_t *nvl);
static void	nvmf_shutdown_controller(nvmf_softc_t *sc);
static boolean_t nvmf_add_namespaces(nvmf_softc_t *sc);
static void	nvmf_rescan_ns_1(nvmf_softc_t *sc, uint32_t nsid,
		    const nvme_identify_nsid_t *data);
static void	nvmf_purge_namespaces(nvmf_softc_t *sc, uint32_t first_nsid,
		    uint32_t next_valid_nsid);

/*
 * PORT-TODO (FreeBSD nvmf.c nvmf_tq): a single driver-wide taskq replaces
 * FreeBSD's taskqueue.  It backs both the disconnect task and the AER tasks.
 * Created in _init(); the per-softc AER work uses it via nvmf_aer_taskq().
 */
static ddi_taskq_t *nvmf_tq;

/*
 * Per-softc completion-status synchronization.  FreeBSD uses a global mutex
 * pool keyed by the status address; illumos uses one global mutex/cv since the
 * synchronous command paths are low-rate (admin property/IDENTIFY).
 */
static kmutex_t nvmf_status_lock;
static kcondvar_t nvmf_status_cv;

ddi_taskq_t *
nvmf_aer_taskq(nvmf_softc_t *sc)
{
	_NOTE(ARGUNUSED(sc));
	return (nvmf_tq);
}

void
nvmf_complete(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_completion_status_t *status = arg;

	status->cqe = *cqe;
	mutex_enter(&nvmf_status_lock);
	status->done = B_TRUE;
	cv_broadcast(&nvmf_status_cv);
	mutex_exit(&nvmf_status_lock);
}

void
nvmf_io_complete(void *arg, size_t xfered, int error)
{
	nvmf_completion_status_t *status = arg;

	_NOTE(ARGUNUSED(xfered));

	status->io_error = error;
	mutex_enter(&nvmf_status_lock);
	status->io_done = B_TRUE;
	cv_broadcast(&nvmf_status_cv);
	mutex_exit(&nvmf_status_lock);
}

void
nvmf_wait_for_reply(nvmf_completion_status_t *status)
{
	mutex_enter(&nvmf_status_lock);
	while (!status->done || !status->io_done)
		cv_wait(&nvmf_status_cv, &nvmf_status_lock);
	mutex_exit(&nvmf_status_lock);
}

static boolean_t
nvmf_cqe_failed(const nvme_cqe_t *cqe)
{
	return (cqe->cqe_sf.sf_sc != 0 || cqe->cqe_sf.sf_sct != 0);
}

static int
nvmf_read_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    uint64_t *value)
{
	const nvmf_fabric_prop_get_rsp_t *rsp;
	nvmf_completion_status_t status;

	nvmf_status_init(&status);
	if (!nvmf_cmd_get_property(sc, offset, size, nvmf_complete, &status,
	    KM_SLEEP))
		return (ECONNABORTED);
	nvmf_wait_for_reply(&status);

	if (nvmf_cqe_failed(&status.cqe)) {
		dev_err(sc->dip, CE_WARN, "!PROPERTY_GET failed");
		return (EIO);
	}

	rsp = (const nvmf_fabric_prop_get_rsp_t *)&status.cqe;
	if (size == 8)
		*value = rsp->nfpr_value.u64;
	else
		*value = rsp->nfpr_value.u32.low;
	return (0);
}

static int
nvmf_write_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    uint64_t value)
{
	nvmf_completion_status_t status;

	nvmf_status_init(&status);
	if (!nvmf_cmd_set_property(sc, offset, size, value, nvmf_complete,
	    &status, KM_SLEEP))
		return (ECONNABORTED);
	nvmf_wait_for_reply(&status);

	if (nvmf_cqe_failed(&status.cqe)) {
		dev_err(sc->dip, CE_WARN, "!PROPERTY_SET failed");
		return (EIO);
	}
	return (0);
}

static void
nvmf_shutdown_controller(nvmf_softc_t *sc)
{
	uint64_t cc;
	int error;

	error = nvmf_read_property(sc, NVMF_PROP_CC, 4, &cc);
	if (error != 0) {
		dev_err(sc->dip, CE_WARN, "!Failed to fetch CC for shutdown");
		return;
	}

	/*
	 * Set Shutdown Notification to "normal shutdown".  CC.SHN occupies bits
	 * 15:14 of the Controller Configuration register (nvme_reg_cc_t.cc_shn).
	 */
	{
		nvme_reg_cc_t reg;

		reg.r = (uint32_t)cc;
		reg.b.cc_shn = NVME_CC_SHN_NORMAL;
		cc = reg.r;
	}

	error = nvmf_write_property(sc, NVMF_PROP_CC, 4, cc);
	if (error != 0) {
		dev_err(sc->dip, CE_WARN,
		    "!Failed to set CC to trigger shutdown");
	}
}

/*
 * Keep-Alive tx/rx handlers.  FreeBSD arms two callouts (ka_tx_timer at half
 * the KeepAlive timeout to send a KeepAlive command, ka_rx_timer at the full
 * timeout to detect an actual timeout).  illumos uses timeout(9F); each handler
 * reschedules itself unless the timer has been disarmed (timer id cleared under
 * connection_lock by the disconnect/detach paths).
 */
static clock_t
nvmf_interval_ticks(hrtime_t interval)
{
	clock_t ticks;

	ticks = drv_usectohz((clock_t)(interval / (NANOSEC / MICROSEC)));
	if (ticks < 1)
		ticks = 1;
	return (ticks);
}

static void
nvmf_keep_alive_complete(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_softc_t *sc = arg;

	(void) atomic_swap_uint(&sc->ka_active_rx_traffic, 1);
	if (nvmf_cqe_failed(cqe)) {
		dev_err(sc->dip, CE_WARN,
		    "!KeepAlive response reported error status");
	}
}

void
nvmf_check_keep_alive(void *arg)
{
	nvmf_softc_t *sc = arg;
	uint_t traffic;

	traffic = atomic_swap_uint(&sc->ka_active_rx_traffic, 0);
	if (traffic == 0) {
		dev_err(sc->dip, CE_WARN,
		    "!disconnecting due to KeepAlive timeout");
		nvmf_disconnect(sc);
		return;
	}

	rw_enter(&sc->connection_lock, RW_READER);
	if (sc->ka_rx_timer != 0) {
		sc->ka_rx_timer = timeout(nvmf_check_keep_alive, sc,
		    nvmf_interval_ticks(sc->ka_rx_interval));
	}
	rw_exit(&sc->connection_lock);
}

void
nvmf_send_keep_alive(void *arg)
{
	nvmf_softc_t *sc = arg;
	uint_t traffic;

	/*
	 * Don't bother sending a KeepAlive command if TKAS is active and
	 * another command has been sent during the interval.  Load atomically
	 * (atomic_or with 0 is a non-destructive read) to match FreeBSD's
	 * atomic_load_int and the atomic_swap_uint that clears the flag below;
	 * the field is written from the qpair dispatch path on another CPU.
	 */
	traffic = atomic_or_uint_nv(&sc->ka_active_tx_traffic, 0);
	if (traffic == 0 && !nvmf_cmd_keep_alive(sc, nvmf_keep_alive_complete,
	    sc, KM_NOSLEEP)) {
		dev_err(sc->dip, CE_WARN,
		    "!Failed to allocate KeepAlive command");
	}

	/* Clear ka_active_tx_traffic after sending the keep alive command. */
	(void) atomic_swap_uint(&sc->ka_active_tx_traffic, 0);

	rw_enter(&sc->connection_lock, RW_READER);
	if (sc->ka_tx_timer != 0) {
		sc->ka_tx_timer = timeout(nvmf_send_keep_alive, sc,
		    nvmf_interval_ticks(sc->ka_tx_interval));
	}
	rw_exit(&sc->connection_lock);
}

/*
 * Arm the KeepAlive timers.  Called once when an association is established,
 * before any handler can run (the timer ids start cleared), so no lock is
 * needed to publish the ids.
 */
static void
nvmf_arm_ka_timers(nvmf_softc_t *sc)
{
	if (sc->ka_rx_interval == 0)
		return;
	sc->ka_rx_timer = timeout(nvmf_check_keep_alive, sc,
	    nvmf_interval_ticks(sc->ka_rx_interval));
	sc->ka_tx_timer = timeout(nvmf_send_keep_alive, sc,
	    nvmf_interval_ticks(sc->ka_tx_interval));
}

/*
 * Disarm the KeepAlive timers.  The caller holds connection_lock as a writer.
 * The ids are cleared first so a racing handler (which reschedules only under
 * RW_READER when the id is non-zero) does not re-arm; the lock is then dropped
 * to untimeout() so an in-flight handler can drain.
 */
static void
nvmf_disarm_ka_timers(nvmf_softc_t *sc)
{
	timeout_id_t rx, tx;

	ASSERT(RW_WRITE_HELD(&sc->connection_lock));

	rx = sc->ka_rx_timer;
	tx = sc->ka_tx_timer;
	sc->ka_rx_timer = 0;
	sc->ka_tx_timer = 0;
	rw_exit(&sc->connection_lock);
	if (rx != 0)
		(void) untimeout(rx);
	if (tx != 0)
		(void) untimeout(tx);
	rw_enter(&sc->connection_lock, RW_WRITER);
}

/*
 * Cancel a single timeout(9F) callback whose id is stored in *idp.  The caller
 * holds connection_lock as a writer; the id is cleared before dropping the lock
 * to untimeout() so the handler (which reschedules only when its id is
 * non-zero) cannot re-arm.
 */
static void
nvmf_cancel_timer(nvmf_softc_t *sc, timeout_id_t *idp)
{
	timeout_id_t id;

	ASSERT(RW_WRITE_HELD(&sc->connection_lock));

	id = *idp;
	if (id == 0)
		return;
	*idp = 0;
	rw_exit(&sc->connection_lock);
	(void) untimeout(id);
	rw_enter(&sc->connection_lock, RW_WRITER);
}

/*
 * PORT-TODO (FreeBSD nvmf_copyin_handoff): the kernel nvlist API in illumos
 * does not provide the nvlist_exists_<type>() predicates FreeBSD uses, so this
 * port checks presence via the lookup return code.  It validates the handoff
 * nvlist shape (trtype, admin/io qpair nvlists, cdata, rparams) and returns the
 * unpacked nvlist for nvmf_establish_connection().  The dle/hostnqn subnqn
 * cross-check against cdata is left as a TODO since it requires the binary
 * discovery-log-entry layout.
 */
int
nvmf_copyin_handoff(const struct nvmf_ioc_nv *nv, nvlist_t **nvlp)
{
	nvlist_t *nvl;
	nvlist_t *rparams, *admin;
	nvlist_t **io;
	uchar_t *cdata;
	uint_t num_io_queues, i, cdlen;
	uint64_t qsize, v64;
	char *hostnqn;
	uchar_t *dle;
	uint_t dlelen;
	int error;
	boolean_t admin_flag = B_FALSE;

	error = nvmf_unpack_ioc_nvlist(nv, &nvl);
	if (error != 0)
		return (error);

	if (nvlist_lookup_uint64(nvl, "trtype", &v64) != 0 ||
	    nvlist_lookup_nvlist(nvl, "admin", &admin) != 0 ||
	    nvlist_lookup_nvlist_array(nvl, "io", &io, &num_io_queues) != 0 ||
	    nvlist_lookup_byte_array(nvl, "cdata", &cdata, &cdlen) != 0 ||
	    nvlist_lookup_nvlist(nvl, "rparams", &rparams) != 0)
		goto invalid;

	if (nvlist_lookup_byte_array(rparams, "dle", &dle, &dlelen) != 0 ||
	    nvlist_lookup_string(rparams, "hostnqn", &hostnqn) != 0 ||
	    nvlist_lookup_uint64(rparams, "num_io_queues", &v64) != 0 ||
	    nvlist_lookup_uint64(rparams, "io_qsize", &qsize) != 0)
		goto invalid;

	if (!nvmf_validate_qpair_nvlist(admin, B_FALSE))
		goto invalid;
	if (nvlist_lookup_boolean_value(admin, "admin", &admin_flag) != 0 ||
	    !admin_flag)
		goto invalid;

	if (num_io_queues < 1 || num_io_queues != v64)
		goto invalid;
	for (i = 0; i < num_io_queues; i++) {
		if (!nvmf_validate_qpair_nvlist(io[i], B_FALSE))
			goto invalid;
	}

	/* Require all I/O queues to be the same size. */
	for (i = 0; i < num_io_queues; i++) {
		if (nvlist_lookup_uint64(io[i], "qsize", &v64) != 0 ||
		    v64 != qsize)
			goto invalid;
	}

	if (cdlen != sizeof (nvme_identify_ctrl_t))
		goto invalid;
	if (dlelen != sizeof (nvmf_discovery_log_page_entry_t))
		goto invalid;

	/*
	 * The discovery-log-entry subnqn must match the controller-data subnqn
	 * (FreeBSD: memcmp(dle->subnqn, cdata->subnqn)).  Both NQN fields are
	 * 256-byte (NVMF_NQN_FIELD_SIZE) on-wire strings.
	 */
	{
		const nvmf_discovery_log_page_entry_t *dlep =
		    (const nvmf_discovery_log_page_entry_t *)dle;
		const nvme_identify_ctrl_t *cdp =
		    (const nvme_identify_ctrl_t *)cdata;

		if (memcmp(dlep->ndle_subnqn, cdp->id_subnqn,
		    sizeof (cdp->id_subnqn)) != 0)
			goto invalid;
	}

	*nvlp = nvl;
	return (0);
invalid:
	nvlist_free(nvl);
	return (EINVAL);
}

static int
nvmf_establish_connection(nvmf_softc_t *sc, nvlist_t *nvl)
{
	nvlist_t **io;
	nvlist_t *admin, *rparams;
	uint64_t kato = 0, v64 = 0;
	uint_t num_io_queues, i;
	nvmf_trtype_t trtype;
	uchar_t *cdata;
	uint_t cdlen;
	char name[16];

	(void) nvlist_lookup_uint64(nvl, "trtype", &v64);
	trtype = (nvmf_trtype_t)v64;
	(void) nvlist_lookup_nvlist(nvl, "admin", &admin);
	(void) nvlist_lookup_nvlist_array(nvl, "io", &io, &num_io_queues);
	(void) nvlist_lookup_uint64(nvl, "kato", &kato);
	v64 = 0;
	(void) nvlist_lookup_uint64(nvl, "reconnect_delay", &v64);
	sc->reconnect_delay = (uint32_t)v64;
	v64 = 0;
	(void) nvlist_lookup_uint64(nvl, "controller_loss_timeout", &v64);
	sc->controller_loss_timeout = (uint32_t)v64;

	/* Setup the admin queue. */
	sc->admin = nvmf_init_qp(sc, trtype, admin, "admin queue", 0);
	if (sc->admin == NULL) {
		dev_err(sc->dip, CE_WARN, "!Failed to setup admin queue");
		return (ENXIO);
	}

	/* Setup I/O queues. */
	sc->io = kmem_zalloc(num_io_queues * sizeof (*sc->io), KM_SLEEP);
	sc->num_io_queues = num_io_queues;
	for (i = 0; i < sc->num_io_queues; i++) {
		(void) snprintf(name, sizeof (name), "I/O queue %u", i);
		sc->io[i] = nvmf_init_qp(sc, trtype, io[i], name, i);
		if (sc->io[i] == NULL) {
			dev_err(sc->dip, CE_WARN,
			    "!Failed to setup I/O queue %u", i);
			return (ENXIO);
		}
	}

	/*
	 * Copy the controller data before starting KeepAlive so the TBKAS check
	 * below sees the controller's actual attributes.  (FreeBSD copies cdata
	 * after, relying on the previous association's value; copying first is
	 * equivalent on reconnect and correct on first attach.)
	 */
	if (nvlist_lookup_byte_array(nvl, "cdata", &cdata, &cdlen) == 0 &&
	    cdlen == sizeof (*sc->cdata))
		bcopy(cdata, sc->cdata, sizeof (*sc->cdata));

	/* Start KeepAlive timers. */
	if (kato != 0) {
		/*
		 * Traffic-Based Keep Alive Support (TBKAS): when the controller
		 * supports it (Identify Controller CTRATT.TBKAS), any traffic on
		 * the queue counts as keep-alive activity, so the tx timer can
		 * skip a KeepAlive command if a command was sent in the interval.
		 */
		sc->ka_traffic = sc->cdata->id_ctratt.ctrat_tbkas != 0;
		sc->ka_rx_interval = (hrtime_t)kato * (NANOSEC / MILLISEC);
		sc->ka_tx_interval = sc->ka_rx_interval / 2;
		nvmf_arm_ka_timers(sc);
	}

	/* Save reconnect parameters. */
	nvlist_free(sc->rparams);
	sc->rparams = NULL;
	if (nvlist_lookup_nvlist(nvl, "rparams", &rparams) == 0)
		(void) nvlist_dup(rparams, &sc->rparams, KM_SLEEP);

	return (0);
}

typedef boolean_t nvmf_scan_active_ns_cb(nvmf_softc_t *, uint32_t,
    const nvme_identify_nsid_t *, void *);

static boolean_t
nvmf_scan_active_nslist(nvmf_softc_t *sc, nvme_identify_nsid_list_t *nslist,
    nvme_identify_nsid_t *data, uint32_t *nsidp, nvmf_scan_active_ns_cb *cb,
    void *cb_arg)
{
	nvmf_completion_status_t status;
	uint32_t nsid;
	uint_t i, n;

	nvmf_status_init(&status);
	nvmf_status_wait_io(&status);
	if (!nvmf_cmd_identify_active_namespaces(sc, *nsidp, nslist,
	    nvmf_complete, &status, nvmf_io_complete, &status, KM_SLEEP)) {
		dev_err(sc->dip, CE_WARN,
		    "!failed to send IDENTIFY active namespaces command");
		return (B_FALSE);
	}
	nvmf_wait_for_reply(&status);

	if (nvmf_cqe_failed(&status.cqe)) {
		dev_err(sc->dip, CE_WARN,
		    "!IDENTIFY active namespaces failed");
		return (B_FALSE);
	}

	if (status.io_error != 0) {
		dev_err(sc->dip, CE_WARN,
		    "!IDENTIFY active namespaces failed with I/O error %d",
		    status.io_error);
		return (B_FALSE);
	}

	n = sizeof (nslist->nl_nsid) / sizeof (nslist->nl_nsid[0]);
	for (i = 0; i < n; i++) {
		nsid = nslist->nl_nsid[i];
		if (nsid == 0) {
			*nsidp = 0;
			return (B_TRUE);
		}

		nvmf_status_init(&status);
		nvmf_status_wait_io(&status);
		if (!nvmf_cmd_identify_namespace(sc, nsid, data, nvmf_complete,
		    &status, nvmf_io_complete, &status, KM_SLEEP)) {
			dev_err(sc->dip, CE_WARN,
			    "!failed to send IDENTIFY namespace %u command",
			    nsid);
			return (B_FALSE);
		}
		nvmf_wait_for_reply(&status);

		if (nvmf_cqe_failed(&status.cqe)) {
			dev_err(sc->dip, CE_WARN,
			    "!IDENTIFY namespace %u failed", nsid);
			return (B_FALSE);
		}

		if (status.io_error != 0) {
			dev_err(sc->dip, CE_WARN,
			    "!IDENTIFY namespace %u failed with I/O error %d",
			    nsid, status.io_error);
			return (B_FALSE);
		}

		/*
		 * PORT-TODO (FreeBSD nvme_namespace_data_swapbytes): illumos
		 * is little-endian and the on-wire Identify data is
		 * little-endian, so no byteswap is required here; the FreeBSD
		 * swap call is intentionally dropped.
		 */
		if (!cb(sc, nsid, data, cb_arg))
			return (B_FALSE);
	}

	ASSERT(nsid == nslist->nl_nsid[n - 1] && nsid != 0);

	if (nsid >= NVME_NSID_BCAST - 1)
		*nsidp = 0;
	else
		*nsidp = nsid;
	return (B_TRUE);
}

static boolean_t
nvmf_scan_active_namespaces(nvmf_softc_t *sc, nvmf_scan_active_ns_cb *cb,
    void *cb_arg)
{
	nvme_identify_nsid_t *data;
	nvme_identify_nsid_list_t *nslist;
	uint32_t nsid;
	boolean_t retval;

	nslist = kmem_alloc(sizeof (*nslist), KM_SLEEP);
	data = kmem_alloc(sizeof (*data), KM_SLEEP);

	nsid = 0;
	retval = B_TRUE;
	for (;;) {
		if (!nvmf_scan_active_nslist(sc, nslist, data, &nsid, cb,
		    cb_arg)) {
			retval = B_FALSE;
			break;
		}
		if (nsid == 0)
			break;
	}

	kmem_free(data, sizeof (*data));
	kmem_free(nslist, sizeof (*nslist));
	return (retval);
}

static boolean_t
nvmf_add_ns(nvmf_softc_t *sc, uint32_t nsid, const nvme_identify_nsid_t *data,
    void *arg)
{
	_NOTE(ARGUNUSED(arg));

	/*
	 * sc->ns is sized cdata->id_nn and indexed by nsid - 1.  A target that
	 * reports an out-of-range nsid must not be allowed to index past the
	 * array; skip it with a warning rather than corrupting the heap.
	 */
	if (nsid < 1 || nsid > sc->cdata->id_nn) {
		dev_err(sc->dip, CE_WARN,
		    "!ignoring out-of-range namespace %u in active list", nsid);
		return (B_TRUE);
	}

	if (sc->ns[nsid - 1] != NULL) {
		dev_err(sc->dip, CE_WARN,
		    "!duplicate namespace %u in active namespace list", nsid);
		return (B_FALSE);
	}

	/*
	 * As in nvme_ns_construct, a size of zero indicates an invalid
	 * namespace.
	 */
	if (data->id_nsize == 0) {
		dev_err(sc->dip, CE_NOTE,
		    "!ignoring active namespace %u with zero size", nsid);
		return (B_TRUE);
	}

	sc->ns[nsid - 1] = nvmf_init_ns(sc, nsid, data);

	nvmf_bd_rescan_ns(sc, nsid);
	return (B_TRUE);
}

static boolean_t
nvmf_add_namespaces(nvmf_softc_t *sc)
{
	sc->ns = kmem_zalloc(sc->cdata->id_nn * sizeof (*sc->ns), KM_SLEEP);
	return (nvmf_scan_active_namespaces(sc, nvmf_add_ns, NULL));
}

void
nvmf_disconnect(nvmf_softc_t *sc)
{
	/*
	 * nvmf_disconnect() may be called from a timeout(9F) handler (the
	 * KeepAlive rx timer) or a transport error callback, so the dispatch
	 * must not sleep.  On the rare allocation failure the disconnect is
	 * retried by the next event (KeepAlive timeout or transport error).
	 */
	if (ddi_taskq_dispatch(nvmf_tq, nvmf_disconnect_task, sc,
	    DDI_NOSLEEP) != DDI_SUCCESS) {
		dev_err(sc->dip, CE_WARN,
		    "!failed to dispatch disconnect task");
	}
}

/*
 * Request a reconnect: notify userland (which re-establishes the association
 * and calls back in via NVMF_RECONNECT_HOST) and re-arm the request timer so
 * the notification repeats until userland succeeds.  Caller holds
 * connection_lock as a writer.
 */
static void
nvmf_request_reconnect(nvmf_softc_t *sc)
{
	ASSERT(RW_WRITE_HELD(&sc->connection_lock));

	/*
	 * FreeBSD posts a devctl "nvme controller RECONNECT" event here.  On
	 * illumos the userland nvmf daemon polls NVMF_CONNECTION_STATUS, so we
	 * simply (re-)arm the request timer; the daemon observes the
	 * disconnected state and drives NVMF_RECONNECT_HOST.
	 */
	if (sc->reconnect_timer == 0) {
		sc->reconnect_timer = timeout(nvmf_request_reconnect_task, sc,
		    drv_usectohz((clock_t)sc->reconnect_delay * MICROSEC));
	}
}

/*
 * The disconnect path is the keep-alive-to-path seam (NVMEOF.md 9.3): on a
 * transport error or keep-alive timeout it marks this association's paths down
 * (without tearing down any bd_handle), quiesces namespace consumers, tears
 * down only this association's qpairs, then arms the reconnect and
 * controller-loss timers.
 */
void
nvmf_disconnect_task(void *arg)
{
	nvmf_softc_t *sc = arg;
	uint_t i;

	rw_enter(&sc->connection_lock, RW_WRITER);
	if (sc->admin == NULL) {
		/* Ignore transport errors if there is no active association. */
		rw_exit(&sc->connection_lock);
		return;
	}

	if (sc->detaching) {
		/*
		 * admin is non-NULL (checked above under this same writer lock).
		 * This unsticks the detach process if a transport error occurs
		 * during detach.
		 */
		nvmf_shutdown_qp(sc->admin);
		rw_exit(&sc->connection_lock);
		return;
	}

	if (!sc->cdev_attached) {
		/*
		 * Transport error occurred during attach
		 * (nvmf_add_namespaces).  Shutdown the admin queue.
		 */
		nvmf_shutdown_qp(sc->admin);
		rw_exit(&sc->connection_lock);
		return;
	}

	gethrestime(&sc->last_disconnect);
	nvmf_disarm_ka_timers(sc);
	sc->ka_traffic = B_FALSE;

	/* Quiesce namespace consumers. */
	nvmf_disconnect_bd(sc);
	for (i = 0; i < sc->cdata->id_nn; i++) {
		if (sc->ns[i] != NULL)
			nvmf_disconnect_ns(sc->ns[i]);
	}

	/* Shutdown the existing qpairs. */
	for (i = 0; i < sc->num_io_queues; i++)
		nvmf_destroy_qp(sc->io[i]);
	kmem_free(sc->io, sc->num_io_queues * sizeof (*sc->io));
	sc->io = NULL;
	sc->num_io_queues = 0;
	nvmf_destroy_qp(sc->admin);
	sc->admin = NULL;

	if (sc->reconnect_delay != 0)
		nvmf_request_reconnect(sc);
	if (sc->controller_loss_timeout != 0 && sc->loss_timer == 0) {
		sc->loss_timer = timeout(nvmf_controller_loss_task, sc,
		    drv_usectohz((clock_t)sc->controller_loss_timeout *
		    MICROSEC));
	}

	rw_exit(&sc->connection_lock);
}

/*
 * Controller-loss timer fired: the association did not reconnect within
 * controller_loss_timeout seconds.  Mark the controller as timed out and tear
 * down its blkdev presentation (the paths and their heads).  FreeBSD detaches
 * the device_t; here the softc persists but its namespaces are removed.
 */
static void
nvmf_controller_loss_task(void *arg)
{
	nvmf_softc_t *sc = arg;
	uint_t i;

	rw_enter(&sc->connection_lock, RW_WRITER);
	sc->loss_timer = 0;
	if (sc->admin != NULL || sc->detaching) {
		/* Reconnected or already detaching. */
		rw_exit(&sc->connection_lock);
		return;
	}

	sc->controller_timedout = B_TRUE;

	/*
	 * Remove the namespaces (and their paths/heads).  Any further I/O fails
	 * because the paths are gone; the bd_handle is detached on last-path
	 * removal inside the multipath layer.
	 */
	if (sc->ns != NULL) {
		for (i = 0; i < sc->cdata->id_nn; i++) {
			if (sc->ns[i] != NULL) {
				nvmf_destroy_ns(sc->ns[i]);
				sc->ns[i] = NULL;
			}
		}
	}
	nvmf_shutdown_bd(sc);

	rw_exit(&sc->connection_lock);
}

/*
 * Reconnect-request timer fired: if still disconnected, re-post the reconnect
 * request so userland keeps trying.
 */
static void
nvmf_request_reconnect_task(void *arg)
{
	nvmf_softc_t *sc = arg;

	rw_enter(&sc->connection_lock, RW_WRITER);
	sc->reconnect_timer = 0;
	if (sc->admin != NULL || sc->detaching || sc->controller_timedout) {
		/* Reconnected or already detaching. */
		rw_exit(&sc->connection_lock);
		return;
	}

	nvmf_request_reconnect(sc);
	rw_exit(&sc->connection_lock);
}

/*
 * Reconcile a single namespace against freshly fetched Identify data: a zero
 * size means the namespace went away (destroy it); otherwise create it if new
 * or update it in place, destroying it if the update is incompatible.
 */
static void
nvmf_rescan_ns_1(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data)
{
	struct nvmf_namespace *ns;

	ASSERT(RW_WRITE_HELD(&sc->connection_lock));

	/*
	 * A target-supplied nsid (changed-namespace log page or active-ns list)
	 * indexes sc->ns, which is sized cdata->id_nn.  Drop anything out of
	 * range or any rescan that raced a teardown freeing sc->ns.
	 */
	if (sc->detaching || sc->ns == NULL)
		return;
	if (nsid < 1 || nsid > sc->cdata->id_nn) {
		dev_err(sc->dip, CE_WARN, "!ignoring out-of-range namespace %u",
		    nsid);
		return;
	}

	ns = sc->ns[nsid - 1];
	if (data->id_nsize == 0) {
		if (ns != NULL) {
			nvmf_destroy_ns(ns);
			sc->ns[nsid - 1] = NULL;
		}
	} else {
		if (ns == NULL) {
			sc->ns[nsid - 1] = nvmf_init_ns(sc, nsid, data);
		} else if (!nvmf_update_ns(ns, data)) {
			nvmf_destroy_ns(ns);
			sc->ns[nsid - 1] = NULL;
		}
	}

	nvmf_bd_rescan_ns(sc, nsid);
}

void
nvmf_rescan_ns(nvmf_softc_t *sc, uint32_t nsid)
{
	nvmf_completion_status_t status;
	nvme_identify_nsid_t *data;

	data = kmem_alloc(sizeof (*data), KM_SLEEP);

	nvmf_status_init(&status);
	nvmf_status_wait_io(&status);
	if (!nvmf_cmd_identify_namespace(sc, nsid, data, nvmf_complete,
	    &status, nvmf_io_complete, &status, KM_SLEEP)) {
		dev_err(sc->dip, CE_WARN,
		    "!failed to send IDENTIFY namespace %u command", nsid);
		kmem_free(data, sizeof (*data));
		return;
	}
	nvmf_wait_for_reply(&status);

	if (nvmf_cqe_failed(&status.cqe) || status.io_error != 0) {
		dev_err(sc->dip, CE_WARN, "!IDENTIFY namespace %u failed",
		    nsid);
		kmem_free(data, sizeof (*data));
		return;
	}

	/*
	 * IDENTIFY is issued without connection_lock (it sleeps for the reply);
	 * take the writer lock only across the sc->ns[] mutation, which is the
	 * serialization the namespace objects require (see nvmf_ns.c).  The AER
	 * taskq is the sole caller, so the lock is never already held here.
	 */
	rw_enter(&sc->connection_lock, RW_WRITER);
	nvmf_rescan_ns_1(sc, nsid, data);
	rw_exit(&sc->connection_lock);

	kmem_free(data, sizeof (*data));
}

/*
 * Destroy any namespaces in the range [first_nsid, next_valid_nsid) that no
 * longer appear in the active namespace list (gaps left by removed
 * namespaces).
 */
static void
nvmf_purge_namespaces(nvmf_softc_t *sc, uint32_t first_nsid,
    uint32_t next_valid_nsid)
{
	struct nvmf_namespace *ns;
	uint32_t nsid;

	ASSERT(RW_WRITE_HELD(&sc->connection_lock));

	if (sc->detaching || sc->ns == NULL)
		return;

	/*
	 * next_valid_nsid is derived from the target's active-namespace list and
	 * is otherwise untrusted; sc->ns is sized cdata->id_nn, so clamp the
	 * range here -- this is the single choke point both purge call sites flow
	 * through -- so a target reporting an out-of-range nsid cannot drive an
	 * out-of-bounds sc->ns[] access.
	 */
	if (first_nsid < 1)
		return;
	if (next_valid_nsid > sc->cdata->id_nn + 1)
		next_valid_nsid = sc->cdata->id_nn + 1;

	for (nsid = first_nsid; nsid < next_valid_nsid; nsid++) {
		ns = sc->ns[nsid - 1];
		if (ns != NULL) {
			nvmf_destroy_ns(ns);
			sc->ns[nsid - 1] = NULL;
			nvmf_bd_rescan_ns(sc, nsid);
		}
	}
}

/*
 * Context threaded through the active-namespace scan.  'locked' records whether
 * the caller already holds connection_lock as a writer (the reconnect path) so
 * the per-mutation sections do not recursively enter the non-recursive rwlock.
 */
typedef struct nvmf_rescan_ctx {
	uint32_t	last_nsid;
	boolean_t	locked;
} nvmf_rescan_ctx_t;

static boolean_t
nvmf_rescan_ns_cb(nvmf_softc_t *sc, uint32_t nsid,
    const nvme_identify_nsid_t *data, void *arg)
{
	nvmf_rescan_ctx_t *ctx = arg;

	/*
	 * The scan issues IDENTIFY between callbacks, so the writer lock cannot
	 * span the whole scan; take it only across the sc->ns[] mutations here
	 * (unless the caller already holds it).
	 */
	if (!ctx->locked)
		rw_enter(&sc->connection_lock, RW_WRITER);

	/* Check for any gaps prior to this namespace. */
	nvmf_purge_namespaces(sc, ctx->last_nsid + 1, nsid);
	ctx->last_nsid = nsid;

	nvmf_rescan_ns_1(sc, nsid, data);

	if (!ctx->locked)
		rw_exit(&sc->connection_lock);
	return (B_TRUE);
}

/*
 * Reconcile the full namespace set against the target's active-namespace list.
 * The scan issues IDENTIFY (which sleeps for its reply); 'locked' tells whether
 * the caller already holds connection_lock as a writer.  Either way the
 * sc->ns[] mutations run under the writer lock (see nvmf_ns.c).
 */
static void
nvmf_rescan_all_ns_impl(nvmf_softc_t *sc, boolean_t locked)
{
	nvmf_rescan_ctx_t ctx = { .last_nsid = 0, .locked = locked };

	if (!nvmf_scan_active_namespaces(sc, nvmf_rescan_ns_cb, &ctx))
		return;

	/*
	 * Check for any namespace devices after the last active namespace.
	 */
	if (!locked)
		rw_enter(&sc->connection_lock, RW_WRITER);
	nvmf_purge_namespaces(sc, ctx.last_nsid + 1, sc->cdata->id_nn + 1);
	if (!locked)
		rw_exit(&sc->connection_lock);
}

/* AER taskq entry point: the caller does NOT hold connection_lock. */
void
nvmf_rescan_all_ns(nvmf_softc_t *sc)
{
	ASSERT(!RW_WRITE_HELD(&sc->connection_lock));
	nvmf_rescan_all_ns_impl(sc, B_FALSE);
}

/* Reconnect entry point: the caller already holds connection_lock as writer. */
static void
nvmf_rescan_all_ns_locked(nvmf_softc_t *sc)
{
	ASSERT(RW_WRITE_HELD(&sc->connection_lock));
	nvmf_rescan_all_ns_impl(sc, B_TRUE);
}

/*
 * ----------------------------------------------------------------------------
 * DDI device-model glue (rebound from FreeBSD newbus).
 *
 * FreeBSD models each association as a newbus device_t whose attach establishes
 * the connection.  illumos uses one nvmf_host pseudo-nexus instance per
 * controller association: DDI_ATTACH brings up the softc and its control minor
 * node WITHOUT an association, and userland establishes the connection later via
 * the NVMF_HANDOFF_HOST ioctl (which runs the ported FreeBSD nvmf_attach body in
 * nvmf_attach_association()).  This matches the userland/kernel split described
 * in NVMEOF.md section 5.2.
 * ----------------------------------------------------------------------------
 */

static void *nvmf_state;

/*
 * Run the ported FreeBSD nvmf_attach() body: establish the admin + I/O qpairs,
 * read CAP/VS, compute the maximum transfer size from MDTS and the transport's
 * limit, bind blkdev, start AER, and enumerate namespaces.  Called with the
 * connection NOT yet established (sc->admin == NULL).
 */
static int
nvmf_attach_association(nvmf_softc_t *sc, nvlist_t *nvl)
{
	nvlist_t **io;
	uchar_t *cdata;
	uint_t cdlen, num_io_queues, i;
	uint64_t mpsmin, val, v64 = 0;
	nvme_reg_cap_t cap;
	boolean_t bd_inited = B_FALSE;
	int error;

	/*
	 * Populate cdata before nvmf_init_aer(), which sizes the AER pool from
	 * cdata->id_aerl, and before nvmf_establish_connection() builds the
	 * admin qpair (which reserves sc->num_aer spare command slots).
	 */
	if (nvlist_lookup_byte_array(nvl, "cdata", &cdata, &cdlen) != 0 ||
	    cdlen != sizeof (*sc->cdata))
		return (EINVAL);
	bcopy(cdata, sc->cdata, sizeof (*sc->cdata));

	nvmf_init_aer(sc);

	error = nvmf_establish_connection(sc, nvl);
	if (error != 0)
		goto out;

	error = nvmf_read_property(sc, NVMF_PROP_CAP, 8, &sc->cap);
	if (error != 0) {
		dev_err(sc->dip, CE_WARN, "!Failed to fetch CAP");
		error = ENXIO;
		goto out;
	}

	error = nvmf_read_property(sc, NVMF_PROP_VS, 4, &val);
	if (error != 0) {
		dev_err(sc->dip, CE_WARN, "!Failed to fetch VS");
		error = ENXIO;
		goto out;
	}
	sc->vs = (uint32_t)val;

	/*
	 * Honor MDTS if it is set.  The minimum memory page size is
	 * 2^(12 + CAP.MPSMIN) (NVMe base spec; FreeBSD NVME_MPS_SHIFT == 12),
	 * and MDTS is expressed in units of that page size.
	 */
	cap.r = sc->cap;
	mpsmin = (uint64_t)1 << (NVME_MPS_SHIFT + cap.b.cap_mpsmin);
	sc->max_xfer_size = maxphys;
	if (sc->cdata->id_mdts != 0) {
		sc->max_xfer_size = MIN(sc->max_xfer_size,
		    mpsmin << sc->cdata->id_mdts);
	}

	/* Honor any transfer size restriction imposed by the transport. */
	val = nvmf_max_xfer_size_qp(sc->io[0]);
	if (val >= mpsmin) {
		sc->max_xfer_size = MIN(sc->max_xfer_size,
		    P2ALIGN(val, mpsmin));
	}

	(void) nvlist_lookup_nvlist_array(nvl, "io", &io, &num_io_queues);
	(void) nvlist_lookup_uint64(io[0], "qsize", &v64);
	sc->max_pending_io = (uint_t)v64 * sc->num_io_queues;

	error = nvmf_init_bd(sc);
	if (error != 0)
		goto out;
	bd_inited = B_TRUE;

	error = nvmf_start_aer(sc);
	if (error != 0)
		goto out;

	if (!nvmf_add_namespaces(sc)) {
		error = ENXIO;
		goto out;
	}

	sc->cdev_attached = B_TRUE;
	return (0);
out:
	/*
	 * Destroy the namespaces BEFORE the multipath/blkdev state: each
	 * nvmf_destroy_ns() removes and frees its own path via
	 * nvmf_mpath_remove_path(), so nvmf_destroy_bd() (which also walks
	 * sc->paths and frees them) must run afterwards on an already-empty
	 * list.  Doing it the other way around double-frees ns->path.
	 */
	if (sc->ns != NULL) {
		for (i = 0; i < sc->cdata->id_nn; i++) {
			if (sc->ns[i] != NULL)
				nvmf_destroy_ns(sc->ns[i]);
		}
		kmem_free(sc->ns, sc->cdata->id_nn * sizeof (*sc->ns));
		sc->ns = NULL;
	}

	if (bd_inited)
		nvmf_destroy_bd(sc);

	rw_enter(&sc->connection_lock, RW_WRITER);
	nvmf_disarm_ka_timers(sc);
	rw_exit(&sc->connection_lock);

	if (sc->admin != NULL)
		nvmf_shutdown_controller(sc);

	for (i = 0; i < sc->num_io_queues; i++) {
		if (sc->io[i] != NULL)
			nvmf_destroy_qp(sc->io[i]);
	}
	if (sc->io != NULL) {
		kmem_free(sc->io, sc->num_io_queues * sizeof (*sc->io));
		sc->io = NULL;
		sc->num_io_queues = 0;
	}
	if (sc->admin != NULL) {
		nvmf_destroy_qp(sc->admin);
		sc->admin = NULL;
	}

	nvmf_destroy_aer(sc);

	nvlist_free(sc->rparams);
	sc->rparams = NULL;
	return (error);
}

/*
 * Re-establish a previously-lost association (FreeBSD nvmf_reconnect_host).
 * Userland supplies new qpair parameters in the handoff nvlist; the controller
 * must be the same NVM subsystem (matching subnqn).
 */
static int
nvmf_reconnect_host(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl;
	uchar_t *cdata;
	uint_t cdlen, i;
	uint64_t v64 = 0;
	int error;

	error = nvmf_copyin_handoff(nv, &nvl);
	if (error != 0)
		return (error);

	/* XXX: Should we permit changing the transport type? */
	(void) nvlist_lookup_uint64(nvl, "trtype", &v64);
	if (sc->trtype != (nvmf_trtype_t)v64) {
		dev_err(sc->dip, CE_WARN,
		    "!transport type mismatch on reconnect");
		nvlist_free(nvl);
		return (EINVAL);
	}

	rw_enter(&sc->connection_lock, RW_WRITER);
	if (sc->admin != NULL || sc->detaching || sc->controller_timedout) {
		error = EBUSY;
		goto out;
	}

	/*
	 * Ensure this is for the same controller.  The controller ID can vary
	 * across associations under the dynamic controller model, so match on
	 * the subsystem NQN carried in the controller data.
	 */
	if (nvlist_lookup_byte_array(nvl, "cdata", &cdata, &cdlen) != 0 ||
	    cdlen != sizeof (*sc->cdata)) {
		error = EINVAL;
		goto out;
	}
	if (memcmp(sc->cdata->id_subnqn,
	    ((const nvme_identify_ctrl_t *)cdata)->id_subnqn,
	    sizeof (sc->cdata->id_subnqn)) != 0) {
		dev_err(sc->dip, CE_WARN,
		    "!controller subsystem NQN mismatch on reconnect");
		error = EINVAL;
		goto out;
	}

	error = nvmf_establish_connection(sc, nvl);
	if (error != 0)
		goto fail;

	error = nvmf_start_aer(sc);
	if (error != 0)
		goto fail;

	dev_err(sc->dip, CE_NOTE,
	    "!established new association with %u I/O queues",
	    sc->num_io_queues);

	/* Restart namespace consumers. */
	for (i = 0; i < sc->cdata->id_nn; i++) {
		if (sc->ns[i] != NULL)
			nvmf_reconnect_ns(sc->ns[i]);
	}
	nvmf_reconnect_bd(sc);

	/*
	 * The caller already holds connection_lock as a writer here, so the
	 * rescan must run in its already-locked mode (it would otherwise
	 * recursively enter the non-recursive rwlock).  The IDENTIFY commands
	 * run on this thread under the lock, exactly as before this fix.
	 */
	nvmf_rescan_all_ns_locked(sc);

	nvmf_cancel_timer(sc, &sc->reconnect_timer);
	nvmf_cancel_timer(sc, &sc->loss_timer);
	rw_exit(&sc->connection_lock);
	nvlist_free(nvl);
	return (0);
fail:
	/*
	 * A partial reconnect leaves a half-built association: sc->admin set
	 * with a possible NULL hole in sc->io[] (the failed nvmf_init_qp), and,
	 * if nvmf_establish_connection() ran to completion before
	 * nvmf_start_aer() failed, armed KeepAlive timers.  Unwind it back to
	 * the clean disconnected state the disconnect task produces, so the
	 * EBUSY guard above clears and a later reconnect can retry.  Reached
	 * only after nvmf_establish_connection(), so sc->admin was set by this
	 * call (it is NULL on entry past the EBUSY guard); the pre-build EBUSY/
	 * EINVAL errors jump straight to out: and must not tear anything down.
	 *
	 * Mirror the disconnect-task teardown ordering: destroy the I/O qpairs
	 * (skipping the hole), free sc->io, then destroy the admin qpair.  Leave
	 * the namespaces in their already-disconnected state, leave the
	 * persistent AER pool intact (freed only at attach failure or detach),
	 * leave sc->rparams (valid reconnect params either way), and leave the
	 * reconnect/loss timers running so userland keeps retrying.  Do not run
	 * nvmf_shutdown_controller(): this is lost-association cleanup, not a
	 * graceful controller shutdown.
	 *
	 * Unpublish sc->admin / sc->io before nvmf_disarm_ka_timers() drops the
	 * lock to untimeout(), so a draining KeepAlive handler cannot reach a
	 * qpair that is about to be destroyed.
	 */
	{
		struct nvmf_host_qpair *admin = sc->admin;
		struct nvmf_host_qpair **io = sc->io;
		uint_t nio = sc->num_io_queues;

		sc->admin = NULL;
		sc->io = NULL;
		sc->num_io_queues = 0;
		sc->ka_traffic = B_FALSE;

		nvmf_disarm_ka_timers(sc);

		for (i = 0; i < nio; i++) {
			if (io[i] != NULL)
				nvmf_destroy_qp(io[i]);
		}
		if (io != NULL)
			kmem_free(io, nio * sizeof (*io));
		if (admin != NULL)
			nvmf_destroy_qp(admin);
	}
out:
	rw_exit(&sc->connection_lock);
	nvlist_free(nvl);
	return (error);
}

/*
 * NVMF_RECONNECT_PARAMS: copy out the saved reconnect parameters so userland
 * can re-establish this association.
 */
static int
nvmf_reconnect_params(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	int error;

	rw_enter(&sc->connection_lock, RW_READER);
	if (sc->rparams == NULL) {
		rw_exit(&sc->connection_lock);
		return (ENXIO);
	}
	error = nvmf_pack_ioc_nvlist(sc->rparams, nv);
	rw_exit(&sc->connection_lock);

	return (error);
}

/*
 * NVMF_CONNECTION_STATUS: report whether the association is currently connected
 * and the time of the last disconnect.
 */
static int
nvmf_connection_status(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl, *nvl_ts;
	int error;

	nvl = fnvlist_alloc();
	nvl_ts = fnvlist_alloc();

	rw_enter(&sc->connection_lock, RW_READER);
	fnvlist_add_boolean_value(nvl, "connected", sc->admin != NULL);
	fnvlist_add_uint64(nvl_ts, "tv_sec", sc->last_disconnect.tv_sec);
	fnvlist_add_uint64(nvl_ts, "tv_nsec", sc->last_disconnect.tv_nsec);
	rw_exit(&sc->connection_lock);
	fnvlist_add_nvlist(nvl, "last_disconnect", nvl_ts);
	fnvlist_free(nvl_ts);

	error = nvmf_pack_ioc_nvlist(nvl, nv);
	fnvlist_free(nvl);
	return (error);
}

/*
 * Copy a fixed-width, space/NUL-padded controller string (an NVMe Identify field
 * or a discovery-log-entry field) into the nvlist as a trimmed C string.
 */
static void
nvmf_list_add_padded(nvlist_t *nvl, const char *key, const char *src,
    size_t srclen)
{
	char buf[260];
	size_t n = MIN(srclen, sizeof (buf) - 1);
	size_t i;

	for (i = 0; i < n && src[i] != '\0'; i++)
		buf[i] = src[i];
	while (i > 0 && buf[i - 1] == ' ')
		i--;
	buf[i] = '\0';
	fnvlist_add_string(nvl, key, buf);
}

/*
 * NVMF_LIST_CONTROLLER: return one nvlist describing this host controller --
 * connection state, the target it is connected to (from the cached reconnect
 * params), the controller identity (Identify Controller), and an array of its
 * namespaces.  Userland (nvmf-connect list) formats it.
 */
static int
nvmf_list_controller(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl;
	int error;

	nvl = fnvlist_alloc();

	rw_enter(&sc->connection_lock, RW_READER);

	/*
	 * While detaching, the namespace array and reconnect params are being
	 * (or are about to be) freed by nvmf_teardown_association(); report a
	 * bare disconnected controller rather than racing that teardown.  All
	 * dynamic reads below are safe because setting sc->detaching takes the
	 * lock as a writer, so it cannot start while we hold it as a reader.
	 */
	if (sc->detaching) {
		fnvlist_add_boolean_value(nvl, "connected", B_FALSE);
		rw_exit(&sc->connection_lock);
		goto pack;
	}

	fnvlist_add_boolean_value(nvl, "connected", sc->admin != NULL);
	fnvlist_add_uint32(nvl, "num_io_queues", sc->num_io_queues);

	nvmf_list_add_padded(nvl, "model", sc->cdata->id_model,
	    sizeof (sc->cdata->id_model));
	nvmf_list_add_padded(nvl, "serial", sc->cdata->id_serial,
	    sizeof (sc->cdata->id_serial));
	nvmf_list_add_padded(nvl, "firmware", sc->cdata->id_fwrev,
	    sizeof (sc->cdata->id_fwrev));

	if (sc->rparams != NULL) {
		uchar_t *dlebytes;
		uint_t dlelen;
		char *hostnqn;

		if (nvlist_lookup_byte_array(sc->rparams, "dle", &dlebytes,
		    &dlelen) == 0 &&
		    dlelen >= sizeof (nvmf_discovery_log_page_entry_t)) {
			const nvmf_discovery_log_page_entry_t *dle =
			    (const nvmf_discovery_log_page_entry_t *)dlebytes;

			nvmf_list_add_padded(nvl, "subnqn",
			    (const char *)dle->ndle_subnqn,
			    sizeof (dle->ndle_subnqn));
			nvmf_list_add_padded(nvl, "traddr",
			    (const char *)dle->ndle_traddr,
			    sizeof (dle->ndle_traddr));
			nvmf_list_add_padded(nvl, "trsvcid",
			    (const char *)dle->ndle_trsvcid,
			    sizeof (dle->ndle_trsvcid));
			fnvlist_add_uint32(nvl, "trtype", dle->ndle_trtype);
		}
		if (nvlist_lookup_string(sc->rparams, "hostnqn", &hostnqn) == 0)
			fnvlist_add_string(nvl, "hostnqn", hostnqn);
	}

	if (sc->ns != NULL && sc->cdata->id_nn > 0) {
		nvlist_t **nsarr;
		uint_t cap = sc->cdata->id_nn;
		uint_t cnt = 0;
		uint32_t i;

		nsarr = kmem_zalloc(cap * sizeof (nvlist_t *), KM_SLEEP);
		for (i = 0; i < cap; i++) {
			nvlist_t *n;
			uint32_t nsid, blksize;
			uint64_t size;
			uint8_t nguid[16], eui64[8];
			boolean_t nsconn;

			if (sc->ns[i] == NULL)
				continue;
			nvmf_ns_get_info(sc->ns[i], &nsid, &size, &blksize,
			    nguid, eui64, &nsconn);
			n = fnvlist_alloc();
			fnvlist_add_uint32(n, "nsid", nsid);
			fnvlist_add_uint64(n, "size", size);
			fnvlist_add_uint32(n, "blksize", blksize);
			fnvlist_add_boolean_value(n, "connected", nsconn);
			fnvlist_add_byte_array(n, "nguid", nguid,
			    sizeof (nguid));
			fnvlist_add_byte_array(n, "eui64", eui64,
			    sizeof (eui64));
			nsarr[cnt++] = n;
		}
		if (cnt > 0)
			fnvlist_add_nvlist_array(nvl, "namespaces", nsarr, cnt);
		for (i = 0; i < cnt; i++)
			fnvlist_free(nsarr[i]);
		kmem_free(nsarr, cap * sizeof (nvlist_t *));
	}
	rw_exit(&sc->connection_lock);

pack:
	error = nvmf_pack_ioc_nvlist(nvl, nv);
	fnvlist_free(nvl);
	return (error);
}

/*
 * Establish the initial association from a copied-in handoff nvlist.  The
 * connection_lock is taken so the disconnect/reconnect state machine sees a
 * consistent view.
 */
static int
nvmf_handoff_host(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl;
	uint64_t v64 = 0;
	int error;

	error = nvmf_copyin_handoff(nv, &nvl);
	if (error != 0)
		return (error);

	rw_enter(&sc->connection_lock, RW_WRITER);
	if (sc->admin != NULL || sc->cdev_attached) {
		rw_exit(&sc->connection_lock);
		nvlist_free(nvl);
		return (EBUSY);
	}
	(void) nvlist_lookup_uint64(nvl, "trtype", &v64);
	sc->trtype = (nvmf_trtype_t)v64;
	rw_exit(&sc->connection_lock);

	error = nvmf_attach_association(sc, nvl);
	nvlist_free(nvl);
	return (error);
}

static int
nvmf_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	nvmf_softc_t *sc;
	int instance;

	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(nvmf_state, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	sc = ddi_get_soft_state(nvmf_state, instance);

	sc->dip = dip;
	rw_init(&sc->connection_lock, NULL, RW_DRIVER, NULL);
	sc->cdata = kmem_zalloc(sizeof (*sc->cdata), KM_SLEEP);
	gethrestime(&sc->last_disconnect);

	/*
	 * Create the control minor node.  Userland issues NVMF_HANDOFF_HOST and
	 * the per-association ioctls (NVMF_RECONNECT_*, NVMF_CONNECTION_STATUS)
	 * against it.  The minor is the instance so getinfo/ioctl can recover
	 * the softc.
	 */
	if (ddi_create_minor_node(dip, "nvmf", S_IFCHR, instance,
	    DDI_PSEUDO, 0) != DDI_SUCCESS) {
		kmem_free(sc->cdata, sizeof (*sc->cdata));
		rw_destroy(&sc->connection_lock);
		ddi_soft_state_free(nvmf_state, instance);
		return (DDI_FAILURE);
	}

	ddi_report_dev(dip);
	return (DDI_SUCCESS);
}

/*
 * Tear down the live association on a softc: namespaces, blkdev disks, I/O and
 * admin qpairs, AERs, and the reconnect/loss timers.  Shared by driver detach
 * and the user-initiated disconnect ioctl.  On return sc holds no association
 * (sc->admin/io/ns NULL, num_io_queues 0, rparams freed); the caller still owns
 * sc->cdata, the control minor, and the softc.
 *
 * The caller must set sc->detaching before calling this so concurrent
 * disconnect/rescan tasks quiesce, and is responsible for the post-teardown
 * disposition (free for detach, reset-for-reuse for disconnect).
 */
static void
nvmf_teardown_association(nvmf_softc_t *sc)
{
	uint_t i;

	/*
	 * Tear down the namespaces BEFORE the blkdev/multipath state: each
	 * nvmf_destroy_ns() frees its own path via nvmf_mpath_remove_path(), so
	 * nvmf_destroy_bd() must run afterwards on an already-empty sc->paths
	 * list to avoid double-freeing ns->path.
	 */
	if (sc->cdev_attached) {
		if (sc->ns != NULL) {
			for (i = 0; i < sc->cdata->id_nn; i++) {
				if (sc->ns[i] != NULL)
					nvmf_destroy_ns(sc->ns[i]);
			}
			kmem_free(sc->ns, sc->cdata->id_nn * sizeof (*sc->ns));
			sc->ns = NULL;
		}
		nvmf_destroy_bd(sc);
	}

	rw_enter(&sc->connection_lock, RW_WRITER);
	nvmf_disarm_ka_timers(sc);
	rw_exit(&sc->connection_lock);

	if (sc->admin != NULL)
		nvmf_shutdown_controller(sc);

	for (i = 0; i < sc->num_io_queues; i++) {
		if (sc->io[i] != NULL)
			nvmf_destroy_qp(sc->io[i]);
	}
	if (sc->io != NULL) {
		kmem_free(sc->io, sc->num_io_queues * sizeof (*sc->io));
		sc->io = NULL;
	}
	sc->num_io_queues = 0;

	/*
	 * Destroy the admin qpair BEFORE draining the taskq.  This stops all
	 * CQE delivery (including late AER completions), so no
	 * nvmf_complete_aer / nvmf_complete_aer_page can dispatch an AER task
	 * after the drain.  This mirrors FreeBSD's
	 * destroy-qpairs-then-drain-AER order.
	 *
	 * Hold connection_lock across the teardown so it cannot race a
	 * concurrent nvmf_disconnect_task: under detaching that task only takes
	 * the lock to call nvmf_shutdown_qp(sc->admin) (it never NULLs
	 * sc->admin), so once we NULL it here the task observes sc->admin ==
	 * NULL and returns.  NULLing sc->admin first also means any disconnect
	 * task that runs during the drain below early-returns and cannot re-arm
	 * the reconnect/loss timers.
	 */
	rw_enter(&sc->connection_lock, RW_WRITER);
	if (sc->admin != NULL) {
		struct nvmf_host_qpair *admin = sc->admin;
		sc->admin = NULL;
		nvmf_destroy_qp(admin);
	}
	rw_exit(&sc->connection_lock);

	/*
	 * Drain any in-flight disconnect task (and any AER task that was queued
	 * before the admin qpair was torn down).  With sc->admin now NULL no
	 * new AER task can be dispatched and no disconnect task can re-arm a
	 * timer.
	 */
	ddi_taskq_wait(nvmf_tq);

	/*
	 * Cancel the reconnect and controller-loss timers.  Don't cancel the
	 * loss timer if it is what triggered this teardown (controller_timedout);
	 * that handler has already cleared its own id.
	 */
	rw_enter(&sc->connection_lock, RW_WRITER);
	nvmf_cancel_timer(sc, &sc->reconnect_timer);
	if (!sc->controller_timedout)
		nvmf_cancel_timer(sc, &sc->loss_timer);
	rw_exit(&sc->connection_lock);

	/*
	 * Final AER teardown.  nvmf_destroy_aer() drains the taskq again (admin
	 * is already NULL, so nothing new can be dispatched) before freeing
	 * each aer->page and destroying aer->lock.
	 */
	if (sc->cdev_attached)
		nvmf_destroy_aer(sc);

	nvlist_free(sc->rparams);
	sc->rparams = NULL;
}

/*
 * User-initiated disconnect (NVMF_DISCONNECT_HOST/ALL).  Tear the association
 * down like detach, but keep the instance and its control minor so the same
 * node can be reconnected with a fresh NVMF_HANDOFF_HOST.  Idempotent: a node
 * with no association returns success.
 */
static int
nvmf_disconnect_host_ioctl(nvmf_softc_t *sc)
{
	rw_enter(&sc->connection_lock, RW_WRITER);
	if (!sc->cdev_attached && sc->admin == NULL) {
		rw_exit(&sc->connection_lock);
		return (0);
	}
	if (sc->detaching) {
		rw_exit(&sc->connection_lock);
		return (EBUSY);
	}
	/*
	 * Reuse the detaching flag to quiesce disconnect/rescan tasks during
	 * teardown; it is cleared again below so the instance stays usable.
	 */
	sc->detaching = B_TRUE;
	rw_exit(&sc->connection_lock);

	nvmf_teardown_association(sc);

	rw_enter(&sc->connection_lock, RW_WRITER);
	sc->cdev_attached = B_FALSE;
	sc->controller_timedout = B_FALSE;
	sc->detaching = B_FALSE;
	bzero(sc->cdata, sizeof (*sc->cdata));
	gethrestime(&sc->last_disconnect);
	rw_exit(&sc->connection_lock);
	return (0);
}

static int
nvmf_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	nvmf_softc_t *sc;
	int instance;

	switch (cmd) {
	case DDI_DETACH:
		break;
	case DDI_SUSPEND:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	instance = ddi_get_instance(dip);
	sc = ddi_get_soft_state(nvmf_state, instance);
	if (sc == NULL)
		return (DDI_FAILURE);

	rw_enter(&sc->connection_lock, RW_WRITER);
	sc->detaching = B_TRUE;
	rw_exit(&sc->connection_lock);

	nvmf_teardown_association(sc);

	ddi_remove_minor_node(dip, NULL);
	kmem_free(sc->cdata, sizeof (*sc->cdata));
	rw_destroy(&sc->connection_lock);
	ddi_soft_state_free(nvmf_state, instance);
	return (DDI_SUCCESS);
}

static int
nvmf_getinfo(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	nvmf_softc_t *sc;
	int instance;

	_NOTE(ARGUNUSED(dip));

	instance = getminor((dev_t)(uintptr_t)arg);
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		sc = ddi_get_soft_state(nvmf_state, instance);
		if (sc == NULL)
			return (DDI_FAILURE);
		*result = sc->dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)(uintptr_t)instance;
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}

static int
nvmf_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
	nvmf_softc_t *sc;
	struct nvmf_ioc_nv nv;
	int error;

	_NOTE(ARGUNUSED(rvalp));

	if (secpolicy_sys_config(credp, B_FALSE) != 0)
		return (EPERM);

	sc = ddi_get_soft_state(nvmf_state, getminor(dev));
	if (sc == NULL)
		return (ENXIO);

	switch (cmd) {
	case NVMF_HANDOFF_HOST:
		if (ddi_copyin((void *)arg, &nv, sizeof (nv), mode) != 0)
			return (EFAULT);
		return (nvmf_handoff_host(sc, &nv));
	case NVMF_DISCONNECT_HOST:
	case NVMF_DISCONNECT_ALL:
		/*
		 * This control node owns exactly one association, so both the
		 * per-host and disconnect-all forms tear down this instance.  The
		 * FreeBSD subsystem-NQN argument is not needed and is ignored.
		 */
		return (nvmf_disconnect_host_ioctl(sc));
	case NVMF_RECONNECT_PARAMS:
		if (ddi_copyin((void *)arg, &nv, sizeof (nv), mode) != 0)
			return (EFAULT);
		error = nvmf_reconnect_params(sc, &nv);
		if (error == 0 &&
		    ddi_copyout(&nv, (void *)arg, sizeof (nv), mode) != 0)
			error = EFAULT;
		return (error);
	case NVMF_RECONNECT_HOST:
		if (ddi_copyin((void *)arg, &nv, sizeof (nv), mode) != 0)
			return (EFAULT);
		return (nvmf_reconnect_host(sc, &nv));
	case NVMF_CONNECTION_STATUS:
		if (ddi_copyin((void *)arg, &nv, sizeof (nv), mode) != 0)
			return (EFAULT);
		error = nvmf_connection_status(sc, &nv);
		if (error == 0 &&
		    ddi_copyout(&nv, (void *)arg, sizeof (nv), mode) != 0)
			error = EFAULT;
		return (error);
	case NVMF_LIST_CONTROLLER:
		if (ddi_copyin((void *)arg, &nv, sizeof (nv), mode) != 0)
			return (EFAULT);
		error = nvmf_list_controller(sc, &nv);
		if (error == 0 &&
		    ddi_copyout(&nv, (void *)arg, sizeof (nv), mode) != 0)
			error = EFAULT;
		return (error);
	default:
		return (ENOTTY);
	}
}

static int
nvmf_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
	nvmf_softc_t *sc;

	_NOTE(ARGUNUSED(flag));

	if (otyp != OTYP_CHR)
		return (EINVAL);
	if (secpolicy_sys_config(credp, B_FALSE) != 0)
		return (EPERM);

	sc = ddi_get_soft_state(nvmf_state, getminor(*devp));
	if (sc == NULL)
		return (ENXIO);
	return (0);
}

static int
nvmf_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
	_NOTE(ARGUNUSED(dev, flag, otyp, credp));
	return (0);
}

static struct cb_ops nvmf_cb_ops = {
	.cb_open = nvmf_open,
	.cb_close = nvmf_close,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nvmf_ioctl,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_NEW | D_MP,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops nvmf_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = nvmf_getinfo,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = nvmf_attach,
	.devo_detach = nvmf_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &nvmf_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_needed
};

/*
 * blkdev child-instance assignment for our pseudo nexus.
 *
 * nvmf_host is a pseudo device (nvmf_host.conf parent="pseudo"), so
 * e_ddi_assign_instance() short-circuits for the blkdev disk children we create
 * beneath us and returns their unassigned (-1) instance instead of allocating
 * one from the instance tree (os/instance.c).  bd_attach()'s
 * ddi_soft_state_zalloc() then fails ("unable to zalloc soft state").  A pseudo
 * nexus is responsible for assigning instances to its own children: pseudonex
 * and i2cnex do this in their INITCHILD; we must do the same for the blkdev
 * children bd_attach_handle() creates under us.  bd_mod_init() installs blkdev's
 * bus_ops (bd_bus_ctl as bus_ctl); we wrap it so INITCHILD also assigns a free
 * instance.
 *
 * NB the proper home for this is blkdev's own bd_bus_ctl (it is a blkdev nexus
 * bug exposed by a pseudo-rooted consumer), but blkdev is pinned at boot and
 * cannot be hot-reloaded for testing, so the contained wrapper lives here.
 */
static struct bus_ops nvmf_bus_ops;
static int (*nvmf_bd_bus_ctl)(dev_info_t *, dev_info_t *, ddi_ctl_enum_t,
    void *, void *);

/*
 * Assign the lowest blkdev instance not already used by a live blkdev node.
 * The number must be unique across ALL blkdev consumers (blkdev's soft state is
 * global), so search the blkdev driver's per-major node list under its lock and
 * set the instance while still holding it, mirroring pseudonex_auto_assign() and
 * i2c_nex_assign_instance().  Returns the instance, or -1 if none is free.
 */
static int
nvmf_blkdev_assign_instance(dev_info_t *child)
{
	major_t		maj = ddi_driver_major(child);
	struct devnames	*dnp = &devnamesp[maj];
	dev_info_t	*tdip;
	int		inst;

	LOCK_DEV_OPS(&dnp->dn_lock);
	for (inst = 0; inst <= MAXMIN32; inst++) {
		for (tdip = dnp->dn_head; tdip != NULL;
		    tdip = ddi_get_next(tdip)) {
			if (tdip != child && ddi_get_instance(tdip) == inst)
				break;
		}
		if (tdip == NULL) {
			DEVI(child)->devi_instance = inst;
			UNLOCK_DEV_OPS(&dnp->dn_lock);
			return (inst);
		}
	}
	UNLOCK_DEV_OPS(&dnp->dn_lock);
	return (-1);
}

static int
nvmf_bus_ctl(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t ctlop,
    void *arg, void *result)
{
	int rv = nvmf_bd_bus_ctl(dip, rdip, ctlop, arg, result);

	if (ctlop == DDI_CTLOPS_INITCHILD && rv == DDI_SUCCESS) {
		dev_info_t *child = (dev_info_t *)arg;

		if (ddi_get_instance(child) == -1 &&
		    nvmf_blkdev_assign_instance(child) < 0)
			return (DDI_FAILURE);
	}
	return (rv);
}

static struct modldrv nvmf_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = "NVMe over Fabrics host",
	.drv_dev_ops = &nvmf_dev_ops
};

static struct modlinkage nvmf_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &nvmf_modldrv, NULL }
};

int
_init(void)
{
	int error;

	error = ddi_soft_state_init(&nvmf_state, sizeof (nvmf_softc_t), 0);
	if (error != 0)
		return (error);

	mutex_init(&nvmf_status_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&nvmf_status_cv, NULL, CV_DRIVER, NULL);

	nvmf_tq = ddi_taskq_create(NULL, "nvmf", 1, TASKQ_DEFAULTPRI, 0);
	if (nvmf_tq == NULL) {
		cv_destroy(&nvmf_status_cv);
		mutex_destroy(&nvmf_status_lock);
		ddi_soft_state_fini(&nvmf_state);
		return (ENOMEM);
	}

	nvmf_mpath_init();

	/*
	 * Register the blkdev bus_ops on our dev_ops so nvmf_host can act as the
	 * nexus for the per-namespace blkdev child nodes (bd_attach_handle);
	 * without this devo_bus_ops stays NULL and ndi_devi_online() of a
	 * blkdev@N,0 child fails ("failed bringing node online").  Mirrors
	 * nvme(4D).
	 */
	bd_mod_init(&nvmf_dev_ops);

	/*
	 * Wrap blkdev's bus_ops so our pseudo-nexus INITCHILD assigns an
	 * instance to each blkdev child (see nvmf_bus_ctl); the framework will
	 * not, because we are a pseudo device.
	 */
	nvmf_bus_ops = *nvmf_dev_ops.devo_bus_ops;
	nvmf_bd_bus_ctl = nvmf_bus_ops.bus_ctl;
	nvmf_bus_ops.bus_ctl = nvmf_bus_ctl;
	nvmf_dev_ops.devo_bus_ops = &nvmf_bus_ops;

	error = mod_install(&nvmf_modlinkage);
	if (error != 0) {
		bd_mod_fini(&nvmf_dev_ops);
		nvmf_mpath_fini();
		ddi_taskq_destroy(nvmf_tq);
		cv_destroy(&nvmf_status_cv);
		mutex_destroy(&nvmf_status_lock);
		ddi_soft_state_fini(&nvmf_state);
	}
	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&nvmf_modlinkage, modinfop));
}

int
_fini(void)
{
	int error;

	error = mod_remove(&nvmf_modlinkage);
	if (error != 0)
		return (error);

	bd_mod_fini(&nvmf_dev_ops);
	nvmf_mpath_fini();
	ddi_taskq_destroy(nvmf_tq);
	cv_destroy(&nvmf_status_cv);
	mutex_destroy(&nvmf_status_lock);
	ddi_soft_state_fini(&nvmf_state);
	return (0);
}

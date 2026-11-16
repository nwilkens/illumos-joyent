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
 * The control ioctls of the NVMe over Fabrics host.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/model.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/errno.h>
#include <sys/cred.h>
#include <sys/policy.h>
#include <sys/zone.h>
#include <sys/nvpair.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "nvmf_var.h"

/* KeepAlive timeout in milliseconds, the other two in seconds. */
#define	NVMF_KATO_MIN		1000
#define	NVMF_KATO_MAX		(3600 * 1000)
#define	NVMF_RECONNECT_DELAY_MAX	3600
#define	NVMF_LOSS_TIMEOUT_MAX	(7 * 24 * 3600)

/* Seconds the disconnect ioctl waits for the namespace worker. */
#define	NVMF_DETACH_WAIT	120

#ifdef _MULTI_DATAMODEL
typedef struct nvmf_ioc_nv32 {
	caddr32_t	data;
	size32_t	len;
	size32_t	size;
} nvmf_ioc_nv32_t;
#endif

static int
nvmf_ioc_copyin(intptr_t arg, int mode, struct nvmf_ioc_nv *nv)
{
#ifdef _MULTI_DATAMODEL
	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		nvmf_ioc_nv32_t nv32;

		if (ddi_copyin((void *)arg, &nv32, sizeof (nv32), mode) != 0)
			return (EFAULT);
		nv->data = (void *)(uintptr_t)nv32.data;
		nv->len = nv32.len;
		nv->size = nv32.size;
		return (0);
	}
#endif
	if (ddi_copyin((void *)arg, nv, sizeof (*nv), mode) != 0)
		return (EFAULT);
	return (0);
}

static int
nvmf_ioc_copyout(const struct nvmf_ioc_nv *nv, intptr_t arg, int mode)
{
#ifdef _MULTI_DATAMODEL
	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		nvmf_ioc_nv32_t nv32;

		if (nv->len > UINT32_MAX)
			return (EOVERFLOW);
		nv32.data = (caddr32_t)(uintptr_t)nv->data;
		nv32.len = (size32_t)nv->len;
		nv32.size = (size32_t)nv->size;
		if (ddi_copyout(&nv32, (void *)arg, sizeof (nv32), mode) != 0)
			return (EFAULT);
		return (0);
	}
#endif
	if (ddi_copyout(nv, (void *)arg, sizeof (*nv), mode) != 0)
		return (EFAULT);
	return (0);
}

/* The timeouts of a handoff, in the units and types the handoff uses. */
static boolean_t
nvmf_timeouts_valid(uint64_t kato, uint64_t reconnect_delay,
    uint64_t loss_timeout)
{
	if (kato != 0 && (kato < NVMF_KATO_MIN || kato > NVMF_KATO_MAX))
		return (B_FALSE);
	return (reconnect_delay <= NVMF_RECONNECT_DELAY_MAX &&
	    loss_timeout <= NVMF_LOSS_TIMEOUT_MAX);
}

/* Copy in a handoff nvlist and check everything the driver reads from it. */
static int
nvmf_copyin_handoff(const struct nvmf_ioc_nv *nv, nvlist_t **nvlp)
{
	const nvmf_discovery_log_page_entry_t *dlep;
	const nvme_identify_ctrl_t *cdp;
	nvlist_t *nvl;
	nvlist_t *rparams, *admin;
	nvlist_t **io;
	uchar_t *cdata;
	uint_t num_io_queues, i, cdlen;
	uint64_t qsize = 0, v64 = 0, trtype = 0;
	uint64_t kato = 0, reconnect_delay = 0, loss_timeout = 0;
	char *hostnqn;
	uchar_t *dle;
	uint_t dlelen;
	int error;
	boolean_t admin_flag = B_FALSE;

	error = nvmf_unpack_ioc_nvlist(nv, &nvl);
	if (error != 0)
		return (error);

	if (nvlist_lookup_uint64(nvl, "trtype", &trtype) != 0 ||
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

	if (trtype > UINT8_MAX)
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

	dlep = (const nvmf_discovery_log_page_entry_t *)dle;
	cdp = (const nvme_identify_ctrl_t *)cdata;
	if (memcmp(dlep->ndle_subnqn, cdp->id_subnqn,
	    sizeof (cdp->id_subnqn)) != 0)
		goto invalid;

	(void) nvlist_lookup_uint64(nvl, "kato", &kato);
	(void) nvlist_lookup_uint64(nvl, "reconnect_delay", &reconnect_delay);
	(void) nvlist_lookup_uint64(nvl, "controller_loss_timeout",
	    &loss_timeout);
	if (!nvmf_timeouts_valid(kato, reconnect_delay, loss_timeout))
		goto invalid;

	*nvlp = nvl;
	return (0);
invalid:
	nvlist_free(nvl);
	return (EINVAL);
}

static int
nvmf_handoff_host(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl;
	int error;

	error = nvmf_copyin_handoff(nv, &nvl);
	if (error != 0)
		return (error);

	mutex_enter(&sc->lock);
	if (sc->state != NVMF_SC_IDLE) {
		mutex_exit(&sc->lock);
		nvlist_free(nvl);
		return (EBUSY);
	}
	sc->state = NVMF_SC_ATTACHING;
	sc->conn_error = B_FALSE;
	sc->conn_events &= ~NVMF_EV_DISCONNECT;
	mutex_exit(&sc->lock);

	error = nvmf_attach_association(sc, nvl);
	if (error != 0) {
		nvmf_teardown(sc, B_TRUE);

		mutex_enter(&sc->lock);
		sc->state = NVMF_SC_IDLE;
		cv_broadcast(&sc->cv);
		mutex_exit(&sc->lock);
	}

	nvlist_free(nvl);
	return (error);
}

static int
nvmf_reconnect_host(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl;
	uchar_t *cdata = NULL;
	uint_t cdlen;
	uint64_t trtype = 0;
	int error;

	error = nvmf_copyin_handoff(nv, &nvl);
	if (error != 0)
		return (error);

	VERIFY0(nvlist_lookup_uint64(nvl, "trtype", &trtype));
	VERIFY0(nvlist_lookup_byte_array(nvl, "cdata", &cdata, &cdlen));

	/*
	 * The controller ID can differ between associations, so the
	 * subsystem NQN is what says this is the same controller.
	 */
	mutex_enter(&sc->lock);
	if (sc->state != NVMF_SC_LOST) {
		error = EBUSY;
	} else if (sc->trtype != (nvmf_trtype_t)trtype ||
	    memcmp(sc->cdata->id_subnqn,
	    ((const nvme_identify_ctrl_t *)cdata)->id_subnqn,
	    sizeof (sc->cdata->id_subnqn)) != 0) {
		error = EINVAL;
	} else {
		sc->state = NVMF_SC_RECONNECTING;
		sc->conn_error = B_FALSE;
	}
	mutex_exit(&sc->lock);

	if (error == EINVAL) {
		dev_err(sc->dip, CE_WARN, "!reconnect is for another "
		    "transport type or subsystem");
	}
	if (error == 0)
		error = nvmf_reconnect_association(sc, nvl);

	nvlist_free(nvl);
	return (error);
}

/*
 * Disconnect at the user's request.  A live association is not taken from
 * under an open disk: that returns EBUSY and changes nothing.  An association
 * that is already lost is torn down even so, and the open disks stay behind
 * with every I/O failing.
 */
static int
nvmf_disconnect_host(nvmf_softc_t *sc)
{
	boolean_t graceful = B_FALSE;
	clock_t deadline;
	int rv;

	mutex_enter(&sc->lock);
	for (;;) {
		if (sc->state == NVMF_SC_RESETTING) {
			cv_wait(&sc->cv, &sc->lock);
			continue;
		}
		if (sc->state != NVMF_SC_CONNECTED)
			break;

		if (sc->detach_req) {
			mutex_exit(&sc->lock);
			return (EBUSY);
		}
		sc->detach_req = B_TRUE;
		sc->detach_done = B_FALSE;
		sc->detach_owned = B_FALSE;
		mutex_exit(&sc->lock);
		nvmf_ns_post(sc, NVMF_NS_EV_DETACH);
		mutex_enter(&sc->lock);

		/*
		 * The namespace worker can be waiting for a controller that
		 * no longer answers.  A signal withdraws the request.  After
		 * NVMF_DETACH_WAIT the association is treated as lost, which
		 * frees the worker.
		 */
		deadline = ddi_get_lbolt() +
		    drv_usectohz((clock_t)NVMF_DETACH_WAIT * MICROSEC);
		rv = 1;
		while (!sc->detach_done && sc->state == NVMF_SC_CONNECTED &&
		    rv > 0)
			rv = cv_timedwait_sig(&sc->cv, &sc->lock, deadline);
		sc->detach_req = B_FALSE;

		if (sc->detach_done) {
			if (sc->detach_busy) {
				mutex_exit(&sc->lock);
				return (EBUSY);
			}
			if (sc->detach_owned) {
				sc->detach_owned = B_FALSE;
				graceful = B_TRUE;
				goto owned;
			}
		} else if (sc->state == NVMF_SC_CONNECTED) {
			if (rv == 0) {
				mutex_exit(&sc->lock);
				return (EINTR);
			}
			nvmf_disconnect_locked(sc);
			while (sc->state == NVMF_SC_CONNECTED)
				cv_wait(&sc->cv, &sc->lock);
		}
	}

	switch (sc->state) {
	case NVMF_SC_IDLE:
	case NVMF_SC_LOST:
	case NVMF_SC_FAILED:
		sc->state = NVMF_SC_DISCONNECTING;
		break;
	default:
		mutex_exit(&sc->lock);
		return (EBUSY);
	}
owned:
	mutex_exit(&sc->lock);

	nvmf_teardown(sc, graceful);

	mutex_enter(&sc->lock);
	sc->state = NVMF_SC_IDLE;
	bzero(sc->cdata, sizeof (*sc->cdata));
	gethrestime(&sc->last_disconnect);
	cv_broadcast(&sc->cv);
	mutex_exit(&sc->lock);
	return (0);
}

static int
nvmf_reconnect_params(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *rparams = NULL;
	int error;

	mutex_enter(&sc->lock);
	if (sc->rparams != NULL)
		rparams = fnvlist_dup(sc->rparams);
	mutex_exit(&sc->lock);
	if (rparams == NULL)
		return (ENXIO);

	error = nvmf_pack_ioc_nvlist(rparams, nv);
	fnvlist_free(rparams);
	return (error);
}

static int
nvmf_connection_status(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl, *nvl_ts;
	nvmf_sc_state_t state;
	timespec_t ts;
	int error;

	mutex_enter(&sc->lock);
	state = sc->state;
	ts = sc->last_disconnect;
	mutex_exit(&sc->lock);

	nvl = fnvlist_alloc();
	nvl_ts = fnvlist_alloc();
	fnvlist_add_boolean_value(nvl, "connected",
	    state == NVMF_SC_CONNECTED);
	fnvlist_add_string(nvl, "state", nvmf_state_name(state));
	fnvlist_add_uint64(nvl_ts, "tv_sec", ts.tv_sec);
	fnvlist_add_uint64(nvl_ts, "tv_nsec", ts.tv_nsec);
	fnvlist_add_nvlist(nvl, "last_disconnect", nvl_ts);
	fnvlist_free(nvl_ts);

	error = nvmf_pack_ioc_nvlist(nvl, nv);
	fnvlist_free(nvl);
	return (error);
}

/* Add a fixed-width, space or NUL padded field as a trimmed string. */
static void
nvmf_list_add_padded(nvlist_t *nvl, const char *key, const char *src,
    size_t srclen)
{
	char str[NVMF_NQN_FIELD_SIZE + 1];
	size_t n = MIN(srclen, sizeof (str) - 1);
	size_t i;

	for (i = 0; i < n && src[i] != '\0'; i++)
		str[i] = src[i];
	while (i > 0 && str[i - 1] == ' ')
		i--;
	str[i] = '\0';
	fnvlist_add_string(nvl, key, str);
}

static void
nvmf_list_add_namespaces(nvmf_softc_t *sc, nvlist_t *nvl, boolean_t connected)
{
	nvmf_ns_info_t *info;
	nvlist_t **nsarr;
	uint_t i, count, max;

	max = nvmf_ns_snapshot(sc, NULL, 0);
	if (max == 0)
		return;

	info = kmem_zalloc(max * sizeof (*info), KM_SLEEP);
	nsarr = kmem_zalloc(max * sizeof (*nsarr), KM_SLEEP);
	count = MIN(nvmf_ns_snapshot(sc, info, max), max);

	for (i = 0; i < count; i++) {
		nsarr[i] = fnvlist_alloc();
		fnvlist_add_uint32(nsarr[i], "nsid", info[i].ni_nsid);
		fnvlist_add_uint64(nsarr[i], "size", info[i].ni_size);
		fnvlist_add_uint32(nsarr[i], "blksize", info[i].ni_blksize);
		fnvlist_add_boolean_value(nsarr[i], "connected", connected);
		fnvlist_add_byte_array(nsarr[i], "nguid", info[i].ni_nguid,
		    sizeof (info[i].ni_nguid));
		fnvlist_add_byte_array(nsarr[i], "eui64", info[i].ni_eui64,
		    sizeof (info[i].ni_eui64));
	}
	if (count > 0)
		fnvlist_add_nvlist_array(nvl, "namespaces", nsarr, count);
	for (i = 0; i < count; i++)
		fnvlist_free(nsarr[i]);

	kmem_free(nsarr, max * sizeof (*nsarr));
	kmem_free(info, max * sizeof (*info));
}

static int
nvmf_list_controller(nvmf_softc_t *sc, struct nvmf_ioc_nv *nv)
{
	const nvmf_discovery_log_page_entry_t *dle;
	nvme_identify_ctrl_t *cdata;
	nvlist_t *nvl, *rparams = NULL;
	nvmf_sc_state_t state;
	uint_t num_io_queues, dlelen;
	uchar_t *dlebytes;
	char *hostnqn;
	int error;

	cdata = kmem_alloc(sizeof (*cdata), KM_SLEEP);

	mutex_enter(&sc->lock);
	state = sc->state;
	num_io_queues = sc->num_io_queues;
	bcopy(sc->cdata, cdata, sizeof (*cdata));
	if (sc->rparams != NULL)
		rparams = fnvlist_dup(sc->rparams);
	mutex_exit(&sc->lock);

	nvl = fnvlist_alloc();
	fnvlist_add_boolean_value(nvl, "connected",
	    state == NVMF_SC_CONNECTED);
	fnvlist_add_string(nvl, "state", nvmf_state_name(state));
	if (state == NVMF_SC_IDLE || state == NVMF_SC_ATTACHING ||
	    state == NVMF_SC_DISCONNECTING)
		goto pack;

	fnvlist_add_uint32(nvl, "num_io_queues", num_io_queues);
	nvmf_list_add_padded(nvl, "model", cdata->id_model,
	    sizeof (cdata->id_model));
	nvmf_list_add_padded(nvl, "serial", cdata->id_serial,
	    sizeof (cdata->id_serial));
	nvmf_list_add_padded(nvl, "firmware", cdata->id_fwrev,
	    sizeof (cdata->id_fwrev));

	if (rparams != NULL) {
		if (nvlist_lookup_byte_array(rparams, "dle", &dlebytes,
		    &dlelen) == 0 && dlelen >= sizeof (*dle)) {
			dle = (const nvmf_discovery_log_page_entry_t *)dlebytes;
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
		if (nvlist_lookup_string(rparams, "hostnqn", &hostnqn) == 0)
			fnvlist_add_string(nvl, "hostnqn", hostnqn);
	}

	nvmf_list_add_namespaces(sc, nvl, state == NVMF_SC_CONNECTED);

pack:
	nvlist_free(rparams);
	kmem_free(cdata, sizeof (*cdata));
	error = nvmf_pack_ioc_nvlist(nvl, nv);
	fnvlist_free(nvl);
	return (error);
}

int
nvmf_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
	nvmf_softc_t *sc;
	struct nvmf_ioc_nv nv;
	int error;

	_NOTE(ARGUNUSED(rvalp));

	/* The nvlist helpers copy from and to user addresses only. */
	if ((mode & FKIOCTL) != 0)
		return (ENOTSUP);
	if (secpolicy_sys_config(credp, B_FALSE) != 0 ||
	    crgetzoneid(credp) != GLOBAL_ZONEID)
		return (EPERM);

	sc = ddi_get_soft_state(nvmf_state, getminor(dev));
	if (sc == NULL)
		return (ENXIO);

	switch (cmd) {
	case NVMF_HANDOFF_HOST:
	case NVMF_RECONNECT_HOST:
	case NVMF_DISCONNECT_HOST:
	case NVMF_DISCONNECT_ALL:
		if ((mode & FWRITE) == 0)
			return (EBADF);
		break;
	case NVMF_RECONNECT_PARAMS:
	case NVMF_CONNECTION_STATUS:
	case NVMF_LIST_CONTROLLER:
		break;
	default:
		return (ENOTTY);
	}

	/* This node has one association, so both forms disconnect it. */
	if (cmd == NVMF_DISCONNECT_HOST || cmd == NVMF_DISCONNECT_ALL)
		return (nvmf_disconnect_host(sc));

	error = nvmf_ioc_copyin(arg, mode, &nv);
	if (error != 0)
		return (error);

	switch (cmd) {
	case NVMF_HANDOFF_HOST:
		return (nvmf_handoff_host(sc, &nv));
	case NVMF_RECONNECT_HOST:
		return (nvmf_reconnect_host(sc, &nv));
	case NVMF_RECONNECT_PARAMS:
		error = nvmf_reconnect_params(sc, &nv);
		break;
	case NVMF_CONNECTION_STATUS:
		error = nvmf_connection_status(sc, &nv);
		break;
	default:
		error = nvmf_list_controller(sc, &nv);
		break;
	}

	if (error == 0)
		error = nvmf_ioc_copyout(&nv, arg, mode);
	return (error);
}

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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/nvmf_transport.c.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Transport-independent support for fabrics queue pairs and commands.  This is
 * the dispatch core: it owns the registry of transport providers (TCP, RDMA),
 * dispatches queue-pair and capsule operations through the provider vtable, and
 * provides the nvlist ioctl helpers.  Every function body is translated
 * one-for-one from the FreeBSD source; the OS-glue substitutions are:
 *
 *   FreeBSD                       illumos
 *   -------                       -------
 *   malloc/free (M_WAITOK/...)    kmem_zalloc/kmem_free (KM_SLEEP/...)
 *   struct sx (sx_xlock/...)      krwlock_t (rw_enter(RW_WRITER)/...)
 *   refcount(9) volatile u_int    atomic_inc/dec_uint_nv with a count guard
 *   wakeup()/sx_sleep()           kcondvar_t + cv_broadcast/cv_wait
 *   SLIST                         list_t (sorted by priority)
 *   module event handler + macro  nvmf_transport_register/unregister + _init
 *
 * The provider is registered explicitly (see nvmf_transport_register) rather
 * than via FreeBSD's declarative NVMF_TRANSPORT() module macro, because illumos
 * has no per-subsystem module load event to hang that off of.
 */

#include <sys/types.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/list.h>
#include <sys/atomic.h>
#include <sys/condvar.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/nvpair.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/sysmacros.h>
#include <sys/modctl.h>
#include <sys/errno.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include "nvmf_core.h"
#include "nvmf_transport_internal.h"

/*
 * A registered transport provider for a given trtype.  FreeBSD threads these
 * together with an SLIST; illumos uses list_t.  nt_active_qpairs is a reference
 * count of live qpairs allocated through this provider, protected (for the
 * unload wait) by the registry lock and signalled through nt_cv.
 */
struct nvmf_transport {
	struct nvmf_transport_ops *nt_ops;

	volatile uint_t nt_active_qpairs;
	list_node_t nt_link;
};

/* nvmf_transports[trtype] is sorted by priority. */
static list_t nvmf_transports[NVMF_TRTYPE_TCP + 1];
static krwlock_t nvmf_transports_lock;

/*
 * Serializes the unload-wait below with qpair release.  FreeBSD reuses the sx
 * lock with sx_sleep()/wakeup(); illumos cv_wait requires a mutex, so a
 * dedicated mutex/cv pair stands in for the wakeup channel keyed on the
 * transport in the FreeBSD source.
 */
static kmutex_t nvmf_transports_cv_lock;
static kcondvar_t nvmf_transports_cv;

static boolean_t
nvmf_supported_trtype(nvmf_trtype_t trtype)
{
	return (trtype < ARRAY_SIZE(nvmf_transports));
}

struct nvmf_qpair *
nvmf_allocate_qpair(nvmf_trtype_t trtype, boolean_t controller,
    const nvlist_t *params, nvmf_qpair_error_t *error_cb, void *error_cb_arg,
    nvmf_capsule_receive_t *receive_cb, void *receive_cb_arg)
{
	struct nvmf_transport *nt;
	struct nvmf_qpair *qp;
	boolean_t admin;

	if (!nvmf_supported_trtype(trtype))
		return (NULL);

	qp = NULL;
	rw_enter(&nvmf_transports_lock, RW_READER);
	for (nt = list_head(&nvmf_transports[trtype]); nt != NULL;
	    nt = list_next(&nvmf_transports[trtype], nt)) {
		qp = nt->nt_ops->allocate_qpair(controller, params);
		if (qp != NULL) {
			atomic_inc_uint(&nt->nt_active_qpairs);
			break;
		}
	}
	rw_exit(&nvmf_transports_lock);
	if (qp == NULL)
		return (NULL);

	qp->nq_transport = nt;
	qp->nq_ops = nt->nt_ops;
	qp->nq_controller = controller;
	qp->nq_error = error_cb;
	qp->nq_error_arg = error_cb_arg;
	qp->nq_receive = receive_cb;
	qp->nq_receive_arg = receive_cb_arg;
	(void) nvlist_lookup_boolean_value((nvlist_t *)params, "admin", &admin);
	qp->nq_admin = admin;
	return (qp);
}

void
nvmf_free_qpair(struct nvmf_qpair *qp)
{
	struct nvmf_transport *nt;

	nt = qp->nq_transport;
	qp->nq_ops->free_qpair(qp);

	/*
	 * FreeBSD: if (refcount_release(&nt->nt_active_qpairs)) wakeup(nt);
	 * refcount_release() returns true when the count reaches zero.  Mirror
	 * that with an atomic decrement and broadcast the unload waiter when
	 * the last qpair drains.
	 */
	if (atomic_dec_uint_nv(&nt->nt_active_qpairs) == 0) {
		mutex_enter(&nvmf_transports_cv_lock);
		cv_broadcast(&nvmf_transports_cv);
		mutex_exit(&nvmf_transports_cv_lock);
	}
}

struct nvmf_capsule *
nvmf_allocate_command(struct nvmf_qpair *qp, const void *sqe, int how)
{
	struct nvmf_capsule *nc;

	ASSERT(how == KM_SLEEP || how == KM_NOSLEEP);
	nc = qp->nq_ops->allocate_capsule(qp, how);
	if (nc == NULL)
		return (NULL);

	nc->nc_qpair = qp;
	nc->nc_qe_len = sizeof (nvme_sqe_t);
	bcopy(sqe, &nc->nc_sqe, nc->nc_qe_len);

	/*
	 * 4.2 of NVMe base spec: Fabrics always uses SGL.
	 *
	 * FreeBSD clears/sets the PSDT field inside the fuse byte via
	 * NVMEM(NVME_CMD_PSDT)/NVMEF(NVME_CMD_PSDT, NVME_PSDT_SGL).  The
	 * illumos generic SQE breaks the same byte out into bitfields, so the
	 * PSDT selector is just the sqe_psdt bit; NVME_PSDT_SGL == 0x1.
	 */
	nc->nc_sqe.sqe_psdt = NVME_PSDT_SGL;
	return (nc);
}

struct nvmf_capsule *
nvmf_allocate_response(struct nvmf_qpair *qp, const void *cqe, int how)
{
	struct nvmf_capsule *nc;

	ASSERT(how == KM_SLEEP || how == KM_NOSLEEP);
	nc = qp->nq_ops->allocate_capsule(qp, how);
	if (nc == NULL)
		return (NULL);

	nc->nc_qpair = qp;
	nc->nc_qe_len = sizeof (nvme_cqe_t);
	bcopy(cqe, &nc->nc_cqe, nc->nc_qe_len);
	return (nc);
}

int
nvmf_capsule_append_data(struct nvmf_capsule *nc, struct nvmf_memdesc *mem,
    size_t len, boolean_t send, nvmf_io_complete_t *complete_cb,
    void *cb_arg)
{
	if (nc->nc_data.io_len != 0)
		return (EBUSY);

	nc->nc_send_data = send;
	nc->nc_data.io_mem = *mem;
	nc->nc_data.io_len = len;
	nc->nc_data.io_complete = complete_cb;
	nc->nc_data.io_complete_arg = cb_arg;
	return (0);
}

void
nvmf_free_capsule(struct nvmf_capsule *nc)
{
	nc->nc_qpair->nq_ops->free_capsule(nc);
}

int
nvmf_transmit_capsule(struct nvmf_capsule *nc)
{
	return (nc->nc_qpair->nq_ops->transmit_capsule(nc));
}

void
nvmf_abort_capsule_data(struct nvmf_capsule *nc, int error)
{
	if (nc->nc_data.io_len != 0)
		nvmf_complete_io_request(&nc->nc_data, 0, error);
}

void *
nvmf_capsule_sqe(struct nvmf_capsule *nc)
{
	ASSERT3U(nc->nc_qe_len, ==, sizeof (nvme_sqe_t));
	return (&nc->nc_sqe);
}

void *
nvmf_capsule_cqe(struct nvmf_capsule *nc)
{
	ASSERT3U(nc->nc_qe_len, ==, sizeof (nvme_cqe_t));
	return (&nc->nc_cqe);
}

boolean_t
nvmf_sqhd_valid(struct nvmf_capsule *nc)
{
	ASSERT3U(nc->nc_qe_len, ==, sizeof (nvme_cqe_t));
	return (nc->nc_sqhd_valid);
}

uint64_t
nvmf_max_xfer_size(struct nvmf_qpair *qp)
{
	return (qp->nq_ops->max_xfer_size(qp));
}

uint32_t
nvmf_max_ioccsz(struct nvmf_qpair *qp)
{
	return (qp->nq_ops->max_ioccsz(qp));
}

uint8_t
nvmf_validate_command_capsule(struct nvmf_capsule *nc)
{
	ASSERT3U(nc->nc_qe_len, ==, sizeof (nvme_sqe_t));

	/*
	 * FreeBSD: NVMEV(NVME_CMD_PSDT, nc_sqe.fuse) != NVME_PSDT_SGL.  The
	 * illumos SQE exposes PSDT as the sqe_psdt bit directly.
	 */
	if (nc->nc_sqe.sqe_psdt != NVME_PSDT_SGL)
		return (NVME_CQE_SC_GEN_INV_FLD);

	return (nc->nc_qpair->nq_ops->validate_command_capsule(nc));
}

size_t
nvmf_capsule_data_len(const struct nvmf_capsule *nc)
{
	return (nc->nc_qpair->nq_ops->capsule_data_len(nc));
}

int
nvmf_receive_controller_data(struct nvmf_capsule *nc, uint32_t data_offset,
    struct nvmf_memdesc *mem, size_t len, nvmf_io_complete_t *complete_cb,
    void *cb_arg)
{
	struct nvmf_io_request io;

	io.io_mem = *mem;
	io.io_len = len;
	io.io_complete = complete_cb;
	io.io_complete_arg = cb_arg;
	return (nc->nc_qpair->nq_ops->receive_controller_data(nc, data_offset,
	    &io));
}

uint_t
nvmf_send_controller_data(struct nvmf_capsule *nc, uint32_t data_offset,
    mblk_t *mp, size_t len)
{
	/*
	 * FreeBSD: MPASS(m_length(m, NULL) == len).  msgdsize() returns the
	 * number of data bytes in an mblk chain, the mblk_t analogue of
	 * m_length().
	 */
	ASSERT3U(msgdsize(mp), ==, len);
	return (nc->nc_qpair->nq_ops->send_controller_data(nc, data_offset, mp,
	    len));
}

int
nvmf_pack_ioc_nvlist(nvlist_t *nvl, struct nvmf_ioc_nv *nv)
{
	char *packed;
	size_t packed_len;
	int error;

	/*
	 * FreeBSD checks nvlist_error(nvl) here; illumos nvlists do not carry a
	 * cumulative error, so the equivalent guard is nvlist_size() failing
	 * below.  nvlist_size()/nvlist_pack() use NV_ENCODE_NATIVE since the
	 * carrier is consumed by the local kernel/userland pair.
	 */
	if (nv->size == 0) {
		error = nvlist_size(nvl, &nv->len, NV_ENCODE_NATIVE);
		return (error);
	}

	packed = NULL;
	packed_len = 0;
	error = nvlist_pack(nvl, &packed, &packed_len, NV_ENCODE_NATIVE,
	    KM_SLEEP);
	if (error != 0)
		return (error);

	nv->len = packed_len;
	if (nv->len > nv->size) {
		error = EFBIG;
	} else if (ddi_copyout(packed, nv->data, nv->len, 0) != 0) {
		error = EFAULT;
	} else {
		error = 0;
	}
	kmem_free(packed, packed_len);
	return (error);
}

/*
 * Upper bound on a packed ioctl nvlist.  nv->size comes verbatim from a
 * (privileged) userland struct nvmf_ioc_nv and drives the kmem_alloc() below.
 * The largest legitimate carrier is the handoff nvlist, whose binary "data"
 * member is a 1KB nvmf_fabric_connect_data_t plus a 64-byte cmd and the small
 * params sub-nvlist; 64KB is generous headroom while still preventing a
 * bogus/garbage size from triggering an arbitrarily large KM_SLEEP allocation
 * (box hang/OOM panic).
 */
#define	NVMF_IOC_NV_MAX	(64 * 1024)

int
nvmf_unpack_ioc_nvlist(const struct nvmf_ioc_nv *nv, nvlist_t **nvlp)
{
	char *packed;
	nvlist_t *nvl;
	int error;

	if (nv->size == 0 || nv->size > NVMF_IOC_NV_MAX)
		return (EINVAL);

	packed = kmem_alloc(nv->size, KM_SLEEP);
	if (ddi_copyin(nv->data, packed, nv->size, 0) != 0) {
		kmem_free(packed, nv->size);
		return (EFAULT);
	}

	error = nvlist_unpack(packed, nv->size, &nvl, KM_SLEEP);
	kmem_free(packed, nv->size);
	if (error != 0)
		return (EINVAL);

	*nvlp = nvl;
	return (0);
}

boolean_t
nvmf_validate_qpair_nvlist(const nvlist_t *nvl, boolean_t controller)
{
	nvlist_t *nv = (nvlist_t *)nvl;
	uint64_t value, qsize;
	boolean_t admin, sqfc, valid;

	/*
	 * FreeBSD uses nvlist_exists_bool()/nvlist_exists_number() to confirm a
	 * key exists with the expected type.  illumos nvpair has no typed
	 * existence predicate, so a successful typed lookup (return value 0)
	 * carries the same meaning.
	 */
	valid = B_TRUE;
	valid &= (nvlist_lookup_boolean_value(nv, "admin", &admin) == 0);
	valid &= (nvlist_lookup_boolean_value(nv, "sq_flow_control",
	    &sqfc) == 0);
	valid &= (nvlist_lookup_uint64(nv, "qsize", &qsize) == 0);
	valid &= (nvlist_lookup_uint64(nv, "sqhd", &value) == 0);
	if (!controller)
		valid &= (nvlist_lookup_uint64(nv, "sqtail", &value) == 0);
	if (!valid)
		return (B_FALSE);

	if (admin) {
		if (qsize < NVME_MIN_ADMIN_ENTRIES ||
		    qsize > NVME_MAX_ADMIN_ENTRIES)
			return (B_FALSE);
	} else {
		if (qsize < NVME_MIN_IO_ENTRIES || qsize > NVME_MAX_IO_ENTRIES)
			return (B_FALSE);
	}
	(void) nvlist_lookup_uint64(nv, "sqhd", &value);
	if (value > qsize - 1)
		return (B_FALSE);
	if (!controller) {
		(void) nvlist_lookup_uint64(nv, "sqtail", &value);
		if (value > qsize - 1)
			return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * Transport provider registration.
 *
 * FreeBSD routes MOD_LOAD/MOD_UNLOAD/MOD_QUIESCE for each transport module
 * through nvmf_transport_module_handler() (declared via the NVMF_TRANSPORT()
 * macro).  illumos transports are misc modules that call these two functions
 * directly from their _init()/_fini() entry points.  The MOD_LOAD body becomes
 * nvmf_transport_register(); the MOD_QUIESCE busy check and the MOD_UNLOAD
 * drain-and-free become nvmf_transport_unregister().
 */
int
nvmf_transport_register(struct nvmf_transport_ops *ops)
{
	struct nvmf_transport *nt, *nt2, *prev;

	if (!nvmf_supported_trtype(ops->trtype)) {
		cmn_err(CE_WARN, "NVMF: Unsupported transport %u", ops->trtype);
		return (EINVAL);
	}

	nt = kmem_zalloc(sizeof (*nt), KM_SLEEP);
	nt->nt_ops = ops;

	rw_enter(&nvmf_transports_lock, RW_WRITER);
	if (list_is_empty(&nvmf_transports[ops->trtype])) {
		list_insert_head(&nvmf_transports[ops->trtype], nt);
	} else {
		prev = NULL;
		for (nt2 = list_head(&nvmf_transports[ops->trtype]); nt2 != NULL;
		    nt2 = list_next(&nvmf_transports[ops->trtype], nt2)) {
			if (ops->priority > nt2->nt_ops->priority)
				break;
			prev = nt2;
		}
		if (prev == NULL)
			list_insert_head(&nvmf_transports[ops->trtype], nt);
		else
			list_insert_after(&nvmf_transports[ops->trtype], prev,
			    nt);
	}
	rw_exit(&nvmf_transports_lock);
	return (0);
}

int
nvmf_transport_unregister(struct nvmf_transport_ops *ops)
{
	struct nvmf_transport *nt;

	if (!nvmf_supported_trtype(ops->trtype))
		return (0);

	/*
	 * MOD_QUIESCE in FreeBSD refuses to quiesce (EBUSY) while any qpair is
	 * still active.  Preserve that gate before unlinking: if the provider
	 * is busy, fail the unregister so the module stays loaded.
	 */
	rw_enter(&nvmf_transports_lock, RW_READER);
	for (nt = list_head(&nvmf_transports[ops->trtype]); nt != NULL;
	    nt = list_next(&nvmf_transports[ops->trtype], nt)) {
		if (nt->nt_ops == ops)
			break;
	}
	if (nt == NULL) {
		rw_exit(&nvmf_transports_lock);
		return (0);
	}
	if (nt->nt_active_qpairs != 0) {
		rw_exit(&nvmf_transports_lock);
		return (EBUSY);
	}
	rw_exit(&nvmf_transports_lock);

	/* MOD_UNLOAD: unlink, drain any racing qpairs, then free. */
	rw_enter(&nvmf_transports_lock, RW_WRITER);
	for (nt = list_head(&nvmf_transports[ops->trtype]); nt != NULL;
	    nt = list_next(&nvmf_transports[ops->trtype], nt)) {
		if (nt->nt_ops == ops)
			break;
	}
	if (nt == NULL) {
		rw_exit(&nvmf_transports_lock);
		return (0);
	}

	list_remove(&nvmf_transports[ops->trtype], nt);
	rw_exit(&nvmf_transports_lock);

	/*
	 * FreeBSD sleeps on the transport (sx_sleep) holding the registry lock
	 * until nt_active_qpairs drains, releasing the lock across the sleep.
	 * Here the provider is already unlinked, so no new qpairs can reference
	 * it; wait on the dedicated cv for in-flight releases to finish.
	 */
	mutex_enter(&nvmf_transports_cv_lock);
	while (nt->nt_active_qpairs != 0)
		cv_wait(&nvmf_transports_cv, &nvmf_transports_cv_lock);
	mutex_exit(&nvmf_transports_cv_lock);

	kmem_free(nt, sizeof (*nt));
	return (0);
}

/*
 * Misc module plumbing.  This replaces FreeBSD's nvmf_transport_modevent() /
 * DECLARE_MODULE(nvmf_transport, ...): _init() initializes the registry the way
 * MOD_LOAD did, and _fini() tears it down.
 */
static struct modlmisc nvmf_transport_modlmisc = {
	&mod_miscops,
	"NVMe over Fabrics transport"
};

static struct modlinkage nvmf_transport_modlinkage = {
	MODREV_1,
	{ &nvmf_transport_modlmisc, NULL }
};

int
_init(void)
{
	uint_t i;
	int error;

	for (i = 0; i < ARRAY_SIZE(nvmf_transports); i++) {
		list_create(&nvmf_transports[i], sizeof (struct nvmf_transport),
		    offsetof(struct nvmf_transport, nt_link));
	}
	rw_init(&nvmf_transports_lock, NULL, RW_DRIVER, NULL);
	mutex_init(&nvmf_transports_cv_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&nvmf_transports_cv, NULL, CV_DRIVER, NULL);

	error = mod_install(&nvmf_transport_modlinkage);
	if (error != 0) {
		cv_destroy(&nvmf_transports_cv);
		mutex_destroy(&nvmf_transports_cv_lock);
		rw_destroy(&nvmf_transports_lock);
		for (i = 0; i < ARRAY_SIZE(nvmf_transports); i++)
			list_destroy(&nvmf_transports[i]);
	}
	return (error);
}

int
_fini(void)
{
	uint_t i;
	int error;

	/* Refuse to unload while any transport provider is still registered. */
	rw_enter(&nvmf_transports_lock, RW_READER);
	for (i = 0; i < ARRAY_SIZE(nvmf_transports); i++) {
		if (!list_is_empty(&nvmf_transports[i])) {
			rw_exit(&nvmf_transports_lock);
			return (EBUSY);
		}
	}
	rw_exit(&nvmf_transports_lock);

	error = mod_remove(&nvmf_transport_modlinkage);
	if (error != 0)
		return (error);

	cv_destroy(&nvmf_transports_cv);
	mutex_destroy(&nvmf_transports_cv_lock);
	rw_destroy(&nvmf_transports_lock);
	for (i = 0; i < ARRAY_SIZE(nvmf_transports); i++)
		list_destroy(&nvmf_transports[i]);
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&nvmf_transport_modlinkage, modinfop));
}

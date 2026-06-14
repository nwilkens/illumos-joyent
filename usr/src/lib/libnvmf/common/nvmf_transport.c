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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/nvmf_transport.c.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Transport-independent association / queue-pair / capsule layer.  Ported
 * field-for-field; OS-glue substitutions:
 *
 *   FreeBSD                        illumos
 *   -------                        -------
 *   refcount(9)                    plain counter (library is single-threaded)
 *   struct nvme_command/completion nvme_sqe_t / nvme_cqe_t
 *   NVMEM/NVMEF(NVME_CMD_PSDT)     sqe_psdt bit (== NVME_PSDT_SGL)
 *   nvlist_t (sys/_nv.h)           nvlist_t (libnvpair)
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libnvmf.h"
#include "internal.h"

struct nvmf_association *
nvmf_allocate_association(nvmf_trtype_t trtype, bool controller,
    const nvmf_association_params_t *params)
{
	struct nvmf_transport_ops *ops;
	struct nvmf_association *na;

	switch (trtype) {
	case NVMF_TRTYPE_TCP:
		ops = &tcp_ops;
		break;
	default:
		errno = EINVAL;
		return (NULL);
	}

	na = ops->allocate_association(controller, params);
	if (na == NULL)
		return (NULL);

	na->na_ops = ops;
	na->na_trtype = trtype;
	na->na_controller = controller;
	na->na_params = *params;
	na->na_last_error = NULL;
	na->na_refs = 1;
	return (na);
}

void
nvmf_update_association(struct nvmf_association *na,
    const nvme_identify_ctrl_t *cdata)
{
	na->na_ops->update_association(na, cdata);
}

void
nvmf_free_association(struct nvmf_association *na)
{
	assert(na->na_refs > 0);
	if (--na->na_refs == 0) {
		free(na->na_last_error);
		na->na_ops->free_association(na);
	}
}

const char *
nvmf_association_error(const struct nvmf_association *na)
{
	return (na->na_last_error);
}

void
na_clear_error(struct nvmf_association *na)
{
	free(na->na_last_error);
	na->na_last_error = NULL;
}

void
na_error(struct nvmf_association *na, const char *fmt, ...)
{
	va_list ap;
	char *str;

	if (na->na_last_error != NULL)
		return;
	va_start(ap, fmt);
	if (vasprintf(&str, fmt, ap) < 0)
		str = NULL;
	va_end(ap);
	na->na_last_error = str;
}

struct nvmf_qpair *
nvmf_allocate_qpair(struct nvmf_association *na,
    const nvmf_qpair_params_t *params)
{
	struct nvmf_qpair *qp;

	na_clear_error(na);
	qp = na->na_ops->allocate_qpair(na, params);
	if (qp == NULL)
		return (NULL);

	na->na_refs++;
	qp->nq_association = na;
	qp->nq_admin = params->nqp_admin;
	TAILQ_INIT(&qp->nq_rx_capsules);
	return (qp);
}

void
nvmf_free_qpair(struct nvmf_qpair *qp)
{
	struct nvmf_association *na;
	struct nvmf_capsule *nc, *tc;

	TAILQ_FOREACH_SAFE(nc, &qp->nq_rx_capsules, nc_link, tc) {
		TAILQ_REMOVE(&qp->nq_rx_capsules, nc, nc_link);
		nvmf_free_capsule(nc);
	}
	na = qp->nq_association;
	na->na_ops->free_qpair(qp);
	nvmf_free_association(na);
}

struct nvmf_capsule *
nvmf_allocate_command(struct nvmf_qpair *qp, const void *sqe)
{
	struct nvmf_capsule *nc;

	nc = qp->nq_association->na_ops->allocate_capsule(qp);
	if (nc == NULL)
		return (NULL);

	nc->nc_qpair = qp;
	nc->nc_qe_len = sizeof (nvme_sqe_t);
	(void) memcpy(&nc->nc_sqe, sqe, nc->nc_qe_len);

	/*
	 * 4.2 of NVMe base spec: Fabrics always uses SGL.  FreeBSD clears and
	 * sets the PSDT field with NVMEM/NVMEF(NVME_CMD_PSDT); the illumos SQE
	 * exposes the PRP-or-SGL selector as the single sqe_psdt bit and
	 * NVME_PSDT_SGL == 0x1.
	 */
	nc->nc_sqe.sqe_psdt = NVME_PSDT_SGL;
	return (nc);
}

struct nvmf_capsule *
nvmf_allocate_response(struct nvmf_qpair *qp, const void *cqe)
{
	struct nvmf_capsule *nc;

	nc = qp->nq_association->na_ops->allocate_capsule(qp);
	if (nc == NULL)
		return (NULL);

	nc->nc_qpair = qp;
	nc->nc_qe_len = sizeof (nvme_cqe_t);
	(void) memcpy(&nc->nc_cqe, cqe, nc->nc_qe_len);
	return (nc);
}

int
nvmf_capsule_append_data(struct nvmf_capsule *nc, void *buf, size_t len,
    bool send)
{
	if (nc->nc_qe_len == sizeof (nvme_cqe_t))
		return (EINVAL);
	if (nc->nc_data_len != 0)
		return (EBUSY);

	nc->nc_data = buf;
	nc->nc_data_len = len;
	nc->nc_send_data = send;
	return (0);
}

void
nvmf_free_capsule(struct nvmf_capsule *nc)
{
	nc->nc_qpair->nq_association->na_ops->free_capsule(nc);
}

int
nvmf_transmit_capsule(struct nvmf_capsule *nc)
{
	return (nc->nc_qpair->nq_association->na_ops->transmit_capsule(nc));
}

int
nvmf_receive_capsule(struct nvmf_qpair *qp, struct nvmf_capsule **ncp)
{
	return (qp->nq_association->na_ops->receive_capsule(qp, ncp));
}

const void *
nvmf_capsule_sqe(const struct nvmf_capsule *nc)
{
	assert(nc->nc_qe_len == sizeof (nvme_sqe_t));
	return (&nc->nc_sqe);
}

const void *
nvmf_capsule_cqe(const struct nvmf_capsule *nc)
{
	assert(nc->nc_qe_len == sizeof (nvme_cqe_t));
	return (&nc->nc_cqe);
}

uint8_t
nvmf_validate_command_capsule(const struct nvmf_capsule *nc)
{
	assert(nc->nc_qe_len == sizeof (nvme_sqe_t));

	if (nc->nc_sqe.sqe_psdt != NVME_PSDT_SGL)
		return (NVME_CQE_SC_GEN_INV_FLD);

	return (nc->nc_qpair->nq_association->na_ops->validate_command_capsule(
	    nc));
}

size_t
nvmf_capsule_data_len(const struct nvmf_capsule *nc)
{
	return (nc->nc_qpair->nq_association->na_ops->capsule_data_len(nc));
}

int
nvmf_receive_controller_data(const struct nvmf_capsule *nc,
    uint32_t data_offset, void *buf, size_t len)
{
	return (nc->nc_qpair->nq_association->na_ops->receive_controller_data(
	    nc, data_offset, buf, len));
}

int
nvmf_send_controller_data(const struct nvmf_capsule *nc, const void *buf,
    size_t len)
{
	return (nc->nc_qpair->nq_association->na_ops->send_controller_data(nc,
	    buf, len));
}

int
nvmf_kernel_handoff_params(struct nvmf_qpair *qp, nvlist_t **nvlp)
{
	nvlist_t *nvl;
	int error;

	if ((error = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0)
		return (error);

	(void) nvlist_add_boolean_value(nvl, "admin", qp->nq_admin);
	(void) nvlist_add_boolean_value(nvl, "sq_flow_control",
	    qp->nq_flow_control);
	(void) nvlist_add_uint64(nvl, "qsize", qp->nq_qsize);
	(void) nvlist_add_uint64(nvl, "sqhd", qp->nq_sqhd);
	if (!qp->nq_association->na_controller)
		(void) nvlist_add_uint64(nvl, "sqtail", qp->nq_sqtail);
	qp->nq_association->na_ops->kernel_handoff_params(qp, nvl);

	*nvlp = nvl;
	return (0);
}

int
nvmf_populate_dle(struct nvmf_qpair *qp, nvmf_discovery_log_page_entry_t *dle)
{
	struct nvmf_association *na = qp->nq_association;

	dle->ndle_trtype = na->na_trtype;
	return (na->na_ops->populate_dle(qp, dle));
}

const char *
nvmf_transport_type(uint8_t trtype)
{
	static __thread char buf[8];

	switch (trtype) {
	case NVMF_TRTYPE_RDMA:
		return ("RDMA");
	case NVMF_TRTYPE_FC:
		return ("Fibre Channel");
	case NVMF_TRTYPE_TCP:
		return ("TCP");
	case NVMF_TRTYPE_INTRA_HOST:
		return ("Intra-host");
	default:
		(void) snprintf(buf, sizeof (buf), "0x%02x", trtype);
		return (buf);
	}
}

/*
 * Pack nvl and store the resulting buffer in nv for an ioctl.  The caller owns
 * nv->data and must free() it.  (FreeBSD allocates with nvlist_pack(); illumos
 * uses libnvpair's nvlist_pack() with NV_ENCODE_NATIVE, which the matching
 * kernel-side nvmf_unpack_ioc_nvlist() expects.)
 */
int
nvmf_pack_ioc_nvlist(struct nvmf_ioc_nv *nv, nvlist_t *nvl)
{
	char *packed;
	size_t packed_len;
	int error;

	(void) memset(nv, 0, sizeof (*nv));

	packed = NULL;
	packed_len = 0;
	error = nvlist_pack(nvl, &packed, &packed_len, NV_ENCODE_NATIVE, 0);
	if (error != 0)
		return (error);

	nv->data = packed;
	nv->len = packed_len;
	nv->size = packed_len;
	return (0);
}

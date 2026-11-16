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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/nvmf_tcp.c.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Userland NVMe/TCP transport: command buffers and the H2C, C2H and R2T
 * PDUs that move data through them.
 */

#include <sys/types.h>
#include <sys/byteorder.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nvmf_tcp_impl.h"

nvmf_tcp_command_buffer_t *
tcp_alloc_command_buffer(nvmf_tcp_qpair_t *qp, void *data,
    uint32_t data_offset, size_t data_len, uint16_t cid, uint16_t ttag,
    bool receive)
{
	nvmf_tcp_command_buffer_t *cb;

	cb = malloc(sizeof (*cb));
	if (cb == NULL)
		return (NULL);
	cb->qp = qp;
	cb->data = data;
	cb->data_offset = data_offset;
	cb->data_len = data_len;
	cb->data_xfered = 0;
	cb->cid = cid;
	cb->ttag = ttag;
	cb->linked = true;

	if (receive)
		LIST_INSERT_HEAD(&qp->rx_buffers, cb, link);
	else
		LIST_INSERT_HEAD(&qp->tx_buffers, cb, link);
	return (cb);
}

static nvmf_tcp_command_buffer_t *
tcp_find_command_buffer(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    bool receive)
{
	struct nvmf_tcp_command_buffer_list *list;
	nvmf_tcp_command_buffer_t *cb;

	list = receive ? &qp->rx_buffers : &qp->tx_buffers;
	LIST_FOREACH(cb, list, link) {
		if (cb->cid == cid && cb->ttag == ttag)
			return (cb);
	}
	return (NULL);
}

void
tcp_purge_command_buffer(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    bool receive)
{
	nvmf_tcp_command_buffer_t *cb;

	cb = tcp_find_command_buffer(qp, cid, ttag, receive);
	if (cb != NULL) {
		LIST_REMOVE(cb, link);
		cb->linked = false;
	}
}

void
tcp_free_command_buffer(nvmf_tcp_command_buffer_t *cb)
{
	if (cb->linked)
		LIST_REMOVE(cb, link);
	free(cb);
}

int
nvmf_tcp_handle_h2c_data(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_h2c_data_hdr_t *h2c;
	nvmf_tcp_command_buffer_t *cb;
	uint32_t data_len, data_offset;
	const char *icd;

	h2c = (void *)pdu->hdr;
	if (LE_32(h2c->nth2c_datal) > qp->maxh2cdata) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED, 0,
		    pdu->hdr, LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	cb = tcp_find_command_buffer(qp, h2c->nth2c_cccid, h2c->nth2c_ttag,
	    true);
	if (cb == NULL) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_ttag), pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_len = LE_32(h2c->nth2c_datal);
	if (data_len != pdu->data_len) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_datal), pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_offset = LE_32(h2c->nth2c_datao);
	if (data_len == 0 || data_offset < cb->data_offset ||
	    (uint64_t)data_offset + data_len >
	    (uint64_t)cb->data_offset + cb->data_len) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE, 0,
		    pdu->hdr, LE_32(pdu->hdr->ntcph_plen),
		    pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	if (data_offset != cb->data_offset + cb->data_xfered) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	if ((cb->data_xfered + data_len == cb->data_len) !=
	    ((pdu->hdr->ntcph_flags & NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU) != 0)) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	cb->data_xfered += data_len;
	data_offset -= cb->data_offset;
	icd = (const char *)pdu->hdr + pdu->hdr->ntcph_pdo;
	(void) memcpy((char *)cb->data + data_offset, icd, data_len);

	nvmf_tcp_free_pdu(pdu);
	return (0);
}

int
nvmf_tcp_handle_c2h_data(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_c2h_data_hdr_t *c2h;
	nvmf_tcp_command_buffer_t *cb;
	uint32_t data_len, data_offset;
	const char *icd;

	c2h = (void *)pdu->hdr;

	cb = tcp_find_command_buffer(qp, c2h->ntc2c_cccid, 0, true);
	if (cb == NULL) {
		/*
		 * XXX: Could be PDU sequence error if cccid is for a
		 * command that doesn't use a command buffer.
		 */
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_c2h_data_hdr_t, ntc2c_cccid), pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_len = LE_32(c2h->ntc2c_datal);
	if (data_len != pdu->data_len) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_c2h_data_hdr_t, ntc2c_datal), pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_offset = LE_32(c2h->ntc2c_datao);
	if (data_len == 0 || data_offset < cb->data_offset ||
	    (uint64_t)data_offset + data_len >
	    (uint64_t)cb->data_offset + cb->data_len) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE, 0,
		    pdu->hdr, LE_32(pdu->hdr->ntcph_plen),
		    pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	if (data_offset != cb->data_offset + cb->data_xfered) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	if ((cb->data_xfered + data_len == cb->data_len) !=
	    ((pdu->hdr->ntcph_flags & NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU) != 0)) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	cb->data_xfered += data_len;
	data_offset -= cb->data_offset;
	icd = (const char *)pdu->hdr + pdu->hdr->ntcph_pdo;
	(void) memcpy((char *)cb->data + data_offset, icd, data_len);

	if ((pdu->hdr->ntcph_flags & NVMF_TCP_C2H_DATA_FLAGS_SUCCESS) != 0) {
		nvme_cqe_t cqe;
		nvmf_tcp_capsule_t *tc;
		struct nvmf_capsule *nc;

		(void) memset(&cqe, 0, sizeof (cqe));
		cqe.cqe_cid = cb->cid;

		nc = nvmf_allocate_response(&qp->qp, &cqe);
		if (nc == NULL) {
			nvmf_tcp_free_pdu(pdu);
			return (ENOMEM);
		}
		nc->nc_sqhd_valid = false;

		tc = TCAP(nc);
		TAILQ_INSERT_TAIL(&qp->rx_capsules, tc, link);
	}

	nvmf_tcp_free_pdu(pdu);
	return (0);
}

/* NB: cid and ttag and little-endian already. */
static int
tcp_send_h2c_pdu(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    uint32_t data_offset, void *buf, size_t len, bool last_pdu)
{
	nvmf_tcp_h2c_data_hdr_t h2c;

	(void) memset(&h2c, 0, sizeof (h2c));
	h2c.nth2c_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_H2C_DATA;
	if (last_pdu)
		h2c.nth2c_common.ntcph_flags |= NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU;
	h2c.nth2c_cccid = cid;
	h2c.nth2c_ttag = ttag;
	h2c.nth2c_datao = LE_32(data_offset);
	h2c.nth2c_datal = LE_32(len);

	return (nvmf_tcp_construct_pdu(qp, &h2c, sizeof (h2c), buf, len));
}

/* Sends one or more H2C_DATA PDUs, subject to MAXH2CDATA. */
static int
tcp_send_h2c_pdus(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    uint32_t data_offset, void *buf, size_t len, bool last_pdu)
{
	char *p;

	p = buf;
	while (len != 0) {
		size_t todo;
		int error;

		todo = len;
		if (todo > qp->maxh2cdata)
			todo = qp->maxh2cdata;
		error = tcp_send_h2c_pdu(qp, cid, ttag, data_offset, p, todo,
		    last_pdu && todo == len);
		if (error != 0)
			return (error);
		p += todo;
		len -= todo;
		data_offset += todo;	/* advance datao for the next PDU */
	}
	return (0);
}

int
nvmf_tcp_handle_r2t(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_command_buffer_t *cb;
	nvmf_tcp_r2t_hdr_t *r2t;
	uint32_t data_len, data_offset;
	int error;

	r2t = (void *)pdu->hdr;

	cb = tcp_find_command_buffer(qp, r2t->ntr2t_cccid, 0, false);
	if (cb == NULL) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_r2t_hdr_t, ntr2t_cccid), pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_offset = LE_32(r2t->ntr2t_r2to);
	if (data_offset != cb->data_xfered) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->hdr,
		    LE_32(pdu->hdr->ntcph_plen), pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	/*
	 * XXX: The spec does not specify how to handle R2T transfers
	 * out of range of the original command.
	 */
	data_len = LE_32(r2t->ntr2t_r2tl);
	if (data_len == 0 ||
	    (uint64_t)data_offset + data_len > cb->data_len) {
		nvmf_tcp_report_error(qp->qp.nq_association, qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE, 0,
		    pdu->hdr, LE_32(pdu->hdr->ntcph_plen),
		    pdu->hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	cb->data_xfered += data_len;

	/*
	 * Write out one or more H2C_DATA PDUs containing the requested data.
	 */
	error = tcp_send_h2c_pdus(qp, r2t->ntr2t_cccid, r2t->ntr2t_ttag,
	    data_offset, (char *)cb->data + data_offset, data_len, true);

	nvmf_tcp_free_pdu(pdu);
	return (error);
}

/* NB: cid and ttag are both little-endian already. */
static int
tcp_send_r2t(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    uint32_t data_offset, uint32_t data_len)
{
	nvmf_tcp_r2t_hdr_t r2t;

	(void) memset(&r2t, 0, sizeof (r2t));
	r2t.ntr2t_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_R2T;
	r2t.ntr2t_cccid = cid;
	r2t.ntr2t_ttag = ttag;
	r2t.ntr2t_r2to = LE_32(data_offset);
	r2t.ntr2t_r2tl = LE_32(data_len);

	return (nvmf_tcp_construct_pdu(qp, &r2t, sizeof (r2t), NULL, 0));
}

static int
tcp_receive_r2t_data(const struct nvmf_capsule *nc, uint32_t data_offset,
    void *buf, size_t len)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_command_buffer_t *cb;
	int error;
	uint16_t ttag;

	/*
	 * Don't bother byte-swapping ttag as it is just a cookie value
	 * returned by the other end as-is.
	 */
	ttag = qp->next_ttag++;

	error = tcp_send_r2t(qp, nc->nc_sqe.sqe_cid, ttag, data_offset, len);
	if (error != 0)
		return (error);

	cb = tcp_alloc_command_buffer(qp, buf, data_offset, len,
	    nc->nc_sqe.sqe_cid, ttag, true);
	if (cb == NULL)
		return (ENOMEM);

	/* Parse received PDUs until the data transfer is complete. */
	while (cb->data_xfered < cb->data_len) {
		error = nvmf_tcp_receive_pdu(qp);
		if (error != 0)
			break;
	}
	tcp_free_command_buffer(cb);
	return (error);
}

static int
tcp_receive_icd_data(const struct nvmf_capsule *nc, uint32_t data_offset,
    void *buf, size_t len)
{
	const nvmf_tcp_capsule_t *tc = CTCAP(nc);
	const char *icd;

	if ((uint64_t)data_offset + len > tc->rx_pdu.data_len)
		return (EFBIG);
	icd = (const char *)tc->rx_pdu.hdr + tc->rx_pdu.hdr->ntcph_pdo +
	    data_offset;
	(void) memcpy(buf, icd, len);
	return (0);
}

int
tcp_receive_controller_data(const struct nvmf_capsule *nc, uint32_t data_offset,
    void *buf, size_t len)
{
	struct nvmf_association *na = nc->nc_qpair->nq_association;
	const nvme_sgl_t *sgl;
	uint8_t sgl_type;
	size_t data_len;

	if (nc->nc_qe_len != sizeof (nvme_sqe_t) || !na->na_controller)
		return (EINVAL);

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	data_len = LE_32(sgl->sgl_len);
	if (len > UINT32_MAX || (uint64_t)data_offset + len > data_len)
		return (EFBIG);

	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	if (sgl_type == NVME_SGL_TYPE_ICD)
		return (tcp_receive_icd_data(nc, data_offset, buf, len));
	else
		return (tcp_receive_r2t_data(nc, data_offset, buf, len));
}

/* NB: cid is little-endian already. */
static int
tcp_send_c2h_pdu(nvmf_tcp_qpair_t *qp, uint16_t cid, uint32_t data_offset,
    const void *buf, size_t len, bool last_pdu, bool success)
{
	nvmf_tcp_c2h_data_hdr_t c2h;

	(void) memset(&c2h, 0, sizeof (c2h));
	c2h.ntc2c_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_C2H_DATA;
	if (last_pdu)
		c2h.ntc2c_common.ntcph_flags |= NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU;
	if (success)
		c2h.ntc2c_common.ntcph_flags |= NVMF_TCP_C2H_DATA_FLAGS_SUCCESS;
	c2h.ntc2c_cccid = cid;
	c2h.ntc2c_datao = LE_32(data_offset);
	c2h.ntc2c_datal = LE_32(len);

	return (nvmf_tcp_construct_pdu(qp, &c2h, sizeof (c2h),
	    (void *)(uintptr_t)buf, len));
}

int
tcp_send_controller_data(const struct nvmf_capsule *nc, const void *buf,
    size_t len)
{
	struct nvmf_association *na = nc->nc_qpair->nq_association;
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	const nvme_sgl_t *sgl;
	uint8_t sgl_type;
	const char *src;
	size_t todo;
	uint32_t data_len, data_offset;
	int error;
	bool last_pdu, send_success_flag;

	if (nc->nc_qe_len != sizeof (nvme_sqe_t) || !na->na_controller)
		return (EINVAL);

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	data_len = LE_32(sgl->sgl_len);
	if (len != data_len) {
		(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
		return (EFBIG);
	}

	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	if (sgl_type != NVME_SGL_TYPE_COMMAND_BUFFER) {
		(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
		return (EINVAL);
	}

	/* Use the SUCCESS flag if SQ flow control is disabled. */
	send_success_flag = !qp->qp.nq_flow_control;

	/*
	 * Write out one or more C2H_DATA PDUs containing the data.
	 * Each PDU is arbitrarily capped at 256k.
	 */
	data_offset = 0;
	src = buf;
	while (len > 0) {
		if (len > 256 * 1024) {
			todo = 256 * 1024;
			last_pdu = false;
		} else {
			todo = len;
			last_pdu = true;
		}
		error = tcp_send_c2h_pdu(qp, nc->nc_sqe.sqe_cid, data_offset,
		    src, todo, last_pdu, last_pdu && send_success_flag);
		if (error != 0) {
			(void) nvmf_send_generic_error(nc,
			    NVME_CQE_SC_GEN_INTERNAL_ERR);
			return (error);
		}
		data_offset += todo;
		src += todo;
		len -= todo;
	}
	if (!send_success_flag)
		(void) nvmf_send_success(nc);
	return (0);
}

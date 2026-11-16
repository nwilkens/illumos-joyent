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
 * Userland NVMe/TCP transport: reading, checking and writing PDUs, with
 * their CRC32C header and data digests.
 */

#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "nvmf_tcp_impl.h"

static const char zero_padding[NVMF_TCP_PDU_PDO_MAX_OFFSET];

static uint32_t
compute_digest(const void *buf, size_t len)
{
	return (nvmf_crc32c(0xffffffff, buf, len) ^ 0xffffffff);
}

/* Wait for the socket, up to the queue's deadline. */
static int
nvmf_tcp_wait(nvmf_tcp_qpair_t *qp, short events)
{
	struct pollfd pfd;
	int64_t left;
	int n, timo;

	for (;;) {
		timo = -1;
		if (qp->qp.nq_deadline != 0) {
			left = qp->qp.nq_deadline - nvmf_now();
			if (left <= 0)
				return (ETIMEDOUT);
			left = left / 1000000 + 1;
			timo = left > INT_MAX ? INT_MAX : (int)left;
		}
		pfd.fd = qp->s;
		pfd.events = events;
		pfd.revents = 0;
		n = poll(&pfd, 1, timo);
		if (n > 0)
			return (0);
		if (n == -1 && errno != EINTR)
			return (errno);
	}
}

static int
nvmf_tcp_write_pdu_iov(nvmf_tcp_qpair_t *qp, struct iovec *iov, u_int iovcnt,
    size_t len)
{
	struct msghdr msg;
	ssize_t nwritten;
	int error;

	if (qp->terminated)
		return (ECONNRESET);

	while (len != 0) {
		(void) memset(&msg, 0, sizeof (msg));
		msg.msg_iov = iov;
		msg.msg_iovlen = iovcnt;
		nwritten = sendmsg(qp->s, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (nwritten < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN)
				return (errno);
			error = nvmf_tcp_wait(qp, POLLOUT);
			if (error != 0)
				return (error);
			continue;
		}

		len -= nwritten;
		while (iovcnt != 0 && iov->iov_len <= (size_t)nwritten) {
			nwritten -= iov->iov_len;
			iovcnt--;
			iov++;
		}
		if (iovcnt != 0) {
			iov->iov_base = (char *)iov->iov_base + nwritten;
			iov->iov_len -= nwritten;
		}
	}
	return (0);
}

int
nvmf_tcp_write_pdu(nvmf_tcp_qpair_t *qp, const void *pdu, size_t len)
{
	struct iovec iov;

	iov.iov_base = (void *)(uintptr_t)pdu;
	iov.iov_len = len;
	return (nvmf_tcp_write_pdu_iov(qp, &iov, 1, len));
}

/* Store a little-endian 32-bit value into the 4-byte fei field. */
static void
tcp_fei_enc(uint8_t fei[4], uint32_t val)
{
	fei[0] = (uint8_t)(val);
	fei[1] = (uint8_t)(val >> 8);
	fei[2] = (uint8_t)(val >> 16);
	fei[3] = (uint8_t)(val >> 24);
}

/* Load a little-endian 32-bit value from the 4-byte fei field. */
uint32_t
tcp_fei_dec(const uint8_t fei[4])
{
	return ((uint32_t)fei[0] | ((uint32_t)fei[1] << 8) |
	    ((uint32_t)fei[2] << 16) | ((uint32_t)fei[3] << 24));
}

void
nvmf_tcp_report_error(struct nvmf_association *na, nvmf_tcp_qpair_t *qp,
    uint16_t fes, uint32_t fei, const void *rx_pdu, size_t pdu_len, u_int hlen)
{
	nvmf_tcp_term_req_hdr_t hdr;
	struct iovec iov[2];

	if (hlen != 0) {
		if (hlen > NVMF_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE)
			hlen = NVMF_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE;
		if (hlen > pdu_len)
			hlen = pdu_len;
	}

	(void) memset(&hdr, 0, sizeof (hdr));
	hdr.nttr_common.ntcph_pdu_type = na->na_controller ?
	    NVMF_TCP_PDU_TYPE_C2H_TERM_REQ : NVMF_TCP_PDU_TYPE_H2C_TERM_REQ;
	hdr.nttr_common.ntcph_hlen = sizeof (hdr);
	hdr.nttr_common.ntcph_plen = LE_32(sizeof (hdr) + hlen);
	hdr.nttr_fes = LE_16(fes);
	tcp_fei_enc(hdr.nttr_fei, fei);
	iov[0].iov_base = (caddr_t)&hdr;
	iov[0].iov_len = sizeof (hdr);
	iov[1].iov_base = (void *)(uintptr_t)rx_pdu;
	iov[1].iov_len = hlen;

	(void) nvmf_tcp_write_pdu_iov(qp, iov, 2, sizeof (hdr) + hlen);
	(void) shutdown(qp->s, SHUT_RDWR);
	qp->terminated = true;
}

int
nvmf_tcp_read_buffer(nvmf_tcp_qpair_t *qp, void *buf, size_t len)
{
	ssize_t nread;
	char *cp;
	int error;

	if (qp->terminated)
		return (ECONNRESET);

	cp = buf;
	while (len != 0) {
		nread = recv(qp->s, cp, len, MSG_DONTWAIT);
		if (nread < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN)
				return (errno);
			error = nvmf_tcp_wait(qp, POLLIN);
			if (error != 0)
				return (error);
			continue;
		}
		if (nread == 0)
			return (ECONNRESET);
		len -= nread;
		cp += nread;
	}
	return (0);
}

static int
nvmf_tcp_check_digests(nvmf_tcp_qpair_t *qp, const nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_common_pdu_hdr_t *ch = pdu->hdr;
	uint32_t digest, plen, rx_digest;

	plen = LE_32(ch->ntcph_plen);
	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_HDGSTF) != 0) {
		digest = compute_digest(ch, ch->ntcph_hlen);
		(void) memcpy(&rx_digest, (const char *)ch + ch->ntcph_hlen,
		    sizeof (rx_digest));
		if (digest != rx_digest) {
			nvmf_tcp_report_error(qp->qp.nq_association, qp,
			    NVMF_TCP_TERM_REQ_FES_HDGST_ERROR, rx_digest, ch,
			    plen, ch->ntcph_hlen);
			return (EBADMSG);
		}
	}

	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_DDGSTF) != 0) {
		digest = compute_digest((const char *)ch + ch->ntcph_pdo,
		    pdu->data_len);
		(void) memcpy(&rx_digest, (const char *)ch + plen -
		    sizeof (rx_digest), sizeof (rx_digest));
		if (digest != rx_digest)
			return (EBADMSG);
	}
	return (0);
}

/*
 * The most data a PDU of this type may carry.  Data PDUs are held to what the
 * queue's own receive buffers still expect, so the peer cannot make the
 * library allocate more than its caller asked to read.
 */
static size_t
nvmf_tcp_max_data(nvmf_tcp_qpair_t *qp, uint8_t pdu_type)
{
	nvmf_tcp_command_buffer_t *cb;
	size_t max;

	switch (pdu_type) {
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
		return (NVMF_TCP_MAX_RX_ICD);
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		max = 0;
		LIST_FOREACH(cb, &qp->rx_buffers, link) {
			if (cb->data_len - cb->data_xfered > max)
				max = cb->data_len - cb->data_xfered;
		}
		return (max);
	default:
		return (0);
	}
}

static bool
nvmf_tcp_is_term_req(const nvmf_tcp_common_pdu_hdr_t *ch)
{
	return (ch->ntcph_pdu_type == NVMF_TCP_PDU_TYPE_H2C_TERM_REQ ||
	    ch->ntcph_pdu_type == NVMF_TCP_PDU_TYPE_C2H_TERM_REQ);
}

int
nvmf_tcp_read_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	struct nvmf_association *na = qp->qp.nq_association;
	nvmf_tcp_common_pdu_hdr_t ch;
	uint32_t data_len, fei, plen;
	uint16_t fes;
	int error;

	(void) memset(pdu, 0, sizeof (*pdu));
	error = nvmf_tcp_read_buffer(qp, &ch, sizeof (ch));
	if (error != 0)
		return (error);

	/* The common header is checked before plen is trusted for anything. */
	error = nvmf_tcp_validate_pdu_header(&ch, na->na_controller,
	    qp->header_digests, qp->data_digests, qp->rxpda, &data_len, &fes,
	    &fei);
	if (error != 0) {
		if (error != ECONNRESET) {
			nvmf_tcp_report_error(na, qp, fes, fei, &ch,
			    sizeof (ch), sizeof (ch));
		}
		return (error);
	}

	/* A termination request's length was bounded by the check above. */
	if (!nvmf_tcp_is_term_req(&ch) &&
	    data_len > nvmf_tcp_max_data(qp, ch.ntcph_pdu_type)) {
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE, 0, &ch,
		    sizeof (ch), sizeof (ch));
		return (EBADMSG);
	}

	/*
	 * The controller side handles one command at a time, so a command
	 * that arrives while it waits for H2C data is never legitimate here.
	 */
	if (ch.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_CAPSULE_CMD &&
	    !LIST_EMPTY(&qp->rx_buffers)) {
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, &ch,
		    sizeof (ch), sizeof (ch));
		return (EBADMSG);
	}

	plen = LE_32(ch.ntcph_plen);
	pdu->hdr = malloc(plen);
	if (pdu->hdr == NULL)
		return (ENOMEM);
	(void) memcpy(pdu->hdr, &ch, sizeof (ch));
	pdu->data_len = data_len;
	error = nvmf_tcp_read_buffer(qp, (char *)pdu->hdr + sizeof (ch),
	    plen - sizeof (ch));
	if (error == 0)
		error = nvmf_tcp_check_digests(qp, pdu);
	if (error != 0) {
		free(pdu->hdr);
		pdu->hdr = NULL;
	}
	return (error);
}

void
nvmf_tcp_free_pdu(nvmf_tcp_rxpdu_t *pdu)
{
	free(pdu->hdr);
	pdu->hdr = NULL;
}

/*
 * Construct and send a PDU that contains an optional data payload.
 * This includes dealing with digests and the length fields in the
 * common header.
 */
int
nvmf_tcp_construct_pdu(nvmf_tcp_qpair_t *qp, void *hdr, size_t hlen,
    void *data, uint32_t data_len)
{
	nvmf_tcp_common_pdu_hdr_t *ch;
	struct iovec iov[5];
	u_int iovcnt;
	uint32_t header_digest, data_digest, pad, pdo, plen;

	plen = hlen;
	if (qp->header_digests)
		plen += sizeof (header_digest);
	if (data_len != 0) {
		/* The PDA is a multiple of 4, not always a power of 2. */
		pdo = ((plen + qp->txpda - 1) / qp->txpda) * qp->txpda;
		pad = pdo - plen;
		if (pad > sizeof (zero_padding))
			return (EINVAL);
		plen = pdo + data_len;
		if (qp->data_digests)
			plen += sizeof (data_digest);
	} else {
		assert(data == NULL);
		pdo = 0;
		pad = 0;
	}

	ch = hdr;
	ch->ntcph_hlen = hlen;
	if (qp->header_digests)
		ch->ntcph_flags |= NVMF_TCP_CH_FLAGS_HDGSTF;
	if (qp->data_digests && data_len != 0)
		ch->ntcph_flags |= NVMF_TCP_CH_FLAGS_DDGSTF;
	ch->ntcph_pdo = pdo;
	ch->ntcph_plen = LE_32(plen);

	/* CH + PSH */
	iov[0].iov_base = hdr;
	iov[0].iov_len = hlen;
	iovcnt = 1;

	/* HDGST */
	if (qp->header_digests) {
		header_digest = compute_digest(hdr, hlen);
		iov[iovcnt].iov_base = (caddr_t)&header_digest;
		iov[iovcnt].iov_len = sizeof (header_digest);
		iovcnt++;
	}

	if (pad != 0) {
		/* PAD */
		iov[iovcnt].iov_base = (void *)(uintptr_t)zero_padding;
		iov[iovcnt].iov_len = pad;
		iovcnt++;
	}

	if (data_len != 0) {
		/* DATA */
		iov[iovcnt].iov_base = data;
		iov[iovcnt].iov_len = data_len;
		iovcnt++;

		/* DDGST */
		if (qp->data_digests) {
			data_digest = compute_digest(data, data_len);
			iov[iovcnt].iov_base = (caddr_t)&data_digest;
			iov[iovcnt].iov_len = sizeof (data_digest);
			iovcnt++;
		}
	}

	return (nvmf_tcp_write_pdu_iov(qp, iov, iovcnt, plen));
}

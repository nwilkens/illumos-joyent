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
 * Userland NVMe/TCP transport: ICReq/ICResp connection setup, PDU framing,
 * CRC32C header/data digests, and command-buffer (R2T / H2C / C2H) data
 * movement over a BSD socket fd.  Ported field-for-field; OS-glue
 * substitutions:
 *
 *   FreeBSD                                 illumos
 *   -------                                 -------
 *   le16toh/le32toh, htole16/32              LE_16/LE_32 (<sys/byteorder.h>)
 *   le32enc/le32dec (fei array)              explicit byte stores/loads
 *   calculate_crc32c()                       nvmf_crc32c() (nvmf_crc32c.c)
 *   struct nvme_tcp_* (nvmf_proto.h)         nvmf_tcp_*_t (<sys/nvme/nvmf.h>),
 *                                            field names ntcph_*, nfic_*, etc.
 *   NVME_TCP_PDU_TYPE_*                      NVMF_TCP_PDU_TYPE_*
 *   struct nvme_command/completion           nvme_sqe_t / nvme_cqe_t
 *   __DECONST / nitems                       explicit casts / ARRAY_SIZE-style
 *
 * The on-wire PDU validation (nvmf_tcp_validate_pdu_header) is shared with the
 * kernel transport via <sys/nvme/nvmf_tcp.h>; that header's inline uses
 * cmn_err()/VERIFY in the kernel.  Because this is userland, we redefine those
 * primitives to printf()/assert() before including it (see below).
 */

#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libnvmf.h"
#include "internal.h"

/*
 * <sys/nvme/nvmf_tcp.h> carries the shared nvmf_tcp_validate_pdu_header()
 * inline.  In the kernel it logs with cmn_err(); CE_WARN and the VERIFY/
 * VERIFY3U macros it also uses are provided for userland by <sys/cmn_err.h>
 * and <sys/debug.h> (which back VERIFY with libc's assfail()).  Only cmn_err()
 * itself is kernel-only, so shim just that: route the message to stderr and
 * drop the leading severity (CE_WARN) argument.  The kernel headers are pulled
 * in first so CE_WARN/VERIFY come from their canonical definitions.
 */
#include <sys/cmn_err.h>
#include <sys/debug.h>

#ifndef	_KERNEL
#define	cmn_err(level, ...)	do {					\
	(void) (level);							\
	(void) fprintf(stderr, __VA_ARGS__);				\
	(void) fputc('\n', stderr);					\
} while (0)
#endif /* !_KERNEL */

#include <sys/nvme/nvmf_tcp.h>

struct nvmf_tcp_qpair;

typedef struct nvmf_tcp_command_buffer {
	struct nvmf_tcp_qpair *qp;

	void	*data;
	size_t	data_len;
	size_t	data_xfered;
	uint32_t data_offset;

	uint16_t cid;
	uint16_t ttag;

	LIST_ENTRY(nvmf_tcp_command_buffer) link;
} nvmf_tcp_command_buffer_t;

LIST_HEAD(nvmf_tcp_command_buffer_list, nvmf_tcp_command_buffer);

typedef struct nvmf_tcp_association {
	struct nvmf_association na;

	uint32_t ioccsz;
} nvmf_tcp_association_t;

typedef struct nvmf_tcp_rxpdu {
	nvmf_tcp_common_pdu_hdr_t *hdr;
	uint32_t data_len;
} nvmf_tcp_rxpdu_t;

typedef struct nvmf_tcp_capsule {
	struct nvmf_capsule nc;

	nvmf_tcp_rxpdu_t rx_pdu;
	nvmf_tcp_command_buffer_t *cb;

	TAILQ_ENTRY(nvmf_tcp_capsule) link;
} nvmf_tcp_capsule_t;

typedef struct nvmf_tcp_qpair {
	struct nvmf_qpair qp;
	int s;

	uint8_t	txpda;
	uint8_t rxpda;
	bool header_digests;
	bool data_digests;
	uint32_t maxr2t;
	uint32_t maxh2cdata;
	uint32_t max_icd;	/* Host only */
	uint16_t next_ttag;	/* Controller only */

	struct nvmf_tcp_command_buffer_list tx_buffers;
	struct nvmf_tcp_command_buffer_list rx_buffers;
	TAILQ_HEAD(, nvmf_tcp_capsule) rx_capsules;
} nvmf_tcp_qpair_t;

#define	TASSOC(na)	((nvmf_tcp_association_t *)(na))
#define	TCAP(nc)	((nvmf_tcp_capsule_t *)(nc))
#define	CTCAP(nc)	((const nvmf_tcp_capsule_t *)(nc))
#define	TQP(qp)		((nvmf_tcp_qpair_t *)(qp))

static const char zero_padding[NVMF_TCP_PDU_PDO_MAX_OFFSET];

static uint32_t
compute_digest(const void *buf, size_t len)
{
	return (nvmf_crc32c(0xffffffff, buf, len) ^ 0xffffffff);
}

static nvmf_tcp_command_buffer_t *
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

static void
tcp_purge_command_buffer(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    bool receive)
{
	nvmf_tcp_command_buffer_t *cb;

	cb = tcp_find_command_buffer(qp, cid, ttag, receive);
	if (cb != NULL)
		LIST_REMOVE(cb, link);
}

static void
tcp_free_command_buffer(nvmf_tcp_command_buffer_t *cb)
{
	LIST_REMOVE(cb, link);
	free(cb);
}

static int
nvmf_tcp_write_pdu(nvmf_tcp_qpair_t *qp, const void *pdu, size_t len)
{
	ssize_t nwritten;
	const char *cp;

	cp = pdu;
	while (len != 0) {
		nwritten = write(qp->s, cp, len);
		if (nwritten < 0)
			return (errno);
		len -= nwritten;
		cp += nwritten;
	}
	return (0);
}

static int
nvmf_tcp_write_pdu_iov(nvmf_tcp_qpair_t *qp, struct iovec *iov, u_int iovcnt,
    size_t len)
{
	ssize_t nwritten;

	for (;;) {
		nwritten = writev(qp->s, iov, iovcnt);
		if (nwritten < 0)
			return (errno);

		len -= nwritten;
		if (len == 0)
			return (0);

		while (iov->iov_len <= (size_t)nwritten) {
			nwritten -= iov->iov_len;
			iovcnt--;
			iov++;
		}

		iov->iov_base = (char *)iov->iov_base + nwritten;
		iov->iov_len -= nwritten;
	}
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
static uint32_t
tcp_fei_dec(const uint8_t fei[4])
{
	return ((uint32_t)fei[0] | ((uint32_t)fei[1] << 8) |
	    ((uint32_t)fei[2] << 16) | ((uint32_t)fei[3] << 24));
}

static void
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
	(void) close(qp->s);
	qp->s = -1;
}

static int
nvmf_tcp_validate_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu,
    size_t pdu_len)
{
	const nvmf_tcp_common_pdu_hdr_t *ch;
	uint32_t data_len, fei, plen;
	uint32_t digest, rx_digest;
	u_int hlen;
	int error;
	uint16_t fes;

	/* Determine how large of a PDU header to return for errors. */
	ch = pdu->hdr;
	hlen = ch->ntcph_hlen;
	plen = LE_32(ch->ntcph_plen);
	if (hlen < sizeof (*ch) || hlen > plen)
		hlen = sizeof (*ch);

	error = nvmf_tcp_validate_pdu_header(ch,
	    qp->qp.nq_association->na_controller, qp->header_digests,
	    qp->data_digests, qp->rxpda, &data_len, &fes, &fei);
	if (error != 0) {
		if (error == ECONNRESET) {
			(void) close(qp->s);
			qp->s = -1;
		} else {
			nvmf_tcp_report_error(qp->qp.nq_association, qp,
			    fes, fei, ch, pdu_len, hlen);
		}
		return (error);
	}

	/* Check header digest if present. */
	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_HDGSTF) != 0) {
		digest = compute_digest(ch, ch->ntcph_hlen);
		(void) memcpy(&rx_digest, (const char *)ch + ch->ntcph_hlen,
		    sizeof (rx_digest));
		if (digest != rx_digest) {
			(void) printf("NVMe/TCP: Header digest mismatch\n");
			nvmf_tcp_report_error(qp->qp.nq_association, qp,
			    NVMF_TCP_TERM_REQ_FES_HDGST_ERROR, rx_digest, ch,
			    pdu_len, hlen);
			return (EBADMSG);
		}
	}

	/* Check data digest if present. */
	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_DDGSTF) != 0) {
		digest = compute_digest((const char *)ch + ch->ntcph_pdo,
		    data_len);
		(void) memcpy(&rx_digest, (const char *)ch + plen -
		    sizeof (rx_digest), sizeof (rx_digest));
		if (digest != rx_digest) {
			(void) printf("NVMe/TCP: Data digest mismatch\n");
			return (EBADMSG);
		}
	}

	pdu->data_len = data_len;
	return (0);
}

/*
 * Read data from a socket, retrying until the data has been fully
 * read or an error occurs.
 */
static int
nvmf_tcp_read_buffer(int s, void *buf, size_t len)
{
	ssize_t nread;
	char *cp;

	cp = buf;
	while (len != 0) {
		nread = read(s, cp, len);
		if (nread < 0)
			return (errno);
		if (nread == 0)
			return (ECONNRESET);
		len -= nread;
		cp += nread;
	}
	return (0);
}

static int
nvmf_tcp_read_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_common_pdu_hdr_t ch;
	uint32_t plen;
	int error;

	(void) memset(pdu, 0, sizeof (*pdu));
	error = nvmf_tcp_read_buffer(qp->s, &ch, sizeof (ch));
	if (error != 0)
		return (error);

	plen = LE_32(ch.ntcph_plen);

	/*
	 * Validate a header with garbage lengths to trigger an error message
	 * without reading more.
	 */
	if (plen < sizeof (ch) || ch.ntcph_hlen > plen) {
		pdu->hdr = &ch;
		error = nvmf_tcp_validate_pdu(qp, pdu, sizeof (ch));
		pdu->hdr = NULL;
		assert(error != 0);
		return (error);
	}

	/* Read the rest of the PDU.  plen is attacker-controlled off the wire. */
	pdu->hdr = malloc(plen);
	if (pdu->hdr == NULL)
		return (ENOMEM);
	(void) memcpy(pdu->hdr, &ch, sizeof (ch));
	error = nvmf_tcp_read_buffer(qp->s, (char *)pdu->hdr + sizeof (ch),
	    plen - sizeof (ch));
	if (error != 0)
		return (error);
	error = nvmf_tcp_validate_pdu(qp, pdu, plen);
	if (error != 0) {
		free(pdu->hdr);
		pdu->hdr = NULL;
	}
	return (error);
}

static void
nvmf_tcp_free_pdu(nvmf_tcp_rxpdu_t *pdu)
{
	free(pdu->hdr);
	pdu->hdr = NULL;
}

static int
nvmf_tcp_handle_term_req(nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_term_req_hdr_t *hdr;

	hdr = (void *)pdu->hdr;

	(void) printf(
	    "NVMe/TCP: Received termination request: fes %#x fei %#x\n",
	    LE_16(hdr->nttr_fes), tcp_fei_dec(hdr->nttr_fei));
	nvmf_tcp_free_pdu(pdu);
	return (ECONNRESET);
}

static int
nvmf_tcp_save_command_capsule(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_cmd_t *cmd;
	struct nvmf_capsule *nc;
	nvmf_tcp_capsule_t *tc;

	cmd = (void *)pdu->hdr;

	nc = nvmf_allocate_command(&qp->qp, cmd->ntc_ccsqe);
	if (nc == NULL)
		return (ENOMEM);

	tc = TCAP(nc);
	tc->rx_pdu = *pdu;

	TAILQ_INSERT_TAIL(&qp->rx_capsules, tc, link);
	return (0);
}

static int
nvmf_tcp_save_response_capsule(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_rsp_t *rsp;
	struct nvmf_capsule *nc;
	nvmf_tcp_capsule_t *tc;
	nvme_cqe_t *cqe;

	rsp = (void *)pdu->hdr;
	cqe = (nvme_cqe_t *)rsp->ntr_rccqe;

	nc = nvmf_allocate_response(&qp->qp, cqe);
	if (nc == NULL)
		return (ENOMEM);

	nc->nc_sqhd_valid = true;
	tc = TCAP(nc);
	tc->rx_pdu = *pdu;

	TAILQ_INSERT_TAIL(&qp->rx_capsules, tc, link);

	/*
	 * Once the CQE has been received, no further transfers to the
	 * command buffer for the associated CID can occur.
	 */
	tcp_purge_command_buffer(qp, cqe->cqe_cid, 0, true);
	tcp_purge_command_buffer(qp, cqe->cqe_cid, 0, false);

	return (0);
}

/*
 * Construct and send a PDU that contains an optional data payload.
 * This includes dealing with digests and the length fields in the
 * common header.
 */
static int
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
		pdo = P2ROUNDUP(plen, qp->txpda);
		pad = pdo - plen;
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

static int
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
	if (data_offset < cb->data_offset ||
	    data_offset + data_len > cb->data_offset + cb->data_len) {
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

static int
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
	if (data_offset < cb->data_offset ||
	    data_offset + data_len > cb->data_offset + cb->data_len) {
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

static int
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
	if (data_offset + data_len > cb->data_len) {
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

static int
nvmf_tcp_receive_pdu(nvmf_tcp_qpair_t *qp)
{
	nvmf_tcp_rxpdu_t pdu;
	int error;

	error = nvmf_tcp_read_pdu(qp, &pdu);
	if (error != 0)
		return (error);

	switch (pdu.hdr->ntcph_pdu_type) {
	default:
		assert(0);
		nvmf_tcp_free_pdu(&pdu);
		return (ECONNRESET);
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
		return (nvmf_tcp_handle_term_req(&pdu));
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
		return (nvmf_tcp_save_command_capsule(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
		return (nvmf_tcp_save_response_capsule(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
		return (nvmf_tcp_handle_h2c_data(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		return (nvmf_tcp_handle_c2h_data(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_R2T:
		return (nvmf_tcp_handle_r2t(qp, &pdu));
	}
}

static bool
nvmf_tcp_validate_ic_pdu(struct nvmf_association *na, nvmf_tcp_qpair_t *qp,
    const nvmf_tcp_common_pdu_hdr_t *ch, size_t pdu_len)
{
	const nvmf_tcp_ic_req_t *pdu;
	uint32_t plen;
	u_int hlen;

	/* Determine how large of a PDU header to return for errors. */
	hlen = ch->ntcph_hlen;
	plen = LE_32(ch->ntcph_plen);
	if (hlen < sizeof (*ch) || hlen > plen)
		hlen = sizeof (*ch);

	/*
	 * Errors must be reported for the lowest incorrect field
	 * first, so validate fields in order.
	 */

	/* Validate pdu_type. */

	/* Controllers only receive PDUs with a PDU direction of 0. */
	if (na->na_controller != ((ch->ntcph_pdu_type & 0x01) == 0)) {
		na_error(na, "NVMe/TCP: Invalid PDU type %u",
		    ch->ntcph_pdu_type);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 0, ch, pdu_len,
		    hlen);
		return (false);
	}

	switch (ch->ntcph_pdu_type) {
	case NVMF_TCP_PDU_TYPE_IC_REQ:
	case NVMF_TCP_PDU_TYPE_IC_RESP:
		break;
	default:
		na_error(na, "NVMe/TCP: Invalid PDU type %u",
		    ch->ntcph_pdu_type);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 0, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate flags. */
	if (ch->ntcph_flags != 0) {
		na_error(na, "NVMe/TCP: Invalid PDU header flags %#x",
		    ch->ntcph_flags);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 1, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate hlen. */
	if (ch->ntcph_hlen != 128) {
		na_error(na, "NVMe/TCP: Invalid PDU header length %u",
		    ch->ntcph_hlen);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 2, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate pdo. */
	if (ch->ntcph_pdo != 0) {
		na_error(na, "NVMe/TCP: Invalid PDU data offset %u",
		    ch->ntcph_pdo);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 3, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate plen. */
	if (plen != 128) {
		na_error(na, "NVMe/TCP: Invalid PDU length %u", plen);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 4, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate fields common to both ICReq and ICResp. */
	pdu = (const nvmf_tcp_ic_req_t *)ch;
	if (LE_16(pdu->nfic_pfv) != 0) {
		na_error(na, "NVMe/TCP: Unsupported PDU version %u",
		    LE_16(pdu->nfic_pfv));
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER,
		    8, ch, pdu_len, hlen);
		return (false);
	}

	if (pdu->nfic_hpda > NVMF_TCP_HPDA_MAX) {
		na_error(na, "NVMe/TCP: Unsupported PDA %u", pdu->nfic_hpda);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 10, ch, pdu_len,
		    hlen);
		return (false);
	}

	if (pdu->nfic_dgst.bits.reserved != 0) {
		na_error(na, "NVMe/TCP: Invalid digest settings");
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 11, ch, pdu_len,
		    hlen);
		return (false);
	}

	return (true);
}

static bool
nvmf_tcp_read_ic_req(struct nvmf_association *na, nvmf_tcp_qpair_t *qp,
    nvmf_tcp_ic_req_t *pdu)
{
	int error;

	error = nvmf_tcp_read_buffer(qp->s, pdu, sizeof (*pdu));
	if (error != 0) {
		na_error(na, "NVMe/TCP: Failed to read IC request: %s",
		    strerror(error));
		return (false);
	}

	return (nvmf_tcp_validate_ic_pdu(na, qp, &pdu->nfic_common,
	    sizeof (*pdu)));
}

static bool
nvmf_tcp_read_ic_resp(struct nvmf_association *na, nvmf_tcp_qpair_t *qp,
    nvmf_tcp_ic_resp_t *pdu)
{
	int error;

	error = nvmf_tcp_read_buffer(qp->s, pdu, sizeof (*pdu));
	if (error != 0) {
		na_error(na, "NVMe/TCP: Failed to read IC response: %s",
		    strerror(error));
		return (false);
	}

	return (nvmf_tcp_validate_ic_pdu(na, qp, &pdu->nfir_common,
	    sizeof (*pdu)));
}

static struct nvmf_association *
tcp_allocate_association(bool controller,
    const nvmf_association_params_t *params)
{
	nvmf_tcp_association_t *ta;

	if (controller) {
		/* 7.4.10.3 */
		if (params->nap_tcp.maxh2cdata < 4096 ||
		    params->nap_tcp.maxh2cdata % 4 != 0)
			return (NULL);
	}

	ta = calloc(1, sizeof (*ta));

	return (&ta->na);
}

static void
tcp_update_association(struct nvmf_association *na,
    const nvme_identify_ctrl_t *cdata)
{
	nvmf_tcp_association_t *ta = TASSOC(na);

	/* IOCCSZ lives in the NVMe-oF region of Identify Controller (+0). */
	(void) memcpy(&ta->ioccsz, &cdata->id_nvmof[0], sizeof (ta->ioccsz));
	ta->ioccsz = LE_32(ta->ioccsz);
}

static void
tcp_free_association(struct nvmf_association *na)
{
	free(na);
}

static bool
tcp_connect(nvmf_tcp_qpair_t *qp, struct nvmf_association *na, bool admin)
{
	const nvmf_association_params_t *params = &na->na_params;
	nvmf_tcp_association_t *ta = TASSOC(na);
	nvmf_tcp_ic_req_t ic_req;
	nvmf_tcp_ic_resp_t ic_resp;
	uint32_t maxh2cdata;
	int error;

	if (!admin) {
		if (ta->ioccsz == 0) {
			na_error(na, "TCP I/O queues require cdata");
			return (false);
		}
		if (ta->ioccsz < 4) {
			na_error(na, "Invalid IOCCSZ %u", ta->ioccsz);
			return (false);
		}
	}

	(void) memset(&ic_req, 0, sizeof (ic_req));
	ic_req.nfic_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_IC_REQ;
	ic_req.nfic_common.ntcph_hlen = sizeof (ic_req);
	ic_req.nfic_common.ntcph_plen = LE_32(sizeof (ic_req));
	ic_req.nfic_pfv = LE_16(0);
	ic_req.nfic_hpda = params->nap_tcp.pda;
	if (params->nap_tcp.header_digests)
		ic_req.nfic_dgst.bits.hdgst_enable = 1;
	if (params->nap_tcp.data_digests)
		ic_req.nfic_dgst.bits.ddgst_enable = 1;
	ic_req.nfic_maxr2t = LE_32(params->nap_tcp.maxr2t);

	error = nvmf_tcp_write_pdu(qp, &ic_req, sizeof (ic_req));
	if (error != 0) {
		na_error(na, "Failed to write IC request: %s",
		    strerror(error));
		return (false);
	}

	if (!nvmf_tcp_read_ic_resp(na, qp, &ic_resp))
		return (false);

	/* Ensure the controller didn't enable digests we didn't request. */
	if ((!params->nap_tcp.header_digests &&
	    ic_resp.nfir_dgst.bits.hdgst_enable != 0) ||
	    (!params->nap_tcp.data_digests &&
	    ic_resp.nfir_dgst.bits.ddgst_enable != 0)) {
		na_error(na, "Controller enabled unrequested digests");
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER,
		    11, &ic_resp, sizeof (ic_resp), sizeof (ic_resp));
		return (false);
	}

	/*
	 * XXX: Is there an upper-bound to enforce here?  Perhaps pick
	 * some large value and report larger values as an unsupported
	 * parameter?
	 */
	maxh2cdata = LE_32(ic_resp.nfir_maxh2cdata);
	if (maxh2cdata < 4096 || maxh2cdata % 4 != 0) {
		na_error(na, "Invalid MAXH2CDATA %u", maxh2cdata);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 12, &ic_resp,
		    sizeof (ic_resp), sizeof (ic_resp));
		return (false);
	}

	qp->rxpda = (params->nap_tcp.pda + 1) * 4;
	qp->txpda = (ic_resp.nfir_cpda + 1) * 4;
	qp->header_digests = ic_resp.nfir_dgst.bits.hdgst_enable != 0;
	qp->data_digests = ic_resp.nfir_dgst.bits.ddgst_enable != 0;
	qp->maxr2t = params->nap_tcp.maxr2t;
	qp->maxh2cdata = maxh2cdata;
	if (admin)
		/* 7.4.3 */
		qp->max_icd = 8192;
	else
		qp->max_icd = (ta->ioccsz - 4) * 16;

	return (true);
}

static bool
tcp_accept(nvmf_tcp_qpair_t *qp, struct nvmf_association *na)
{
	const nvmf_association_params_t *params = &na->na_params;
	nvmf_tcp_ic_req_t ic_req;
	nvmf_tcp_ic_resp_t ic_resp;
	int error;

	if (!nvmf_tcp_read_ic_req(na, qp, &ic_req))
		return (false);

	(void) memset(&ic_resp, 0, sizeof (ic_resp));
	ic_resp.nfir_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_IC_RESP;
	ic_resp.nfir_common.ntcph_hlen = sizeof (ic_req);
	ic_resp.nfir_common.ntcph_plen = LE_32(sizeof (ic_req));
	ic_resp.nfir_pfv = LE_16(0);
	ic_resp.nfir_cpda = params->nap_tcp.pda;
	if (params->nap_tcp.header_digests &&
	    ic_req.nfic_dgst.bits.hdgst_enable != 0)
		ic_resp.nfir_dgst.bits.hdgst_enable = 1;
	if (params->nap_tcp.data_digests &&
	    ic_req.nfic_dgst.bits.ddgst_enable != 0)
		ic_resp.nfir_dgst.bits.ddgst_enable = 1;
	ic_resp.nfir_maxh2cdata = LE_32(params->nap_tcp.maxh2cdata);

	error = nvmf_tcp_write_pdu(qp, &ic_resp, sizeof (ic_resp));
	if (error != 0) {
		na_error(na, "Failed to write IC response: %s",
		    strerror(error));
		return (false);
	}

	qp->rxpda = (params->nap_tcp.pda + 1) * 4;
	qp->txpda = (ic_req.nfic_hpda + 1) * 4;
	qp->header_digests = ic_resp.nfir_dgst.bits.hdgst_enable != 0;
	qp->data_digests = ic_resp.nfir_dgst.bits.ddgst_enable != 0;
	qp->maxr2t = LE_32(ic_req.nfic_maxr2t);
	qp->maxh2cdata = params->nap_tcp.maxh2cdata;
	qp->max_icd = 0;	/* XXX */
	return (true);
}

static struct nvmf_qpair *
tcp_allocate_qpair(struct nvmf_association *na,
    const nvmf_qpair_params_t *qparams)
{
	const nvmf_association_params_t *aparams = &na->na_params;
	nvmf_tcp_qpair_t *qp;
	bool ok;

	if (aparams->nap_tcp.pda > NVMF_TCP_CPDA_MAX) {
		na_error(na, "Invalid PDA");
		return (NULL);
	}

	qp = calloc(1, sizeof (*qp));
	qp->s = qparams->nqp_tcp.fd;
	LIST_INIT(&qp->rx_buffers);
	LIST_INIT(&qp->tx_buffers);
	TAILQ_INIT(&qp->rx_capsules);
	if (na->na_controller)
		ok = tcp_accept(qp, na);
	else
		ok = tcp_connect(qp, na, qparams->nqp_admin);
	if (!ok) {
		free(qp);
		return (NULL);
	}

	return (&qp->qp);
}

static void
tcp_free_qpair(struct nvmf_qpair *nq)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	nvmf_tcp_capsule_t *ntc, *tc;
	nvmf_tcp_command_buffer_t *ncb, *cb;

	TAILQ_FOREACH_SAFE(tc, &qp->rx_capsules, link, ntc) {
		TAILQ_REMOVE(&qp->rx_capsules, tc, link);
		nvmf_free_capsule(&tc->nc);
	}
	LIST_FOREACH_SAFE(cb, &qp->rx_buffers, link, ncb) {
		tcp_free_command_buffer(cb);
	}
	LIST_FOREACH_SAFE(cb, &qp->tx_buffers, link, ncb) {
		tcp_free_command_buffer(cb);
	}
	free(qp);
}

static void
tcp_kernel_handoff_params(struct nvmf_qpair *nq, nvlist_t *nvl)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);

	(void) nvlist_add_uint64(nvl, "fd", qp->s);
	(void) nvlist_add_uint64(nvl, "rxpda", qp->rxpda);
	(void) nvlist_add_uint64(nvl, "txpda", qp->txpda);
	(void) nvlist_add_boolean_value(nvl, "header_digests",
	    qp->header_digests);
	(void) nvlist_add_boolean_value(nvl, "data_digests", qp->data_digests);
	(void) nvlist_add_uint64(nvl, "maxr2t", qp->maxr2t);
	(void) nvlist_add_uint64(nvl, "maxh2cdata", qp->maxh2cdata);
	(void) nvlist_add_uint64(nvl, "max_icd", qp->max_icd);
}

static int
tcp_populate_dle(struct nvmf_qpair *nq, nvmf_discovery_log_page_entry_t *dle)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	struct sockaddr_storage ss;
	socklen_t ss_len;

	ss_len = sizeof (ss);
	if (getpeername(qp->s, (struct sockaddr *)&ss, &ss_len) == -1)
		return (errno);

	if (getnameinfo((struct sockaddr *)&ss, ss_len,
	    (char *)dle->ndle_traddr, sizeof (dle->ndle_traddr),
	    (char *)dle->ndle_trsvcid, sizeof (dle->ndle_trsvcid),
	    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
		return (EINVAL);

	return (0);
}

static struct nvmf_capsule *
tcp_allocate_capsule(struct nvmf_qpair *qp __unused)
{
	nvmf_tcp_capsule_t *nc;

	nc = calloc(1, sizeof (*nc));
	return (&nc->nc);
}

static void
tcp_free_capsule(struct nvmf_capsule *nc)
{
	nvmf_tcp_capsule_t *tc = TCAP(nc);

	nvmf_tcp_free_pdu(&tc->rx_pdu);
	if (tc->cb != NULL)
		tcp_free_command_buffer(tc->cb);
	free(tc);
}

static int
tcp_transmit_command(struct nvmf_capsule *nc)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_capsule_t *tc = TCAP(nc);
	nvmf_tcp_cmd_t cmd;
	nvme_sqe_t *sqe;
	nvme_sgl_t *sgl;
	uint8_t sgl_type;
	int error;
	bool use_icd;

	use_icd = false;
	if (nc->nc_data_len != 0 && nc->nc_send_data &&
	    nc->nc_data_len <= qp->max_icd)
		use_icd = true;

	(void) memset(&cmd, 0, sizeof (cmd));
	cmd.ntc_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_CMD;
	(void) memcpy(cmd.ntc_ccsqe, &nc->nc_sqe, sizeof (nc->nc_sqe));

	/* Populate SGL in SQE. */
	sqe = (nvme_sqe_t *)cmd.ntc_ccsqe;
	sgl = &sqe->sqe_dptr.d_sgl;
	(void) memset(sgl, 0, sizeof (*sgl));
	sgl->sgl_addr = 0;
	sgl->sgl_len = LE_32(nc->nc_data_len);
	if (use_icd)
		sgl_type = NVME_SGL_TYPE_ICD;		/* in-capsule data */
	else
		sgl_type = NVME_SGL_TYPE_COMMAND_BUFFER; /* command buffer */
	sgl->sgl_type = (uint8_t)(sgl_type >> 4);
	sgl->sgl_zero = (uint8_t)(sgl_type & 0xf);

	/* Send command capsule. */
	error = nvmf_tcp_construct_pdu(qp, &cmd, sizeof (cmd), use_icd ?
	    nc->nc_data : NULL, use_icd ? nc->nc_data_len : 0);
	if (error != 0)
		return (error);

	/*
	 * If data will be transferred using a command buffer, allocate a
	 * buffer structure and queue it.
	 */
	if (nc->nc_data_len != 0 && !use_icd) {
		tc->cb = tcp_alloc_command_buffer(qp, nc->nc_data, 0,
		    nc->nc_data_len, sqe->sqe_cid, 0, !nc->nc_send_data);
		if (tc->cb == NULL)
			return (ENOMEM);
	}

	return (0);
}

static int
tcp_transmit_response(struct nvmf_capsule *nc)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_rsp_t rsp;

	(void) memset(&rsp, 0, sizeof (rsp));
	rsp.ntr_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_RESP;
	(void) memcpy(rsp.ntr_rccqe, &nc->nc_cqe, sizeof (nc->nc_cqe));

	return (nvmf_tcp_construct_pdu(qp, &rsp, sizeof (rsp), NULL, 0));
}

static int
tcp_transmit_capsule(struct nvmf_capsule *nc)
{
	if (nc->nc_qe_len == sizeof (nvme_sqe_t))
		return (tcp_transmit_command(nc));
	else
		return (tcp_transmit_response(nc));
}

static int
tcp_receive_capsule(struct nvmf_qpair *nq, struct nvmf_capsule **ncp)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	nvmf_tcp_capsule_t *tc;
	int error;

	while (TAILQ_EMPTY(&qp->rx_capsules)) {
		error = nvmf_tcp_receive_pdu(qp);
		if (error != 0)
			return (error);
	}
	tc = TAILQ_FIRST(&qp->rx_capsules);
	TAILQ_REMOVE(&qp->rx_capsules, tc, link);
	*ncp = &tc->nc;
	return (0);
}

static uint8_t
tcp_validate_command_capsule(const struct nvmf_capsule *nc)
{
	const nvmf_tcp_capsule_t *tc = CTCAP(nc);
	const nvme_sgl_t *sgl;
	uint8_t sgl_type;

	assert(tc->rx_pdu.hdr != NULL);

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	switch (sgl_type) {
	case NVME_SGL_TYPE_ICD:
		if (tc->rx_pdu.data_len != LE_32(sgl->sgl_len)) {
			(void) printf("NVMe/TCP: Command Capsule with "
			    "mismatched ICD length\n");
			return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
		}
		break;
	case NVME_SGL_TYPE_COMMAND_BUFFER:
		if (tc->rx_pdu.data_len != 0) {
			(void) printf("NVMe/TCP: Command Buffer SGL with ICD\n");
			return (NVME_CQE_SC_GEN_INV_FLD);
		}
		break;
	default:
		(void) printf("NVMe/TCP: Invalid SGL type in Command "
		    "Capsule\n");
		return (NVME_CQE_SC_GEN_INV_SGL_DESC);
	}

	if (sgl->sgl_addr != 0) {
		(void) printf("NVMe/TCP: Invalid SGL offset in Command "
		    "Capsule\n");
		return (NVME_CQE_SC_GEN_INV_SGL_OFF);
	}

	return (NVME_CQE_SC_GEN_SUCCESS);
}

static size_t
tcp_capsule_data_len(const struct nvmf_capsule *nc)
{
	assert(nc->nc_qe_len == sizeof (nvme_sqe_t));
	return (LE_32(nc->nc_sqe.sqe_dptr.d_sgl.sgl_len));
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

	icd = (const char *)tc->rx_pdu.hdr + tc->rx_pdu.hdr->ntcph_pdo +
	    data_offset;
	(void) memcpy(buf, icd, len);
	return (0);
}

static int
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
	if (data_offset + len > data_len)
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

static int
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

struct nvmf_transport_ops tcp_ops = {
	.allocate_association = tcp_allocate_association,
	.update_association = tcp_update_association,
	.free_association = tcp_free_association,
	.allocate_qpair = tcp_allocate_qpair,
	.free_qpair = tcp_free_qpair,
	.kernel_handoff_params = tcp_kernel_handoff_params,
	.populate_dle = tcp_populate_dle,
	.allocate_capsule = tcp_allocate_capsule,
	.free_capsule = tcp_free_capsule,
	.transmit_capsule = tcp_transmit_capsule,
	.receive_capsule = tcp_receive_capsule,
	.validate_command_capsule = tcp_validate_command_capsule,
	.capsule_data_len = tcp_capsule_data_len,
	.receive_controller_data = tcp_receive_controller_data,
	.send_controller_data = tcp_send_controller_data,
};

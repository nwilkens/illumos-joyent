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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/nvmf_tcp.c.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * NVMe/TCP PDUs: the digests, and building and checking a PDU.
 */

#include <sys/types.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/list.h>
#include <sys/atomic.h>
#include <sys/condvar.h>
#include <sys/mutex.h>
#include <sys/sysmacros.h>
#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/byteorder.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/strsun.h>
#include <sys/socket.h>
#include <sys/ksocket.h>
#include <sys/file.h>
#include <sys/socketvar.h>
#include <netinet/in.h>
#include <sys/cred.h>
#include <sys/crc32.h>
#include <sys/disp.h>
#include <sys/taskq.h>
#include <sys/time.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>
#include <sys/nvme/nvmf_tcp.h>

#include "nvmf_tcp_impl.h"

/* Minimum spacing of the warnings that a peer can trigger. */
static hrtime_t nvmf_tcp_warn_interval = 5 * (hrtime_t)NANOSEC;
static volatile hrtime_t nvmf_tcp_warn_last;

uint32_t nvmf_tcp_crc32c_table[256];

static uint32_t
nvmf_tcp_crc32c(const void *buf, size_t len, uint32_t start)
{
	uint32_t crc = start;

	CRC32(crc, buf, len, start, nvmf_tcp_crc32c_table);
	return (crc);
}

static uint32_t
nvmf_tcp_compute_digest(const void *buf, size_t len)
{
	return (nvmf_tcp_crc32c(buf, len, 0xffffffffU) ^ 0xffffffffU);
}

/* CRC-32C of len bytes at offset in an mblk chain. */
static uint32_t
nvmf_tcp_mblk_crc32c(mblk_t *mp, uint_t offset, uint_t len)
{
	uint32_t digest = 0xffffffffU;

	while (mp != NULL && offset >= MBLKL(mp)) {
		offset -= MBLKL(mp);
		mp = mp->b_cont;
	}
	while (len != 0 && mp != NULL) {
		uint_t todo = (uint_t)MIN(MBLKL(mp) - offset, len);
		digest = nvmf_tcp_crc32c(mp->b_rptr + offset, todo, digest);
		offset = 0;
		len -= todo;
		mp = mp->b_cont;
	}
	return (digest ^ 0xffffffffU);
}

/* Copy len bytes at off of an mblk chain into a flat buffer. */
static void
nvmf_tcp_mblk_copydata(mblk_t *mp, uint_t off, uint_t len, void *dst)
{
	uint8_t *out = dst;

	while (mp != NULL && off >= MBLKL(mp)) {
		off -= MBLKL(mp);
		mp = mp->b_cont;
	}
	while (len != 0 && mp != NULL) {
		uint_t todo = (uint_t)MIN(MBLKL(mp) - off, len);
		bcopy(mp->b_rptr + off, out, todo);
		off = 0;
		out += todo;
		len -= todo;
		mp = mp->b_cont;
	}
}

/* The unlocked update can let an extra message through. */
boolean_t
nvmf_tcp_warn_ok(void)
{
	hrtime_t now = gethrtime();

	if (now - nvmf_tcp_warn_last < nvmf_tcp_warn_interval)
		return (B_FALSE);
	nvmf_tcp_warn_last = now;
	return (B_TRUE);
}

/* An mblk of len bytes; this sleeps for memory and does not fail. */
mblk_t *
nvmf_tcp_allocb(size_t len)
{
	mblk_t *mp;
	int error;

	mp = allocb_wait(len, BPRI_MED, STR_NOSIG, &error);
	mp->b_wptr = mp->b_rptr + len;
	return (mp);
}

/*
 * Queue a termination request that carries the first hlen bytes of the
 * PDU header at fault.
 */
void
nvmf_tcp_send_term_req(nvmf_tcp_qpair_t *qp, uint16_t fes, uint32_t fei,
    const void *pdu_hdr, uint_t hlen)
{
	nvmf_tcp_term_req_hdr_t *hdr;
	mblk_t *mp;

	hlen = MIN(hlen, NVMF_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);

	mp = nvmf_tcp_allocb(sizeof (*hdr) + hlen);
	hdr = (nvmf_tcp_term_req_hdr_t *)mp->b_rptr;
	bzero(hdr, sizeof (*hdr));
	hdr->nttr_common.ntcph_pdu_type = qp->qp.nq_controller ?
	    NVMF_TCP_PDU_TYPE_C2H_TERM_REQ : NVMF_TCP_PDU_TYPE_H2C_TERM_REQ;
	hdr->nttr_common.ntcph_hlen = sizeof (*hdr);
	hdr->nttr_common.ntcph_plen = LE_32((uint32_t)(sizeof (*hdr) + hlen));
	hdr->nttr_fes = LE_16(fes);
	hdr->nttr_fei[0] = (uint8_t)(fei);
	hdr->nttr_fei[1] = (uint8_t)(fei >> 8);
	hdr->nttr_fei[2] = (uint8_t)(fei >> 16);
	hdr->nttr_fei[3] = (uint8_t)(fei >> 24);

	if (hlen != 0)
		bcopy(pdu_hdr, hdr + 1, hlen);

	nvmf_tcp_write_pdu(qp, mp);
}

/* A received PDU is one mblk. */
void
nvmf_tcp_report_error(nvmf_tcp_qpair_t *qp, uint16_t fes, uint32_t fei,
    mblk_t *rx_pdu, uint_t hlen)
{
	nvmf_tcp_send_term_req(qp, fes, fei, rx_pdu->b_rptr,
	    (uint_t)MIN(hlen, MBLKL(rx_pdu)));
}

int
nvmf_tcp_validate_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_common_pdu_hdr_t *ch;
	mblk_t *mp = pdu->rp_mp;
	uint32_t data_len, fei, plen;
	uint32_t digest, rx_digest;
	uint_t hlen;
	int error;
	uint16_t fes;

	/* Determine how large of a PDU header to return for errors. */
	ch = pdu->rp_hdr;
	hlen = ch->ntcph_hlen;
	plen = LE_32(ch->ntcph_plen);
	if (hlen < sizeof (*ch) || hlen > plen)
		hlen = sizeof (*ch);

	error = nvmf_tcp_validate_pdu_header(ch, qp->qp.nq_controller,
	    qp->tq_header_digests, qp->tq_data_digests, qp->tq_rxpda, &data_len,
	    &fes, &fei);
	if (error != 0) {
		if (error != ECONNRESET)
			nvmf_tcp_report_error(qp, fes, fei, mp, hlen);
		return (error);
	}

	/* The header checks imply this; the handlers rely on it. */
	if (plen != MBLKL(mp) || ch->ntcph_hlen > plen)
		return (ECONNRESET);

	/* Check header digest if present. */
	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_HDGSTF) != 0) {
		digest = nvmf_tcp_mblk_crc32c(mp, 0, ch->ntcph_hlen);
		nvmf_tcp_mblk_copydata(mp, ch->ntcph_hlen, sizeof (rx_digest),
		    &rx_digest);
		if (digest != rx_digest) {
			if (nvmf_tcp_warn_ok())
				cmn_err(CE_WARN,
				    "NVMe/TCP: Header digest mismatch");
			nvmf_tcp_report_error(qp,
			    NVMF_TCP_TERM_REQ_FES_HDGST_ERROR, rx_digest, mp,
			    hlen);
			return (EBADMSG);
		}
	}

	/* Check data digest if present. */
	pdu->rp_data_digest_mismatch = B_FALSE;
	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_DDGSTF) != 0) {
		digest = nvmf_tcp_mblk_crc32c(mp, ch->ntcph_pdo, data_len);
		nvmf_tcp_mblk_copydata(mp, plen - sizeof (rx_digest),
		    sizeof (rx_digest), &rx_digest);
		if (digest != rx_digest) {
			if (nvmf_tcp_warn_ok())
				cmn_err(CE_WARN,
				    "NVMe/TCP: Data digest mismatch");
			pdu->rp_data_digest_mismatch = B_TRUE;
		}
	}

	pdu->rp_data_len = data_len;
	return (0);
}

void
nvmf_tcp_free_pdu(nvmf_tcp_rxpdu_t *pdu)
{
	freemsg(pdu->rp_mp);
	pdu->rp_mp = NULL;
	pdu->rp_hdr = NULL;
}

/*
 * Build a PDU: header, header digest, pad, then the data chain and its
 * digest.  This sleeps for memory and consumes data.
 */
mblk_t *
nvmf_tcp_construct_pdu(nvmf_tcp_qpair_t *qp, void *hdr, size_t hlen,
    mblk_t *data, uint32_t data_len)
{
	nvmf_tcp_common_pdu_hdr_t *ch;
	mblk_t *top;
	uint32_t digest, pad, pdo, plen, mlen;

	plen = (uint32_t)hlen;
	if (qp->tq_header_digests)
		plen += sizeof (digest);
	if (data_len != 0) {
		uint32_t pda = qp->tq_txpda;

		ASSERT3U(msgdsize(data), ==, data_len);
		/* A PDA is a multiple of four, not always a power of two. */
		pdo = ((plen + pda - 1) / pda) * pda;
		pad = pdo - plen;
		plen = pdo + data_len;
		if (qp->tq_data_digests)
			plen += sizeof (digest);
		mlen = pdo;
	} else {
		ASSERT3P(data, ==, NULL);
		pdo = 0;
		pad = 0;
		mlen = plen;
	}

	top = nvmf_tcp_allocb(mlen);
	ch = (nvmf_tcp_common_pdu_hdr_t *)top->b_rptr;
	bcopy(hdr, ch, hlen);
	ch->ntcph_hlen = (uint8_t)hlen;
	if (qp->tq_header_digests)
		ch->ntcph_flags |= NVMF_TCP_CH_FLAGS_HDGSTF;
	if (qp->tq_data_digests && data_len != 0)
		ch->ntcph_flags |= NVMF_TCP_CH_FLAGS_DDGSTF;
	ch->ntcph_pdo = (uint8_t)pdo;
	ch->ntcph_plen = LE_32(plen);

	/* HDGST */
	if (qp->tq_header_digests) {
		digest = nvmf_tcp_compute_digest(ch, hlen);
		bcopy(&digest, (char *)ch + hlen, sizeof (digest));
	}

	/* PAD */
	if (pad != 0)
		bzero((char *)ch + pdo - pad, pad);

	if (data_len != 0) {
		/* DATA */
		top->b_cont = data;

		/* DDGST */
		if (qp->tq_data_digests) {
			mblk_t *dtail = data;
			mblk_t *dmp;

			digest = nvmf_tcp_mblk_crc32c(data, 0, data_len);

			while (dtail->b_cont != NULL)
				dtail = dtail->b_cont;

			dmp = nvmf_tcp_allocb(sizeof (digest));
			bcopy(&digest, dmp->b_rptr, sizeof (digest));
			dtail->b_cont = dmp;
		}
	}

	return (top);
}

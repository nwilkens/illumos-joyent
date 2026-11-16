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
 * NVMe over Fabrics TCP transport: the NVMe/TCP PDU framing on a TCP socket
 * that a userland daemon connected and handed off by file descriptor.  Each
 * qpair has a receive thread that reads whole PDUs from the socket and a
 * transmit thread that writes them.
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

#include "nvmf_core.h"
#include "nvmf_transport_internal.h"

/* Largest data payload of one transmitted PDU. */
static uint_t nvmf_tcp_max_transmit_data = 256 * 1024;

/* Largest in-capsule data a controller accepts; IOCCSZ follows from it. */
static uint_t nvmf_tcp_max_icd = 16 * 1024;

/* Largest transfer of one host command; it bounds a received C2H PDU. */
static uint_t nvmf_tcp_max_io_size = 1024 * 1024;

/* How long nvmf_free_qpair() lets queued PDUs reach the socket. */
static uint_t nvmf_tcp_drain_secs = 2;

/* How long to wait for the peer to close after a termination request. */
static uint_t nvmf_tcp_term_wait_secs = 30;

/* Minimum spacing of the warnings that a peer can trigger. */
static hrtime_t nvmf_tcp_warn_interval = 5 * (hrtime_t)NANOSEC;
static volatile hrtime_t nvmf_tcp_warn_last;

#define	NVMF_TCP_MIN_MAXH2CDATA	4096
#define	NVMF_TCP_MIN_PDA	4
#define	NVMF_TCP_MAX_PDA	128

struct nvmf_tcp_capsule;
struct nvmf_tcp_qpair;

static void	tcp_release_capsule(struct nvmf_tcp_capsule *tc);
static mblk_t	*capsule_to_pdu(struct nvmf_tcp_qpair *qp,
		    struct nvmf_tcp_capsule *tc);

/*
 * A data transfer of a command: host data sent for an R2T, host data received
 * by C2H, or controller data received by H2C.
 */
typedef struct nvmf_tcp_command_buffer {
	struct nvmf_tcp_qpair		*tcb_qp;

	struct nvmf_io_request		tcb_io;
	size_t				tcb_data_len;
	size_t				tcb_data_xfered;
	uint32_t			tcb_data_offset;

	volatile uint_t			tcb_refs;
	int				tcb_error;
	/* The callback ran at cancel; tq_rx_buffers.tcbl_lock. */
	boolean_t			tcb_done;

	uint16_t			tcb_cid;
	uint16_t			tcb_ttag;

	list_node_t			tcb_link;

	/* Controller only. */
	struct nvmf_tcp_capsule		*tcb_tc;
} nvmf_tcp_command_buffer_t;

typedef struct nvmf_tcp_command_buffer_list {
	list_t				tcbl_list;
	kmutex_t			tcbl_lock;
} nvmf_tcp_command_buffer_list_t;

typedef struct nvmf_tcp_qpair {
	struct nvmf_qpair		qp;

	/* Our hold on the daemon's open file keeps the socket. */
	file_t				*tq_fp;
	ksocket_t			tq_so;
	boolean_t			tq_started;

	volatile uint_t			tq_refs;	/* one per capsule */
	uint8_t				tq_txpda;
	uint8_t				tq_rxpda;
	boolean_t			tq_header_digests;
	boolean_t			tq_data_digests;
	uint32_t			tq_maxr2t;
	uint32_t			tq_maxh2cdata;	/* controller only */
	uint32_t			tq_max_tx_data;
	uint32_t			tq_max_icd;	/* host only */
	uint32_t			tq_max_rx_icd;	/* controller only */
	uint32_t			tq_max_io;	/* host only */
	uint32_t			tq_max_rx_pdu;
	uint16_t			tq_next_ttag;	/* controller only */
	uint_t				tq_num_ttags;	/* controller only */
	uint_t				tq_active_ttags; /* controller only */
	boolean_t			tq_send_success; /* controller only */

	kmutex_t			tq_rx_lock;
	kcondvar_t			tq_rx_cv;
	boolean_t			tq_rx_shutdown;
	taskq_t				*tq_rx_taskq;

	kmutex_t			tq_tx_lock;
	kcondvar_t			tq_tx_cv;
	kcondvar_t			tq_tx_done_cv;
	boolean_t			tq_tx_draining;
	boolean_t			tq_tx_shutdown;
	boolean_t			tq_tx_exited;
	mblk_t				*tq_tx_pdus_head;
	mblk_t				*tq_tx_pdus_tail;
	list_t				tq_tx_capsules;
	taskq_t				*tq_tx_taskq;

	nvmf_tcp_command_buffer_list_t	tq_tx_buffers;
	nvmf_tcp_command_buffer_list_t	tq_rx_buffers;

	/*
	 * A controller's receive buffer waits on tq_rx_buffers for a transfer
	 * tag and then sits in tq_open_ttags[] under that tag.  Both, and
	 * tc_rx_busy of every capsule, are under tq_rx_buffers.tcbl_lock.
	 */
	nvmf_tcp_command_buffer_t	**tq_open_ttags; /* controller only */
	boolean_t			tq_rx_closed;
	kcondvar_t			tq_cancel_cv;
} nvmf_tcp_qpair_t;

typedef struct nvmf_tcp_rxpdu {
	mblk_t				*rp_mp;
	const nvmf_tcp_common_pdu_hdr_t	*rp_hdr;
	uint32_t			rp_data_len;
	boolean_t			rp_data_digest_mismatch;
} nvmf_tcp_rxpdu_t;

typedef struct nvmf_tcp_capsule {
	struct nvmf_capsule		nc;

	volatile uint_t			tc_refs;

	nvmf_tcp_rxpdu_t		tc_rx_pdu;

	uint32_t			tc_active_r2ts;	/* controller only */
	/* Threads copying into or completing a receive of this capsule. */
	uint_t				tc_rx_busy;	/* controller only */
#ifdef DEBUG
	uint32_t			tc_tx_data_offset; /* controller only */
	uint_t				tc_pending_r2ts;   /* controller only */
#endif

	list_node_t			tc_link;
} nvmf_tcp_capsule_t;

#define	TCAP(nc)	((nvmf_tcp_capsule_t *)(nc))
#define	TQP(qp)		((nvmf_tcp_qpair_t *)(qp))

static boolean_t
nvmf_tcp_refcount_release(volatile uint_t *count)
{
	ASSERT3U(*count, >, 0);
	return (atomic_dec_uint_nv(count) == 0);
}

static void
nvmf_tcp_refcount_acquire(volatile uint_t *count)
{
	atomic_inc_uint(count);
}

/* The header and data digests are CRC-32C, reflected polynomial. */
#define	NVMF_TCP_CRC32C_POLY	0x82F63B78U
static uint32_t nvmf_tcp_crc32c_table[256];

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
static boolean_t
nvmf_tcp_warn_ok(void)
{
	hrtime_t now = gethrtime();

	if (now - nvmf_tcp_warn_last < nvmf_tcp_warn_interval)
		return (B_FALSE);
	nvmf_tcp_warn_last = now;
	return (B_TRUE);
}

/* Is [off, off + len) inside [base, base + total)?  The sums can wrap. */
static boolean_t
nvmf_tcp_range_ok(uint32_t off, uint32_t len, uint32_t base, size_t total)
{
	return (off >= base && (uint64_t)off - base <= total &&
	    len <= total - ((uint64_t)off - base));
}

/* Check the transport's values of a qpair handoff. */
static boolean_t
nvmf_tcp_params_ok(uint64_t rxpda, uint64_t txpda, uint64_t maxr2t,
    uint64_t maxh2cdata, uint64_t max_icd, uint64_t qsize)
{
	if (rxpda < NVMF_TCP_MIN_PDA || rxpda > NVMF_TCP_MAX_PDA ||
	    rxpda % 4 != 0 || txpda < NVMF_TCP_MIN_PDA ||
	    txpda > NVMF_TCP_MAX_PDA || txpda % 4 != 0)
		return (B_FALSE);
	/* maxr2t is 0's based. */
	if (maxr2t >= UINT32_MAX || max_icd > UINT32_MAX)
		return (B_FALSE);
	if (maxh2cdata < NVMF_TCP_MIN_MAXH2CDATA || maxh2cdata > UINT32_MAX ||
	    maxh2cdata % 4 != 0)
		return (B_FALSE);
	return (qsize >= 2 && qsize <= (uint64_t)UINT16_MAX + 1);
}

/*
 * The longest PDU a peer may send when no PDU carries more than max_data
 * bytes.  The command capsule has the longest header that precedes data.
 */
static uint32_t
nvmf_tcp_rx_limit(uint32_t max_data, uint8_t rxpda, boolean_t header_digests,
    boolean_t data_digests)
{
	uint64_t len = sizeof (nvmf_tcp_cmd_t);

	if (header_digests)
		len += sizeof (uint32_t);
	len = ((len + rxpda - 1) / rxpda) * rxpda + max_data;
	if (data_digests)
		len += sizeof (uint32_t);
	len = MAX(len, NVMF_TCP_TERM_REQ_PDU_MAX_SIZE);
	return ((uint32_t)MIN(len, UINT32_MAX));
}

static nvmf_tcp_command_buffer_t *
tcp_alloc_command_buffer(nvmf_tcp_qpair_t *qp, const struct nvmf_io_request *io,
    uint32_t data_offset, size_t data_len, uint16_t cid)
{
	nvmf_tcp_command_buffer_t *cb;

	cb = kmem_zalloc(sizeof (*cb), KM_SLEEP);
	cb->tcb_qp = qp;
	cb->tcb_io = *io;
	cb->tcb_data_offset = data_offset;
	cb->tcb_data_len = data_len;
	cb->tcb_refs = 1;
	cb->tcb_cid = cid;
	return (cb);
}

static void
tcp_hold_command_buffer(nvmf_tcp_command_buffer_t *cb)
{
	nvmf_tcp_refcount_acquire(&cb->tcb_refs);
}

static void
tcp_free_command_buffer(nvmf_tcp_command_buffer_t *cb)
{
	if (!cb->tcb_done) {
		nvmf_complete_io_request(&cb->tcb_io, cb->tcb_data_xfered,
		    cb->tcb_error);
	}
	if (cb->tcb_tc != NULL)
		tcp_release_capsule(cb->tcb_tc);
	kmem_free(cb, sizeof (*cb));
}

static void
tcp_release_command_buffer(nvmf_tcp_command_buffer_t *cb)
{
	if (nvmf_tcp_refcount_release(&cb->tcb_refs))
		tcp_free_command_buffer(cb);
}

static void
tcp_add_command_buffer(nvmf_tcp_command_buffer_list_t *list,
    nvmf_tcp_command_buffer_t *cb)
{
	ASSERT(MUTEX_HELD(&list->tcbl_lock));
	list_insert_head(&list->tcbl_list, cb);
}

static nvmf_tcp_command_buffer_t *
tcp_find_command_buffer(nvmf_tcp_command_buffer_list_t *list, uint16_t cid,
    uint16_t ttag)
{
	nvmf_tcp_command_buffer_t *cb;

	ASSERT(MUTEX_HELD(&list->tcbl_lock));
	for (cb = list_head(&list->tcbl_list); cb != NULL;
	    cb = list_next(&list->tcbl_list, cb)) {
		if (cb->tcb_cid == cid && cb->tcb_ttag == ttag)
			return (cb);
	}
	return (NULL);
}

static void
tcp_remove_command_buffer(nvmf_tcp_command_buffer_list_t *list,
    nvmf_tcp_command_buffer_t *cb)
{
	ASSERT(MUTEX_HELD(&list->tcbl_lock));
	list_remove(&list->tcbl_list, cb);
}

static void
tcp_purge_command_buffer(nvmf_tcp_command_buffer_list_t *list, uint16_t cid,
    uint16_t ttag)
{
	nvmf_tcp_command_buffer_t *cb;

	mutex_enter(&list->tcbl_lock);
	cb = tcp_find_command_buffer(list, cid, ttag);
	if (cb != NULL) {
		tcp_remove_command_buffer(list, cb);
		mutex_exit(&list->tcbl_lock);
		tcp_release_command_buffer(cb);
	} else {
		mutex_exit(&list->tcbl_lock);
	}
}

/* Queue a PDU for the tx worker; b_next links the queue. */
static void
nvmf_tcp_write_pdu(nvmf_tcp_qpair_t *qp, mblk_t *mp)
{
	mutex_enter(&qp->tq_tx_lock);
	if (qp->tq_tx_shutdown) {
		mutex_exit(&qp->tq_tx_lock);
		freemsg(mp);
		return;
	}
	mp->b_next = NULL;
	if (qp->tq_tx_pdus_tail == NULL)
		qp->tq_tx_pdus_head = mp;
	else
		qp->tq_tx_pdus_tail->b_next = mp;
	qp->tq_tx_pdus_tail = mp;
	cv_signal(&qp->tq_tx_cv);
	mutex_exit(&qp->tq_tx_lock);
}

/* An mblk of len bytes; this sleeps for memory and does not fail. */
static mblk_t *
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
static void
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
static void
nvmf_tcp_report_error(nvmf_tcp_qpair_t *qp, uint16_t fes, uint32_t fei,
    mblk_t *rx_pdu, uint_t hlen)
{
	nvmf_tcp_send_term_req(qp, fes, fei, rx_pdu->b_rptr,
	    (uint_t)MIN(hlen, MBLKL(rx_pdu)));
}

static int
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

static void
nvmf_tcp_free_pdu(nvmf_tcp_rxpdu_t *pdu)
{
	freemsg(pdu->rp_mp);
	pdu->rp_mp = NULL;
	pdu->rp_hdr = NULL;
}

static int
nvmf_tcp_handle_term_req(nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_term_req_hdr_t *hdr;

	hdr = (const void *)pdu->rp_hdr;

	if (nvmf_tcp_warn_ok()) {
		cmn_err(CE_WARN,
		    "NVMe/TCP: Received termination request: fes 0x%x fei 0x%x",
		    LE_16(hdr->nttr_fes),
		    (uint32_t)hdr->nttr_fei[0] |
		    ((uint32_t)hdr->nttr_fei[1] << 8) |
		    ((uint32_t)hdr->nttr_fei[2] << 16) |
		    ((uint32_t)hdr->nttr_fei[3] << 24));
	}
	nvmf_tcp_free_pdu(pdu);
	return (ECONNRESET);
}

static int
nvmf_tcp_save_command_capsule(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_cmd_t *cmd;
	struct nvmf_capsule *nc;
	nvmf_tcp_capsule_t *tc;

	cmd = (const void *)pdu->rp_hdr;

	if (pdu->rp_data_len > qp->tq_max_rx_icd) {
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED, 0,
		    pdu->rp_mp, pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	nc = nvmf_allocate_command(&qp->qp, cmd->ntc_ccsqe, KM_SLEEP);

	/* Keep the PSDT the host sent, for nvmf_validate_command_capsule(). */
	bcopy(cmd->ntc_ccsqe, &nc->nc_sqe, sizeof (nc->nc_sqe));

	tc = TCAP(nc);
	tc->tc_rx_pdu = *pdu;

	nvmf_capsule_received(&qp->qp, nc);
	return (0);
}

static int
nvmf_tcp_save_response_capsule(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_rsp_t *rsp;
	struct nvmf_capsule *nc;
	nvmf_tcp_capsule_t *tc;
	uint16_t cid;

	rsp = (const void *)pdu->rp_hdr;

	nc = nvmf_allocate_response(&qp->qp, rsp->ntr_rccqe, KM_SLEEP);

	nc->nc_sqhd_valid = B_TRUE;
	tc = TCAP(nc);
	tc->tc_rx_pdu = *pdu;

	/*
	 * Once the CQE has been received, no further transfers to the
	 * command buffer for the associated CID can occur.
	 */
	cid = nc->nc_cqe.cqe_cid;
	tcp_purge_command_buffer(&qp->tq_rx_buffers, cid, 0);
	tcp_purge_command_buffer(&qp->tq_tx_buffers, cid, 0);

	nvmf_capsule_received(&qp->qp, nc);
	return (0);
}

/*
 * Build a PDU: header, header digest, pad, then the data chain and its
 * digest.  This sleeps for memory and consumes data.
 */
static mblk_t *
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

static nvmf_tcp_command_buffer_t *
nvmf_tcp_next_r2t(nvmf_tcp_qpair_t *qp)
{
	nvmf_tcp_command_buffer_t *cb, *ncb;

	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));
	ASSERT3U(qp->tq_active_ttags, <, qp->tq_num_ttags);

	for (cb = list_head(&qp->tq_rx_buffers.tcbl_list); cb != NULL;
	    cb = ncb) {
		ncb = list_next(&qp->tq_rx_buffers.tcbl_list, cb);
		/* NB: maxr2t is 0's based. */
		if (cb->tcb_tc->tc_active_r2ts > qp->tq_maxr2t)
			continue;
#ifdef DEBUG
		cb->tcb_tc->tc_pending_r2ts--;
#endif
		list_remove(&qp->tq_rx_buffers.tcbl_list, cb);
		return (cb);
	}
	return (NULL);
}

static void
nvmf_tcp_allocate_ttag(nvmf_tcp_qpair_t *qp, nvmf_tcp_command_buffer_t *cb)
{
	uint16_t ttag;

	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));

	ttag = qp->tq_next_ttag;
	for (;;) {
		if (qp->tq_open_ttags[ttag] == NULL)
			break;
		if (ttag == qp->tq_num_ttags - 1)
			ttag = 0;
		else
			ttag++;
		ASSERT3U(ttag, !=, qp->tq_next_ttag);
	}
	if (ttag == qp->tq_num_ttags - 1)
		qp->tq_next_ttag = 0;
	else
		qp->tq_next_ttag = ttag + 1;

	cb->tcb_tc->tc_active_r2ts++;
	qp->tq_active_ttags++;
	qp->tq_open_ttags[ttag] = cb;

	/* ttag is an opaque cookie returned by the host as-is. */
	cb->tcb_ttag = ttag;
}

/* NB: cid and ttag are both little-endian already. */
static void
tcp_send_r2t(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    uint32_t data_offset, uint32_t data_len)
{
	nvmf_tcp_r2t_hdr_t r2t;
	mblk_t *mp;

	bzero(&r2t, sizeof (r2t));
	r2t.ntr2t_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_R2T;
	r2t.ntr2t_cccid = cid;
	r2t.ntr2t_ttag = ttag;
	r2t.ntr2t_r2to = LE_32(data_offset);
	r2t.ntr2t_r2tl = LE_32(data_len);

	mp = nvmf_tcp_construct_pdu(qp, &r2t, sizeof (r2t), NULL, 0);
	nvmf_tcp_write_pdu(qp, mp);
}

/* Release cb's transfer tag and send the next R2T.  Drops tcbl_lock. */
static void
nvmf_tcp_send_next_r2t(nvmf_tcp_qpair_t *qp, nvmf_tcp_command_buffer_t *cb)
{
	nvmf_tcp_command_buffer_t *ncb;

	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));
	ASSERT3P(qp->tq_open_ttags[cb->tcb_ttag], ==, cb);

	qp->tq_open_ttags[cb->tcb_ttag] = NULL;
	qp->tq_active_ttags--;
	cb->tcb_tc->tc_active_r2ts--;

	ncb = nvmf_tcp_next_r2t(qp);
	if (ncb != NULL) {
		nvmf_tcp_allocate_ttag(qp, ncb);
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		tcp_send_r2t(qp, ncb->tcb_cid, ncb->tcb_ttag,
		    ncb->tcb_data_offset, (uint32_t)ncb->tcb_data_len);
	} else {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
	}
}

/*
 * A thread that copies into a controller receive buffer or runs its callback
 * outside tcbl_lock marks the capsule busy, so that a cancel can wait.
 */
static void
tcp_rx_busy_enter(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));
	nvmf_tcp_refcount_acquire(&tc->tc_refs);
	tc->tc_rx_busy++;
}

static void
tcp_rx_busy_exit(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	tc->tc_rx_busy--;
	cv_broadcast(&qp->tq_cancel_cv);
	mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
	tcp_release_capsule(tc);
}

/* Copy len bytes at skip of an mblk chain to io_offset of an I/O buffer. */
static void
nvmf_tcp_mblk_copyto_io(mblk_t *mp, uint_t skip, uint_t len,
    struct nvmf_io_request *io, uint_t io_offset)
{
	while (mp != NULL && MBLKL(mp) <= skip) {
		skip -= MBLKL(mp);
		mp = mp->b_cont;
	}
	while (len != 0 && mp != NULL) {
		uint_t todo = (uint_t)MIN(MBLKL(mp) - skip, len);

		nvmf_memdesc_copyin(&io->io_mem, io_offset, mp->b_rptr + skip,
		    todo);
		skip = 0;
		io_offset += todo;
		len -= todo;
		mp = mp->b_cont;
	}
}

/* A copy of part of a command buffer's data, to transmit. */
static mblk_t *
nvmf_tcp_command_buffer_mblk(nvmf_tcp_command_buffer_t *cb,
    uint32_t data_offset, uint32_t data_len)
{
	mblk_t *mp;

	mp = nvmf_tcp_allocb(data_len);
	nvmf_memdesc_copyout(&cb->tcb_io.io_mem, data_offset, mp->b_rptr,
	    data_len);
	return (mp);
}

static int
nvmf_tcp_handle_h2c_data(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_h2c_data_hdr_t *h2c;
	nvmf_tcp_command_buffer_t *cb;
	nvmf_tcp_capsule_t *tc;
	uint32_t data_len, data_offset;
	uint16_t fes, ttag;
	uint32_t fei = 0;
	boolean_t copy;

	h2c = (const void *)pdu->rp_hdr;
	if (LE_32(h2c->nth2c_datal) > qp->tq_maxh2cdata) {
		fes = NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED;
		goto fail;
	}

	/* The transfer tag is ours and is not byte-swapped. */
	ttag = h2c->nth2c_ttag;
	if (ttag >= qp->tq_num_ttags) {
		fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		fei = offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_ttag);
		goto fail;
	}

	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	cb = qp->tq_open_ttags[ttag];
	if (cb == NULL) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		fei = offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_ttag);
		goto fail;
	}
	ASSERT3U(cb->tcb_ttag, ==, ttag);
	tc = cb->tcb_tc;

	/* For a data digest mismatch, fail the I/O request. */
	if (pdu->rp_data_digest_mismatch) {
		tcp_rx_busy_enter(qp, tc);
		nvmf_tcp_send_next_r2t(qp, cb);	/* drops tcbl_lock */
		cb->tcb_error = EIO;
		tcp_release_command_buffer(cb);
		tcp_rx_busy_exit(qp, tc);
		nvmf_tcp_free_pdu(pdu);
		return (0);
	}

	data_len = LE_32(h2c->nth2c_datal);
	if (data_len != pdu->rp_data_len) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		fei = offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_datal);
		goto fail;
	}

	data_offset = LE_32(h2c->nth2c_datao);
	if (!nvmf_tcp_range_ok(data_offset, data_len, cb->tcb_data_offset,
	    cb->tcb_data_len)) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		fes = NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto fail;
	}

	if (data_offset != cb->tcb_data_offset + cb->tcb_data_xfered ||
	    (cb->tcb_data_xfered + data_len == cb->tcb_data_len) !=
	    ((pdu->rp_hdr->ntcph_flags & NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU) !=
	    0)) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		fes = NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
		goto fail;
	}

	cb->tcb_data_xfered += data_len;
	data_offset -= cb->tcb_data_offset;
	/* A cancelled transfer keeps its tag and discards the data. */
	copy = !cb->tcb_done;
	tcp_rx_busy_enter(qp, tc);
	if (cb->tcb_data_xfered == cb->tcb_data_len) {
		nvmf_tcp_send_next_r2t(qp, cb);	/* drops tcbl_lock */
	} else {
		tcp_hold_command_buffer(cb);
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
	}

	if (copy) {
		nvmf_tcp_mblk_copyto_io(pdu->rp_mp, pdu->rp_hdr->ntcph_pdo,
		    data_len, &cb->tcb_io, data_offset);
	}

	tcp_release_command_buffer(cb);
	tcp_rx_busy_exit(qp, tc);
	nvmf_tcp_free_pdu(pdu);
	return (0);

fail:
	nvmf_tcp_report_error(qp, fes, fei, pdu->rp_mp,
	    pdu->rp_hdr->ntcph_hlen);
	nvmf_tcp_free_pdu(pdu);
	return (EBADMSG);
}

static int
nvmf_tcp_handle_c2h_data(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_c2h_data_hdr_t *c2h;
	nvmf_tcp_command_buffer_t *cb;
	uint32_t data_len, data_offset;

	c2h = (const void *)pdu->rp_hdr;

	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	cb = tcp_find_command_buffer(&qp->tq_rx_buffers, c2h->ntc2c_cccid, 0);
	if (cb == NULL) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_c2h_data_hdr_t, ntc2c_cccid), pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	/* For a data digest mismatch, fail the I/O request. */
	if (pdu->rp_data_digest_mismatch) {
		cb->tcb_error = EIO;
		tcp_remove_command_buffer(&qp->tq_rx_buffers, cb);
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		tcp_release_command_buffer(cb);
		nvmf_tcp_free_pdu(pdu);
		return (0);
	}

	data_len = LE_32(c2h->ntc2c_datal);
	if (data_len != pdu->rp_data_len) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_c2h_data_hdr_t, ntc2c_datal), pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_offset = LE_32(c2h->ntc2c_datao);
	if (!nvmf_tcp_range_ok(data_offset, data_len, cb->tcb_data_offset,
	    cb->tcb_data_len)) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE, 0,
		    pdu->rp_mp, pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	if (data_offset != cb->tcb_data_offset + cb->tcb_data_xfered) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	if ((cb->tcb_data_xfered + data_len == cb->tcb_data_len) !=
	    ((pdu->rp_hdr->ntcph_flags & NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU) !=
	    0)) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	cb->tcb_data_xfered += data_len;
	data_offset -= cb->tcb_data_offset;
	if (cb->tcb_data_xfered == cb->tcb_data_len)
		tcp_remove_command_buffer(&qp->tq_rx_buffers, cb);
	else
		tcp_hold_command_buffer(cb);
	mutex_exit(&qp->tq_rx_buffers.tcbl_lock);

	nvmf_tcp_mblk_copyto_io(pdu->rp_mp, pdu->rp_hdr->ntcph_pdo, data_len,
	    &cb->tcb_io, data_offset);

	tcp_release_command_buffer(cb);

	if ((pdu->rp_hdr->ntcph_flags & NVMF_TCP_C2H_DATA_FLAGS_SUCCESS) != 0) {
		nvme_cqe_t cqe;
		struct nvmf_capsule *nc;

		bzero(&cqe, sizeof (cqe));
		cqe.cqe_cid = c2h->ntc2c_cccid;

		nc = nvmf_allocate_response(&qp->qp, &cqe, KM_SLEEP);
		nc->nc_sqhd_valid = B_FALSE;

		nvmf_capsule_received(&qp->qp, nc);
	}

	nvmf_tcp_free_pdu(pdu);
	return (0);
}

/* NB: cid and ttag are little-endian already. */
static void
tcp_send_h2c_pdu(nvmf_tcp_qpair_t *qp, uint16_t cid, uint16_t ttag,
    uint32_t data_offset, mblk_t *mp, size_t len, boolean_t last_pdu)
{
	nvmf_tcp_h2c_data_hdr_t h2c;
	mblk_t *top;

	bzero(&h2c, sizeof (h2c));
	h2c.nth2c_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_H2C_DATA;
	if (last_pdu) {
		h2c.nth2c_common.ntcph_flags |=
		    NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU;
	}
	h2c.nth2c_cccid = cid;
	h2c.nth2c_ttag = ttag;
	h2c.nth2c_datao = LE_32(data_offset);
	h2c.nth2c_datal = LE_32((uint32_t)len);

	top = nvmf_tcp_construct_pdu(qp, &h2c, sizeof (h2c), mp, (uint32_t)len);
	nvmf_tcp_write_pdu(qp, top);
}

static int
nvmf_tcp_handle_r2t(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_r2t_hdr_t *r2t;
	nvmf_tcp_command_buffer_t *cb;
	uint32_t data_len, data_offset;

	r2t = (const void *)pdu->rp_hdr;

	mutex_enter(&qp->tq_tx_buffers.tcbl_lock);
	cb = tcp_find_command_buffer(&qp->tq_tx_buffers, r2t->ntr2t_cccid, 0);
	if (cb == NULL) {
		mutex_exit(&qp->tq_tx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_r2t_hdr_t, ntr2t_cccid), pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_offset = LE_32(r2t->ntr2t_r2to);
	if (data_offset != cb->tcb_data_xfered) {
		mutex_exit(&qp->tq_tx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR, 0, pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_len = LE_32(r2t->ntr2t_r2tl);
	if (!nvmf_tcp_range_ok(data_offset, data_len, 0, cb->tcb_data_len)) {
		mutex_exit(&qp->tq_tx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE, 0,
		    pdu->rp_mp, pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	cb->tcb_data_xfered += data_len;
	if (cb->tcb_data_xfered == cb->tcb_data_len)
		tcp_remove_command_buffer(&qp->tq_tx_buffers, cb);
	else
		tcp_hold_command_buffer(cb);
	mutex_exit(&qp->tq_tx_buffers.tcbl_lock);

	/* Queue one or more H2C_DATA PDUs containing the requested data. */
	while (data_len > 0) {
		mblk_t *mp;
		uint32_t todo;

		todo = (uint32_t)MIN(data_len, qp->tq_max_tx_data);
		mp = nvmf_tcp_command_buffer_mblk(cb, data_offset, todo);
		tcp_send_h2c_pdu(qp, r2t->ntr2t_cccid, r2t->ntr2t_ttag,
		    data_offset, mp, todo, todo == data_len);

		data_offset += todo;
		data_len -= todo;
	}

	tcp_release_command_buffer(cb);
	nvmf_tcp_free_pdu(pdu);
	return (0);
}

static int
nvmf_tcp_dispatch_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	switch (pdu->rp_hdr->ntcph_pdu_type) {
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
		return (nvmf_tcp_handle_term_req(pdu));
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
		return (nvmf_tcp_save_command_capsule(qp, pdu));
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
		return (nvmf_tcp_save_response_capsule(qp, pdu));
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
		return (nvmf_tcp_handle_h2c_data(qp, pdu));
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		return (nvmf_tcp_handle_c2h_data(qp, pdu));
	case NVMF_TCP_PDU_TYPE_R2T:
		return (nvmf_tcp_handle_r2t(qp, pdu));
	default:
		nvmf_tcp_free_pdu(pdu);
		return (ECONNRESET);
	}
}

/*
 * Split the first n bytes off the chain *mpp and return them; the rest stays
 * in *mpp.  Returns NULL, with *mpp unchanged, if memory is short.  The chain
 * has at least n bytes.
 */
static mblk_t *
nvmf_tcp_chain_split(mblk_t **mpp, size_t n)
{
	mblk_t *head = *mpp;
	mblk_t *mp = head;
	mblk_t *prev = NULL;
	mblk_t *tail;
	size_t off = n;

	while (mp != NULL && off >= MBLKL(mp)) {
		off -= MBLKL(mp);
		prev = mp;
		mp = mp->b_cont;
	}

	if (off == 0) {
		*mpp = mp;
		if (prev != NULL)
			prev->b_cont = NULL;
		return (head);
	}

	tail = dupb(mp);
	if (tail == NULL)
		return (NULL);

	tail->b_rptr = mp->b_rptr + off;
	tail->b_cont = mp->b_cont;
	mp->b_wptr = mp->b_rptr + off;
	mp->b_cont = NULL;

	*mpp = tail;
	return (head);
}

/*
 * Read exactly len bytes.  *donep is short only at the end of the stream.
 * A receive timeout that the daemon left on the socket is not an error.
 */
static int
nvmf_tcp_recv(nvmf_tcp_qpair_t *qp, void *buf, size_t len, size_t *donep)
{
	size_t done = 0, n;
	boolean_t shutdown;
	int error = 0;

	while (done < len) {
		n = 0;
		error = ksocket_recv(qp->tq_so, (uint8_t *)buf + done,
		    len - done, MSG_WAITALL, &n, CRED());
		if (error == EAGAIN) {
			mutex_enter(&qp->tq_rx_lock);
			if (!qp->tq_rx_shutdown) {
				(void) cv_reltimedwait(&qp->tq_rx_cv,
				    &qp->tq_rx_lock, drv_usectohz(100000),
				    TR_CLOCK_TICK);
			}
			shutdown = qp->tq_rx_shutdown;
			mutex_exit(&qp->tq_rx_lock);
			if (shutdown)
				break;
			error = 0;
			continue;
		}
		if (error != 0 || n == 0)
			break;
		done += n;
	}
	*donep = done;
	return (error);
}

/*
 * Read the next PDU into one mblk.  Returns 0 with rp_mp NULL if the peer
 * closed the connection between PDUs.  *termp is set if a termination request
 * went out.
 */
static int
nvmf_tcp_recv_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu,
    boolean_t *termp)
{
	nvmf_tcp_common_pdu_hdr_t ch;
	mblk_t *mp;
	size_t done;
	uint32_t plen;
	int error;

	pdu->rp_mp = NULL;
	pdu->rp_hdr = NULL;

	error = nvmf_tcp_recv(qp, &ch, sizeof (ch), &done);
	if (error != 0 || done == 0)
		return (error);
	if (done != sizeof (ch))
		return (ECONNRESET);

	/* nvmf_tcp_validate_pdu() reports lengths that make no sense. */
	plen = LE_32(ch.ntcph_plen);
	if (plen < sizeof (ch) || ch.ntcph_hlen > plen)
		plen = sizeof (ch);

	if (plen > qp->tq_max_rx_pdu) {
		switch (ch.ntcph_pdu_type) {
		case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
		case NVMF_TCP_PDU_TYPE_H2C_DATA:
		case NVMF_TCP_PDU_TYPE_C2H_DATA:
			nvmf_tcp_send_term_req(qp,
			    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED,
			    0, &ch, sizeof (ch));
			break;
		default:
			nvmf_tcp_send_term_req(qp,
			    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
			    offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_plen),
			    &ch, sizeof (ch));
			break;
		}
		*termp = B_TRUE;
		return (EBADMSG);
	}

	mp = nvmf_tcp_allocb(plen);
	bcopy(&ch, mp->b_rptr, sizeof (ch));
	error = nvmf_tcp_recv(qp, mp->b_rptr + sizeof (ch),
	    plen - sizeof (ch), &done);
	if (error == 0 && done != plen - sizeof (ch))
		error = ECONNRESET;
	if (error != 0) {
		freemsg(mp);
		return (error);
	}

	pdu->rp_mp = mp;
	pdu->rp_hdr = (const nvmf_tcp_common_pdu_hdr_t *)mp->b_rptr;
	return (0);
}

/*
 * After a termination request, give the peer time to close the connection
 * and discard what it still sends.
 */
static void
nvmf_tcp_wait_close(nvmf_tcp_qpair_t *qp)
{
	clock_t deadline = ddi_get_lbolt() +
	    SEC_TO_TICK(nvmf_tcp_term_wait_secs);
	size_t len = 4096, n;
	uint8_t *buf;
	int error;

	buf = kmem_alloc(len, KM_SLEEP);
	mutex_enter(&qp->tq_rx_lock);
	while (!qp->tq_rx_shutdown && ddi_get_lbolt() < deadline) {
		mutex_exit(&qp->tq_rx_lock);
		n = 0;
		error = ksocket_recv(qp->tq_so, buf, len, MSG_DONTWAIT, &n,
		    CRED());
		mutex_enter(&qp->tq_rx_lock);
		if (error == 0 && n != 0)
			continue;
		if (error != EWOULDBLOCK)
			break;
		(void) cv_reltimedwait(&qp->tq_rx_cv, &qp->tq_rx_lock,
		    drv_usectohz(100000), TR_CLOCK_TICK);
	}
	mutex_exit(&qp->tq_rx_lock);
	kmem_free(buf, len);
}

/*
 * The rx worker.  It reads and dispatches PDUs until the connection ends or
 * fails, reports that once, and then waits for tcp_free_qpair().
 */
static void
nvmf_tcp_so_rx(void *arg)
{
	nvmf_tcp_qpair_t *qp = arg;
	nvmf_tcp_rxpdu_t pdu;
	boolean_t term = B_FALSE, shutdown;
	int error;

	for (;;) {
		mutex_enter(&qp->tq_rx_lock);
		shutdown = qp->tq_rx_shutdown;
		mutex_exit(&qp->tq_rx_lock);
		if (shutdown)
			return;

		error = nvmf_tcp_recv_pdu(qp, &pdu, &term);
		if (error != 0 || pdu.rp_mp == NULL)
			break;

		error = nvmf_tcp_validate_pdu(qp, &pdu);
		if (error != 0)
			nvmf_tcp_free_pdu(&pdu);
		else
			error = nvmf_tcp_dispatch_pdu(qp, &pdu);
		if (error != 0) {
			/* Every error but ECONNRESET sent a request. */
			term = (error != ECONNRESET);
			break;
		}
	}

	if (term)
		nvmf_tcp_wait_close(qp);

	mutex_enter(&qp->tq_rx_lock);
	if (!qp->tq_rx_shutdown) {
		mutex_exit(&qp->tq_rx_lock);
		nvmf_qpair_error(&qp->qp, error);
		mutex_enter(&qp->tq_rx_lock);
	}
	while (!qp->tq_rx_shutdown)
		cv_wait(&qp->tq_rx_cv, &qp->tq_rx_lock);
	mutex_exit(&qp->tq_rx_lock);
}

/*
 * The tx worker.  It writes queued PDUs, then queued capsules, to the socket
 * and may block there.  It exits on a send error, at shutdown, or once the
 * queues are empty while draining.
 */
static void
nvmf_tcp_so_tx(void *arg)
{
	nvmf_tcp_qpair_t *qp = arg;
	nvmf_tcp_capsule_t *tc;
	mblk_t *mp;
	struct msghdr msg;
	boolean_t failed;
	int error = 0;

	mutex_enter(&qp->tq_tx_lock);
	while (error == 0 && !qp->tq_tx_shutdown) {
		mp = qp->tq_tx_pdus_head;
		if (mp != NULL) {
			qp->tq_tx_pdus_head = mp->b_next;
			if (qp->tq_tx_pdus_head == NULL)
				qp->tq_tx_pdus_tail = NULL;
			mp->b_next = NULL;
			mutex_exit(&qp->tq_tx_lock);
		} else {
			tc = list_remove_head(&qp->tq_tx_capsules);
			if (tc == NULL) {
				if (qp->tq_tx_draining)
					break;
				cv_wait(&qp->tq_tx_cv, &qp->tq_tx_lock);
				continue;
			}
			mutex_exit(&qp->tq_tx_lock);
			mp = capsule_to_pdu(qp, tc);
			tcp_release_capsule(tc);
		}

		/* A send can take part of the chain and leave the rest. */
		while (mp != NULL && error == 0) {
			bzero(&msg, sizeof (msg));
			error = ksocket_sendmblk(qp->tq_so, &msg, 0, &mp,
			    CRED());
		}
		freemsg(mp);
		mutex_enter(&qp->tq_tx_lock);
	}
	failed = (error != 0 && !qp->tq_tx_shutdown);
	mutex_exit(&qp->tq_tx_lock);

	if (failed)
		nvmf_qpair_error(&qp->qp, error);

	mutex_enter(&qp->tq_tx_lock);
	qp->tq_tx_exited = B_TRUE;
	cv_broadcast(&qp->tq_tx_done_cv);
	mutex_exit(&qp->tq_tx_lock);
}

static mblk_t *
tcp_command_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	struct nvmf_capsule *nc = &tc->nc;
	nvmf_tcp_command_buffer_t *cb;
	nvme_sqe_t sqe;
	nvme_sgl_t *sgl;
	nvmf_tcp_cmd_t cmd;
	mblk_t *data = NULL;

	if (nc->nc_data.io_len != 0) {
		cb = tcp_alloc_command_buffer(qp, &nc->nc_data, 0,
		    nc->nc_data.io_len, nc->nc_sqe.sqe_cid);

		if (nc->nc_send_data && nc->nc_data.io_len <= qp->tq_max_icd) {
			data = nvmf_tcp_command_buffer_mblk(cb, 0,
			    (uint32_t)nc->nc_data.io_len);
			cb->tcb_data_xfered = nc->nc_data.io_len;
			tcp_release_command_buffer(cb);
		} else if (nc->nc_send_data) {
			mutex_enter(&qp->tq_tx_buffers.tcbl_lock);
			tcp_add_command_buffer(&qp->tq_tx_buffers, cb);
			mutex_exit(&qp->tq_tx_buffers.tcbl_lock);
		} else {
			mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
			tcp_add_command_buffer(&qp->tq_rx_buffers, cb);
			mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		}
	}

	/* ntc_ccsqe is unaligned in the PDU, so build the SQE apart. */
	sqe = nc->nc_sqe;
	sgl = &sqe.sqe_dptr.d_sgl;
	bzero(sgl, sizeof (*sgl));
	sgl->sgl_len = LE_32((uint32_t)nc->nc_data.io_len);
	if (data != NULL) {
		sgl->sgl_type = NVME_SGL_TYPE_ICD >> 4;
		sgl->sgl_zero = NVME_SGL_TYPE_ICD & 0xf;
	} else {
		sgl->sgl_type = NVME_SGL_TYPE_COMMAND_BUFFER >> 4;
		sgl->sgl_zero = NVME_SGL_TYPE_COMMAND_BUFFER & 0xf;
	}

	bzero(&cmd, sizeof (cmd));
	cmd.ntc_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_CMD;
	bcopy(&sqe, cmd.ntc_ccsqe, sizeof (sqe));

	return (nvmf_tcp_construct_pdu(qp, &cmd, sizeof (cmd), data,
	    data != NULL ? (uint32_t)nc->nc_data.io_len : 0));
}

static mblk_t *
tcp_response_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	struct nvmf_capsule *nc = &tc->nc;
	nvmf_tcp_rsp_t rsp;

	bzero(&rsp, sizeof (rsp));
	rsp.ntr_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_RESP;
	bcopy(&nc->nc_cqe, rsp.ntr_rccqe, sizeof (nc->nc_cqe));

	return (nvmf_tcp_construct_pdu(qp, &rsp, sizeof (rsp), NULL, 0));
}

static mblk_t *
capsule_to_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	if (tc->nc.nc_qe_len == sizeof (nvme_sqe_t))
		return (tcp_command_pdu(qp, tc));
	else
		return (tcp_response_pdu(qp, tc));
}

/*
 * Take a hold on the open file behind the daemon's connected TCP socket and
 * mark the socket as ours.  The hold keeps the socket whether or not the
 * daemon closes its descriptor, and closef() gives it back without waiting.
 */
static int
tcp_adopt_socket(uint64_t fd, file_t **fpp, ksocket_t *ksp)
{
	struct sonode *so;
	file_t *fp;
	int error = 0;

	if (fd > INT_MAX)
		return (EBADF);

	so = getsonode((int)fd, &error, &fp);
	if (so == NULL)
		return (error != 0 ? error : EBADF);

	mutex_enter(&so->so_lock);
	if (fp->f_vnode != SOTOV(so) || fp->f_vnode->v_stream != NULL) {
		error = EOPNOTSUPP;
	} else if (so->so_type != SOCK_STREAM ||
	    (so->so_family != AF_INET && so->so_family != AF_INET6) ||
	    (so->so_protocol != 0 && so->so_protocol != IPPROTO_TCP)) {
		error = EPROTONOSUPPORT;
	} else if ((so->so_state & SS_ISCONNECTED) == 0) {
		error = ENOTCONN;
	} else if ((so->so_state & (SS_NDELAY | SS_NONBLOCK)) != 0) {
		error = EINVAL;
	} else if ((so->so_mode & SM_KERNEL) != 0) {
		error = EBUSY;
	} else {
		/* The ksocket interfaces refuse a socket without this. */
		so->so_mode |= SM_KERNEL;
	}
	mutex_exit(&so->so_lock);

	if (error == 0) {
		struct timeval tv = { 0, 0 };

		mutex_enter(&fp->f_tlock);
		fp->f_count++;
		mutex_exit(&fp->f_tlock);
		*fpp = fp;
		*ksp = (ksocket_t)(uintptr_t)so;

		/* The workers block; drop the daemon's handshake timeouts. */
		(void) ksocket_setsockopt(*ksp, SOL_SOCKET, SO_RCVTIMEO, &tv,
		    sizeof (tv), CRED());
		(void) ksocket_setsockopt(*ksp, SOL_SOCKET, SO_SNDTIMEO, &tv,
		    sizeof (tv), CRED());
	}
	releasef((int)fd);
	return (error);
}

static void
tcp_release_socket(nvmf_tcp_qpair_t *qp)
{
	struct sonode *so = (struct sonode *)(uintptr_t)qp->tq_so;

	mutex_enter(&so->so_lock);
	so->so_mode &= ~SM_KERNEL;
	mutex_exit(&so->so_lock);
	(void) closef(qp->tq_fp);
}

/*
 * Everything that can fail happens before the qpair exists.  The workers
 * start in tcp_start_qpair(), once the core has set the callbacks.
 */
static struct nvmf_qpair *
tcp_allocate_qpair(boolean_t controller, const nvlist_t *params)
{
	nvlist_t *nvl = (nvlist_t *)params;
	nvmf_tcp_qpair_t *qp;
	file_t *fp;
	ksocket_t so;
	uint64_t fd, rxpda, txpda, maxr2t, maxh2cdata, max_icd, qsize;
	boolean_t header_digests, data_digests, sqfc;
	uint32_t max_data;

	if (nvlist_lookup_uint64(nvl, "fd", &fd) != 0 ||
	    nvlist_lookup_uint64(nvl, "rxpda", &rxpda) != 0 ||
	    nvlist_lookup_uint64(nvl, "txpda", &txpda) != 0 ||
	    nvlist_lookup_boolean_value(nvl, "header_digests",
	    &header_digests) != 0 ||
	    nvlist_lookup_boolean_value(nvl, "data_digests",
	    &data_digests) != 0 ||
	    nvlist_lookup_uint64(nvl, "maxr2t", &maxr2t) != 0 ||
	    nvlist_lookup_uint64(nvl, "maxh2cdata", &maxh2cdata) != 0 ||
	    nvlist_lookup_uint64(nvl, "max_icd", &max_icd) != 0 ||
	    nvlist_lookup_uint64(nvl, "qsize", &qsize) != 0 ||
	    nvlist_lookup_boolean_value(nvl, "sq_flow_control", &sqfc) != 0)
		return (NULL);

	if (!nvmf_tcp_params_ok(rxpda, txpda, maxr2t, maxh2cdata, max_icd,
	    qsize))
		return (NULL);

	if (tcp_adopt_socket(fd, &fp, &so) != 0)
		return (NULL);

	qp = kmem_zalloc(sizeof (*qp), KM_SLEEP);
	qp->tq_fp = fp;
	qp->tq_so = so;
	qp->tq_refs = 1;
	qp->tq_txpda = (uint8_t)txpda;
	qp->tq_rxpda = (uint8_t)rxpda;
	qp->tq_header_digests = header_digests;
	qp->tq_data_digests = data_digests;
	qp->tq_maxr2t = (uint32_t)maxr2t;
	qp->tq_max_tx_data = MAX(nvmf_tcp_max_transmit_data,
	    NVMF_TCP_MIN_MAXH2CDATA);

	if (controller) {
		qp->tq_maxh2cdata = (uint32_t)maxh2cdata;
		/* The handoff's max_icd is the host's limit, not ours. */
		qp->tq_max_rx_icd = nvmf_tcp_max_icd;
		max_data = MAX(qp->tq_maxh2cdata, qp->tq_max_rx_icd);

		/* Use the SUCCESS flag if SQ flow control is disabled. */
		qp->tq_send_success = !sqfc;

		/* NB: maxr2t is 0's based. */
		qp->tq_num_ttags = (uint_t)MIN((uint64_t)UINT16_MAX + 1,
		    qsize * (maxr2t + 1));
		qp->tq_open_ttags = kmem_zalloc(qp->tq_num_ttags *
		    sizeof (*qp->tq_open_ttags), KM_SLEEP);
	} else {
		qp->tq_max_tx_data = (uint32_t)MIN(qp->tq_max_tx_data,
		    maxh2cdata);
		qp->tq_max_icd = (uint32_t)max_icd;
		qp->tq_max_io = MAX(nvmf_tcp_max_io_size,
		    NVMF_TCP_MIN_MAXH2CDATA);
		max_data = qp->tq_max_io;
	}
	qp->tq_max_rx_pdu = nvmf_tcp_rx_limit(max_data, qp->tq_rxpda,
	    header_digests, data_digests);

	list_create(&qp->tq_rx_buffers.tcbl_list,
	    sizeof (nvmf_tcp_command_buffer_t),
	    offsetof(nvmf_tcp_command_buffer_t, tcb_link));
	list_create(&qp->tq_tx_buffers.tcbl_list,
	    sizeof (nvmf_tcp_command_buffer_t),
	    offsetof(nvmf_tcp_command_buffer_t, tcb_link));
	mutex_init(&qp->tq_rx_buffers.tcbl_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&qp->tq_tx_buffers.tcbl_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&qp->tq_cancel_cv, NULL, CV_DRIVER, NULL);
	list_create(&qp->tq_tx_capsules, sizeof (nvmf_tcp_capsule_t),
	    offsetof(nvmf_tcp_capsule_t, tc_link));

	mutex_init(&qp->tq_rx_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&qp->tq_tx_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&qp->tq_rx_cv, NULL, CV_DRIVER, NULL);
	cv_init(&qp->tq_tx_cv, NULL, CV_DRIVER, NULL);
	cv_init(&qp->tq_tx_done_cv, NULL, CV_DRIVER, NULL);

	qp->tq_tx_taskq = taskq_create("nvmf_tcp_tx", 1, minclsyspri, 1, 1,
	    TASKQ_PREPOPULATE);
	qp->tq_rx_taskq = taskq_create("nvmf_tcp_rx", 1, minclsyspri, 1, 1,
	    TASKQ_PREPOPULATE);

	return (&qp->qp);
}

static void
tcp_start_qpair(struct nvmf_qpair *nq)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);

	mutex_enter(&qp->tq_tx_lock);
	qp->tq_started = B_TRUE;
	mutex_exit(&qp->tq_tx_lock);

	(void) taskq_dispatch(qp->tq_tx_taskq, nvmf_tcp_so_tx, qp, TQ_SLEEP);
	(void) taskq_dispatch(qp->tq_rx_taskq, nvmf_tcp_so_rx, qp, TQ_SLEEP);
}

/* The last reference can be a capsule's, in any context. */
static void
tcp_release_qpair(nvmf_tcp_qpair_t *qp)
{
	if (!nvmf_tcp_refcount_release(&qp->tq_refs))
		return;

	if (qp->tq_open_ttags != NULL) {
		kmem_free(qp->tq_open_ttags, qp->tq_num_ttags *
		    sizeof (*qp->tq_open_ttags));
	}
	list_destroy(&qp->tq_rx_buffers.tcbl_list);
	list_destroy(&qp->tq_tx_buffers.tcbl_list);
	list_destroy(&qp->tq_tx_capsules);
	mutex_destroy(&qp->tq_rx_buffers.tcbl_lock);
	mutex_destroy(&qp->tq_tx_buffers.tcbl_lock);
	mutex_destroy(&qp->tq_rx_lock);
	mutex_destroy(&qp->tq_tx_lock);
	cv_destroy(&qp->tq_cancel_cv);
	cv_destroy(&qp->tq_rx_cv);
	cv_destroy(&qp->tq_tx_cv);
	cv_destroy(&qp->tq_tx_done_cv);
	kmem_free(qp, sizeof (*qp));
}

/*
 * Complete a controller receive buffer that is off the lists.  Called with
 * tcbl_lock held, which is dropped for the callback.
 */
static void
tcp_abort_rx_buffer(nvmf_tcp_qpair_t *qp, nvmf_tcp_command_buffer_t *cb)
{
	nvmf_tcp_capsule_t *tc = cb->tcb_tc;

	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));
	if (tc != NULL)
		tcp_rx_busy_enter(qp, tc);
	mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
	cb->tcb_error = ECONNABORTED;
	tcp_release_command_buffer(cb);
	if (tc != NULL)
		tcp_rx_busy_exit(qp, tc);
	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
}

/*
 * Only the two workers use the socket.  Each is stopped and joined here
 * before the state it uses goes away, and every transfer still registered
 * completes before this returns.
 */
static void
tcp_free_qpair(struct nvmf_qpair *nq)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	nvmf_tcp_command_buffer_t *cb;
	nvmf_tcp_capsule_t *tc;
	mblk_t *pmp;
	clock_t deadline;

	/* The shutdown ends a receive that the rx worker sleeps in. */
	mutex_enter(&qp->tq_rx_lock);
	qp->tq_rx_shutdown = B_TRUE;
	cv_broadcast(&qp->tq_rx_cv);
	mutex_exit(&qp->tq_rx_lock);
	(void) ksocket_shutdown(qp->tq_so, SHUT_RD, CRED());
	taskq_wait(qp->tq_rx_taskq);

	/*
	 * Give the tx worker a bounded time to pass what is queued to the
	 * socket.  The shutdown then ends a send that it still sleeps in.
	 */
	mutex_enter(&qp->tq_tx_lock);
	if (qp->tq_started) {
		qp->tq_tx_draining = B_TRUE;
		cv_broadcast(&qp->tq_tx_cv);
		deadline = ddi_get_lbolt() + SEC_TO_TICK(nvmf_tcp_drain_secs);
		while (!qp->tq_tx_exited) {
			if (cv_timedwait(&qp->tq_tx_done_cv, &qp->tq_tx_lock,
			    deadline) == -1)
				break;
		}
	}
	qp->tq_tx_shutdown = B_TRUE;
	cv_broadcast(&qp->tq_tx_cv);
	mutex_exit(&qp->tq_tx_lock);
	(void) ksocket_shutdown(qp->tq_so, SHUT_WR, CRED());
	taskq_wait(qp->tq_tx_taskq);

	taskq_destroy(qp->tq_rx_taskq);
	taskq_destroy(qp->tq_tx_taskq);

	mutex_enter(&qp->tq_tx_lock);
	while ((pmp = qp->tq_tx_pdus_head) != NULL) {
		qp->tq_tx_pdus_head = pmp->b_next;
		pmp->b_next = NULL;
		freemsg(pmp);
	}
	qp->tq_tx_pdus_tail = NULL;
	while ((tc = list_remove_head(&qp->tq_tx_capsules)) != NULL) {
		mutex_exit(&qp->tq_tx_lock);
		nvmf_abort_capsule_data(&tc->nc, ECONNABORTED);
		tcp_release_capsule(tc);
		mutex_enter(&qp->tq_tx_lock);
	}
	mutex_exit(&qp->tq_tx_lock);

	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	qp->tq_rx_closed = B_TRUE;
	for (uint_t i = 0; i < qp->tq_num_ttags; i++) {
		cb = qp->tq_open_ttags[i];
		if (cb == NULL)
			continue;
		qp->tq_open_ttags[i] = NULL;
		qp->tq_active_ttags--;
		cb->tcb_tc->tc_active_r2ts--;
		tcp_abort_rx_buffer(qp, cb);
	}
	while ((cb = list_remove_head(&qp->tq_rx_buffers.tcbl_list)) != NULL) {
#ifdef DEBUG
		if (cb->tcb_tc != NULL)
			cb->tcb_tc->tc_pending_r2ts--;
#endif
		tcp_abort_rx_buffer(qp, cb);
	}
	mutex_exit(&qp->tq_rx_buffers.tcbl_lock);

	mutex_enter(&qp->tq_tx_buffers.tcbl_lock);
	while ((cb = list_remove_head(&qp->tq_tx_buffers.tcbl_list)) != NULL) {
		mutex_exit(&qp->tq_tx_buffers.tcbl_lock);
		cb->tcb_error = ECONNABORTED;
		tcp_release_command_buffer(cb);
		mutex_enter(&qp->tq_tx_buffers.tcbl_lock);
	}
	mutex_exit(&qp->tq_tx_buffers.tcbl_lock);

	tcp_release_socket(qp);
	tcp_release_qpair(qp);
}

static uint32_t
tcp_max_ioccsz(struct nvmf_qpair *nq)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);

	if (!nq->nq_controller)
		return (0);
	return ((uint32_t)sizeof (nvme_sqe_t) + qp->tq_max_rx_icd);
}

static uint64_t
tcp_max_xfer_size(struct nvmf_qpair *nq)
{
	return (TQP(nq)->tq_max_io);
}

/* ARGSUSED */
static uint32_t
tcp_caps(struct nvmf_qpair *nq)
{
	return (NVMF_QP_CAP_SGL_TRANSPORT | NVMF_QP_CAP_SGL_OFFSET);
}

static struct nvmf_capsule *
tcp_allocate_capsule(struct nvmf_qpair *nq, int how)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	nvmf_tcp_capsule_t *tc;

	tc = kmem_zalloc(sizeof (*tc), how);
	if (tc == NULL)
		return (NULL);
	tc->tc_refs = 1;
	nvmf_tcp_refcount_acquire(&qp->tq_refs);
	return (&tc->nc);
}

static void
tcp_release_capsule(nvmf_tcp_capsule_t *tc)
{
	nvmf_tcp_qpair_t *qp = TQP(tc->nc.nc_qpair);

	if (!nvmf_tcp_refcount_release(&tc->tc_refs))
		return;

	ASSERT3U(tc->tc_active_r2ts, ==, 0);
#ifdef DEBUG
	ASSERT3U(tc->tc_pending_r2ts, ==, 0);
#endif

	nvmf_tcp_free_pdu(&tc->tc_rx_pdu);
	kmem_free(tc, sizeof (*tc));
	tcp_release_qpair(qp);
}

static void
tcp_free_capsule(struct nvmf_capsule *nc)
{
	tcp_release_capsule(TCAP(nc));
}

static int
tcp_transmit_capsule(struct nvmf_capsule *nc)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_capsule_t *tc = TCAP(nc);

	mutex_enter(&qp->tq_tx_lock);
	if (qp->tq_tx_shutdown) {
		mutex_exit(&qp->tq_tx_lock);
		return (ECONNABORTED);
	}
	nvmf_tcp_refcount_acquire(&tc->tc_refs);
	list_insert_tail(&qp->tq_tx_capsules, tc);
	cv_signal(&qp->tq_tx_cv);
	mutex_exit(&qp->tq_tx_lock);
	return (0);
}

static uint8_t
tcp_validate_command_capsule(struct nvmf_capsule *nc)
{
	nvmf_tcp_capsule_t *tc = TCAP(nc);
	nvme_sgl_t *sgl;
	uint8_t sgl_type;

	ASSERT3P(tc->tc_rx_pdu.rp_hdr, !=, NULL);

	if (tc->tc_rx_pdu.rp_data_digest_mismatch)
		return (NVME_CQE_SC_GEN_TRANSIENT);

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	switch (sgl_type) {
	case NVME_SGL_TYPE_ICD:
		if (tc->tc_rx_pdu.rp_data_len != LE_32(sgl->sgl_len)) {
			if (nvmf_tcp_warn_ok())
				cmn_err(CE_WARN, "NVMe/TCP: Command Capsule "
				    "with mismatched ICD length");
			return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
		}
		break;
	case NVME_SGL_TYPE_COMMAND_BUFFER:
		if (tc->tc_rx_pdu.rp_data_len != 0) {
			if (nvmf_tcp_warn_ok())
				cmn_err(CE_WARN,
				    "NVMe/TCP: Command Buffer SGL with ICD");
			return (NVME_CQE_SC_GEN_INV_FLD);
		}
		break;
	default:
		if (nvmf_tcp_warn_ok())
			cmn_err(CE_WARN,
			    "NVMe/TCP: Invalid SGL type in Command Capsule");
		return (NVME_CQE_SC_GEN_INV_SGL_DESC);
	}

	if (sgl->sgl_addr != 0) {
		if (nvmf_tcp_warn_ok())
			cmn_err(CE_WARN,
			    "NVMe/TCP: Invalid SGL offset in Command Capsule");
		return (NVME_CQE_SC_GEN_INV_SGL_OFF);
	}

	return (NVME_CQE_SC_GEN_SUCCESS);
}

static size_t
tcp_capsule_data_len(const struct nvmf_capsule *nc)
{
	ASSERT3U(nc->nc_qe_len, ==, sizeof (nvme_sqe_t));
	return (LE_32(nc->nc_sqe.sqe_dptr.d_sgl.sgl_len));
}

/* Receive command data from the host with an R2T. */
static void
tcp_receive_r2t_data(struct nvmf_capsule *nc, uint32_t data_offset,
    struct nvmf_io_request *io)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_capsule_t *tc = TCAP(nc);
	nvmf_tcp_command_buffer_t *cb;

	if (io->io_len == 0) {
		nvmf_complete_io_request(io, 0, 0);
		return;
	}

	cb = tcp_alloc_command_buffer(qp, io, data_offset, io->io_len,
	    nc->nc_sqe.sqe_cid);

	cb->tcb_tc = tc;
	nvmf_tcp_refcount_acquire(&tc->tc_refs);

	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	if (qp->tq_rx_closed) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		cb->tcb_error = ECONNABORTED;
		tcp_release_command_buffer(cb);
		return;
	}

	/*
	 * If this command has too many active R2Ts or there are no available
	 * transfer tags, queue the request for later.  NB: maxr2t is 0's based.
	 */
	if (tc->tc_active_r2ts > qp->tq_maxr2t ||
	    qp->tq_active_ttags == qp->tq_num_ttags) {
#ifdef DEBUG
		tc->tc_pending_r2ts++;
#endif
		list_insert_tail(&qp->tq_rx_buffers.tcbl_list, cb);
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		return;
	}

	nvmf_tcp_allocate_ttag(qp, cb);
	mutex_exit(&qp->tq_rx_buffers.tcbl_lock);

	tcp_send_r2t(qp, nc->nc_sqe.sqe_cid, cb->tcb_ttag, data_offset,
	    (uint32_t)io->io_len);
}

static void
tcp_receive_icd_data(struct nvmf_capsule *nc, uint32_t data_offset,
    struct nvmf_io_request *io)
{
	nvmf_tcp_capsule_t *tc = TCAP(nc);

	nvmf_tcp_mblk_copyto_io(tc->tc_rx_pdu.rp_mp,
	    tc->tc_rx_pdu.rp_hdr->ntcph_pdo + data_offset, (uint_t)io->io_len,
	    io, 0);
	nvmf_complete_io_request(io, io->io_len, 0);
}

static int
tcp_receive_controller_data(struct nvmf_capsule *nc, uint32_t data_offset,
    struct nvmf_io_request *io)
{
	nvmf_tcp_capsule_t *tc = TCAP(nc);
	nvme_sgl_t *sgl;
	size_t data_len;
	uint8_t sgl_type;

	if (nc->nc_qe_len != sizeof (nvme_sqe_t) ||
	    !nc->nc_qpair->nq_controller)
		return (EINVAL);

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	data_len = LE_32(sgl->sgl_len);
	if (data_offset > data_len || io->io_len > data_len - data_offset)
		return (EFBIG);

	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	if (sgl_type == NVME_SGL_TYPE_ICD) {
		/* The SGL length is the caller's to check against the PDU. */
		if (data_len > tc->tc_rx_pdu.rp_data_len)
			return (EFBIG);
		tcp_receive_icd_data(nc, data_offset, io);
	} else {
		tcp_receive_r2t_data(nc, data_offset, io);
	}
	return (0);
}

/* An uncompleted receive buffer of tc, from either place one can be. */
static nvmf_tcp_command_buffer_t *
tcp_find_rx_buffer(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc,
    boolean_t *queuedp)
{
	nvmf_tcp_command_buffer_t *cb;

	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));

	for (cb = list_head(&qp->tq_rx_buffers.tcbl_list); cb != NULL;
	    cb = list_next(&qp->tq_rx_buffers.tcbl_list, cb)) {
		if (cb->tcb_tc == tc && !cb->tcb_done) {
			*queuedp = B_TRUE;
			return (cb);
		}
	}

	*queuedp = B_FALSE;
	if (tc->tc_active_r2ts == 0)
		return (NULL);
	for (uint_t i = 0; i < qp->tq_num_ttags; i++) {
		cb = qp->tq_open_ttags[i];
		if (cb != NULL && cb->tcb_tc == tc && !cb->tcb_done)
			return (cb);
	}
	return (NULL);
}

/*
 * See nvmf_cancel_controller_data().  A buffer whose R2T went out stays under
 * its transfer tag, marked done, so that the data the host still sends for it
 * is taken and dropped.
 */
static void
tcp_cancel_controller_data(struct nvmf_capsule *nc)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_capsule_t *tc = TCAP(nc);
	nvmf_tcp_command_buffer_t *cb;
	struct nvmf_io_request io;
	size_t xfered;
	boolean_t queued;

	if (nc->nc_qe_len != sizeof (nvme_sqe_t) ||
	    !nc->nc_qpair->nq_controller)
		return;

	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	for (;;) {
		/* Nobody copies into or completes a buffer of tc now. */
		while (tc->tc_rx_busy != 0) {
			cv_wait(&qp->tq_cancel_cv,
			    &qp->tq_rx_buffers.tcbl_lock);
		}

		cb = tcp_find_rx_buffer(qp, tc, &queued);
		if (cb == NULL)
			break;

		cb->tcb_done = B_TRUE;
		io = cb->tcb_io;
		xfered = cb->tcb_data_xfered;
		if (queued) {
			list_remove(&qp->tq_rx_buffers.tcbl_list, cb);
#ifdef DEBUG
			tc->tc_pending_r2ts--;
#endif
		}
		tc->tc_rx_busy++;
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);

		nvmf_complete_io_request(&io, xfered, ECANCELED);
		if (queued)
			tcp_release_command_buffer(cb);

		mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
		tc->tc_rx_busy--;
		cv_broadcast(&qp->tq_cancel_cv);
	}
	mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
}

/* NB: cid is little-endian already. */
static void
tcp_send_c2h_pdu(nvmf_tcp_qpair_t *qp, uint16_t cid, uint32_t data_offset,
    mblk_t *mp, size_t len, boolean_t last_pdu, boolean_t success)
{
	nvmf_tcp_c2h_data_hdr_t c2h;
	mblk_t *top;

	bzero(&c2h, sizeof (c2h));
	c2h.ntc2c_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_C2H_DATA;
	if (last_pdu) {
		c2h.ntc2c_common.ntcph_flags |=
		    NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU;
	}
	if (success)
		c2h.ntc2c_common.ntcph_flags |= NVMF_TCP_C2H_DATA_FLAGS_SUCCESS;
	c2h.ntc2c_cccid = cid;
	c2h.ntc2c_datao = LE_32(data_offset);
	c2h.ntc2c_datal = LE_32((uint32_t)len);

	top = nvmf_tcp_construct_pdu(qp, &c2h, sizeof (c2h), mp,
	    (uint32_t)len);
	nvmf_tcp_write_pdu(qp, top);
}

/* Send command data to the host as C2H_DATA PDUs.  mp is always consumed. */
static uint_t
tcp_send_controller_data(struct nvmf_capsule *nc, uint32_t data_offset,
    mblk_t *mp, size_t len)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvme_sgl_t *sgl;
	uint32_t data_len;
	uint8_t sgl_type;
	boolean_t last_xfer;

	if (nc->nc_qe_len != sizeof (nvme_sqe_t) || !qp->qp.nq_controller) {
		freemsg(mp);
		return (NVME_CQE_SC_GEN_INV_FLD);
	}

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	data_len = LE_32(sgl->sgl_len);
	if (data_offset > data_len || len > data_len - data_offset) {
		freemsg(mp);
		return (NVME_CQE_SC_GEN_INV_FLD);
	}
	last_xfer = (len == data_len - data_offset);

	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	if (sgl_type != NVME_SGL_TYPE_COMMAND_BUFFER) {
		freemsg(mp);
		return (NVME_CQE_SC_GEN_INV_FLD);
	}

	if (len == 0) {
		freemsg(mp);
		return (data_len == 0 ? NVME_CQE_SC_GEN_SUCCESS :
		    NVME_CQE_SC_GEN_INV_FLD);
	}

#ifdef DEBUG
	ASSERT3U(data_offset, ==, TCAP(nc)->tc_tx_data_offset);
#endif

	while (len > 0) {
		mblk_t *chunk;
		boolean_t last_pdu;
		size_t todo = MIN(len, qp->tq_max_tx_data);

		if (todo < len) {
			chunk = nvmf_tcp_chain_split(&mp, todo);
			if (chunk == NULL) {
				freemsg(mp);
				return (NVME_CQE_SC_GEN_INTERNAL_ERR);
			}
		} else {
			chunk = mp;
			mp = NULL;
		}

		last_pdu = (mp == NULL && last_xfer);
		tcp_send_c2h_pdu(qp, nc->nc_sqe.sqe_cid, data_offset, chunk,
		    todo, last_pdu, last_pdu && qp->tq_send_success);

		data_offset += (uint32_t)todo;
		len -= todo;
	}

#ifdef DEBUG
	TCAP(nc)->tc_tx_data_offset = data_offset;
#endif
	if (!last_xfer)
		return (NVMF_MORE);
	else if (qp->tq_send_success)
		return (NVMF_SUCCESS_SENT);
	else
		return (NVME_CQE_SC_GEN_SUCCESS);
}

static struct nvmf_transport_ops tcp_ops = {
	.allocate_qpair = tcp_allocate_qpair,
	.free_qpair = tcp_free_qpair,
	.start_qpair = tcp_start_qpair,
	.max_ioccsz = tcp_max_ioccsz,
	.max_xfer_size = tcp_max_xfer_size,
	.allocate_capsule = tcp_allocate_capsule,
	.free_capsule = tcp_free_capsule,
	.transmit_capsule = tcp_transmit_capsule,
	.validate_command_capsule = tcp_validate_command_capsule,
	.capsule_data_len = tcp_capsule_data_len,
	.receive_controller_data = tcp_receive_controller_data,
	.cancel_controller_data = tcp_cancel_controller_data,
	.send_controller_data = tcp_send_controller_data,
	.caps = tcp_caps,
	.trtype = NVMF_TRTYPE_TCP,
	.priority = 0,
};

static struct modlmisc nvmf_tcp_modlmisc = {
	&mod_miscops,
	"NVMe over Fabrics TCP transport"
};

static struct modlinkage nvmf_tcp_modlinkage = {
	MODREV_1,
	{ &nvmf_tcp_modlmisc, NULL }
};

int
_init(void)
{
	int error;

	CRC32_INIT(nvmf_tcp_crc32c_table, NVMF_TCP_CRC32C_POLY);

	/* Register last, so that there is nothing to undo with qpairs live. */
	error = mod_install(&nvmf_tcp_modlinkage);
	if (error != 0)
		return (error);

	error = nvmf_transport_register(&tcp_ops);
	if (error != 0)
		(void) mod_remove(&nvmf_tcp_modlinkage);

	return (error);
}

int
_fini(void)
{
	int error;

	/* EBUSY while a qpair or capsule of this transport exists. */
	error = nvmf_transport_unregister(&tcp_ops);
	if (error != 0)
		return (error);

	error = mod_remove(&nvmf_tcp_modlinkage);
	if (error != 0)
		(void) nvmf_transport_register(&tcp_ops);

	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&nvmf_tcp_modlinkage, modinfop));
}

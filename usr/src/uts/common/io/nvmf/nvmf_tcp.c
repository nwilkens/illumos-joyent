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
 * NVMe over Fabrics TCP transport.  This is the provider half of the transport
 * contract defined in nvmf_transport_internal.h: it implements the
 * nvmf_transport_ops vtable (queue-pair lifecycle, capsule alloc/transmit, and
 * controller-side data movement) and the NVMe/TCP PDU framing on top of a
 * kernel TCP socket.
 *
 * The control plane (vtable, registration, PDU header format/parse, capsule
 * construction), the data plane (the event-driven RX PDU-framing state machine,
 * the mblk-based TX worker, and the H2C/C2H/R2T data-transfer paths), and the
 * HDGST/DDGST CRC-32C digests are all implemented and match the FreeBSD
 * structure one-for-one.  The userland socket file descriptor passed in the
 * handoff nvlist is turned into a ksocket_t in tcp_allocate_qpair() via
 * tcp_adopt_socket_fd() (getsonode() in the ioctl caller's process context,
 * SM_KERNEL + ksocket_hold(), then releasef()), after which the krecv/tx wiring
 * binds to the live connection.
 *
 * OS-glue substitutions versus the FreeBSD source:
 *
 *   FreeBSD                       illumos
 *   -------                       -------
 *   struct mbuf                   mblk_t (sys/stream.h)
 *   m_get2/m_get/m_freem          allocb/freemsg/freeb (sys/strsubr.h)
 *   m_copydata/m_apply            bcopy over b_rptr / mblk walk
 *   struct socket *so             ksocket_t (sys/ksocket.h)
 *   soupcall_set(SO_RCV)          ksocket_krecv_set() event-driven RX
 *   sosend()/soreceive()          ksocket_sendmblk()/krecv callback
 *   rx kthread                    krecv callback (accumulate) + rx taskq worker
 *   tx kthread                    persistent taskq worker (nvmf_tcp_so_tx)
 *   cv_wait/cv_signal             kcondvar_t cv_wait/cv_signal
 *   struct mtx                    kmutex_t
 *   refcount(9)                   atomic_inc/dec_uint_nv with a count guard
 *   malloc(M_NVMF_TCP)            kmem_alloc/kmem_zalloc
 *   calculate_crc32c              CRC32() over a CRC-32C (Castagnoli) table
 *   TAILQ/STAILQ                  list_t (sys/list.h)
 *   NVMF_TRANSPORT() macro        nvmf_transport_register() from _init()
 *
 * The mbuf->mblk_t substitution in the data-movement paths is the RDMA seam:
 * the shape is preserved so a future RDMA transport can reuse the contract.
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
#include <sys/file.h>		/* getf / releasef */
#include <sys/socketvar.h>	/* getsonode, struct sonode, SM_KERNEL */
#include <sys/cred.h>
#include <sys/crc32.h>
#include <sys/disp.h>
#include <sys/taskq.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>
#include <sys/nvme/nvmf_tcp.h>

#include "nvmf_core.h"
#include "nvmf_transport_internal.h"

/*
 * Maximum size of the data payload in a single transmitted PDU.  FreeBSD
 * exposes this as a sysctl (kern.nvmf.tcp.max_transmit_data); illumos transports
 * conventionally use a tunable global, overridable through /etc/system.
 */
static uint_t nvmf_tcp_max_transmit_data = 256 * 1024;

struct nvmf_tcp_capsule;
struct nvmf_tcp_qpair;

static void	tcp_release_capsule(struct nvmf_tcp_capsule *tc);
static void	tcp_free_qpair(struct nvmf_qpair *nq);
static mblk_t	*capsule_to_pdu(struct nvmf_tcp_qpair *qp,
		    struct nvmf_tcp_capsule *tc);

/*
 * A command buffer tracks an in-flight data transfer associated with a command
 * (either host-side data being sent in response to an R2T, or controller-side
 * data being received via H2C, or data received via C2H).  FreeBSD threads
 * these through TAILQs keyed by (cid, ttag); illumos uses list_t.
 */
typedef struct nvmf_tcp_command_buffer {
	struct nvmf_tcp_qpair		*tcb_qp;

	struct nvmf_io_request		tcb_io;
	size_t				tcb_data_len;
	size_t				tcb_data_xfered;
	uint32_t			tcb_data_offset;

	volatile uint_t			tcb_refs;
	int				tcb_error;

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

	ksocket_t			tq_so;
	boolean_t			tq_krecv_set;	/* krecv cb installed */

	volatile uint_t			tq_refs;	/* held by every capsule */
	uint8_t				tq_txpda;
	uint8_t				tq_rxpda;
	boolean_t			tq_header_digests;
	boolean_t			tq_data_digests;
	uint32_t			tq_maxr2t;
	uint32_t			tq_maxh2cdata;	/* controller only */
	uint32_t			tq_max_tx_data;
	uint32_t			tq_max_icd;	/* host only */
	uint16_t			tq_next_ttag;	/* controller only */
	uint_t				tq_num_ttags;	/* controller only */
	uint_t				tq_active_ttags; /* controller only */
	boolean_t			tq_send_success; /* controller only */

	/*
	 * Receive state.
	 *
	 * FreeBSD runs a dedicated rx kthread blocked on so_rcv.  illumos drives
	 * RX from the ksocket_krecv_set() callback (nvmf_tcp_krecv()), which only
	 * accumulates received bytes in tq_rx_mp and wakes a persistent rx worker
	 * (nvmf_tcp_so_rx) on tq_rx_taskq.  Framing and dispatch run on that
	 * worker, not in the krecv upcall: the upcall may run in a non-sleepable
	 * socket data-arrival context, whereas the worker is a sleepable kernel
	 * thread where KM_SLEEP allocation and blocking upstack delivery are
	 * legal (matching FreeBSD's dedicated rx kthread).
	 */
	kmutex_t			tq_rx_lock;
	kcondvar_t			tq_rx_cv;
	boolean_t			tq_rx_shutdown;
	boolean_t			tq_rx_ready;	/* work pending for worker */
	mblk_t				*tq_rx_mp;	/* accumulated partial PDU */
	boolean_t			tq_rx_have_header;
	uint32_t			tq_rx_needed;
	taskq_t				*tq_rx_taskq;

	/*
	 * Transmit state.
	 *
	 * FreeBSD runs a dedicated tx kthread that drains tx_pdus/tx_capsules.
	 * illumos uses a persistent taskq worker (nvmf_tcp_so_tx) woken by
	 * tq_tx_cv.
	 */
	kmutex_t			tq_tx_lock;
	kcondvar_t			tq_tx_cv;
	boolean_t			tq_tx_shutdown;
	mblk_t				*tq_tx_pdus_head; /* mblk b_next queue */
	mblk_t				*tq_tx_pdus_tail;
	list_t				tq_tx_capsules;
	taskq_t				*tq_tx_taskq;

	nvmf_tcp_command_buffer_list_t	tq_tx_buffers;
	nvmf_tcp_command_buffer_list_t	tq_rx_buffers;

	/*
	 * For the controller, an RX command buffer is either queued on
	 * tq_rx_buffers (waiting for an R2T slot/transfer tag) or, once it has
	 * been assigned an active tag, lives in tq_open_ttags[] indexed by tag.
	 * All protected by tq_rx_buffers.tcbl_lock.
	 */
	nvmf_tcp_command_buffer_t	**tq_open_ttags; /* controller only */
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
#ifdef DEBUG
	uint32_t			tc_tx_data_offset; /* controller only */
	uint_t				tc_pending_r2ts;   /* controller only */
#endif

	list_node_t			tc_link;
} nvmf_tcp_capsule_t;

#define	TCAP(nc)	((nvmf_tcp_capsule_t *)(nc))
#define	TQP(qp)		((nvmf_tcp_qpair_t *)(qp))

/*
 * Reference-count helpers.  FreeBSD's refcount_release() returns true when the
 * count reached zero; reproduce that with atomic_dec_uint_nv.
 */
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

/*
 * NVMe/TCP header and data digests use CRC-32C (Castagnoli): the reflected
 * polynomial is 0x82F63B78 (the bit-reversal of 0x1EDC6F41).  The generic
 * <sys/crc32.h> CRC32() macro is table-driven, so we build a CRC-32C table at
 * module init and feed it to the same macro the rest of the kernel uses for
 * the standard CRC-32.  FreeBSD performs the equivalent computation in
 * calculate_crc32c() from sys/gsb_crc32.h.
 */
#define	NVMF_TCP_CRC32C_POLY	0x82F63B78U
static uint32_t nvmf_tcp_crc32c_table[256];

static uint32_t
nvmf_tcp_crc32c(const void *buf, size_t len, uint32_t start)
{
	uint32_t crc = start;

	CRC32(crc, buf, len, start, nvmf_tcp_crc32c_table);
	return (crc);
}

/* mblk equivalent of FreeBSD compute_digest() over a flat buffer. */
static uint32_t
nvmf_tcp_compute_digest(const void *buf, size_t len)
{
	return (nvmf_tcp_crc32c(buf, len, 0xffffffffU) ^ 0xffffffffU);
}

/*
 * mblk equivalent of FreeBSD mbuf_crc32c(): CRC-32C over [offset, offset+len)
 * of an mblk chain.  Walks the b_cont chain accumulating the running CRC-32C
 * across block boundaries (FreeBSD uses m_apply()).
 */
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

/*
 * Copy len bytes starting at chain offset 'off' out of an mblk chain into a
 * flat buffer.  mblk equivalent of FreeBSD m_copydata().
 */
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

/*
 * Command-buffer allocation and reference counting.  Direct ports of the
 * FreeBSD helpers of the same name.
 */
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
	cb->tcb_data_xfered = 0;
	cb->tcb_refs = 1;
	cb->tcb_error = 0;
	cb->tcb_cid = cid;
	cb->tcb_ttag = 0;
	cb->tcb_tc = NULL;
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
	nvmf_complete_io_request(&cb->tcb_io, cb->tcb_data_xfered,
	    cb->tcb_error);
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

/*
 * TX PDU queueing.
 *
 * FreeBSD's nvmf_tcp_write_pdu() enqueues an mbuf chain on qp->tx_pdus under the
 * snd sockbuf lock and signals the tx kthread.  illumos chains PDUs via b_next
 * under tq_tx_lock and kicks the tx taskq worker.
 */
static void
nvmf_tcp_write_pdu(nvmf_tcp_qpair_t *qp, mblk_t *mp)
{
	mutex_enter(&qp->tq_tx_lock);
	mp->b_next = NULL;
	if (qp->tq_tx_pdus_tail == NULL)
		qp->tq_tx_pdus_head = mp;
	else
		qp->tq_tx_pdus_tail->b_next = mp;
	qp->tq_tx_pdus_tail = mp;
	cv_signal(&qp->tq_tx_cv);
	mutex_exit(&qp->tq_tx_lock);
}

/*
 * Build and queue a termination-request PDU describing a fatal error.  This is
 * a straightforward format routine; the only OS glue is the mblk allocation and
 * the copy of the offending header bytes into the error-data region.
 */
static void
nvmf_tcp_report_error(nvmf_tcp_qpair_t *qp, uint16_t fes, uint32_t fei,
    mblk_t *rx_pdu, uint_t hlen)
{
	nvmf_tcp_term_req_hdr_t *hdr;
	mblk_t *mp;
	size_t mlen;

	if (hlen != 0) {
		hlen = MIN(hlen, NVMF_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE);
		hlen = (uint_t)MIN(hlen, msgdsize(rx_pdu));
	}

	mlen = sizeof (*hdr) + hlen;
	mp = allocb(mlen, BPRI_MED);
	if (mp == NULL)
		return;
	mp->b_wptr = mp->b_rptr + mlen;
	hdr = (nvmf_tcp_term_req_hdr_t *)mp->b_rptr;
	bzero(hdr, sizeof (*hdr));
	hdr->nttr_common.ntcph_pdu_type = qp->qp.nq_controller ?
	    NVMF_TCP_PDU_TYPE_C2H_TERM_REQ : NVMF_TCP_PDU_TYPE_H2C_TERM_REQ;
	hdr->nttr_common.ntcph_hlen = sizeof (*hdr);
	/*
	 * plen is a little-endian wire field.  This LE_32() is a deliberate
	 * divergence from FreeBSD, which assigns plen without htole32() (a latent
	 * big-endian bug); the wrap is correct on all endiannesses here.
	 */
	hdr->nttr_common.ntcph_plen = LE_32((uint32_t)(sizeof (*hdr) + hlen));
	hdr->nttr_fes = LE_16(fes);
	/* fei is a little-endian 4-byte field. */
	hdr->nttr_fei[0] = (uint8_t)(fei);
	hdr->nttr_fei[1] = (uint8_t)(fei >> 8);
	hdr->nttr_fei[2] = (uint8_t)(fei >> 16);
	hdr->nttr_fei[3] = (uint8_t)(fei >> 24);

	if (hlen != 0)
		nvmf_tcp_mblk_copydata(rx_pdu, 0, hlen, (caddr_t)(hdr + 1));

	nvmf_tcp_write_pdu(qp, mp);
}

/*
 * Validate a received PDU header and (if present) its header and data digests.
 * Header parsing and digest verification are straightforward; this is a direct
 * port of FreeBSD's nvmf_tcp_validate_pdu().
 */
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

	/* Check header digest if present. */
	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_HDGSTF) != 0) {
		digest = nvmf_tcp_mblk_crc32c(mp, 0, ch->ntcph_hlen);
		nvmf_tcp_mblk_copydata(mp, ch->ntcph_hlen, sizeof (rx_digest),
		    &rx_digest);
		if (digest != rx_digest) {
			cmn_err(CE_WARN, "NVMe/TCP: Header digest mismatch");
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
			cmn_err(CE_WARN, "NVMe/TCP: Data digest mismatch");
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

	cmn_err(CE_WARN,
	    "NVMe/TCP: Received termination request: fes 0x%x fei 0x%x",
	    LE_16(hdr->nttr_fes),
	    (uint32_t)hdr->nttr_fei[0] | ((uint32_t)hdr->nttr_fei[1] << 8) |
	    ((uint32_t)hdr->nttr_fei[2] << 16) |
	    ((uint32_t)hdr->nttr_fei[3] << 24));
	nvmf_tcp_free_pdu(pdu);
	return (ECONNRESET);
}

/*
 * Wrap a received command capsule into an nvmf_capsule and deliver it upstack.
 * Straightforward: the SQE lives at a fixed offset in the PDU.
 */
static int
nvmf_tcp_save_command_capsule(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_cmd_t *cmd;
	struct nvmf_capsule *nc;
	nvmf_tcp_capsule_t *tc;

	cmd = (const void *)pdu->rp_hdr;

	nc = nvmf_allocate_command(&qp->qp, cmd->ntc_ccsqe, KM_SLEEP);

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
 * Construct a PDU mblk chain containing a header, optional pad, optional data
 * payload, and optional header/data digests, filling in the common header
 * length fields.  This is a direct, mostly mechanical port of FreeBSD's
 * nvmf_tcp_construct_pdu(); the only OS glue is mblk allocation and chaining.
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
		ASSERT3U(msgdsize(data), ==, data_len);
		pdo = P2ROUNDUP(plen, qp->tq_txpda);
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

	top = allocb(mlen, BPRI_MED);
	if (top == NULL) {
		freemsg(data);
		return (NULL);
	}
	top->b_wptr = top->b_rptr + mlen;
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

			dmp = allocb(sizeof (digest), BPRI_MED);
			if (dmp == NULL) {
				/*
				 * FreeBSD uses M_WAITOK so the tail mbuf cannot
				 * fail to allocate.  We cannot emit a short PDU
				 * whose plen advertises a data digest that is
				 * not present (a peer protocol error), so fail
				 * construction; data is chained on top and freed
				 * with it.
				 */
				freemsg(top);
				return (NULL);
			}
			dmp->b_wptr = dmp->b_rptr + sizeof (digest);
			bcopy(&digest, dmp->b_rptr, sizeof (digest));
			dtail->b_cont = dmp;
		}
	}

	return (top);
}

/*
 * R2T scheduling helpers (controller only).  These are pure bookkeeping over
 * tq_open_ttags[] / tq_rx_buffers and are direct ports of the FreeBSD helpers.
 */
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
	if (mp != NULL)
		nvmf_tcp_write_pdu(qp, mp);
}

/*
 * Release a transfer tag and schedule another R2T.  NB: drops
 * tq_rx_buffers.tcbl_lock.  Direct port of nvmf_tcp_send_next_r2t().
 */
static void
nvmf_tcp_send_next_r2t(nvmf_tcp_qpair_t *qp, nvmf_tcp_command_buffer_t *cb)
{
	nvmf_tcp_command_buffer_t *ncb;

	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));
	ASSERT3P(qp->tq_open_ttags[cb->tcb_ttag], ==, cb);

	/* Release this transfer tag. */
	qp->tq_open_ttags[cb->tcb_ttag] = NULL;
	qp->tq_active_ttags--;
	cb->tcb_tc->tc_active_r2ts--;

	/* Schedule another R2T. */
	ncb = nvmf_tcp_next_r2t(qp);
	if (ncb != NULL) {
		nvmf_tcp_allocate_ttag(qp, ncb);
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		tcp_send_r2t(qp, ncb->tcb_cid, ncb->tcb_ttag,
		    ncb->tcb_data_offset, ncb->tcb_data_len);
	} else {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
	}
}

/*
 * Copy 'len' bytes into the destination mblk chain 'dmp' starting at chain
 * offset 'io_offset'.  Helper for the NVMF_MEMDESC_MBLK case of
 * nvmf_tcp_mblk_copyto_io().
 */
static void
nvmf_tcp_copyto_mblk(mblk_t *dmp, uint_t io_offset, const uint8_t *src,
    uint_t len)
{
	while (dmp != NULL && io_offset >= MBLKL(dmp)) {
		io_offset -= MBLKL(dmp);
		dmp = dmp->b_cont;
	}
	while (len != 0 && dmp != NULL) {
		uint_t todo = (uint_t)MIN(MBLKL(dmp) - io_offset, len);
		bcopy(src, dmp->b_rptr + io_offset, todo);
		io_offset = 0;
		src += todo;
		len -= todo;
		dmp = dmp->b_cont;
	}
}

/*
 * Copy len bytes starting at offset skip from an mblk chain into an I/O buffer
 * (nvmf_memdesc) at destination offset io_offset.  mblk equivalent of FreeBSD
 * mbuf_copyto_io(); honors both the flat-buffer (NVMF_MEMDESC_VADDR) and the
 * destination mblk chain (NVMF_MEMDESC_MBLK) backing stores the same way
 * FreeBSD's memdesc_copyback() dispatches on memdesc type.
 */
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

		if (io->io_mem.nmd_type == NVMF_MEMDESC_VADDR) {
			bcopy(mp->b_rptr + skip,
			    (caddr_t)io->io_mem.nmd_u.nmd_vaddr + io_offset,
			    todo);
		} else {
			nvmf_tcp_copyto_mblk(io->io_mem.nmd_u.nmd_mp, io_offset,
			    mp->b_rptr + skip, todo);
		}
		skip = 0;
		io_offset += todo;
		len -= todo;
		mp = mp->b_cont;
	}
}

/*
 * Wrap a [data_offset, data_offset+data_len) window of a command buffer's I/O
 * memory as a freshly-allocated mblk chain to transmit.  This is the illumos
 * equivalent of FreeBSD's nvmf_tcp_command_buffer_mbuf(): rather than building
 * zero-copy external mbufs that hold a reference on the command buffer, we copy
 * the bytes into a normal mblk.  The caller still holds its command-buffer
 * reference for the duration of the transfer, so correctness is preserved; the
 * cost is an extra copy on the TX data path (a future optimization could use
 * esballoc() to share the command buffer's pages).
 */
static mblk_t *
nvmf_tcp_command_buffer_mblk(nvmf_tcp_command_buffer_t *cb,
    uint32_t data_offset, uint32_t data_len)
{
	nvmf_memdesc_t *md = &cb->tcb_io.io_mem;
	mblk_t *mp;

	mp = allocb(data_len, BPRI_MED);
	if (mp == NULL)
		return (NULL);
	mp->b_wptr = mp->b_rptr + data_len;

	if (md->nmd_type == NVMF_MEMDESC_VADDR) {
		bcopy((caddr_t)md->nmd_u.nmd_vaddr + data_offset, mp->b_rptr,
		    data_len);
	} else {
		nvmf_tcp_mblk_copydata(md->nmd_u.nmd_mp, data_offset, data_len,
		    mp->b_rptr);
	}
	return (mp);
}

/*
 * Inbound data PDU handlers.
 *
 * These are the heart of the data-transfer state machine: range/sequence
 * validation, command-buffer lookup, R2T release, copy-out of the received mblk
 * into the consumer's memdesc, and the completion/CQE handoff, all preserved
 * field-for-field from FreeBSD.
 */
static int
nvmf_tcp_handle_h2c_data(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	const nvmf_tcp_h2c_data_hdr_t *h2c;
	nvmf_tcp_command_buffer_t *cb;
	uint32_t data_len, data_offset;
	uint16_t ttag;

	h2c = (const void *)pdu->rp_hdr;
	if (LE_32(h2c->nth2c_datal) > qp->tq_maxh2cdata) {
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED, 0,
		    pdu->rp_mp, pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	/* NB: ttag is not byte-swapped (opaque cookie). */
	ttag = h2c->nth2c_ttag;
	if (ttag >= qp->tq_num_ttags) {
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_ttag), pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	cb = qp->tq_open_ttags[ttag];
	if (cb == NULL) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_ttag), pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}
	ASSERT3U(cb->tcb_ttag, ==, ttag);

	/*
	 * For a data digest mismatch, fail the I/O request.  FreeBSD reports
	 * EINTEGRITY here; illumos has no such errno, so EIO is used to flag the
	 * digest failure to the consumer's io_complete callback.
	 */
	if (pdu->rp_data_digest_mismatch) {
		nvmf_tcp_send_next_r2t(qp, cb);	/* drops tcbl_lock */
		cb->tcb_error = EIO;
		tcp_release_command_buffer(cb);
		nvmf_tcp_free_pdu(pdu);
		return (0);
	}

	data_len = LE_32(h2c->nth2c_datal);
	if (data_len != pdu->rp_data_len) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		nvmf_tcp_report_error(qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD,
		    offsetof(nvmf_tcp_h2c_data_hdr_t, nth2c_datal), pdu->rp_mp,
		    pdu->rp_hdr->ntcph_hlen);
		nvmf_tcp_free_pdu(pdu);
		return (EBADMSG);
	}

	data_offset = LE_32(h2c->nth2c_datao);
	if (data_offset < cb->tcb_data_offset ||
	    data_offset + data_len > cb->tcb_data_offset + cb->tcb_data_len) {
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
	    ((pdu->rp_hdr->ntcph_flags & NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU) !=
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
	if (cb->tcb_data_xfered == cb->tcb_data_len) {
		nvmf_tcp_send_next_r2t(qp, cb);	/* drops tcbl_lock */
	} else {
		tcp_hold_command_buffer(cb);
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
	}

	nvmf_tcp_mblk_copyto_io(pdu->rp_mp, pdu->rp_hdr->ntcph_pdo, data_len,
	    &cb->tcb_io, data_offset);

	tcp_release_command_buffer(cb);
	nvmf_tcp_free_pdu(pdu);
	return (0);
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
		/*
		 * XXX: Could be PDU sequence error if cccid is for a command
		 * that doesn't use a command buffer.
		 */
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
	if (data_offset < cb->tcb_data_offset ||
	    data_offset + data_len > cb->tcb_data_offset + cb->tcb_data_len) {
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
	if (last_pdu)
		h2c.nth2c_common.ntcph_flags |= NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU;
	h2c.nth2c_cccid = cid;
	h2c.nth2c_ttag = ttag;
	h2c.nth2c_datao = LE_32(data_offset);
	h2c.nth2c_datal = LE_32((uint32_t)len);

	top = nvmf_tcp_construct_pdu(qp, &h2c, sizeof (h2c), mp, (uint32_t)len);
	if (top != NULL)
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

	/*
	 * XXX: The spec does not specify how to handle R2T transfers out of
	 * range of the original command.
	 */
	data_len = LE_32(r2t->ntr2t_r2tl);
	if (data_offset + data_len > cb->tcb_data_len) {
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
		if (mp == NULL) {
			cb->tcb_error = ENOMEM;
			break;
		}
		tcp_send_h2c_pdu(qp, r2t->ntr2t_cccid, r2t->ntr2t_ttag,
		    data_offset, mp, todo, todo == data_len);

		data_offset += todo;
		data_len -= todo;
	}

	tcp_release_command_buffer(cb);
	nvmf_tcp_free_pdu(pdu);
	return (0);
}

/*
 * Ensure the PDU header is contiguous in the lead mblk, then dispatch by PDU
 * type.  mblk equivalent of FreeBSD's pullup_pdu_hdr() + nvmf_tcp_dispatch_pdu().
 */
static int
nvmf_tcp_dispatch_pdu(nvmf_tcp_qpair_t *qp,
    const nvmf_tcp_common_pdu_hdr_t *ch, nvmf_tcp_rxpdu_t *pdu)
{
	/*
	 * Ensure the first mblk holds at least ch->ntcph_hlen bytes so the
	 * type-specific header can be dereferenced contiguously (FreeBSD
	 * pullup_pdu_hdr()).  pullupmsg() only fails on allocation failure here,
	 * which we treat as a fatal connection error.
	 */
	if (MBLKL(pdu->rp_mp) < ch->ntcph_hlen) {
		if (pullupmsg(pdu->rp_mp, ch->ntcph_hlen) == 0) {
			nvmf_tcp_free_pdu(pdu);
			return (ECONNRESET);
		}
	}
	pdu->rp_hdr = (const nvmf_tcp_common_pdu_hdr_t *)pdu->rp_mp->b_rptr;

	switch (ch->ntcph_pdu_type) {
	default:
		VERIFY(0);
		return (ECONNRESET);
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
	}
}

/*
 * Event-driven socket receive.
 *
 * This is the illumos replacement for FreeBSD's dedicated rx kthread
 * (nvmf_tcp_receive()).  ksocket_krecv_set() registers nvmf_tcp_krecv() to be
 * called with received data as mblk chains; that callback only accumulates
 * bytes into tq_rx_mp and wakes the rx worker (nvmf_tcp_so_rx), which frames out
 * whole PDUs (8-byte common header, then ntcph_plen bytes) and validates and
 * dispatches them in a sleepable context.
 */
/*
 * Split exactly 'n' bytes off the head of the chain '*mpp', returning that
 * prefix as its own chain and leaving the remainder (if any) in '*mpp'.  If
 * 'n' falls in the middle of an mblk, that mblk is duplicated so the two halves
 * do not share a data block (the head half keeps the data, the tail half
 * references the same dblk past the split point via dupb()).  Returns NULL on
 * allocation failure, leaving '*mpp' unchanged.
 *
 * Caller guarantees msgdsize(*mpp) >= n.  This is the mblk equivalent of
 * FreeBSD's m_split() as used by the RX framing loop.
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
		/* Split lands on a clean mblk boundary. */
		*mpp = mp;
		if (prev != NULL)
			prev->b_cont = NULL;
		return (head);
	}

	/*
	 * The split lands inside 'mp': duplicate it so the tail references the
	 * bytes after the split and the head keeps the bytes before it.
	 */
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
 * Read the 8-byte common PDU header out of the head of the accumulated chain
 * into 'ch' (the header may be split across mblks).  Caller guarantees the
 * chain holds at least sizeof (*ch) bytes.
 */
static void
nvmf_tcp_peek_common_hdr(mblk_t *mp, nvmf_tcp_common_pdu_hdr_t *ch)
{
	nvmf_tcp_mblk_copydata(mp, 0, sizeof (*ch), ch);
}

/*
 * Frame and dispatch every complete PDU accumulated in qp->tq_rx_mp.  This is
 * the illumos port of the per-PDU framing loop in FreeBSD
 * nvmf_tcp.c:nvmf_tcp_receive():
 *
 *   1. Once at least a common header is buffered, read it to learn plen
 *      (clamped to >= the common header size); cache it in tq_rx_needed.
 *   2. Once tq_rx_needed bytes are buffered, split exactly that many bytes off
 *      tq_rx_mp into a standalone PDU chain (the rest stays buffered for the
 *      next PDU).
 *   3. Validate + dispatch the PDU; repeat while another whole PDU is buffered.
 *
 * Returns 0 to continue or an errno on a fatal framing/dispatch error.  The
 * tq_rx_lock is dropped across validate/dispatch (which call upstack) and
 * retaken; tq_rx_mp is only mutated under the lock.
 */
static int
nvmf_tcp_rx_drain(nvmf_tcp_qpair_t *qp)
{
	nvmf_tcp_rxpdu_t pdu;
	nvmf_tcp_common_pdu_hdr_t ch;
	mblk_t *pdu_mp;
	int error;

	ASSERT(MUTEX_HELD(&qp->tq_rx_lock));

	for (;;) {
		size_t avail;

		if (qp->tq_rx_mp == NULL)
			return (0);

		avail = msgdsize(qp->tq_rx_mp);

		if (!qp->tq_rx_have_header) {
			if (avail < sizeof (ch))
				return (0);

			nvmf_tcp_peek_common_hdr(qp->tq_rx_mp, &ch);
			qp->tq_rx_needed = LE_32(ch.ntcph_plen);

			/*
			 * Malformed PDUs will be reported as errors by
			 * nvmf_tcp_validate_pdu(); just frame off a common
			 * header's worth if the length is implausible.
			 */
			if (qp->tq_rx_needed < sizeof (ch) ||
			    ch.ntcph_hlen > qp->tq_rx_needed)
				qp->tq_rx_needed = sizeof (ch);

			qp->tq_rx_have_header = B_TRUE;
		}

		if (avail < qp->tq_rx_needed)
			return (0);

		pdu_mp = nvmf_tcp_chain_split(&qp->tq_rx_mp, qp->tq_rx_needed);
		if (pdu_mp == NULL)
			return (ENOMEM);

		qp->tq_rx_have_header = B_FALSE;

		pdu.rp_mp = pdu_mp;
		pdu.rp_hdr = NULL;

		mutex_exit(&qp->tq_rx_lock);
		/*
		 * Make the common header contiguous so rp_hdr can be
		 * dereferenced for validation; the type-specific pullup happens
		 * in nvmf_tcp_dispatch_pdu().
		 */
		if (MBLKL(pdu.rp_mp) < sizeof (ch) &&
		    pullupmsg(pdu.rp_mp, sizeof (ch)) == 0) {
			nvmf_tcp_free_pdu(&pdu);
			mutex_enter(&qp->tq_rx_lock);
			return (ENOMEM);
		}
		pdu.rp_hdr = (const nvmf_tcp_common_pdu_hdr_t *)pdu.rp_mp->b_rptr;

		error = nvmf_tcp_validate_pdu(qp, &pdu);
		if (error != 0)
			nvmf_tcp_free_pdu(&pdu);
		else
			error = nvmf_tcp_dispatch_pdu(qp, pdu.rp_hdr, &pdu);
		mutex_enter(&qp->tq_rx_lock);

		if (error != 0)
			return (error);
	}
}

/*
 * Inbound socket worker.
 *
 * illumos replacement for FreeBSD's rx kthread (nvmf_tcp_receive()).  Frames and
 * dispatches whole PDUs out of tq_rx_mp.  This runs on a dedicated single-thread
 * taskq (a sleepable kernel-thread context), not in the krecv upcall, so the
 * KM_SLEEP allocations and blocking upstack delivery reached via
 * nvmf_tcp_rx_drain() -> nvmf_tcp_dispatch_pdu() are legal here.
 */
static void
nvmf_tcp_so_rx(void *arg)
{
	nvmf_tcp_qpair_t *qp = arg;
	int error;

	mutex_enter(&qp->tq_rx_lock);
	for (;;) {
		while (!qp->tq_rx_shutdown && !qp->tq_rx_ready)
			cv_wait(&qp->tq_rx_cv, &qp->tq_rx_lock);
		if (qp->tq_rx_shutdown)
			break;

		qp->tq_rx_ready = B_FALSE;

		/* nvmf_tcp_rx_drain() drops and retakes tq_rx_lock. */
		error = nvmf_tcp_rx_drain(qp);
		if (error != 0) {
			mutex_exit(&qp->tq_rx_lock);
			/*
			 * On a fatal framing/dispatch error, surface it to the
			 * qpair owner and stop processing until torn down.
			 */
			nvmf_qpair_error(&qp->qp, error);
			mutex_enter(&qp->tq_rx_lock);
			break;
		}
	}
	mutex_exit(&qp->tq_rx_lock);
}

/*
 * Event-driven socket receive upcall.  This must not assume a sleepable context:
 * it only appends the received chain to tq_rx_mp and wakes the rx worker, which
 * does the framing and (blocking) dispatch.
 */
/* ARGSUSED */
static boolean_t
nvmf_tcp_krecv(ksocket_t so, mblk_t *mp, size_t msgsize, int oob, void *arg)
{
	nvmf_tcp_qpair_t *qp = arg;

	mutex_enter(&qp->tq_rx_lock);
	if (qp->tq_rx_shutdown) {
		mutex_exit(&qp->tq_rx_lock);
		freemsg(mp);
		return (B_TRUE);
	}

	/* Append the newly received chain to any accumulated partial PDU. */
	if (qp->tq_rx_mp == NULL) {
		qp->tq_rx_mp = mp;
	} else {
		mblk_t *tail = qp->tq_rx_mp;
		while (tail->b_cont != NULL)
			tail = tail->b_cont;
		tail->b_cont = mp;
	}

	qp->tq_rx_ready = B_TRUE;
	cv_signal(&qp->tq_rx_cv);
	mutex_exit(&qp->tq_rx_lock);

	return (B_TRUE);
}

/*
 * Outbound socket worker.
 *
 * illumos replacement for FreeBSD's tx kthread (nvmf_tcp_send()).  Drains queued
 * PDUs (tq_tx_pdus_head) and pending capsules (tq_tx_capsules), converting
 * capsules to PDUs and pushing mblks down the socket with ksocket_sendmblk().
 */
static void
nvmf_tcp_so_tx(void *arg)
{
	nvmf_tcp_qpair_t *qp = arg;
	nvmf_tcp_capsule_t *tc;
	mblk_t *mp;
	struct msghdr msg;
	int rc;

	mutex_enter(&qp->tq_tx_lock);
	while (!qp->tq_tx_shutdown) {
		/* Next ready-to-send PDU mblk, if any. */
		mp = qp->tq_tx_pdus_head;
		if (mp != NULL) {
			qp->tq_tx_pdus_head = mp->b_next;
			if (qp->tq_tx_pdus_head == NULL)
				qp->tq_tx_pdus_tail = NULL;
			mp->b_next = NULL;
		} else {
			/* Otherwise convert the next queued capsule to a PDU. */
			tc = list_remove_head(&qp->tq_tx_capsules);
			if (tc == NULL) {
				cv_wait(&qp->tq_tx_cv, &qp->tq_tx_lock);
				continue;
			}
			mutex_exit(&qp->tq_tx_lock);
			mp = capsule_to_pdu(qp, tc);
			tcp_release_capsule(tc);
			mutex_enter(&qp->tq_tx_lock);
			if (mp == NULL)
				continue;
		}
		mutex_exit(&qp->tq_tx_lock);

		/*
		 * ksocket_sendmblk() may consume only part of the chain,
		 * returning the unsent remainder in &mp.  This worker is a
		 * dedicated kernel thread (not a softint/datagram context like
		 * overlay_mux_tx()), so it is safe to block until the entire
		 * chain has been handed to the socket: a blocking send drives
		 * the chain out under TCP flow control without us reimplementing
		 * the socket-buffer accounting FreeBSD's nvmf_tcp_send() does.
		 */
		rc = 0;
		while (mp != NULL) {
			/*
			 * Bail out of the blocking send loop if teardown started,
			 * so a flow-controlled send cannot re-park us after
			 * tcp_free_qpair() has shut the socket down.  This is an
			 * advisory racy read; ksocket_shutdown() is the authoritative
			 * wakeup that makes the in-progress ksocket_sendmblk() return.
			 */
			if (qp->tq_tx_shutdown) {
				freemsg(mp);
				mp = NULL;
				rc = 0;
				break;
			}
			bzero(&msg, sizeof (msg));
			rc = ksocket_sendmblk(qp->tq_so, &msg, 0, &mp, CRED());
			if (rc != 0)
				break;
		}
		if (rc != 0) {
			freemsg(mp);
			nvmf_qpair_error(&qp->qp, rc);
			mutex_enter(&qp->tq_tx_lock);
			break;
		}

		mutex_enter(&qp->tq_tx_lock);
	}
	mutex_exit(&qp->tq_tx_lock);
}

/*
 * Capsule -> PDU construction (TX side).  These build the on-wire command and
 * response PDUs from an nvmf_capsule, wrapping any in-capsule data as an mblk
 * chain and registering out-of-line command buffers for later R2T/C2H/H2C
 * movement.
 */
static mblk_t *
tcp_command_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	struct nvmf_capsule *nc = &tc->nc;
	nvmf_tcp_command_buffer_t *cb;
	nvme_sqe_t sqe;
	nvme_sgl_t *sgl;
	nvmf_tcp_cmd_t cmd;
	mblk_t *top, *data;
	boolean_t use_icd;

	use_icd = B_FALSE;
	cb = NULL;
	data = NULL;

	if (nc->nc_data.io_len != 0) {
		cb = tcp_alloc_command_buffer(qp, &nc->nc_data, 0,
		    nc->nc_data.io_len, nc->nc_sqe.sqe_cid);

		if (nc->nc_send_data && nc->nc_data.io_len <= qp->tq_max_icd) {
			data = nvmf_tcp_command_buffer_mblk(cb, 0,
			    (uint32_t)nc->nc_data.io_len);
			if (data != NULL) {
				use_icd = B_TRUE;
				cb->tcb_data_xfered = nc->nc_data.io_len;
				tcp_release_command_buffer(cb);
			} else {
				/*
				 * FreeBSD uses M_WAITOK here so the ICD copy
				 * cannot fail.  allocb(BPRI_MED) can, so fall
				 * back to the out-of-line command-buffer path
				 * (R2T/H2C) rather than emitting an ICD SGL with
				 * no data, which a target would reject and which
				 * would drop the write payload.
				 */
				mutex_enter(&qp->tq_tx_buffers.tcbl_lock);
				tcp_add_command_buffer(&qp->tq_tx_buffers, cb);
				mutex_exit(&qp->tq_tx_buffers.tcbl_lock);
			}
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

	/*
	 * Build the SQE (with its SGL) in a properly-aligned local before
	 * copying it into the packed PDU; ntc_ccsqe is a byte array in the
	 * pack(1) PDU struct so it cannot be type-punned in place.
	 */
	sqe = nc->nc_sqe;
	sgl = &sqe.sqe_dptr.d_sgl;
	bzero(sgl, sizeof (*sgl));
	sgl->sgl_addr = 0;
	sgl->sgl_len = LE_32((uint32_t)nc->nc_data.io_len);
	if (use_icd) {
		/* In-capsule data. */
		sgl->sgl_type = NVME_SGL_TYPE_ICD >> 4;
		sgl->sgl_zero = NVME_SGL_TYPE_ICD & 0xf;
	} else {
		/* Command buffer (out-of-line via R2T/H2C/C2H). */
		sgl->sgl_type = NVME_SGL_TYPE_COMMAND_BUFFER >> 4;
		sgl->sgl_zero = NVME_SGL_TYPE_COMMAND_BUFFER & 0xf;
	}

	bzero(&cmd, sizeof (cmd));
	cmd.ntc_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_CMD;
	bcopy(&sqe, cmd.ntc_ccsqe, sizeof (sqe));

	top = nvmf_tcp_construct_pdu(qp, &cmd, sizeof (cmd), data,
	    data != NULL ? (uint32_t)nc->nc_data.io_len : 0);

	/*
	 * FreeBSD's M_WAITOK construct_pdu never fails; allocb(BPRI_MED) can.
	 * If we registered an out-of-line command buffer (not ICD) but the PDU
	 * was dropped, no R2T/C2H will ever arrive for it, so the I/O would hang
	 * until qpair teardown.  Purge the registered buffer to complete the I/O
	 * with the error from tcb_error rather than leaking it.
	 */
	if (top == NULL && cb != NULL && !use_icd) {
		cb->tcb_error = ENOMEM;
		if (nc->nc_send_data) {
			tcp_purge_command_buffer(&qp->tq_tx_buffers,
			    nc->nc_sqe.sqe_cid, 0);
		} else {
			tcp_purge_command_buffer(&qp->tq_rx_buffers,
			    nc->nc_sqe.sqe_cid, 0);
		}
	}

	return (top);
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
 * Adopt a userland TCP socket file descriptor as a kernel ksocket_t.
 *
 * The handoff nvlist carries an open, connected socket fd that the userland
 * Fabrics daemon created and ran the CONNECT exchange on.  getsonode() resolves
 * the fd to its sonode in the ioctl caller's process context (getf() indexes the
 * calling process's open-file table), returning with the fd held; the caller
 * must releasef() it.  A ksocket_t is an opaque handle that is, internally, just
 * the sonode pointer (io/ksocket/ksocket_impl.h: SOTOKS(so) == the cast below);
 * a sonode becomes usable through the ksocket_* KPI once SM_KERNEL is set, which
 * ksocket_socket()/ksocket_accept() do for kernel-created sockets and which we
 * must set by hand for an adopted one.
 *
 * Reference counting (see ksocket_hold/ksocket_rele/ksocket_close):
 *   - a userland socket() starts at so_count == 1 and its vnode at v_count == 1
 *     (both held by the daemon's open fd);
 *   - ksocket_hold() bumps so_count so the sonode survives releasef() and the
 *     daemon's subsequent close(fd) (which only decrements so_count, not to 0);
 *   - VN_HOLD() bumps the underlying vnode's v_count for the same reason: the
 *     daemon's close(fd) does a vn_rele(), and without our hold v_count would
 *     drop to 0, so the vn_rele() in ksocket_close() -> socket_destroy() would
 *     underflow ("vp->v_count > 0" assertion panic).  A kernel-created socket
 *     owns exactly this vnode reference; an adopted one must take it too;
 *   - tcp_free_qpair() calls ksocket_close(), which waits for so_count to drain
 *     to 1 (our hold), performs the single owning close, and releases our
 *     VN_HOLD via socket_destroy() -> vn_rele().  We therefore do NOT also call
 *     ksocket_rele(): exactly one owning close is correct, and pairing
 *     rele()+close() would either panic (so_count < 2) or race the daemon's
 *     close.
 */
static int
tcp_adopt_socket_fd(uint64_t fd, ksocket_t *ksp)
{
	struct sonode *so;
	int error = 0;

	*ksp = NULL;
	if (fd > INT_MAX)
		return (EBADF);

	so = getsonode((int)fd, &error, NULL);
	if (so == NULL)
		return (error != 0 ? error : EBADF);

	/* The transport only drives connection-oriented stream sockets. */
	if (so->so_type != SOCK_STREAM) {
		releasef((int)fd);
		return (ENOTSOCK);
	}

	/*
	 * NVMe/TCP runs over IP only.  getsonode() already rejects non-sockets
	 * and the SOCK_STREAM check above rejects datagram sockets, but a
	 * connected AF_UNIX SOCK_STREAM fd would otherwise pass; refuse any
	 * non-INET family so the framing engine never drives a local socket.
	 */
	if (so->so_family != AF_INET && so->so_family != AF_INET6) {
		releasef((int)fd);
		return (EAFNOSUPPORT);
	}

	/*
	 * Mark the sonode kernel-owned and take the sonode + vnode references
	 * that outlive the userland fd, then drop the fd hold from getsonode().
	 * Order matters: the holds must be taken while the getf() reference still
	 * pins the sonode, so a concurrent last close cannot free it underneath
	 * us.
	 */
	mutex_enter(&so->so_lock);
	so->so_mode |= SM_KERNEL;
	mutex_exit(&so->so_lock);

	*ksp = (ksocket_t)(uintptr_t)so;
	ksocket_hold(*ksp);
	VN_HOLD(SOTOV(so));
	releasef((int)fd);

	return (0);
}

/*
 * Queue-pair lifecycle.
 */
static struct nvmf_qpair *
tcp_allocate_qpair(boolean_t controller, const nvlist_t *nvl)
{
	nvmf_tcp_qpair_t *qp;
	ksocket_t so;
	uint64_t fd, scratch;
	boolean_t bscratch;
	int error;

	/*
	 * Confirm every required TCP-transport key is present and of the right
	 * type.  illumos has no nvlist_exists_<type>() helpers; instead each
	 * nvlist_lookup_<type>() returns 0 only when the key exists with the
	 * matching type, which is the same gate FreeBSD's
	 * nvlist_exists_number()/nvlist_exists_bool() express.
	 */
	if (nvlist_lookup_uint64((nvlist_t *)nvl, "fd", &fd) != 0 ||
	    nvlist_lookup_uint64((nvlist_t *)nvl, "rxpda", &scratch) != 0 ||
	    nvlist_lookup_uint64((nvlist_t *)nvl, "txpda", &scratch) != 0 ||
	    nvlist_lookup_boolean_value((nvlist_t *)nvl, "header_digests",
	    &bscratch) != 0 ||
	    nvlist_lookup_boolean_value((nvlist_t *)nvl, "data_digests",
	    &bscratch) != 0 ||
	    nvlist_lookup_uint64((nvlist_t *)nvl, "maxr2t", &scratch) != 0 ||
	    nvlist_lookup_uint64((nvlist_t *)nvl, "maxh2cdata", &scratch) != 0 ||
	    nvlist_lookup_uint64((nvlist_t *)nvl, "max_icd", &scratch) != 0)
		return (NULL);

	/*
	 * Turn the userland file descriptor "fd" into a ksocket_t.  This runs in
	 * the ioctl caller's process context (getsonode()/getf() resolve against
	 * the calling process's open-file table), which is guaranteed because the
	 * whole handoff path -- nvmft_drv_ioctl() -> nvmft_handoff_*_queue() ->
	 * nvmft_qpair_init() -> nvmf_allocate_qpair() -> here -- is synchronous on
	 * the ioctl thread.  (FreeBSD nvmf_tcp.c:tcp_allocate_qpair)
	 */
	error = tcp_adopt_socket_fd(fd, &so);
	if (error != 0)
		return (NULL);

	qp = kmem_zalloc(sizeof (*qp), KM_SLEEP);
	qp->tq_so = so;
	qp->tq_refs = 1;
	qp->tq_txpda = (uint8_t)fnvlist_lookup_uint64((nvlist_t *)nvl, "txpda");
	qp->tq_rxpda = (uint8_t)fnvlist_lookup_uint64((nvlist_t *)nvl, "rxpda");
	qp->tq_header_digests = fnvlist_lookup_boolean_value((nvlist_t *)nvl,
	    "header_digests");
	qp->tq_data_digests = fnvlist_lookup_boolean_value((nvlist_t *)nvl,
	    "data_digests");
	qp->tq_maxr2t = (uint32_t)fnvlist_lookup_uint64((nvlist_t *)nvl,
	    "maxr2t");
	if (controller) {
		qp->tq_maxh2cdata = (uint32_t)fnvlist_lookup_uint64(
		    (nvlist_t *)nvl, "maxh2cdata");
	}
	qp->tq_max_tx_data = nvmf_tcp_max_transmit_data;
	if (!controller) {
		qp->tq_max_tx_data = (uint32_t)MIN(qp->tq_max_tx_data,
		    fnvlist_lookup_uint64((nvlist_t *)nvl, "maxh2cdata"));
		qp->tq_max_icd = (uint32_t)fnvlist_lookup_uint64((nvlist_t *)nvl,
		    "max_icd");
	}

	if (controller) {
		/* Use the SUCCESS flag if SQ flow control is disabled. */
		qp->tq_send_success =
		    !fnvlist_lookup_boolean_value((nvlist_t *)nvl,
		    "sq_flow_control");

		/* NB: maxr2t is 0's based. */
		qp->tq_num_ttags = (uint_t)MIN((uint64_t)UINT16_MAX + 1,
		    fnvlist_lookup_uint64((nvlist_t *)nvl, "qsize") *
		    ((uint64_t)qp->tq_maxr2t + 1));
		qp->tq_open_ttags = kmem_zalloc(qp->tq_num_ttags *
		    sizeof (*qp->tq_open_ttags), KM_SLEEP);
	}

	list_create(&qp->tq_rx_buffers.tcbl_list,
	    sizeof (nvmf_tcp_command_buffer_t),
	    offsetof(nvmf_tcp_command_buffer_t, tcb_link));
	list_create(&qp->tq_tx_buffers.tcbl_list,
	    sizeof (nvmf_tcp_command_buffer_t),
	    offsetof(nvmf_tcp_command_buffer_t, tcb_link));
	mutex_init(&qp->tq_rx_buffers.tcbl_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&qp->tq_tx_buffers.tcbl_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&qp->tq_tx_capsules, sizeof (nvmf_tcp_capsule_t),
	    offsetof(nvmf_tcp_capsule_t, tc_link));

	mutex_init(&qp->tq_rx_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&qp->tq_tx_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&qp->tq_rx_cv, NULL, CV_DRIVER, NULL);
	cv_init(&qp->tq_tx_cv, NULL, CV_DRIVER, NULL);

	qp->tq_tx_taskq = taskq_create("nvmf_tcp_tx", 1, minclsyspri, 1, 1,
	    TASKQ_PREPOPULATE);
	qp->tq_rx_taskq = taskq_create("nvmf_tcp_rx", 1, minclsyspri, 1, 1,
	    TASKQ_PREPOPULATE);

	/*
	 * Spin up the persistent tx worker and register the event-driven receive
	 * callback, replacing FreeBSD's kthread_add()+soupcall_set(SO_RCV) pair.
	 * The single tx worker runs for the life of the qpair (woken by tq_tx_cv)
	 * just like FreeBSD's tx kthread, so write_pdu()/transmit_capsule() only
	 * need to signal it.
	 *
	 * tq_so is now bound to the adopted socket (tcp_adopt_socket_fd() above
	 * returned non-NULL or we would have bailed).  A TQ_SLEEP dispatch into a
	 * single-thread taskq cannot fail; if ksocket_krecv_set() fails, tear the
	 * qpair back down (which ksocket_close()s the adopted socket) and return
	 * NULL.
	 */
	(void) taskq_dispatch(qp->tq_tx_taskq, nvmf_tcp_so_tx, qp, TQ_SLEEP);
	(void) taskq_dispatch(qp->tq_rx_taskq, nvmf_tcp_so_rx, qp, TQ_SLEEP);
	if (ksocket_krecv_set(qp->tq_so, nvmf_tcp_krecv, qp) != 0) {
		tcp_free_qpair(&qp->qp);
		return (NULL);
	}
	qp->tq_krecv_set = B_TRUE;

	return (&qp->qp);
}

static void
tcp_release_qpair(nvmf_tcp_qpair_t *qp)
{
	if (nvmf_tcp_refcount_release(&qp->tq_refs)) {
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
		cv_destroy(&qp->tq_rx_cv);
		cv_destroy(&qp->tq_tx_cv);
		if (qp->tq_tx_taskq != NULL)
			taskq_destroy(qp->tq_tx_taskq);
		if (qp->tq_rx_taskq != NULL)
			taskq_destroy(qp->tq_rx_taskq);
		kmem_free(qp, sizeof (*qp));
	}
}

static void
tcp_free_qpair(struct nvmf_qpair *nq)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	nvmf_tcp_command_buffer_t *cb;
	nvmf_tcp_capsule_t *tc;
	mblk_t *pmp;

	/*
	 * Quiesce RX, then mark shutdown and wake the rx worker so it exits
	 * before we touch the state it consumes.  Order matters:
	 * ksocket_krecv_unblock() releases any receive backpressure the socket
	 * asserted, but so_krecv_unblock() VERIFYs the krecv callback is still
	 * set, so it MUST run before ksocket_krecv_set(NULL) clears the callback.
	 * Clearing first and then unblocking panics the kernel (so_krecv_cb ==
	 * NULL, sockcommon_subr.c).  A krecv upcall racing between the two calls
	 * is harmless: it only accumulates under tq_rx_lock and the worker is
	 * shut down immediately below.
	 *
	 * Only unblock if the callback was actually installed: the allocate-qpair
	 * error path frees a qpair whose ksocket_krecv_set() failed (e.g. ENOTSUP
	 * on a fallback socket), leaving so_krecv_cb NULL -- unblocking that socket
	 * would trip the same VERIFY.
	 */
	if (qp->tq_so != NULL) {
		if (qp->tq_krecv_set)
			ksocket_krecv_unblock(qp->tq_so);
		(void) ksocket_krecv_set(qp->tq_so, NULL, NULL);

		/*
		 * Shut the socket down for both directions before waiting on the
		 * workers.  The tx worker does a BLOCKING ksocket_sendmblk() and
		 * only checks tq_tx_shutdown at its outer loop, so if the peer
		 * vanished without a RST (e.g. keepalive timeout) it can be parked
		 * indefinitely in so_snd_wait_qnotfull() with the send window
		 * full; cv_signal(tq_tx_cv) does not reach it there.  socantsendmore
		 * via shutdown broadcasts so_snd_cv, so the blocked send returns
		 * EPIPE, the worker exits, and taskq_wait() below cannot hang the
		 * (single-threaded) ns_taskq teardown thread.  ksocket_close() at
		 * the end still does the owning close + vn_rele.
		 */
		(void) ksocket_shutdown(qp->tq_so, SHUT_RDWR, CRED());
	}

	mutex_enter(&qp->tq_rx_lock);
	qp->tq_rx_shutdown = B_TRUE;
	cv_signal(&qp->tq_rx_cv);
	mutex_exit(&qp->tq_rx_lock);
	if (qp->tq_rx_taskq != NULL)
		taskq_wait(qp->tq_rx_taskq);

	/* The rx worker has exited; free any partial PDU it left buffered. */
	mutex_enter(&qp->tq_rx_lock);
	freemsg(qp->tq_rx_mp);
	qp->tq_rx_mp = NULL;
	qp->tq_rx_have_header = B_FALSE;
	mutex_exit(&qp->tq_rx_lock);

	/*
	 * Stop the TX worker and wait for it to drain off the taskq before we
	 * touch the tx queues it consumes.
	 */
	mutex_enter(&qp->tq_tx_lock);
	qp->tq_tx_shutdown = B_TRUE;
	cv_signal(&qp->tq_tx_cv);
	mutex_exit(&qp->tq_tx_lock);
	if (qp->tq_tx_taskq != NULL)
		taskq_wait(qp->tq_tx_taskq);

	/* Drain any PDUs that were queued but never sent. */
	mutex_enter(&qp->tq_tx_lock);
	while ((pmp = qp->tq_tx_pdus_head) != NULL) {
		qp->tq_tx_pdus_head = pmp->b_next;
		pmp->b_next = NULL;
		freemsg(pmp);
	}
	qp->tq_tx_pdus_tail = NULL;
	mutex_exit(&qp->tq_tx_lock);

	while ((tc = list_remove_head(&qp->tq_tx_capsules)) != NULL) {
		nvmf_abort_capsule_data(&tc->nc, ECONNABORTED);
		tcp_release_capsule(tc);
	}

	if (qp->tq_open_ttags != NULL) {
		for (uint_t i = 0; i < qp->tq_num_ttags; i++) {
			cb = qp->tq_open_ttags[i];
			if (cb != NULL) {
				cb->tcb_tc->tc_active_r2ts--;
				cb->tcb_error = ECONNABORTED;
				tcp_release_command_buffer(cb);
			}
		}
	}

	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
	while ((cb = list_remove_head(&qp->tq_rx_buffers.tcbl_list)) != NULL) {
		mutex_exit(&qp->tq_rx_buffers.tcbl_lock);
		/*
		 * A buffer queued here by tcp_receive_r2t_data() bumped its
		 * capsule's pending_r2ts (DEBUG-only accounting); undo that
		 * before releasing so tcp_release_capsule()'s assert holds.
		 * (FreeBSD nvmf_tcp.c tcp_free_qpair).
		 */
#ifdef DEBUG
		if (cb->tcb_tc != NULL)
			cb->tcb_tc->tc_pending_r2ts--;
#endif
		cb->tcb_error = ECONNABORTED;
		tcp_release_command_buffer(cb);
		mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
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

	if (qp->tq_so != NULL)
		(void) ksocket_close(qp->tq_so, CRED());

	tcp_release_qpair(qp);
}

/* ARGSUSED */
static uint32_t
tcp_max_ioccsz(struct nvmf_qpair *nq)
{
	return (0);
}

/* ARGSUSED */
static uint64_t
tcp_max_xfer_size(struct nvmf_qpair *nq)
{
	return (0);
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

	nvmf_tcp_refcount_acquire(&tc->tc_refs);
	mutex_enter(&qp->tq_tx_lock);
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

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	switch (sgl_type) {
	case NVME_SGL_TYPE_ICD:
		if (tc->tc_rx_pdu.rp_data_len != LE_32(sgl->sgl_len)) {
			cmn_err(CE_WARN, "NVMe/TCP: Command Capsule with "
			    "mismatched ICD length");
			return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
		}
		break;
	case NVME_SGL_TYPE_COMMAND_BUFFER:
		if (tc->tc_rx_pdu.rp_data_len != 0) {
			cmn_err(CE_WARN,
			    "NVMe/TCP: Command Buffer SGL with ICD");
			return (NVME_CQE_SC_GEN_INV_FLD);
		}
		break;
	default:
		cmn_err(CE_WARN,
		    "NVMe/TCP: Invalid SGL type in Command Capsule");
		return (NVME_CQE_SC_GEN_INV_SGL_DESC);
	}

	if (sgl->sgl_addr != 0) {
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

/*
 * Controller-side receive of command data (e.g. for a WRITE): either copy
 * in-capsule data already present in the received PDU, or kick off an R2T
 * to pull the data from the host.
 */
static void
tcp_receive_r2t_data(struct nvmf_capsule *nc, uint32_t data_offset,
    struct nvmf_io_request *io)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_capsule_t *tc = TCAP(nc);
	nvmf_tcp_command_buffer_t *cb;

	cb = tcp_alloc_command_buffer(qp, io, data_offset, io->io_len,
	    nc->nc_sqe.sqe_cid);

	cb->tcb_tc = tc;
	nvmf_tcp_refcount_acquire(&tc->tc_refs);

	/*
	 * If this command has too many active R2Ts or there are no available
	 * transfer tags, queue the request for later.  NB: maxr2t is 0's based.
	 */
	mutex_enter(&qp->tq_rx_buffers.tcbl_lock);
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
	nvme_sgl_t *sgl;
	size_t data_len;
	uint8_t sgl_type;

	if (nc->nc_qe_len != sizeof (nvme_sqe_t) ||
	    !nc->nc_qpair->nq_controller)
		return (EINVAL);

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	data_len = LE_32(sgl->sgl_len);
	if (data_offset + io->io_len > data_len)
		return (EFBIG);

	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	if (sgl_type == NVME_SGL_TYPE_ICD)
		tcp_receive_icd_data(nc, data_offset, io);
	else
		tcp_receive_r2t_data(nc, data_offset, io);
	return (0);
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
	if (last_pdu)
		c2h.ntc2c_common.ntcph_flags |= NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU;
	if (success)
		c2h.ntc2c_common.ntcph_flags |= NVMF_TCP_C2H_DATA_FLAGS_SUCCESS;
	c2h.ntc2c_cccid = cid;
	c2h.ntc2c_datao = LE_32(data_offset);
	c2h.ntc2c_datal = LE_32((uint32_t)len);

	top = nvmf_tcp_construct_pdu(qp, &c2h, sizeof (c2h), mp,
	    (uint32_t)len);
	if (top != NULL)
		nvmf_tcp_write_pdu(qp, top);
}

/*
 * Controller-side send of response data (e.g. for a READ), as one or more
 * C2H_DATA PDUs.  The mblk chain in 'mp' is consumed by this function even on
 * error, matching the contract in nvmf_send_controller_data().
 */
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
	if (data_offset + len > data_len) {
		freemsg(mp);
		return (NVME_CQE_SC_GEN_INV_FLD);
	}
	last_xfer = (data_offset + len == data_len);

	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	if (sgl_type != NVME_SGL_TYPE_COMMAND_BUFFER) {
		freemsg(mp);
		return (NVME_CQE_SC_GEN_INV_FLD);
	}

#ifdef DEBUG
	ASSERT3U(data_offset, ==, TCAP(nc)->tc_tx_data_offset);
#endif

	/*
	 * Emit one or more C2H_DATA PDUs of at most tq_max_tx_data bytes each,
	 * splitting the mblk chain at byte boundaries (FreeBSD walks the mbuf
	 * chain with m_split()).  LAST_PDU (and the implicit-SUCCESS flag, when
	 * tq_send_success is set) is set on the final chunk of the final
	 * transfer.
	 */
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

/*
 * The transport-ops vtable.  Shape is identical to FreeBSD's tcp_ops; the
 * data-movement entry points carry the mblk_t seam.
 */
static struct nvmf_transport_ops tcp_ops = {
	.allocate_qpair = tcp_allocate_qpair,
	.free_qpair = tcp_free_qpair,
	.max_ioccsz = tcp_max_ioccsz,
	.max_xfer_size = tcp_max_xfer_size,
	.allocate_capsule = tcp_allocate_capsule,
	.free_capsule = tcp_free_capsule,
	.transmit_capsule = tcp_transmit_capsule,
	.validate_command_capsule = tcp_validate_command_capsule,
	.capsule_data_len = tcp_capsule_data_len,
	.receive_controller_data = tcp_receive_controller_data,
	.send_controller_data = tcp_send_controller_data,
	.trtype = NVMF_TRTYPE_TCP,
	.priority = 0,
};

/*
 * Module linkage.  FreeBSD hangs registration off the NVMF_TRANSPORT() macro and
 * the kernel module event handler; illumos registers explicitly from _init() and
 * unregisters from _fini() against the transport core (see nvmf_transport.c).
 */
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

	/* Build the CRC-32C (Castagnoli) digest table used for HDGST/DDGST. */
	CRC32_INIT(nvmf_tcp_crc32c_table, NVMF_TCP_CRC32C_POLY);

	error = nvmf_transport_register(&tcp_ops);
	if (error != 0)
		return (error);

	error = mod_install(&nvmf_tcp_modlinkage);
	if (error != 0)
		(void) nvmf_transport_unregister(&tcp_ops);

	return (error);
}

int
_fini(void)
{
	int error;

	/*
	 * Refuse to unload while the transport core still has live qpairs from
	 * this provider; nvmf_transport_unregister() enforces that and returns
	 * EBUSY if so.  Check it before mod_remove() so we do not unlink the
	 * module while still registered.
	 */
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

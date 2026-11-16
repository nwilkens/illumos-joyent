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
 * NVMe/TCP data transfer: the command buffers of both sides, R2T and the
 * data PDUs, and the controller's data operations.
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

/* Is [off, off + len) inside [base, base + total)?  The sums can wrap. */
static boolean_t
nvmf_tcp_range_ok(uint32_t off, uint32_t len, uint32_t base, size_t total)
{
	return (off >= base && (uint64_t)off - base <= total &&
	    len <= total - ((uint64_t)off - base));
}

nvmf_tcp_command_buffer_t *
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

void
tcp_release_command_buffer(nvmf_tcp_command_buffer_t *cb)
{
	if (nvmf_tcp_refcount_release(&cb->tcb_refs))
		tcp_free_command_buffer(cb);
}

void
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

void
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
void
tcp_rx_busy_enter(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	ASSERT(MUTEX_HELD(&qp->tq_rx_buffers.tcbl_lock));
	nvmf_tcp_refcount_acquire(&tc->tc_refs);
	tc->tc_rx_busy++;
}

void
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
mblk_t *
nvmf_tcp_command_buffer_mblk(nvmf_tcp_command_buffer_t *cb,
    uint32_t data_offset, uint32_t data_len)
{
	mblk_t *mp;

	mp = nvmf_tcp_allocb(data_len);
	nvmf_memdesc_copyout(&cb->tcb_io.io_mem, data_offset, mp->b_rptr,
	    data_len);
	return (mp);
}

int
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

int
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

int
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

uint8_t
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

size_t
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

int
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
void
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
uint_t
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

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
 * The connection of an NVMe/TCP qpair: the socket that the daemon handed
 * over, the rx worker that reads and dispatches PDUs, and the tx worker.
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

/* How long to wait for the peer to close after a termination request. */
static uint_t nvmf_tcp_term_wait_secs = 30;

/* Queue a PDU for the tx worker; b_next links the queue. */
void
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
void
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
void
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

/*
 * Take a hold on the open file behind the daemon's connected TCP socket and
 * mark the socket as ours.  The hold keeps the socket whether or not the
 * daemon closes its descriptor, and closef() gives it back without waiting.
 */
int
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

void
tcp_release_socket(nvmf_tcp_qpair_t *qp)
{
	struct sonode *so = (struct sonode *)(uintptr_t)qp->tq_so;

	mutex_enter(&so->so_lock);
	so->so_mode &= ~SM_KERNEL;
	mutex_exit(&so->so_lock);
	(void) closef(qp->tq_fp);
}

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

#include "nvmf_tcp_impl.h"

/* Largest data payload of one transmitted PDU. */
static uint_t nvmf_tcp_max_transmit_data = 256 * 1024;

/* Largest in-capsule data a controller accepts; IOCCSZ follows from it. */
static uint_t nvmf_tcp_max_icd = 16 * 1024;

/* Largest transfer of one host command; it bounds a received C2H PDU. */
static uint_t nvmf_tcp_max_io_size = 1024 * 1024;

/* How long nvmf_free_qpair() lets queued PDUs reach the socket. */
static uint_t nvmf_tcp_drain_secs = 2;

#define	NVMF_TCP_MIN_MAXH2CDATA	4096
#define	NVMF_TCP_MIN_PDA	4
#define	NVMF_TCP_MAX_PDA	128

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

mblk_t *
capsule_to_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc)
{
	if (tc->nc.nc_qe_len == sizeof (nvme_sqe_t))
		return (tcp_command_pdu(qp, tc));
	else
		return (tcp_response_pdu(qp, tc));
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
	    nvlist_lookup_uint64(nvl, "qsize", &qsize) != 0 ||
	    nvlist_lookup_boolean_value(nvl, "sq_flow_control", &sqfc) != 0)
		return (NULL);

	/* A controller sets its own limit; a host has the controller's. */
	max_icd = 0;
	if (!controller && nvlist_lookup_uint64(nvl, "max_icd", &max_icd) != 0)
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

void
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

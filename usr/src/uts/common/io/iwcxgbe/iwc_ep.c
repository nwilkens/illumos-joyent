/*
 * Copyright (c) 2009-2014 Chelsio, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */


/*
 * Endpoints of the iWARP connection manager: their lifetime, the TCP
 * options and MPA frames they send, and how a connection ends.  Follows
 * Linux cxgb4/cm.c under the OpenIB license.
 *
 * An endpoint (iwc_ep_t) is counted: each TID binding, the framework's use
 * of it and each queued CPL hold a reference.  ep_lock serializes its state
 * and is held across the operations that wait for the firmware, as Linux
 * holds the endpoint mutex.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/strsun.h>
#include <sys/atomic.h>
#include <sys/ethernet.h>
#include <sys/vlan.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "iwc.h"
#include "common/t4_msg.h"

#define	IWC_TX_CHUNK		240

void
iwc_ep_hold(iwc_ep_t *ep)
{
	atomic_inc_32(&ep->ep_refs);
}

void
iwc_ep_rele(iwc_ep_t *ep)
{
	iwc_t *iwc = ep->ep_iwc;

	if (atomic_dec_32_nv(&ep->ep_refs) != 0)
		return;
	mutex_enter(&iwc->iwc_ep_lock);
	list_remove(&iwc->iwc_eps, ep);
	cv_broadcast(&iwc->iwc_ep_cv);
	mutex_exit(&iwc->iwc_ep_lock);
	if (ep->ep_parent != NULL)
		iwc_ep_rele(ep->ep_parent);
	cv_destroy(&ep->ep_cv);
	mutex_destroy(&ep->ep_lock);
	kmem_free(ep, sizeof (*ep));
}

iwc_ep_t *
iwc_ep_alloc(iwc_t *iwc, iwc_dev_t *dev)
{
	iwc_ep_t *ep = kmem_zalloc(sizeof (*ep), KM_SLEEP);

	ep->ep_iwc = iwc;
	ep->ep_dev = dev;
	ep->ep_port = dev->d_port;
	ep->ep_refs = 1;
	ep->ep_atid = ep->ep_stid = ep->ep_tid = T4_RDMA_TID_NONE;
	ep->ep_l2t = T4_RDMA_TID_NONE;
	mutex_init(&ep->ep_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ep->ep_cv, NULL, CV_DRIVER, NULL);
	mutex_enter(&iwc->iwc_ep_lock);
	list_insert_tail(&iwc->iwc_eps, ep);
	mutex_exit(&iwc->iwc_ep_lock);
	return (ep);
}

void
iwc_ep_deadline(iwc_ep_t *ep, uint32_t ms)
{
	ASSERT(MUTEX_HELD(&ep->ep_lock));
	ep->ep_deadline = ms == 0 ? 0 : gethrtime() + MSEC2NSEC(ms);
}

int
iwc_cpl_errno(uint_t status)
{
	switch (status) {
	case CPL_ERR_CONN_RESET:
		return (ECONNREFUSED);
	case CPL_ERR_ARP_MISS:
		return (EHOSTUNREACH);
	case CPL_ERR_CONN_TIMEDOUT:
		return (ETIMEDOUT);
	case CPL_ERR_TCAM_FULL:
		return (ENOMEM);
	case CPL_ERR_CONN_EXIST:
		return (EADDRINUSE);
	default:
		return (EIO);
	}
}

static uint8_t
iwc_mtu_idx(iwc_t *iwc, uint32_t mtu)
{
	uint8_t i = T4_RDMA_NMTUS - 1;

	while (i > 0 && iwc->iwc_info.tri_mtus[i] > mtu)
		i--;
	return (i);
}

uint32_t
iwc_path_mtu(iwc_ep_t *ep, uint32_t route_mtu)
{
	const t4_rdma_port_t *p = &ep->ep_iwc->iwc_info.tri_port[ep->ep_port];
	uint32_t mtu = p->trpo_mtu != 0 ? p->trpo_mtu : 1500;

	if (route_mtu != 0)
		mtu = MIN(mtu, route_mtu);
	return (mtu);
}

/*
 * TCP settings of new connections; iwcxgbe.conf does not exist, so these
 * are for /etc/system.  The chip holds up to 1023 KB of receive window in
 * the connection's options.
 */
uint32_t iwc_rcv_win = IWC_RCV_WIN;
uint32_t iwc_snd_win = IWC_SND_WIN;
uint32_t iwc_cong = IWC_CONG;
uint32_t iwc_tstamps = 0;

void
iwc_tcp_opts(iwc_ep_t *ep, uint32_t mtu, t4_rdma_tcp_opts_t *o)
{
	bzero(o, sizeof (*o));
	o->trt_rcv_win = MAX(MIN(iwc_rcv_win, 1023 * 1024), 16 * 1024);
	o->trt_cong = (uint8_t)MIN(iwc_cong, CONG_ALG_HIGHSPEED);
	o->trt_timestamps = iwc_tstamps != 0;
	o->trt_mtu_idx = iwc_mtu_idx(ep->ep_iwc, mtu);
	o->trt_ulp_mode = ULP_MODE_TCPDDP;
	o->trt_p2p_iss = B_TRUE;
	ep->ep_mtu_idx = o->trt_mtu_idx;
}

/* The effective MSS from the options the chip negotiated (Linux set_emss). */
void
iwc_set_emss(iwc_ep_t *ep, uint16_t opt)
{
	const uint_t idx = G_TCPOPT_MSS(opt);
	int emss;

	emss = (int)ep->ep_iwc->iwc_info.tri_mtus[MIN(idx,
	    T4_RDMA_NMTUS - 1)] - (int)(sizeof (struct ip) +
	    sizeof (struct tcphdr));
	if (G_TCPOPT_TSTAMP(opt))
		emss -= 12;
	ep->ep_emss = (uint16_t)MAX(emss, 128);
	ep->ep_snd_wscale = (uint8_t)G_TCPOPT_SND_WSCALE(opt);
}

int
iwc_flowc(iwc_ep_t *ep)
{
	iwc_t *iwc = ep->ep_iwc;
	t4_rdma_flowc_t f;

	bzero(&f, sizeof (f));
	f.trf_snd_nxt = ep->ep_snd_seq;
	f.trf_rcv_nxt = ep->ep_rcv_seq;
	f.trf_sndbuf = MAX(MIN(iwc_snd_win, 16U << 20), 16 * 1024);
	f.trf_mss = ep->ep_emss;
	f.trf_rcv_scale = MIN(ep->ep_snd_wscale, 14);
	return (iwc->iwc_ops->tro_flowc(iwc->iwc_peer, ep->ep_tid, &f));
}

/* Send bytes on the connection before RDMA mode. */
static int
iwc_tx(iwc_ep_t *ep, const uint8_t *buf, size_t len)
{
	iwc_t *iwc = ep->ep_iwc;
	size_t n;
	int ret;

	while (len != 0) {
		n = MIN(len, IWC_TX_CHUNK);
		if ((ret = iwc->iwc_ops->tro_tx_data(iwc->iwc_peer, ep->ep_tid,
		    buf, n)) != 0)
			return (ret);
		ep->ep_snd_seq += (uint32_t)n;
		buf += n;
		len -= n;
	}
	return (0);
}

int
iwc_send_mpa(iwc_ep_t *ep, boolean_t reply, uint8_t flags, const void *pdata,
    uint16_t plen)
{
	uint8_t frame[IWC_MPA_MAX_FRAME];
	const boolean_t v2 = ep->ep_attr.ma_version == 2 &&
	    ep->ep_attr.ma_enhanced;
	size_t len;

	len = iwc_mpa_build(frame, sizeof (frame), reply,
	    flags | IWC_MPA_CRC, ep->ep_attr.ma_version, v2,
	    (uint16_t)ep->ep_ird, (uint16_t)ep->ep_ord,
	    ep->ep_attr.ma_p2p_type, pdata, plen);
	if (len == 0)
		return (EINVAL);
	return (iwc_tx(ep, frame, len));
}

/* Deliver a provider event; the final ones end the framework's use. */
void
iwc_ep_event(iwc_ep_t *ep, enum rdk_iw_event_type type, int status,
    const void *pdata, uint16_t plen)
{
	struct rdk_iw_cm_event ev;
	struct rdk_iw_cm_id *cmid = ep->ep_cmid;

	ASSERT(MUTEX_HELD(&ep->ep_lock));
	if (cmid == NULL || (ep->ep_flags & EPF_FINAL_SENT) != 0)
		return;
	bzero(&ev, sizeof (ev));
	ev.ev_type = type;
	ev.ev_status = status;
	ev.ev_pdata = pdata;
	ev.ev_pdata_len = plen;
	ev.ev_ird = ep->ep_ird;
	ev.ev_ord = ep->ep_ord;
	if (type == RDK_IW_EVENT_CLOSE || (type ==
	    RDK_IW_EVENT_CONNECT_REPLY && status != 0)) {
		ep->ep_flags |= EPF_FINAL_SENT;
		ep->ep_cmid = NULL;
	}
	(void) rdk_iw_cm_event(cmid, &ev);
}

/*
 * The connection is over: give the TID and L2T entry back, flush the QP
 * and send the final event.  ep_lock is held.
 */
void
iwc_ep_release(iwc_ep_t *ep, int status)
{
	iwc_t *iwc = ep->ep_iwc;
	iwc_qp_t *qp;
	boolean_t established;

	ASSERT(MUTEX_HELD(&ep->ep_lock));
	if ((ep->ep_flags & EPF_RELEASED) != 0)
		return;
	ep->ep_flags |= EPF_RELEASED;
	established = (ep->ep_flags & EPF_UP) != 0;
	ep->ep_state = IWC_EP_DEAD;
	ep->ep_deadline = 0;

	if ((qp = ep->ep_qp) != NULL) {
		ep->ep_qp = NULL;
		iwc_qp_error(qp, ep);
		iwc_qp_put(iwc, qp);
	}
	if ((ep->ep_flags & EPF_TID) != 0) {
		ep->ep_flags &= ~EPF_TID;
		(void) iwc->iwc_ops->tro_tid_release(iwc->iwc_peer,
		    ep->ep_tid);
		iwc_ep_rele(ep);
	}
	if ((ep->ep_flags & EPF_ATID) != 0) {
		ep->ep_flags &= ~EPF_ATID;
		iwc->iwc_ops->tro_atid_free(iwc->iwc_peer, ep->ep_atid);
		iwc_ep_rele(ep);
	}
	if (ep->ep_l2t != T4_RDMA_TID_NONE) {
		iwc->iwc_ops->tro_l2t_put(iwc->iwc_peer, ep->ep_l2t);
		ep->ep_l2t = T4_RDMA_TID_NONE;
	}
	if (ep->ep_admit != NULL) {
		rdk_iw_cm_unadmit(ep->ep_admit);
		ep->ep_admit = NULL;
	}
	if (ep->ep_cmid != NULL) {
		if (ep->ep_attr.ma_initiator && !established)
			iwc_ep_event(ep, RDK_IW_EVENT_CONNECT_REPLY,
			    status != 0 ? status : ECONNRESET, NULL, 0);
		else
			iwc_ep_event(ep, RDK_IW_EVENT_CLOSE, status, NULL, 0);
	}
	cv_broadcast(&ep->ep_cv);
}

/* Reset the connection.  ep_lock is held. */
void
iwc_ep_abort_locked(iwc_ep_t *ep, int status)
{
	iwc_t *iwc = ep->ep_iwc;

	ASSERT(MUTEX_HELD(&ep->ep_lock));
	if ((ep->ep_flags & EPF_RELEASED) != 0 ||
	    (ep->ep_flags & EPF_ABORT_SENT) != 0)
		return;
	if (ep->ep_status == 0)
		ep->ep_status = status;
	if (ep->ep_qp != NULL)
		iwc_qp_error(ep->ep_qp, ep);
	if ((ep->ep_flags & EPF_TID) == 0) {
		/* An active open without a TID yet: its reply ends it. */
		if (ep->ep_state == IWC_EP_CONNECTING) {
			ep->ep_flags |= EPF_ABORT_SENT;
			return;
		}
		iwc_ep_release(ep, status);
		return;
	}
	ep->ep_flags |= EPF_ABORT_SENT;
	ep->ep_state = IWC_EP_ABORTING;
	iwc_ep_deadline(ep, IWC_CLOSE_TIMEOUT_MS);
	if (iwc->iwc_ops->tro_abort(iwc->iwc_peer, ep->ep_tid, B_TRUE) != 0)
		iwc_ep_release(ep, status);
}

void
iwc_ep_abort(iwc_ep_t *ep, int status)
{
	mutex_enter(&ep->ep_lock);
	iwc_ep_abort_locked(ep, status);
	mutex_exit(&ep->ep_lock);
}

/*
 * Leave RDMA mode and send our FIN.  The QP is flushed; the connection is
 * released once both sides closed.  ep_lock is held.
 */
void
iwc_ep_close(iwc_ep_t *ep)
{
	iwc_t *iwc = ep->ep_iwc;
	iwc_qp_t *qp = ep->ep_qp;

	ASSERT(MUTEX_HELD(&ep->ep_lock));
	if ((ep->ep_flags & (EPF_CLOSE_SENT | EPF_RELEASED |
	    EPF_ABORT_SENT)) != 0)
		return;
	if (qp != NULL && iwc_qp_close(qp, ep) != 0) {
		iwc_ep_abort_locked(ep, ECONNRESET);
		return;
	}
	ep->ep_state = IWC_EP_CLOSING;
	ep->ep_flags |= EPF_CLOSE_SENT;
	iwc_ep_deadline(ep, IWC_CLOSE_TIMEOUT_MS);
	if (iwc->iwc_ops->tro_close_con(iwc->iwc_peer, ep->ep_tid) != 0)
		iwc_ep_abort_locked(ep, ECONNRESET);
}

/*
 * An error the chip found on a connection in RDMA mode: tell the peer with
 * a TERMINATE, then close.  The FIN follows the TERMINATE on the
 * connection.  Without RDMA mode, or if the TERMINATE cannot be queued,
 * the connection is aborted.
 */
void
iwc_ep_terminate(iwc_ep_t *ep, uint8_t layer, uint8_t ecode)
{
	iwc_t *iwc = ep->ep_iwc;
	iwc_qp_t *qp;

	mutex_enter(&ep->ep_lock);
	qp = ep->ep_qp;
	if (ep->ep_state != IWC_EP_FPDU || qp == NULL ||
	    (ep->ep_flags & (EPF_CLOSE_SENT | EPF_RELEASED |
	    EPF_ABORT_SENT)) != 0 ||
	    iwc->iwc_ops->tro_ri_terminate(iwc->iwc_peer, ep->ep_tid,
	    qp->qp_wq.sq.qid, layer, ecode) != 0) {
		iwc_ep_abort_locked(ep, ECONNRESET);
		mutex_exit(&ep->ep_lock);
		return;
	}
	IWC_STAT(iwc, is_term_sent);
	if ((ep->ep_flags & EPF_DISC_SENT) == 0) {
		ep->ep_flags |= EPF_DISC_SENT;
		iwc_ep_event(ep, RDK_IW_EVENT_DISCONNECT, ECONNRESET, NULL, 0);
	}
	iwc_ep_close(ep);
	mutex_exit(&ep->ep_lock);
}

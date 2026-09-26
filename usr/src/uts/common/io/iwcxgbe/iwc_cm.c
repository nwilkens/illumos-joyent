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
 * The iWARP connection manager of the provider: the CPLs of active and
 * passive opens over the t4nex offload core, the MPA exchange, the move to
 * RDMA mode and the close.  The endpoint state machine follows Linux
 * cxgb4/cm.c under the OpenIB license; the host socket, route and
 * neighbour code of Linux is replaced by the rdmak connection manager and
 * the t4nex operations.  CPLs arrive in interrupt context and are handled
 * one at a time on the CM taskq.
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

/* CPLs the taskq may have waiting; more are dropped. */
#define	IWC_CM_QMAX		8192

typedef struct iwc_cmq {
	struct iwc_cmq	*cm_next;
	uint8_t		cm_opcode;
	uint8_t		cm_port;
	t4_rdma_queue_t	cm_queue;
	uint32_t	cm_tid;
	uint32_t	cm_ltid;
	iwc_ep_t	*cm_ep;		/* held, or NULL */
	mblk_t		*cm_mp;
} iwc_cmq_t;

/*
 * The active side's connection is up: send the MPA request.
 */
static void
iwc_act_establish(iwc_ep_t *ep, const t4_rdma_cpl_t *cpl, mblk_t *mp)
{
	iwc_t *iwc = ep->ep_iwc;
	const struct cpl_act_establish *req = (const void *)mp->b_rptr;
	int ret;

	mutex_enter(&ep->ep_lock);
	if (ep->ep_state != IWC_EP_CONNECTING ||
	    (ep->ep_flags & EPF_TID) != 0) {
		mutex_exit(&ep->ep_lock);
		(void) iwc->iwc_ops->tro_abort(iwc->iwc_peer, cpl->trc_tid,
		    B_TRUE);
		return;
	}
	if ((ret = iwc->iwc_ops->tro_tid_bind(iwc->iwc_peer, cpl->trc_tid,
	    ep)) != 0) {
		(void) iwc->iwc_ops->tro_abort(iwc->iwc_peer, cpl->trc_tid,
		    B_TRUE);
		iwc_ep_release(ep, ret);
		mutex_exit(&ep->ep_lock);
		return;
	}
	iwc_ep_hold(ep);
	ep->ep_tid = cpl->trc_tid;
	ep->ep_flags |= EPF_TID | EPF_ESTABLISHED;
	if ((ep->ep_flags & EPF_ATID) != 0) {
		ep->ep_flags &= ~EPF_ATID;
		iwc->iwc_ops->tro_atid_free(iwc->iwc_peer, ep->ep_atid);
		iwc_ep_rele(ep);
	}
	ep->ep_snd_seq = BE_32(req->snd_isn);
	ep->ep_rcv_seq = BE_32(req->rcv_isn);
	iwc_set_emss(ep, BE_16(req->tcp_opt));
	if ((ep->ep_flags & EPF_ABORT_SENT) != 0) {
		ep->ep_flags &= ~EPF_ABORT_SENT;
		iwc_ep_abort_locked(ep, ep->ep_status);
		mutex_exit(&ep->ep_lock);
		return;
	}
	if ((ret = iwc_flowc(ep)) != 0 ||
	    (ret = iwc_send_mpa(ep, B_FALSE, 0, ep->ep_pdata,
	    ep->ep_pdata_len)) != 0) {
		iwc_ep_abort_locked(ep, ret);
		mutex_exit(&ep->ep_lock);
		return;
	}
	ep->ep_state = IWC_EP_MPA_REQ_SENT;
	iwc_ep_deadline(ep, IWC_MPA_TIMEOUT_MS);
	mutex_exit(&ep->ep_lock);
}

static void
iwc_act_open_rpl(iwc_ep_t *ep, mblk_t *mp)
{
	const struct cpl_act_open_rpl *rpl = (const void *)mp->b_rptr;
	const uint_t status = G_AOPEN_STATUS(BE_32(rpl->atid_status));

	mutex_enter(&ep->ep_lock);
	if (ep->ep_state == IWC_EP_CONNECTING)
		iwc_ep_release(ep, iwc_cpl_errno(status));
	mutex_exit(&ep->ep_lock);
}

/*
 * The MPA reply arrived in full: negotiate and enter RDMA mode.  ep_lock
 * is held.
 */
static void
iwc_mpa_reply(iwc_ep_t *ep, const iwc_mpa_msg_t *m)
{
	iwc_t *iwc = ep->ep_iwc;
	const uint32_t max = MIN(iwc->iwc_info.tri_vres.trv_max_ordird_qp,
	    IWC_MAX_ORDIRD);
	uint8_t pdata[IWC_MPA_MAX_PDATA];
	uint16_t plen = m->mm_pdata_len;
	int ret;

	bcopy(m->mm_pdata, pdata, plen);
	ep->ep_deadline = 0;
	if ((m->mm_flags & IWC_MPA_REJECT) != 0) {
		iwc_ep_event(ep, RDK_IW_EVENT_CONNECT_REPLY, ECONNREFUSED,
		    pdata, plen);
		iwc_ep_abort_locked(ep, ECONNREFUSED);
		return;
	}
	if ((m->mm_flags & IWC_MPA_MARKERS) != 0) {
		IWC_STAT(iwc, is_mpa_bad);
		iwc_ep_abort_locked(ep, EPROTO);
		return;
	}
	ep->ep_attr.ma_crc = B_TRUE;
	ep->ep_attr.ma_version = m->mm_rev;
	ep->ep_attr.ma_enhanced = m->mm_v2;
	if (m->mm_v2) {
		/* Relaxed IRD negotiation, as Linux does. */
		if (ep->ep_ird < m->mm_ord) {
			if (m->mm_ord > max) {
				iwc_ep_abort_locked(ep, ENOMEM);
				return;
			}
			ep->ep_ird = m->mm_ord;
		} else if (ep->ep_ird > m->mm_ord) {
			ep->ep_ird = m->mm_ord;
		}
		if (ep->ep_ord > m->mm_ird)
			ep->ep_ord = m->mm_ird;
		if (!m->mm_p2p || m->mm_rtr != IWC_P2P_READ) {
			IWC_STAT(iwc, is_mpa_bad);
			iwc_ep_abort_locked(ep, EPROTO);
			return;
		}
		ep->ep_attr.ma_p2p_type = IWC_P2P_READ;
	} else {
		ep->ep_attr.ma_p2p_type = IWC_P2P_READ;
		ep->ep_ird = ep->ep_ord = MIN(max, 1);
	}
	if (ep->ep_qp == NULL || (ret = iwc_qp_rts(ep->ep_qp, ep)) != 0) {
		iwc_ep_abort_locked(ep, EIO);
		return;
	}
	ep->ep_state = IWC_EP_FPDU;
	ep->ep_flags |= EPF_UP;
	IWC_STAT(iwc, is_conn_est);
	iwc_ep_event(ep, RDK_IW_EVENT_CONNECT_REPLY, 0, pdata, plen);
}

/*
 * The MPA request arrived in full: offer it to the listener's consumer.
 * ep_lock is held.
 */
static void
iwc_mpa_request(iwc_ep_t *ep, const iwc_mpa_msg_t *m)
{
	iwc_t *iwc = ep->ep_iwc;
	iwc_ep_t *lep = ep->ep_parent;
	const uint32_t max = MIN(iwc->iwc_info.tri_vres.trv_max_ordird_qp,
	    IWC_MAX_ORDIRD);
	struct rdk_iw_cm_event ev;
	int ret;

	ep->ep_deadline = 0;
	if ((m->mm_flags & (IWC_MPA_MARKERS | IWC_MPA_REJECT)) != 0) {
		IWC_STAT(iwc, is_mpa_bad);
		iwc_ep_abort_locked(ep, EPROTO);
		return;
	}
	ep->ep_attr.ma_initiator = B_FALSE;
	ep->ep_attr.ma_crc = B_TRUE;
	ep->ep_attr.ma_version = m->mm_rev;
	ep->ep_attr.ma_enhanced = m->mm_v2;
	ep->ep_attr.ma_p2p_type = IWC_P2P_DISABLED;
	if (m->mm_v2) {
		ep->ep_ird = MIN(m->mm_ird, max);
		ep->ep_ord = MIN(m->mm_ord, max);
		if (m->mm_p2p)
			ep->ep_attr.ma_p2p_type = m->mm_rtr;
	} else {
		ep->ep_ird = ep->ep_ord = max;
		ep->ep_attr.ma_p2p_type = IWC_P2P_READ;
	}
	ep->ep_pdata_len = m->mm_pdata_len;
	bcopy(m->mm_pdata, ep->ep_pdata, m->mm_pdata_len);
	ep->ep_state = IWC_EP_MPA_REQ_RCVD;
	/* The consumer decides in its handler; this bounds a lost decision. */
	iwc_ep_deadline(ep, IWC_MPA_TIMEOUT_MS);

	bzero(&ev, sizeof (ev));
	ev.ev_type = RDK_IW_EVENT_CONNECT_REQUEST;
	ev.ev_pdata = ep->ep_pdata;
	ev.ev_pdata_len = ep->ep_pdata_len;
	ev.ev_ird = ep->ep_ird;
	ev.ev_ord = ep->ep_ord;
	ev.ev_laddr = ep->ep_laddr;
	ev.ev_raddr = ep->ep_raddr;
	ev.ev_port = 1;
	ev.ev_child = ep;
	ev.ev_admit = ep->ep_admit;

	mutex_enter(&lep->ep_lock);
	if (lep->ep_state != IWC_EP_LISTEN || lep->ep_cmid == NULL) {
		mutex_exit(&lep->ep_lock);
		iwc_ep_abort_locked(ep, ECONNREFUSED);
		return;
	}
	lep->ep_embryos++;
	ret = rdk_iw_cm_event(lep->ep_cmid, &ev);
	lep->ep_embryos--;
	cv_broadcast(&lep->ep_cv);
	mutex_exit(&lep->ep_lock);
	if (ret != 0) {
		iwc_ep_abort_locked(ep, ECONNREFUSED);
		return;
	}
	/* The framework owns the admission and holds the endpoint. */
	ep->ep_admit = NULL;
	ep->ep_flags |= EPF_REQ_SENT | EPF_CM_REF;
	iwc_ep_hold(ep);
}

static void
iwc_rx_data(iwc_ep_t *ep, mblk_t *mp)
{
	iwc_t *iwc = ep->ep_iwc;
	const struct cpl_rx_data *hdr;
	iwc_mpa_msg_t m;
	uint32_t dlen;
	int ret;

	if (!pullupmsg(mp, -1) || MBLKL(mp) < sizeof (*hdr))
		return;
	hdr = (const void *)mp->b_rptr;
	dlen = BE_16(hdr->len);
	if (MBLKL(mp) - sizeof (*hdr) < dlen) {
		IWC_STAT(iwc, is_mpa_bad);
		return;
	}
	mutex_enter(&ep->ep_lock);
	if ((ep->ep_flags & (EPF_RELEASED | EPF_ABORT_SENT)) != 0) {
		mutex_exit(&ep->ep_lock);
		return;
	}
	if (dlen != 0)
		(void) iwc->iwc_ops->tro_rx_credits(iwc->iwc_peer, ep->ep_tid,
		    dlen);
	ep->ep_rcv_seq += dlen;
	switch (ep->ep_state) {
	case IWC_EP_MPA_REQ_SENT:
	case IWC_EP_MPA_REQ_WAIT:
		ret = iwc_mpa_rx(&ep->ep_mpa, mp->b_rptr + sizeof (*hdr), dlen,
		    ep->ep_state == IWC_EP_MPA_REQ_SENT, &m);
		if (ret == EAGAIN)
			break;
		if (ret != 0) {
			IWC_STAT(iwc, is_mpa_bad);
			iwc_ep_abort_locked(ep, EPROTO);
			break;
		}
		if (ep->ep_state == IWC_EP_MPA_REQ_SENT)
			iwc_mpa_reply(ep, &m);
		else
			iwc_mpa_request(ep, &m);
		break;
	default:
		/* Data outside the MPA exchange ends the connection. */
		if (dlen != 0)
			iwc_ep_abort_locked(ep, EPROTO);
		break;
	}
	mutex_exit(&ep->ep_lock);
}

/*
 * A SYN for a listener.  The peer must be on the listener's allow-list
 * before the chip answers it.
 */
static void
iwc_pass_accept_req(iwc_ep_t *lep, const t4_rdma_cpl_t *cpl, mblk_t *mp)
{
	iwc_t *iwc = lep->ep_iwc;
	const struct cpl_pass_accept_req *req;
	const uint8_t *p;
	struct sockaddr_in peer, local;
	t4_rdma_accept_t a;
	uint32_t hl, eth, iph, tcph, mtu;
	const struct ip *ip;
	const struct tcphdr *th;
	uint8_t mac[ETHERADDRL];
	void *admit = NULL;
	iwc_ep_t *ep;
	int ret;

	if (!pullupmsg(mp, -1) || MBLKL(mp) < sizeof (*req))
		goto refuse;
	req = (const void *)mp->b_rptr;
	hl = BE_32(req->hdr_len);
	if (iwc->iwc_info.tri_chip >= CHELSIO_T6) {
		eth = G_T6_ETH_HDR_LEN(hl);
		iph = G_T6_IP_HDR_LEN(hl);
		tcph = G_T6_TCP_HDR_LEN(hl);
	} else {
		eth = G_ETH_HDR_LEN(hl);
		iph = G_IP_HDR_LEN(hl);
		tcph = G_TCP_HDR_LEN(hl);
	}
	/* Untagged IPv4 only for now. */
	if (eth != sizeof (struct ether_header) || iph < sizeof (struct ip) ||
	    tcph < sizeof (struct tcphdr) ||
	    MBLKL(mp) < sizeof (*req) + eth + iph + tcph)
		goto refuse;
	p = mp->b_rptr + sizeof (*req);
	bcopy(p + ETHERADDRL, mac, ETHERADDRL);
	ip = (const struct ip *)(p + eth);
	th = (const struct tcphdr *)(p + eth + iph);
	if (ip->ip_v != IPVERSION || (mac[0] & 0x01) != 0)
		goto refuse;
	bzero(&peer, sizeof (peer));
	peer.sin_family = AF_INET;
	peer.sin_addr = ip->ip_src;
	peer.sin_port = th->th_sport;
	bzero(&local, sizeof (local));
	local.sin_family = AF_INET;
	local.sin_addr = ip->ip_dst;
	local.sin_port = th->th_dport;

	mutex_enter(&lep->ep_lock);
	if (lep->ep_state != IWC_EP_LISTEN || lep->ep_cmid == NULL ||
	    cpl->trc_port != lep->ep_port ||
	    local.sin_addr.s_addr != lep->ep_laddr.sin_addr.s_addr ||
	    local.sin_port != lep->ep_laddr.sin_port ||
	    rdk_iw_cm_admit(lep->ep_cmid, &peer, &admit) != 0) {
		mutex_exit(&lep->ep_lock);
		IWC_STAT(iwc, is_syn_refused);
		goto refuse;
	}
	mtu = iwc_path_mtu(lep, lep->ep_cmid->iw_mtu);
	mutex_exit(&lep->ep_lock);

	ep = iwc_ep_alloc(iwc, lep->ep_dev);
	iwc_ep_hold(lep);
	ep->ep_parent = lep;
	ep->ep_admit = admit;
	ep->ep_laddr = local;
	ep->ep_raddr = peer;
	ep->ep_tid = cpl->trc_tid;
	mutex_enter(&ep->ep_lock);
	if ((ret = iwc->iwc_ops->tro_tid_bind(iwc->iwc_peer, cpl->trc_tid,
	    ep)) != 0) {
		ep->ep_tid = T4_RDMA_TID_NONE;
		(void) iwc->iwc_ops->tro_tid_release(iwc->iwc_peer,
		    cpl->trc_tid);
		iwc_ep_release(ep, ret);
		mutex_exit(&ep->ep_lock);
		iwc_ep_rele(ep);
		return;
	}
	iwc_ep_hold(ep);
	ep->ep_flags |= EPF_TID;
	if ((ret = iwc->iwc_ops->tro_l2t_get(iwc->iwc_peer, ep->ep_port,
	    CPL_L2T_VLAN_NONE, mac, &ep->ep_l2t)) != 0) {
		ep->ep_l2t = T4_RDMA_TID_NONE;
		iwc_ep_release(ep, ret);
		mutex_exit(&ep->ep_lock);
		iwc_ep_rele(ep);
		return;
	}
	bzero(&a, sizeof (a));
	a.trac_tid = ep->ep_tid;
	a.trac_port = ep->ep_port;
	a.trac_l2t = ep->ep_l2t;
	iwc_tcp_opts(ep, mtu, &a.trac_opts);
	if ((ret = iwc->iwc_ops->tro_accept(iwc->iwc_peer, &a)) != 0) {
		iwc_ep_release(ep, ret);
		mutex_exit(&ep->ep_lock);
		iwc_ep_rele(ep);
		return;
	}
	ep->ep_state = IWC_EP_ACCEPTING;
	iwc_ep_deadline(ep, IWC_MPA_TIMEOUT_MS);
	mutex_exit(&ep->ep_lock);
	iwc_ep_rele(ep);
	return;
refuse:
	(void) iwc->iwc_ops->tro_tid_release(iwc->iwc_peer, cpl->trc_tid);
}

static void
iwc_pass_establish(iwc_ep_t *ep, mblk_t *mp)
{
	const struct cpl_pass_establish *req = (const void *)mp->b_rptr;
	int ret;

	mutex_enter(&ep->ep_lock);
	if (ep->ep_state != IWC_EP_ACCEPTING) {
		mutex_exit(&ep->ep_lock);
		return;
	}
	ep->ep_flags |= EPF_ESTABLISHED;
	ep->ep_snd_seq = BE_32(req->snd_isn);
	ep->ep_rcv_seq = BE_32(req->rcv_isn);
	iwc_set_emss(ep, BE_16(req->tcp_opt));
	if ((ret = iwc_flowc(ep)) != 0) {
		iwc_ep_abort_locked(ep, ret);
		mutex_exit(&ep->ep_lock);
		return;
	}
	ep->ep_state = IWC_EP_MPA_REQ_WAIT;
	iwc_ep_deadline(ep, IWC_MPA_TIMEOUT_MS);
	mutex_exit(&ep->ep_lock);
}

static void
iwc_peer_close(iwc_ep_t *ep)
{
	mutex_enter(&ep->ep_lock);
	if ((ep->ep_flags & (EPF_RELEASED | EPF_PEER_CLOSED)) != 0) {
		mutex_exit(&ep->ep_lock);
		return;
	}
	ep->ep_flags |= EPF_PEER_CLOSED;
	switch (ep->ep_state) {
	case IWC_EP_FPDU:
		if ((ep->ep_flags & EPF_DISC_SENT) == 0) {
			ep->ep_flags |= EPF_DISC_SENT;
			iwc_ep_event(ep, RDK_IW_EVENT_DISCONNECT, 0, NULL, 0);
		}
		iwc_ep_close(ep);
		break;
	case IWC_EP_CLOSING:
		if ((ep->ep_flags & EPF_CLOSE_ACKED) != 0)
			iwc_ep_release(ep, 0);
		break;
	case IWC_EP_ABORTING:
		break;
	default:
		/* A FIN before RDMA mode: the peer gave up. */
		iwc_ep_abort_locked(ep, ECONNRESET);
		break;
	}
	mutex_exit(&ep->ep_lock);
}

static void
iwc_close_con_rpl(iwc_ep_t *ep)
{
	mutex_enter(&ep->ep_lock);
	ep->ep_flags |= EPF_CLOSE_ACKED;
	if (ep->ep_state == IWC_EP_CLOSING &&
	    (ep->ep_flags & EPF_PEER_CLOSED) != 0)
		iwc_ep_release(ep, 0);
	mutex_exit(&ep->ep_lock);
}

static void
iwc_abort_req(iwc_ep_t *ep, mblk_t *mp)
{
	iwc_t *iwc = ep->ep_iwc;
	const struct cpl_abort_req_rss *req = (const void *)mp->b_rptr;

	if (req->status == CPL_ERR_RTX_NEG_ADVICE ||
	    req->status == CPL_ERR_PERSIST_NEG_ADVICE ||
	    req->status == CPL_ERR_KEEPALV_NEG_ADVICE)
		return;
	mutex_enter(&ep->ep_lock);
	IWC_STAT(iwc, is_conn_abort);
	(void) iwc->iwc_ops->tro_abort_rpl(iwc->iwc_peer, ep->ep_tid,
	    B_FALSE);
	/* Our own abort crossed the peer's: its reply releases. */
	if ((ep->ep_flags & EPF_ABORT_SENT) == 0) {
		if (ep->ep_state == IWC_EP_FPDU &&
		    (ep->ep_flags & EPF_DISC_SENT) == 0) {
			ep->ep_flags |= EPF_DISC_SENT;
			iwc_ep_event(ep, RDK_IW_EVENT_DISCONNECT, ECONNRESET,
			    NULL, 0);
		}
		iwc_ep_release(ep, ECONNRESET);
	}
	mutex_exit(&ep->ep_lock);
}

static void
iwc_abort_rpl(iwc_ep_t *ep)
{
	mutex_enter(&ep->ep_lock);
	if ((ep->ep_flags & EPF_ABORT_SENT) != 0) {
		iwc_ep_release(ep, ep->ep_status != 0 ? ep->ep_status :
		    ECONNRESET);
	}
	mutex_exit(&ep->ep_lock);
}

static void
iwc_stid_rpl(iwc_ep_t *lep, uint8_t opcode, mblk_t *mp)
{
	const uint8_t status = opcode == CPL_PASS_OPEN_RPL ?
	    ((const struct cpl_pass_open_rpl *)mp->b_rptr)->status :
	    ((const struct cpl_close_listsvr_rpl *)mp->b_rptr)->status;

	mutex_enter(&lep->ep_lock);
	lep->ep_open_done = B_TRUE;
	lep->ep_open_status = status == CPL_ERR_NONE ? 0 : EADDRINUSE;
	cv_broadcast(&lep->ep_cv);
	mutex_exit(&lep->ep_lock);
}

static void
iwc_cm_dispatch(iwc_t *iwc, iwc_cmq_t *q)
{
	iwc_ep_t *ep = q->cm_ep;
	mblk_t *mp = q->cm_mp;
	t4_rdma_cpl_t cpl;

	bzero(&cpl, sizeof (cpl));
	cpl.trc_opcode = q->cm_opcode;
	cpl.trc_port = q->cm_port;
	cpl.trc_queue = q->cm_queue;
	cpl.trc_tid = q->cm_tid;
	cpl.trc_ltid = q->cm_ltid;

	if (q->cm_opcode == CPL_FW6_MSG) {
		const struct cpl_fw6_msg *m = (const void *)mp->b_rptr;

		if (MBLKL(mp) >= sizeof (*m) && m->type == FW6_TYPE_CQE)
			iwc_qp_async(iwc, (const t4_cqe_t *)&m->data[0]);
		return;
	}
	if (ep == NULL) {
		/* A connection CPL for a TID with no endpoint. */
		if (q->cm_opcode == CPL_ABORT_REQ_RSS) {
			(void) iwc->iwc_ops->tro_abort_rpl(iwc->iwc_peer,
			    q->cm_tid, B_FALSE);
			(void) iwc->iwc_ops->tro_tid_release(iwc->iwc_peer,
			    q->cm_tid);
		}
		IWC_STAT(iwc, is_cpl_drop);
		return;
	}
	switch (q->cm_opcode) {
	case CPL_PASS_OPEN_RPL:
	case CPL_CLOSE_LISTSRV_RPL:
		iwc_stid_rpl(ep, q->cm_opcode, mp);
		break;
	case CPL_PASS_ACCEPT_REQ:
		iwc_pass_accept_req(ep, &cpl, mp);
		break;
	case CPL_ACT_OPEN_RPL:
		iwc_act_open_rpl(ep, mp);
		break;
	case CPL_ACT_ESTABLISH:
		iwc_act_establish(ep, &cpl, mp);
		break;
	case CPL_PASS_ESTABLISH:
		iwc_pass_establish(ep, mp);
		break;
	case CPL_RX_DATA:
		iwc_rx_data(ep, mp);
		break;
	case CPL_PEER_CLOSE:
		iwc_peer_close(ep);
		break;
	case CPL_CLOSE_CON_RPL:
		iwc_close_con_rpl(ep);
		break;
	case CPL_ABORT_REQ_RSS:
		iwc_abort_req(ep, mp);
		break;
	case CPL_ABORT_RPL_RSS:
		iwc_abort_rpl(ep);
		break;
	case CPL_RDMA_TERMINATE:
		iwc_ep_abort(ep, ECONNRESET);
		break;
	default:
		break;
	}
}

/* Endpoints past their deadline.  CM taskq. */
static void
iwc_cm_timeouts(iwc_t *iwc)
{
	const hrtime_t now = gethrtime();
	iwc_ep_t *ep, *due[32];
	uint_t n = 0, i;

	mutex_enter(&iwc->iwc_ep_lock);
	for (ep = list_head(&iwc->iwc_eps); ep != NULL && n < 32;
	    ep = list_next(&iwc->iwc_eps, ep)) {
		if (ep->ep_deadline != 0 && ep->ep_deadline <= now) {
			iwc_ep_hold(ep);
			due[n++] = ep;
		}
	}
	mutex_exit(&iwc->iwc_ep_lock);
	for (i = 0; i < n; i++) {
		ep = due[i];
		mutex_enter(&ep->ep_lock);
		if (ep->ep_deadline != 0 && ep->ep_deadline <= now) {
			ep->ep_deadline = 0;
			if (ep->ep_state == IWC_EP_CONNECTING ||
			    ep->ep_state == IWC_EP_ABORTING ||
			    ep->ep_state == IWC_EP_CLOSING) {
				/* The chip never answered; give up. */
				if (ep->ep_state == IWC_EP_CLOSING)
					iwc_ep_abort_locked(ep, ETIMEDOUT);
				else
					iwc_ep_release(ep, ETIMEDOUT);
			} else {
				iwc_ep_abort_locked(ep, ETIMEDOUT);
			}
		}
		mutex_exit(&ep->ep_lock);
		iwc_ep_rele(ep);
	}
}

void
iwc_cm_task(void *arg)
{
	iwc_t *iwc = arg;
	iwc_cmq_t *q;
	iwc_ep_t *ep, *lost;
	boolean_t tick;

	for (;;) {
		mutex_enter(&iwc->iwc_cm_qlock);
		tick = iwc->iwc_tick_pending;
		iwc->iwc_tick_pending = B_FALSE;
		lost = iwc->iwc_cm_lost;
		iwc->iwc_cm_lost = NULL;
		for (ep = lost; ep != NULL; ep = ep->ep_lost_next)
			ep->ep_lost = B_FALSE;
		if ((q = iwc->iwc_cm_qhead) != NULL) {
			iwc->iwc_cm_qhead = q->cm_next;
			if (iwc->iwc_cm_qhead == NULL)
				iwc->iwc_cm_qtail = NULL;
			iwc->iwc_cm_qlen--;
		} else if (!tick && lost == NULL) {
			iwc->iwc_cm_queued = B_FALSE;
			mutex_exit(&iwc->iwc_cm_qlock);
			return;
		}
		mutex_exit(&iwc->iwc_cm_qlock);

		while ((ep = lost) != NULL) {
			lost = ep->ep_lost_next;
			iwc_ep_abort(ep, ECONNRESET);
			iwc_ep_rele(ep);
		}
		if (tick)
			iwc_cm_timeouts(iwc);
		if (q != NULL) {
			iwc_cm_dispatch(iwc, q);
			freemsg(q->cm_mp);
			if (q->cm_ep != NULL)
				iwc_ep_rele(q->cm_ep);
			kmem_free(q, sizeof (*q));
		}
	}
}

/* Make the task run.  iwc_cm_qlock is held. */
static void
iwc_cm_kick(iwc_t *iwc)
{
	ASSERT(MUTEX_HELD(&iwc->iwc_cm_qlock));
	if (!iwc->iwc_cm_queued) {
		iwc->iwc_cm_queued = B_TRUE;
		taskq_dispatch_ent(iwc->iwc_cm_tq, iwc_cm_task, iwc, 0,
		    &iwc->iwc_cm_ent);
	}
}

void
iwc_cm_tick(void *arg)
{
	iwc_t *iwc = arg;

	mutex_enter(&iwc->iwc_cm_qlock);
	if (iwc->iwc_tick_stop) {
		mutex_exit(&iwc->iwc_cm_qlock);
		return;
	}
	iwc->iwc_tick_pending = B_TRUE;
	iwc_cm_kick(iwc);
	iwc->iwc_tick = timeout(iwc_cm_tick, iwc, drv_usectohz(MICROSEC / 2));
	mutex_exit(&iwc->iwc_cm_qlock);
}

/*
 * Whether dropping this CPL loses the state of a connection.  Listener
 * replies time out on their own, and SYNs are refused where they drop.
 */
static boolean_t
iwc_cpl_lost(const t4_rdma_cpl_t *cpl)
{
	const mblk_t *mp = cpl->trc_mp;
	const struct cpl_abort_req_rss *req;

	if (cpl->trc_ctx == NULL)
		return (B_FALSE);
	switch (cpl->trc_opcode) {
	case CPL_PASS_OPEN_RPL:
	case CPL_CLOSE_LISTSRV_RPL:
	case CPL_PASS_ACCEPT_REQ:
		return (B_FALSE);
	case CPL_ABORT_REQ_RSS:
		if (mp == NULL || MBLKL(mp) < sizeof (*req))
			return (B_TRUE);
		req = (const void *)mp->b_rptr;
		return (req->status != CPL_ERR_RTX_NEG_ADVICE &&
		    req->status != CPL_ERR_PERSIST_NEG_ADVICE &&
		    req->status != CPL_ERR_KEEPALV_NEG_ADVICE);
	default:
		return (B_TRUE);
	}
}

/*
 * A CPL of ep was dropped: have the CM task abort the connection, since
 * no later message may come to end it.  No allocation, so this cannot fail.
 * iwc_cm_qlock is held.
 */
static void
iwc_cm_lose(iwc_t *iwc, iwc_ep_t *ep)
{
	ASSERT(MUTEX_HELD(&iwc->iwc_cm_qlock));
	IWC_STAT(iwc, is_cpl_lost);
	if (ep->ep_lost)
		return;
	ep->ep_lost = B_TRUE;
	iwc_ep_hold(ep);
	ep->ep_lost_next = iwc->iwc_cm_lost;
	iwc->iwc_cm_lost = ep;
	iwc_cm_kick(iwc);
}

/*
 * t4nex hands over a CPL, in interrupt context.  The context is an
 * endpoint while t4nex holds its ID, so a hold taken here is safe.
 */
void
iwc_cpl(void *arg, t4_rdma_cpl_t *cpl)
{
	iwc_t *iwc = arg;
	iwc_cmq_t *q;

	q = kmem_alloc(sizeof (*q), KM_NOSLEEP);
	mutex_enter(&iwc->iwc_cm_qlock);
	if (q == NULL || iwc->iwc_cm_closing ||
	    iwc->iwc_cm_qlen >= IWC_CM_QMAX) {
		if (!iwc->iwc_cm_closing && iwc_cpl_lost(cpl))
			iwc_cm_lose(iwc, cpl->trc_ctx);
		mutex_exit(&iwc->iwc_cm_qlock);
		IWC_STAT(iwc, is_cpl_drop);
		if (cpl->trc_opcode == CPL_PASS_ACCEPT_REQ)
			(void) iwc->iwc_ops->tro_tid_release(iwc->iwc_peer,
			    cpl->trc_tid);
		if (q != NULL)
			kmem_free(q, sizeof (*q));
		freemsg(cpl->trc_mp);
		return;
	}
	q->cm_next = NULL;
	q->cm_opcode = cpl->trc_opcode;
	q->cm_port = cpl->trc_port;
	q->cm_queue = cpl->trc_queue;
	q->cm_tid = cpl->trc_tid;
	q->cm_ltid = cpl->trc_ltid;
	q->cm_mp = cpl->trc_mp;
	q->cm_ep = cpl->trc_ctx;
	if (q->cm_ep != NULL)
		iwc_ep_hold(q->cm_ep);
	if (iwc->iwc_cm_qtail == NULL)
		iwc->iwc_cm_qhead = q;
	else
		iwc->iwc_cm_qtail->cm_next = q;
	iwc->iwc_cm_qtail = q;
	iwc->iwc_cm_qlen++;
	iwc_cm_kick(iwc);
	mutex_exit(&iwc->iwc_cm_qlock);
}

/*
 * Detach: stop the timer and the queue and drop what is left.  The
 * framework is gone and t4nex has swept the client's IDs.
 */
void
iwc_cm_fini(iwc_t *iwc)
{
	iwc_cmq_t *q;
	iwc_ep_t *ep;
	timeout_id_t tid;

	mutex_enter(&iwc->iwc_cm_qlock);
	iwc->iwc_tick_stop = B_TRUE;
	iwc->iwc_cm_closing = B_TRUE;
	tid = iwc->iwc_tick;
	iwc->iwc_tick = 0;
	mutex_exit(&iwc->iwc_cm_qlock);
	if (tid != 0)
		(void) untimeout(tid);
	if (iwc->iwc_cm_tq != NULL)
		taskq_wait(iwc->iwc_cm_tq);
	mutex_enter(&iwc->iwc_cm_qlock);
	while ((q = iwc->iwc_cm_qhead) != NULL) {
		iwc->iwc_cm_qhead = q->cm_next;
		freemsg(q->cm_mp);
		if (q->cm_ep != NULL)
			iwc_ep_rele(q->cm_ep);
		kmem_free(q, sizeof (*q));
	}
	iwc->iwc_cm_qtail = NULL;
	iwc->iwc_cm_qlen = 0;
	while ((ep = iwc->iwc_cm_lost) != NULL) {
		iwc->iwc_cm_lost = ep->ep_lost_next;
		ep->ep_lost = B_FALSE;
		iwc_ep_rele(ep);
	}
	mutex_exit(&iwc->iwc_cm_qlock);
}

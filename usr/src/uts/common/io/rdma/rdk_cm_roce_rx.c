// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2004-2007 Intel Corporation.  All rights reserved.
 * Copyright (c) 2004 Topspin Corporation.  All rights reserved.
 * Copyright (c) 2004, 2005 Voltaire Corporation.  All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2019, Mellanox Technologies inc.  All rights reserved.
 * Copyright (c) 2005 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2002-2005, Network Appliance, Inc. All rights reserved.
 * Copyright (c) 1999-2019, Mellanox Technologies, Inc. All rights reserved.
 * Copyright (c) 2005-2006 Intel Corporation.  All rights reserved.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * The IB CM messages the RoCE GSI agents receive.  Each one is remote
 * input: a message that is malformed, that does not come from the
 * addresses of the connection it names, or that carries IDs or a
 * transaction ID the connection does not have, is dropped without a word.
 *
 * A request is bound three ways: the IPv4 source and destination of the
 * packet, the GIDs in the REQ and the addresses in the RDMA CM header must
 * all agree, and the destination must be a GID of the port it came in on.
 * The passive side then finds its own way back to the requester (route and
 * neighbor) before the consumer sees the request.  Replies to messages no
 * connection owns are rate limited and never start a neighbor resolution.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <sys/zone.h>
#include <netinet/in.h>

#include "rdk_impl.h"
#include "rdk_cm_roce.h"

/* Requests being resolved at once, all ports together. */
uint_t rdk_cm_roce_max_resolving = 256;
/*
 * Bounds on the REP retransmissions a requester asks for: the interval,
 * which also bounds an MRA's wait, and all of them together (Linux
 * defaults take 4096 ms and 65.5 s).
 */
uint_t rdk_ibcm_passive_resp_max_ms = 8192;
uint_t rdk_ibcm_passive_max_ms = 70000;

static void rdk_ibconn_arp_hold(void *);
static void rdk_ibconn_arp_rele(void *);

static const rdk_cm_arp_ops_t rdk_ibconn_arp_ops = {
	.rao_done = rdk_cm_roce_resolve_done,
	.rao_hold = rdk_ibconn_arp_hold,
	.rao_rele = rdk_ibconn_arp_rele
};

static void
rdk_ibconn_arp_hold(void *arg)
{
	rdk_ibconn_hold(arg);
}

static void
rdk_ibconn_arp_rele(void *arg)
{
	rdk_ibconn_rele(arg);
}

/* The message came from the connection's peer to its local address. */
static boolean_t
rdk_ibconn_from(const rdk_ibconn_t *c, const rdk_gsi_rx_t *rx)
{
	return (rx->rx_gsi == c->ic_gsi && rx->rx_ip.ip_src == c->ic_rip &&
	    rx->rx_ip.ip_dst == c->ic_lip);
}

/*
 * A path back to the sender of a MAD, and the held local GID it was sent
 * to.  The reply goes to the frame's own source MAC, for the packet's
 * source IP, so that no reply depends on or starts a neighbor resolution;
 * the neighbor cache serves only when the device gave no source MAC.
 */
static int
rdk_cm_roce_back(rdk_gsi_t *g, const rdk_gsi_rx_t *rx, rdk_gsi_path_t *gp)
{
	rdk_cm_path_t p;
	rdk_gid_t gid;
	int ret;

	bzero(gp, sizeof (*gp));
	rdk_gid_from_ipv4(&gid, rx->rx_ip.ip_dst);
	if ((gp->gp_sgid = rdk_find_gid(g->rg_dev, g->rg_port, &gid,
	    RDK_VLAN_NONE)) == NULL)
		return (EADDRNOTAVAIL);
	if ((ret = rdk_cm_route_lookup(kcred, rx->rx_ip.ip_src,
	    rx->rx_ip.ip_dst, &p)) == 0) {
		if (p.cp_dev->rcd_dev != g->rg_dev || p.cp_port != g->rg_port)
			ret = ENETUNREACH;
		else if (p.cp_local)
			bcopy(p.cp_smac, gp->gp_dmac, ETHERADDRL);
		else if (rx->rx_has_smac)
			bcopy(rx->rx_smac, gp->gp_dmac, ETHERADDRL);
		else
			ret = rdk_cm_nexthop_lookup(GLOBAL_ZONEID,
			    p.cp_ifindex, p.cp_nexthop, gp->gp_dmac);
		gp->gp_hop = p.cp_ttl;
		rdk_cm_dev_rele(p.cp_dev);
	}
	if (ret != 0) {
		rdk_put_gid_attr(gp->gp_sgid);
		gp->gp_sgid = NULL;
		return (ret);
	}
	rdk_gid_from_ipv4(&gp->gp_dgid, rx->rx_ip.ip_src);
	return (0);
}

/*
 * Answer a message no connection takes: a REJ to a REQ, or a DREP to a
 * DREQ (Linux cm_issue_rej() and cm_issue_drep()).
 */
void
rdk_cm_roce_reply(rdk_gsi_t *g, const rdk_gsi_rx_t *rx,
    const rdk_ibcm_msg_t *m, uint16_t attr, uint16_t reason, uint8_t msg)
{
	uint8_t buf[IBCM_MAD_LEN];
	rdk_gsi_path_t gp;
	rdk_ibcm_msg_t r;

	if (!rdk_gsi_reply_ok(g) || rdk_cm_roce_back(g, rx, &gp) != 0)
		return;
	bzero(&r, sizeof (r));
	r.m_attr = attr;
	r.m_tid = m->m_tid;
	r.m_local_id = m->m_remote_id;
	r.m_remote_id = m->m_local_id;
	r.m_reason = reason;
	r.m_msg = msg;
	rdk_ibcm_build(buf, &r);
	(void) rdk_gsi_send(g, &gp, buf);
	rdk_put_gid_attr(gp.gp_sgid);
}

static void
rdk_ibconn_reject(rdk_ibconn_t *c, uint16_t reason)
{
	rdk_ibconn_in_t in;

	mutex_enter(&c->ic_lock);
	c->ic_pdata_len = 0;
	mutex_exit(&c->ic_lock);
	bzero(&in, sizeof (in));
	in.ci_in.ii_input = IBCI_REJECT;
	in.ci_in.ii_rej_reason = reason;
	(void) rdk_ibconn_step(c, &in);
}

/*
 * Passive resolution ends once, by its answer, its deadline or the end of
 * the connection: whichever clears ic_awaiting gives back the slot and the
 * resolver's owner reference.
 */
static boolean_t
rdk_ibconn_resolved(rdk_ibconn_t *c)
{
	rdk_cm_arp_t *arp;

	mutex_enter(&c->ic_lock);
	if (!c->ic_awaiting || c->ic_fsm.f_state != IBCS_REQ_RCVD) {
		mutex_exit(&c->ic_lock);
		return (B_FALSE);
	}
	c->ic_awaiting = B_FALSE;
	arp = c->ic_arp;
	c->ic_arp = NULL;
	mutex_exit(&c->ic_lock);
	rdk_cm_arp_cancel(arp);
	atomic_dec_uint(&rdk_cm_roce_resolving);
	return (B_TRUE);
}

/*
 * The way back to the requester is known: make the child ID and give the
 * consumer the request.
 */
static void
rdk_ibconn_offer(rdk_ibconn_t *c)
{
	rdk_cm_child_t cc;
	rdk_cm_id_t *listener, *child;
	uint8_t pdata[IBCM_CMA_REQ_PDATA];
	int ret;

	bzero(&cc, sizeof (cc));
	mutex_enter(&c->ic_lock);
	listener = c->ic_listener;
	c->ic_listener = NULL;
	cc.cc_laddr.sin_family = AF_INET;
	cc.cc_laddr.sin_addr.s_addr = c->ic_lip;
	cc.cc_laddr.sin_port = c->ic_lport;
	cc.cc_raddr.sin_family = AF_INET;
	cc.cc_raddr.sin_addr.s_addr = c->ic_rip;
	cc.cc_raddr.sin_port = c->ic_rport;
	cc.cc_port = c->ic_port;
	cc.cc_pdata_len = MIN(c->ic_pdata_len, sizeof (pdata));
	bcopy(c->ic_pdata, pdata, cc.cc_pdata_len);
	cc.cc_pdata = pdata;
	cc.cc_ird = c->ic_peer_resp_res;
	cc.cc_ord = c->ic_peer_init_depth;
	cc.cc_admit = c->ic_admit;
	cc.cc_dmac = c->ic_path.gp_dmac;
	cc.cc_mtu = (uint32_t)rdk_mtu_enum_to_int((enum rdk_mtu)c->ic_mtu) +
	    96;
	mutex_exit(&c->ic_lock);
	if (listener == NULL)
		return;

	rdk_ibconn_hold(c);
	cc.cc_conn = c;
	if ((ret = rdk_cm_child_new(listener, &cc, &child)) == 0) {
		mutex_enter(&c->ic_lock);
		c->ic_admit = NULL;
		mutex_exit(&c->ic_lock);
	} else {
		rdk_ibconn_rele(c);
		rdk_ibconn_reject(c, IBCM_REJ_CONSUMER_DEFINED);
	}
	rdk_cm_rele(listener);
}

void
rdk_cm_roce_resolve_done(void *arg, uint32_t gen, int err, const uint8_t *mac)
{
	rdk_ibconn_t *c = arg;

	_NOTE(ARGUNUSED(gen));
	if (!rdk_ibconn_resolved(c))
		return;
	rdk_ibconn_arm(c, 0);
	if (err != 0) {
		rdk_ibconn_reject(c, IBCM_REJ_INVALID_GID);
		return;
	}
	mutex_enter(&c->ic_lock);
	bcopy(mac, c->ic_path.gp_dmac, ETHERADDRL);
	mutex_exit(&c->ic_lock);
	rdk_ibconn_offer(c);
}

void
rdk_cm_roce_resolve_timeout(rdk_ibconn_t *c)
{
	if (rdk_ibconn_resolved(c))
		rdk_ibconn_reject(c, IBCM_REJ_CONSUMER_DEFINED);
}

/* Fill a passive connection from its REQ. */
static void
rdk_ibconn_from_req(rdk_ibconn_t *c, const rdk_ibcm_msg_t *m,
    const rdk_cma_hdr_t *h, uint16_t port)
{
	const struct rdk_device_attr *da = &c->ic_cd->rcd_dev->rd_attr;
	uint8_t life = m->m_ack_timeout > 0 ? m->m_ack_timeout - 1 : 0;
	uint32_t resp, tries;

	c->ic_rid = m->m_local_id;
	c->ic_rguid = m->m_ca_guid;
	c->ic_tid = m->m_tid;
	c->ic_rqpn = m->m_qpn;
	c->ic_rpsn = m->m_psn;
	c->ic_lip = h->ch_dst;
	c->ic_rip = h->ch_src;
	c->ic_lport = htons(port);
	c->ic_rport = h->ch_sport;
	c->ic_service_id = m->m_service_id;
	c->ic_mtu = m->m_mtu;
	c->ic_retry = m->m_retry;
	c->ic_rnr_retry = m->m_rnr_retry;
	c->ic_peer_resp_res = m->m_resp_res;
	c->ic_peer_init_depth = m->m_init_depth;
	/* The most the consumer may grant (Linux cm_req_handler()). */
	c->ic_resp_res = (uint8_t)MIN(m->m_init_depth,
	    MAX(da->max_qp_rd_atom, 0));
	c->ic_init_depth = (uint8_t)MIN(m->m_resp_res,
	    MAX(da->max_qp_init_rd_atom, 0));
	c->ic_flow = m->m_flow_label;
	c->ic_path.gp_tclass = m->m_tclass;
	rdk_gid_from_ipv4(&c->ic_path.gp_dgid, h->ch_src);
	c->ic_ack_timeout = rdk_ibcm_ack_timeout(da->local_ca_ack_delay, life);
	c->ic_pdata_len = IBCM_CMA_REQ_PDATA;
	bcopy(m->m_pdata + IBCM_CMA_HDR_LEN, c->ic_pdata, IBCM_CMA_REQ_PDATA);
	resp = MAX(MIN(rdk_ibcm_time_ms(m->m_local_resp_to),
	    rdk_ibcm_passive_resp_max_ms), 1);
	tries = MIN(m->m_max_retries, rdk_ibcm_passive_max_ms / resp);
	rdk_ibcm_fsm_init(&c->ic_fsm, B_FALSE,
	    (uint8_t)(tries > 0 ? tries - 1 : 0), resp,
	    rdk_ibcm_time_ms(c->ic_ack_timeout),
	    rdk_ibcm_time_ms(c->ic_ack_timeout));
	c->ic_fsm.f_mra_max_ms = rdk_ibcm_passive_resp_max_ms;
}

/*
 * Start finding the way back to the requester.  The REQ's MTU must fit
 * the local path.
 */
static void
rdk_ibconn_resolve(rdk_ibconn_t *c, rdk_cm_id_t *listener)
{
	rdk_cm_path_t p;
	rdk_cm_arp_t *arp = NULL;
	int ret;

	if ((ret = rdk_cm_route_lookup(listener->rci_cred, c->ic_rip,
	    c->ic_lip, &p)) != 0) {
		rdk_ibconn_reject(c, IBCM_REJ_INVALID_GID);
		return;
	}
	rdk_cm_dev_rele(p.cp_dev);
	if (p.cp_dev != c->ic_cd || p.cp_port != c->ic_port) {
		rdk_ibconn_reject(c, IBCM_REJ_INVALID_GID);
		return;
	}
	if (c->ic_mtu > rdk_cm_roce_mtu(c->ic_cd->rcd_dev, c->ic_port,
	    p.cp_mtu)) {
		rdk_ibconn_reject(c, IBCM_REJ_INVALID_MTU);
		return;
	}
	if (atomic_inc_uint_nv(&rdk_cm_roce_resolving) >
	    rdk_cm_roce_max_resolving) {
		atomic_dec_uint(&rdk_cm_roce_resolving);
		rdk_ibconn_reject(c, IBCM_REJ_CONSUMER_DEFINED);
		return;
	}
	mutex_enter(&c->ic_lock);
	c->ic_path.gp_hop = p.cp_ttl;
	c->ic_awaiting = B_TRUE;
	mutex_exit(&c->ic_lock);
	if (p.cp_local) {
		rdk_cm_roce_resolve_done(c, 0, 0, p.cp_smac);
		return;
	}
	rdk_ibconn_arm(c, rdk_cm_roce_resolve_ms);
	(void) rdk_cm_arp_start(crgetzoneid(listener->rci_cred), p.cp_ifindex,
	    p.cp_nexthop, &rdk_ibconn_arp_ops, c, 0, &arp);
	mutex_enter(&c->ic_lock);
	if (c->ic_awaiting) {
		c->ic_arp = arp;
		arp = NULL;
	}
	mutex_exit(&c->ic_lock);
	rdk_cm_arp_cancel(arp);
}

/* Linux cm_req_handler(), cm_match_req() and cma_ib_req_handler(). */
void
rdk_cm_roce_req(rdk_gsi_t *g, const rdk_gsi_rx_t *rx, const rdk_ibcm_msg_t *m)
{
	const struct rdk_gid_attr *sgid;
	rdk_gsi_path_t gp;
	rdk_cma_hdr_t h;
	rdk_ibconn_t *c, *o;
	rdk_cm_id_t *lis;
	rdk_gid_t gid;
	uint32_t lgid, rgid;
	uint16_t port;
	void *admit;
	int ret;

	port = (uint16_t)m->m_service_id;
	if ((m->m_service_id >> 16) != IBCM_PS_TCP || port == 0) {
		rdk_cm_roce_reply(g, rx, m, IBCM_ATTR_REJ,
		    IBCM_REJ_INVALID_SERVICE_ID, IBCM_MSG_RESPONSE_REQ);
		return;
	}
	if (rdk_cma_hdr_parse(m->m_pdata, m->m_pdata_len, &h) != 0 ||
	    !rdk_ibcm_gid_ip4(m->m_lgid, &lgid) ||
	    !rdk_ibcm_gid_ip4(m->m_rgid, &rgid) ||
	    h.ch_src != rx->rx_ip.ip_src || lgid != h.ch_src ||
	    h.ch_dst != rx->rx_ip.ip_dst || rgid != h.ch_dst)
		return;

	if ((c = rdk_ibconn_find_remote(m->m_ca_guid, m->m_local_id)) != NULL) {
		if (rdk_ibconn_from(c, rx))
			rdk_ibconn_input(c, IBCI_DUP_REQ);
		rdk_ibconn_rele(c);
		return;
	}
	if ((c = rdk_ibconn_find_qpn(m->m_ca_guid, m->m_qpn)) != NULL) {
		/* A QP of a live connection asks again: that one is stale. */
		if (rdk_ibconn_from(c, rx)) {
			rdk_cm_roce_reply(g, rx, m, IBCM_ATTR_REJ,
			    IBCM_REJ_STALE_CONN, IBCM_MSG_RESPONSE_REQ);
			rdk_ibconn_input(c, IBCI_DISCONNECT);
		}
		rdk_ibconn_rele(c);
		return;
	}

	rdk_gid_from_ipv4(&gid, h.ch_dst);
	if ((sgid = rdk_find_gid(g->rg_dev, g->rg_port, &gid,
	    RDK_VLAN_NONE)) == NULL)
		return;
	lis = rdk_cm_roce_listener(h.ch_dst, htons(port));
	if (lis == NULL || lis->rci_dev->rcd_dev != g->rg_dev ||
	    lis->rci_port != g->rg_port) {
		rdk_cm_roce_reply(g, rx, m, IBCM_ATTR_REJ,
		    IBCM_REJ_INVALID_SERVICE_ID, IBCM_MSG_RESPONSE_REQ);
		goto out;
	}
	if ((ret = rdk_cm_admit(lis, h.ch_src, &admit)) != 0) {
		if (ret != EACCES) {
			rdk_cm_roce_reply(g, rx, m, IBCM_ATTR_REJ,
			    IBCM_REJ_CONSUMER_DEFINED, IBCM_MSG_RESPONSE_REQ);
		}
		goto out;
	}
	if ((c = rdk_ibconn_alloc(lis->rci_dev, g->rg_port, B_FALSE)) == NULL) {
		rdk_cm_unadmit(admit);
		goto out;
	}
	c->ic_admit = admit;
	c->ic_sgid = sgid;
	c->ic_path.gp_sgid = sgid;
	sgid = NULL;
	rdk_ibconn_from_req(c, m, &h, port);
	/* A reject sent before the neighbor answers goes back the REQ's way. */
	if (rdk_cm_roce_back(g, rx, &gp) == 0) {
		bcopy(gp.gp_dmac, c->ic_path.gp_dmac, ETHERADDRL);
		c->ic_path.gp_hop = gp.gp_hop;
		rdk_put_gid_attr(gp.gp_sgid);
	}
	if (rdk_ibconn_insert(c) != 0) {
		rdk_cm_roce_reply(g, rx, m, IBCM_ATTR_REJ,
		    IBCM_REJ_CONSUMER_DEFINED, IBCM_MSG_RESPONSE_REQ);
		rdk_ibconn_unlink(c);
		rdk_ibconn_rele(c);
		goto out;
	}
	if (rdk_ibconn_insert_remote(c, &o) != 0) {
		/* The same request came twice at once; the other one has it. */
		if (o != NULL)
			rdk_ibconn_rele(o);
		rdk_ibconn_unlink(c);
		rdk_ibconn_rele(c);
		goto out;
	}
	c->ic_listener = lis;
	lis = NULL;
	/* The tables keep the base reference; this one outlives resolve. */
	rdk_ibconn_hold(c);
	rdk_ibconn_resolve(c, c->ic_listener);
	rdk_ibconn_rele(c);
out:
	rdk_put_gid_attr(sgid);
	if (lis != NULL)
		rdk_cm_rele(lis);
}

/* Linux cm_rep_handler(), with the stale check of cm_insert_remote_qpn(). */
static void
rdk_cm_roce_rep(rdk_gsi_t *g, const rdk_gsi_rx_t *rx, const rdk_ibcm_msg_t *m)
{
	rdk_ibconn_in_t in;
	rdk_ibconn_t *c, *o;
	boolean_t dup = B_FALSE, take = B_FALSE;
	uint8_t life;
	int ret;

	if ((c = rdk_ibconn_find(m->m_remote_id)) == NULL)
		return;
	mutex_enter(&c->ic_lock);
	if (!c->ic_fsm.f_active || !rdk_ibconn_from(c, rx) ||
	    m->m_tid != c->ic_tid) {
		mutex_exit(&c->ic_lock);
		rdk_ibconn_rele(c);
		return;
	}
	if (c->ic_rid != 0) {
		dup = m->m_local_id == c->ic_rid && m->m_ca_guid == c->ic_rguid;
	} else if (c->ic_fsm.f_state == IBCS_REQ_SENT ||
	    c->ic_fsm.f_state == IBCS_MRA_REQ_RCVD) {
		take = B_TRUE;
		c->ic_rid = m->m_local_id;
		c->ic_rguid = m->m_ca_guid;
		c->ic_rqpn = m->m_qpn;
		c->ic_rpsn = m->m_psn;
		c->ic_peer_resp_res = m->m_resp_res;
		c->ic_peer_init_depth = m->m_init_depth;
		c->ic_rnr_retry = m->m_rnr_retry;
		c->ic_resp_res = MIN(c->ic_resp_res, m->m_init_depth);
		c->ic_init_depth = MIN(c->ic_init_depth, m->m_resp_res);
		life = c->ic_ack_timeout > 0 ? c->ic_ack_timeout - 1 : 0;
		c->ic_ack_timeout = rdk_ibcm_ack_timeout(m->m_target_ack_delay,
		    life);
		c->ic_fsm.f_tw_ms = rdk_ibcm_time_ms(c->ic_ack_timeout);
		c->ic_fsm.f_life_ms = c->ic_fsm.f_tw_ms;
		c->ic_pdata_len = IBCM_REP_PDATA;
		bcopy(m->m_pdata, c->ic_pdata, IBCM_REP_PDATA);
	}
	mutex_exit(&c->ic_lock);

	if (dup) {
		rdk_ibconn_input(c, IBCI_DUP_REP);
	} else if (take) {
		bzero(&in, sizeof (in));
		in.ci_msg = m;
		if ((ret = rdk_ibconn_insert_remote(c, &o)) == EEXIST) {
			rdk_cm_roce_reply(g, rx, m, IBCM_ATTR_REJ,
			    IBCM_REJ_STALE_CONN, IBCM_MSG_RESPONSE_REP);
			if (o != c)
				rdk_ibconn_input(o, IBCI_DISCONNECT);
			rdk_ibconn_rele(o);
			in.ci_in.ii_input = IBCI_REJ;
			in.ci_in.ii_rej_reason = IBCM_REJ_STALE_CONN;
			in.ci_msg = NULL;
		} else {
			in.ci_in.ii_input = IBCI_REP;
		}
		/* A connection that ended meanwhile takes no REP. */
		if (ret != ENOENT)
			(void) rdk_ibconn_step(c, &in);
	}
	rdk_ibconn_rele(c);
}

/*
 * Messages for a connection by its local ID; the peer's ID must match
 * when known, and the transaction must be the connection's.
 */
static void
rdk_cm_roce_conn_msg(const rdk_gsi_rx_t *rx, const rdk_ibcm_msg_t *m)
{
	rdk_ibconn_in_t in;
	rdk_ibconn_t *c;
	uint64_t guid;
	boolean_t ok, by_guid;

	by_guid = rdk_ibcm_rej_by_guid(m, &guid);
	c = by_guid ? rdk_ibconn_find_remote(guid, m->m_local_id) :
	    rdk_ibconn_find(m->m_remote_id);
	if (c == NULL) {
		if (m->m_attr == IBCM_ATTR_DREQ) {
			rdk_cm_roce_reply(rx->rx_gsi, rx, m, IBCM_ATTR_DREP,
			    0, 0);
		}
		return;
	}
	bzero(&in, sizeof (in));
	in.ci_msg = m;
	mutex_enter(&c->ic_lock);
	ok = rdk_ibconn_from(c, rx);
	switch (m->m_attr) {
	case IBCM_ATTR_RTU:
		in.ci_in.ii_input = IBCI_RTU;
		ok = ok && m->m_local_id == c->ic_rid && m->m_tid == c->ic_tid;
		break;
	case IBCM_ATTR_REJ:
		in.ci_in.ii_input = IBCI_REJ;
		in.ci_in.ii_rej_reason = m->m_reason;
		ok = ok && (by_guid ? m->m_remote_id == 0 ||
		    m->m_remote_id == c->ic_lid :
		    m->m_msg == IBCM_MSG_RESPONSE_REQ ||
		    m->m_local_id == c->ic_rid) &&
		    (m->m_tid == c->ic_tid || m->m_tid == c->ic_dreq_tid);
		break;
	case IBCM_ATTR_MRA:
		in.ci_in.ii_input = IBCI_MRA;
		in.ci_in.ii_mra_msg = m->m_msg;
		in.ci_in.ii_mra_timeout = m->m_service_timeout;
		ok = ok && (m->m_msg == IBCM_MSG_RESPONSE_REQ ||
		    m->m_local_id == c->ic_rid) && m->m_tid == c->ic_tid;
		break;
	case IBCM_ATTR_DREQ:
		in.ci_in.ii_input = IBCI_DREQ;
		ok = ok && c->ic_rid != 0 && m->m_local_id == c->ic_rid &&
		    m->m_qpn == c->ic_lqpn;
		break;
	case IBCM_ATTR_DREP:
		in.ci_in.ii_input = IBCI_DREP;
		ok = ok && m->m_local_id == c->ic_rid &&
		    m->m_tid == c->ic_dreq_tid;
		break;
	default:
		ok = B_FALSE;
		break;
	}
	mutex_exit(&c->ic_lock);
	if (ok)
		(void) rdk_ibconn_step(c, &in);
	rdk_ibconn_rele(c);
}

/* A MAD from a GSI agent, in a CM taskq thread. */
void
rdk_cm_roce_recv(rdk_gsi_rx_t *rx)
{
	rdk_ibcm_msg_t m;

	if (rdk_ibcm_parse(rx->rx_mad, &m) != 0)
		return;
	switch (m.m_attr) {
	case IBCM_ATTR_REQ:
		rdk_cm_roce_req(rx->rx_gsi, rx, &m);
		break;
	case IBCM_ATTR_REP:
		rdk_cm_roce_rep(rx->rx_gsi, rx, &m);
		break;
	default:
		rdk_cm_roce_conn_msg(rx, &m);
		break;
	}
}

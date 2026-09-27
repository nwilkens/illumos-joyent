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
 * The RoCE backend of the connection manager: IB CM connections over the
 * ports' GSI agents, compatible with the Linux RDMA CM.  A connection's
 * state machine (rdk_ibcm_fsm.c) decides; this file runs the actions: the
 * QP transitions, the messages and the events, and the transport
 * operations.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rdk_impl.h"
#include "rdk_cm_roce.h"

/* The RNR NAK timer a QP asks of its peer: 12 is 0.64 ms. */
uint_t rdk_ibcm_min_rnr_timer = 12;

/*
 * QP transitions (Linux cm_init_qp_init_attr(), cm_init_qp_rtr_attr() and
 * cm_init_qp_rts_attr()).
 */
static int
rdk_ibconn_qp_init(rdk_ibconn_t *c, struct rdk_qp *qp)
{
	struct rdk_qp_init_attr init;
	struct rdk_qp_attr a;
	int ret, mask;

	if ((ret = rdk_query_qp(qp, &a, RDK_QP_STATE, &init)) != 0)
		return (ret);
	if (a.qp_state != RDK_QPS_RESET && a.qp_state != RDK_QPS_INIT)
		return (EINVAL);
	mask = RDK_QP_STATE | RDK_QP_ACCESS_FLAGS;
	if (a.qp_state == RDK_QPS_RESET)
		mask |= RDK_QP_PKEY_INDEX | RDK_QP_PORT;
	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_INIT;
	a.pkey_index = 0;
	a.port_num = c->ic_port;
	a.qp_access_flags = RDK_ACCESS_REMOTE_WRITE;
	if (c->ic_resp_res != 0) {
		a.qp_access_flags |= RDK_ACCESS_REMOTE_READ |
		    RDK_ACCESS_REMOTE_ATOMIC;
	}
	return (rdk_modify_qp(qp, &a, mask));
}

static int
rdk_ibconn_qp_rtr_rts(rdk_ibconn_t *c, struct rdk_qp *qp)
{
	struct rdk_qp_attr a;
	struct rdk_ah_attr *ah = &a.ah_attr;
	int ret;

	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_RTR;
	ah->type = RDK_AH_ATTR_TYPE_ROCE;
	ah->ah_flags = RDK_AH_GRH;
	ah->port_num = c->ic_port;
	ah->grh.sgid_index = (uint8_t)c->ic_sgid->index;
	ah->grh.dgid = c->ic_path.gp_dgid;
	ah->grh.hop_limit = c->ic_path.gp_hop;
	ah->grh.traffic_class = c->ic_path.gp_tclass;
	ah->grh.flow_label = c->ic_flow;
	bcopy(c->ic_path.gp_dmac, ah->roce.dmac, ETHERADDRL);
	a.path_mtu = (enum rdk_mtu)c->ic_mtu;
	a.dest_qp_num = c->ic_rqpn;
	a.rq_psn = c->ic_rpsn;
	a.max_dest_rd_atomic = c->ic_resp_res;
	a.min_rnr_timer = (uint8_t)MIN(rdk_ibcm_min_rnr_timer, 31);
	if ((ret = rdk_modify_qp(qp, &a, RDK_QP_STATE | RDK_QP_AV |
	    RDK_QP_PATH_MTU | RDK_QP_DEST_QPN | RDK_QP_RQ_PSN |
	    RDK_QP_MAX_DEST_RD_ATOMIC | RDK_QP_MIN_RNR_TIMER)) != 0)
		return (ret);

	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_RTS;
	a.sq_psn = c->ic_spsn;
	a.timeout = c->ic_ack_timeout;
	a.retry_cnt = c->ic_retry;
	a.rnr_retry = c->ic_rnr_retry;
	a.max_rd_atomic = c->ic_init_depth;
	return (rdk_modify_qp(qp, &a, RDK_QP_STATE | RDK_QP_SQ_PSN |
	    RDK_QP_TIMEOUT | RDK_QP_RETRY_CNT | RDK_QP_RNR_RETRY |
	    RDK_QP_MAX_QP_RD_ATOMIC));
}

static void
rdk_ibconn_qp_err(rdk_ibconn_t *c)
{
	struct rdk_qp_attr a;
	struct rdk_qp *qp;

	if ((qp = rdk_cm_qp_lease(c)) == NULL)
		return;
	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_ERR;
	(void) rdk_modify_qp(qp, &a, RDK_QP_STATE);
	rdk_cm_qp_unlease(qp);
}

/*
 * Messages.  ic_lock is held.
 */
void
rdk_ibcm_msg_init(const rdk_ibconn_t *c, rdk_ibcm_msg_t *m, uint16_t attr)
{
	bzero(m, sizeof (*m));
	m->m_attr = attr;
	m->m_tid = c->ic_tid;
	m->m_local_id = c->ic_lid;
	m->m_remote_id = c->ic_rid;
}

static void
rdk_ibconn_build(rdk_ibconn_t *c, const rdk_ibcm_act_t *a, uint8_t *buf)
{
	struct rdk_device *dev = c->ic_cd->rcd_dev;
	uint8_t pd[IBCM_REQ_PDATA];
	rdk_cma_hdr_t h;
	rdk_ibcm_msg_t m;

	ASSERT(MUTEX_HELD(&c->ic_lock));
	switch (a->ia_send) {
	case IBCM_SEND_REQ:
		rdk_ibcm_msg_init(c, &m, IBCM_ATTR_REQ);
		m.m_service_id = c->ic_service_id;
		m.m_ca_guid = dev->rd_node_guid;
		m.m_qpn = c->ic_lqpn;
		m.m_resp_res = c->ic_resp_res;
		m.m_init_depth = c->ic_init_depth;
		m.m_remote_resp_to = RDK_IBCM_RESP_TIMEOUT;
		m.m_transport = IBCM_TRANSPORT_RC;
		m.m_flow_ctl = 1;
		m.m_psn = c->ic_spsn;
		m.m_local_resp_to = RDK_IBCM_RESP_TIMEOUT;
		m.m_retry = c->ic_retry;
		m.m_pkey = IBCM_PKEY_DEFAULT;
		m.m_mtu = c->ic_mtu;
		m.m_rnr_retry = c->ic_ask_rnr;
		m.m_max_retries = RDK_IBCM_MAX_RETRIES;
		if (c->ic_path.gp_hop > 1)
			m.m_llid = m.m_rlid = IBCM_LID_PERMISSIVE;
		bcopy(c->ic_sgid->gid.raw, m.m_lgid, 16);
		bcopy(c->ic_path.gp_dgid.raw, m.m_rgid, 16);
		m.m_flow_label = c->ic_flow;
		m.m_tclass = c->ic_path.gp_tclass;
		m.m_hop_limit = c->ic_path.gp_hop;
		m.m_subnet_local = c->ic_path.gp_hop <= 1;
		m.m_ack_timeout = c->ic_ack_timeout;
		h.ch_src = c->ic_lip;
		h.ch_dst = c->ic_rip;
		h.ch_sport = c->ic_lport;
		bzero(pd, sizeof (pd));
		rdk_cma_hdr_build(pd, &h);
		bcopy(c->ic_pdata, pd + IBCM_CMA_HDR_LEN,
		    MIN(c->ic_pdata_len, IBCM_CMA_REQ_PDATA));
		m.m_pdata = pd;
		m.m_pdata_len = sizeof (pd);
		break;
	case IBCM_SEND_REP:
		rdk_ibcm_msg_init(c, &m, IBCM_ATTR_REP);
		m.m_qpn = c->ic_lqpn;
		m.m_psn = c->ic_spsn;
		m.m_resp_res = c->ic_resp_res;
		m.m_init_depth = c->ic_init_depth;
		m.m_target_ack_delay = dev->rd_attr.local_ca_ack_delay;
		m.m_flow_ctl = 1;
		m.m_rnr_retry = c->ic_ask_rnr;
		m.m_ca_guid = dev->rd_node_guid;
		m.m_pdata = c->ic_pdata;
		m.m_pdata_len = MIN(c->ic_pdata_len, IBCM_REP_PDATA);
		break;
	case IBCM_SEND_RTU:
		rdk_ibcm_msg_init(c, &m, IBCM_ATTR_RTU);
		break;
	case IBCM_SEND_REJ:
		rdk_ibcm_msg_init(c, &m, IBCM_ATTR_REJ);
		m.m_msg = a->ia_rej_msg;
		m.m_reason = a->ia_rej_reason;
		if (m.m_msg == IBCM_MSG_RESPONSE_REQ)
			m.m_local_id = 0;
		if (m.m_reason == IBCM_REJ_TIMEOUT) {
			uint64_t guid = dev->rd_node_guid;
			uint_t i;

			for (i = 0; i < 8; i++)
				m.m_ari[i] = (uint8_t)(guid >> (56 - 8 * i));
			m.m_ari_len = 8;
		}
		if (m.m_reason == IBCM_REJ_CONSUMER_DEFINED) {
			m.m_pdata = c->ic_pdata;
			m.m_pdata_len = MIN(c->ic_pdata_len, IBCM_REJ_PDATA);
		}
		break;
	case IBCM_SEND_MRA:
		rdk_ibcm_msg_init(c, &m, IBCM_ATTR_MRA);
		m.m_msg = a->ia_mra_msg;
		m.m_service_timeout = RDK_IBCM_MRA_TIMEOUT;
		break;
	case IBCM_SEND_DREQ:
		rdk_ibcm_msg_init(c, &m, IBCM_ATTR_DREQ);
		m.m_tid = c->ic_dreq_tid;
		m.m_qpn = c->ic_rqpn;
		break;
	case IBCM_SEND_DREP:
		rdk_ibcm_msg_init(c, &m, IBCM_ATTR_DREP);
		break;
	default:
		return;
	}
	rdk_ibcm_build(buf, &m);
}

static rdk_cm_tev_t
rdk_ibconn_tev(rdk_ibcm_ev_t ev)
{
	switch (ev) {
	case IBCE_REPLY:
		return (RCT_REPLY);
	case IBCE_ESTABLISHED:
		return (RCT_ESTABLISHED);
	case IBCE_DISCONNECT:
		return (RCT_DISCONNECT);
	default:
		return (RCT_CLOSE);
	}
}

/*
 * The state machine took a CONNECT or ACCEPT: the ID now waits for this
 * connection's final event.  Lock order: ic_lock, then rci_lock.
 */
static void
rdk_ibconn_attach_id(rdk_ibconn_t *c, rdk_cm_id_t *id)
{
	ASSERT(MUTEX_HELD(&c->ic_lock));
	rdk_cm_hold(id);
	c->ic_id = id;
	mutex_enter(&id->rci_lock);
	id->rci_tp_ref = B_TRUE;
	mutex_exit(&id->rci_lock);
}

/*
 * Run one input and the actions it gives, and the input the actions give
 * back (the result of the QP transitions), one step at a time.  Returns
 * whether a CONNECT or ACCEPT was taken and its ID attached.
 */
boolean_t
rdk_ibconn_step(rdk_ibconn_t *c, const rdk_ibconn_in_t *in0)
{
	rdk_ibconn_in_t in = *in0;
	rdk_ibcm_act_t a;
	uint8_t buf[IBCM_MAD_LEN];
	uint8_t pdata[IBCM_REP_PDATA];
	uint16_t plen;
	rdk_gsi_path_t path;
	rdk_cm_arp_t *arp;
	rdk_cm_id_t *id;
	struct rdk_qp *qp;
	timeout_id_t old;
	boolean_t send, more, freed = B_FALSE, took = B_FALSE;
	uint8_t ird, ord;
	int ret;

	mutex_enter(&c->ic_lock);
	while (c->ic_busy)
		cv_wait(&c->ic_cv, &c->ic_lock);
	c->ic_busy = B_TRUE;
	do {
		more = B_FALSE;
		rdk_ibcm_fsm_step(&c->ic_fsm, &in.ci_in, &a);
		if (in.ci_in.ii_input == IBCI_REJ ||
		    in.ci_in.ii_input == IBCI_REJECT)
			a.ia_reason = in.ci_in.ii_rej_reason;
		if (in.ci_attach != NULL && c->ic_fsm.f_attached &&
		    c->ic_id == NULL) {
			rdk_ibconn_attach_id(c, in.ci_attach);
			took = B_TRUE;
		}
		arp = NULL;
		if (a.ia_free) {
			arp = c->ic_arp;
			c->ic_arp = NULL;
			if (c->ic_awaiting) {
				c->ic_awaiting = B_FALSE;
				atomic_dec_uint(&rdk_cm_roce_resolving);
			}
		}

		/* A DREP answers the DREQ's transaction. */
		if (in.ci_in.ii_input == IBCI_DREQ &&
		    a.ia_send == IBCM_SEND_DREP && in.ci_msg != NULL)
			c->ic_tid = in.ci_msg->m_tid;
		send = B_FALSE;
		if (a.ia_resend && c->ic_msg_valid) {
			bcopy(c->ic_msg, buf, sizeof (buf));
			send = B_TRUE;
		} else if (a.ia_send != IBCM_SEND_NONE) {
			rdk_ibconn_build(c, &a, buf);
			if (a.ia_send == IBCM_SEND_REQ ||
			    a.ia_send == IBCM_SEND_REP ||
			    a.ia_send == IBCM_SEND_DREQ) {
				bcopy(buf, c->ic_msg, sizeof (buf));
				c->ic_msg_valid = B_TRUE;
			}
			send = B_TRUE;
		}
		path = c->ic_path;
		old = 0;
		if (a.ia_timer_stop || a.ia_timer_ms != 0) {
			old = rdk_ibconn_timer_set(c, a.ia_timer_ms);
			c->ic_resolving = B_FALSE;
		}

		id = NULL;
		plen = 0;
		ird = ord = 0;
		if (a.ia_ev != IBCE_NONE && (id = c->ic_id) != NULL) {
			if (a.ia_final)
				c->ic_id = NULL;
			else
				rdk_cm_hold(id);
			if (a.ia_ev == IBCE_REPLY && a.ia_status == 0) {
				plen = c->ic_pdata_len;
				bcopy(c->ic_pdata, pdata, plen);
				ird = c->ic_peer_resp_res;
				ord = c->ic_peer_init_depth;
			} else if (in.ci_in.ii_input == IBCI_REJ &&
			    in.ci_msg != NULL) {
				plen = MIN(in.ci_msg->m_pdata_len,
				    IBCM_REJ_PDATA);
				bcopy(in.ci_msg->m_pdata, pdata, plen);
			}
		}
		mutex_exit(&c->ic_lock);

		rdk_ibconn_timer_cancel(c, old);
		if (a.ia_qp_err)
			rdk_ibconn_qp_err(c);
		if (send)
			(void) rdk_gsi_send(c->ic_gsi, &path, buf);
		if (a.ia_qp_rtr_rts) {
			ret = ENXIO;
			if ((qp = rdk_cm_qp_lease(c)) != NULL) {
				ret = rdk_ibconn_qp_rtr_rts(c, qp);
				rdk_cm_qp_unlease(qp);
			}
			bzero(&in, sizeof (in));
			in.ci_in.ii_input = ret == 0 ? IBCI_QP_READY :
			    IBCI_QP_FAILED;
			more = B_TRUE;
		}
		if (id != NULL) {
			rdk_cm_conn_event(id, rdk_ibconn_tev(a.ia_ev),
			    a.ia_status, a.ia_reason, plen != 0 ? pdata : NULL,
			    plen, ird, ord);
			if (a.ia_final) {
				rdk_cm_qp_detach(c);
				rdk_cm_final(id);
			}
			rdk_cm_rele(id);
		}
		if (a.ia_drop_remote)
			rdk_ibconn_drop_remote(c);
		if (a.ia_free && !freed) {
			rdk_cm_arp_cancel(arp);
			rdk_cm_qp_detach(c);
			rdk_ibconn_unlink(c);
			freed = B_TRUE;
		}
		mutex_enter(&c->ic_lock);
	} while (more);
	c->ic_busy = B_FALSE;
	cv_broadcast(&c->ic_cv);
	mutex_exit(&c->ic_lock);
	if (freed)
		rdk_ibconn_rele(c);
	return (took);
}

void
rdk_ibconn_input(rdk_ibconn_t *c, rdk_ibcm_input_t input)
{
	rdk_ibconn_in_t in;

	bzero(&in, sizeof (in));
	in.ci_in.ii_input = input;
	(void) rdk_ibconn_step(c, &in);
}

/*
 * The transport operations.
 */
static int
rdk_cm_roce_listen(rdk_cm_id_t *id)
{
	rdk_gsi_t *g;
	int ret;

	if ((g = rdk_cm_roce_gsi(id->rci_dev, id->rci_port)) == NULL)
		return (ENXIO);
	rdk_gsi_rele(g);
	if ((ret = rdk_cm_resv_listen(id->rci_resv, id)) == 0) {
		mutex_enter(&id->rci_lock);
		id->rci_roce_listen = B_TRUE;
		mutex_exit(&id->rci_lock);
	}
	return (ret);
}

static void
rdk_cm_roce_unlisten(rdk_cm_id_t *id)
{
	boolean_t listening;

	mutex_enter(&id->rci_lock);
	listening = id->rci_roce_listen;
	id->rci_roce_listen = B_FALSE;
	mutex_exit(&id->rci_lock);
	if (listening)
		rdk_cm_resv_unlisten(id->rci_resv, id);
}

static rdk_ibconn_t *
rdk_cm_roce_conn(rdk_cm_id_t *id)
{
	rdk_ibconn_t *c;

	mutex_enter(&id->rci_lock);
	if ((c = id->rci_conn) != NULL)
		rdk_ibconn_hold(c);
	mutex_exit(&id->rci_lock);
	return (c);
}

static uint8_t
rdk_cm_roce_retry(uint8_t v)
{
	return (v == 0 || v > 7 ? 7 : v);
}

static int
rdk_cm_roce_connect(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	struct rdk_device *dev = id->rci_dev->rcd_dev;
	const struct rdk_device_attr *da = &dev->rd_attr;
	struct rdk_cm_route rt;
	rdk_ibconn_in_t in;
	rdk_ibconn_t *c;
	rdk_gid_t gid;
	uint8_t ttl;
	int ret;

	if ((ret = rdk_cm_route(id, &rt)) != 0)
		return (ret);
	mutex_enter(&id->rci_lock);
	ttl = id->rci_ttl;
	mutex_exit(&id->rci_lock);
	if ((c = rdk_ibconn_alloc(id->rci_dev, rt.rcr_port, B_TRUE)) == NULL)
		return (ENXIO);
	rdk_gid_from_ipv4(&gid, rt.rcr_src.sin_addr.s_addr);
	if ((c->ic_sgid = rdk_find_gid(dev, rt.rcr_port, &gid,
	    rt.rcr_vlan)) == NULL) {
		ret = EADDRNOTAVAIL;
		goto fail;
	}
	if ((c->ic_mtu = rdk_cm_roce_mtu(dev, rt.rcr_port,
	    rt.rcr_mtu)) == 0) {
		ret = EINVAL;
		goto fail;
	}
	c->ic_lip = rt.rcr_src.sin_addr.s_addr;
	c->ic_rip = rt.rcr_dst.sin_addr.s_addr;
	c->ic_lport = rt.rcr_src.sin_port;
	c->ic_rport = rt.rcr_dst.sin_port;
	c->ic_service_id = IBCM_SID_TCP(ntohs(rt.rcr_dst.sin_port));
	c->ic_path.gp_sgid = c->ic_sgid;
	rdk_gid_from_ipv4(&c->ic_path.gp_dgid, c->ic_rip);
	bcopy(rt.rcr_dmac, c->ic_path.gp_dmac, ETHERADDRL);
	c->ic_path.gp_hop = ttl != 0 ? ttl : 64;
	c->ic_path.gp_tclass = rt.rcr_tos;
	c->ic_resp_res = (uint8_t)MIN(p->responder_resources,
	    MAX(da->max_qp_rd_atom, 0));
	c->ic_init_depth = (uint8_t)MIN(p->initiator_depth,
	    MAX(da->max_qp_init_rd_atom, 0));
	c->ic_retry = rdk_cm_roce_retry(p->retry_count);
	c->ic_ask_rnr = rdk_cm_roce_retry(p->rnr_retry_count);
	c->ic_ack_timeout = rdk_ibcm_ack_timeout(da->local_ca_ack_delay,
	    RDK_IBCM_PLT);
	c->ic_pdata_len = MIN(p->private_data_len, IBCM_CMA_REQ_PDATA);
	bcopy(p->private_data, c->ic_pdata, c->ic_pdata_len);
	rdk_ibcm_fsm_init(&c->ic_fsm, B_TRUE, RDK_IBCM_MAX_RETRIES,
	    2 * rdk_ibcm_time_ms(RDK_IBCM_PLT) +
	    rdk_ibcm_time_ms(RDK_IBCM_RESP_TIMEOUT),
	    rdk_ibcm_time_ms(c->ic_ack_timeout),
	    rdk_ibcm_time_ms(c->ic_ack_timeout));

	if ((ret = rdk_cm_qp_attach(c, p->qp)) != 0)
		goto fail;
	if ((ret = rdk_ibconn_qp_init(c, p->qp)) != 0) {
		rdk_cm_qp_detach(c);
		goto fail;
	}
	if ((ret = rdk_ibconn_insert(c)) != 0) {
		rdk_cm_qp_detach(c);
		goto fail;
	}
	c->ic_tid = rdk_ibcm_hi_tid | c->ic_lid;
	c->ic_dreq_tid = c->ic_tid;

	/* The ID's reference; the base one stays with the tables. */
	rdk_ibconn_hold(c);
	mutex_enter(&id->rci_lock);
	id->rci_conn = c;
	mutex_exit(&id->rci_lock);
	bzero(&in, sizeof (in));
	in.ci_in.ii_input = IBCI_CONNECT;
	in.ci_attach = id;
	VERIFY(rdk_ibconn_step(c, &in));
	return (0);
fail:
	rdk_ibconn_unlink(c);
	rdk_ibconn_rele(c);
	return (ret);
}

/* Linux cma_accept_ib(): the QP goes to RTS, then the REP goes out. */
static int
rdk_cm_roce_accept(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	const struct rdk_device_attr *da = &id->rci_dev->rcd_dev->rd_attr;
	rdk_ibconn_in_t in;
	rdk_ibconn_t *c;
	int ret;

	if ((c = rdk_cm_roce_conn(id)) == NULL)
		return (ENXIO);
	mutex_enter(&c->ic_lock);
	if (c->ic_fsm.f_state != IBCS_REQ_RCVD) {
		mutex_exit(&c->ic_lock);
		rdk_ibconn_rele(c);
		return (ECONNRESET);
	}
	c->ic_resp_res = (uint8_t)MIN(p->responder_resources,
	    MIN(c->ic_resp_res, MAX(da->max_qp_rd_atom, 0)));
	c->ic_init_depth = (uint8_t)MIN(p->initiator_depth,
	    MIN(c->ic_init_depth, MAX(da->max_qp_init_rd_atom, 0)));
	c->ic_ask_rnr = rdk_cm_roce_retry(p->rnr_retry_count);
	c->ic_pdata_len = MIN(p->private_data_len, IBCM_REP_PDATA);
	bcopy(p->private_data, c->ic_pdata, c->ic_pdata_len);
	mutex_exit(&c->ic_lock);

	if ((ret = rdk_cm_qp_attach(c, p->qp)) == 0) {
		if ((ret = rdk_ibconn_qp_init(c, p->qp)) == 0)
			ret = rdk_ibconn_qp_rtr_rts(c, p->qp);
		if (ret != 0)
			rdk_ibconn_qp_err(c);
	}
	if (ret != 0) {
		rdk_cm_qp_detach(c);
		mutex_enter(&c->ic_lock);
		c->ic_pdata_len = 0;
		mutex_exit(&c->ic_lock);
		bzero(&in, sizeof (in));
		in.ci_in.ii_input = IBCI_REJECT;
		in.ci_in.ii_rej_reason = IBCM_REJ_CONSUMER_DEFINED;
		(void) rdk_ibconn_step(c, &in);
		rdk_ibconn_rele(c);
		return (ret);
	}
	bzero(&in, sizeof (in));
	in.ci_in.ii_input = IBCI_ACCEPT;
	in.ci_attach = id;
	if (!rdk_ibconn_step(c, &in)) {
		/* The request ended meanwhile, and the QP with it. */
		rdk_ibconn_qp_err(c);
		rdk_cm_qp_detach(c);
		ret = ECONNRESET;
	}
	rdk_ibconn_rele(c);
	return (ret);
}

static int
rdk_cm_roce_reject(rdk_cm_id_t *id, const void *pdata, uint16_t len)
{
	rdk_ibconn_in_t in;
	rdk_ibconn_t *c;

	if ((c = rdk_cm_roce_conn(id)) == NULL)
		return (ENXIO);
	mutex_enter(&c->ic_lock);
	c->ic_pdata_len = MIN(len, IBCM_REJ_PDATA);
	if (c->ic_pdata_len != 0)
		bcopy(pdata, c->ic_pdata, c->ic_pdata_len);
	mutex_exit(&c->ic_lock);
	bzero(&in, sizeof (in));
	in.ci_in.ii_input = IBCI_REJECT;
	in.ci_in.ii_rej_reason = IBCM_REJ_CONSUMER_DEFINED;
	(void) rdk_ibconn_step(c, &in);
	rdk_ibconn_rele(c);
	return (0);
}

static void
rdk_cm_roce_disconnect(rdk_cm_id_t *id, boolean_t abrupt)
{
	rdk_ibconn_t *c;

	if ((c = rdk_cm_roce_conn(id)) == NULL)
		return;
	rdk_ibconn_input(c, abrupt ? IBCI_ABORT : IBCI_DISCONNECT);
	rdk_ibconn_rele(c);
}

/* The ID is going; its connection lives on, for timewait, if it must. */
static void
rdk_cm_roce_release(rdk_cm_id_t *id)
{
	rdk_ibconn_t *c;
	rdk_cm_id_t *held = NULL;

	mutex_enter(&id->rci_lock);
	c = id->rci_conn;
	id->rci_conn = NULL;
	mutex_exit(&id->rci_lock);
	if (c == NULL)
		return;
	mutex_enter(&c->ic_lock);
	if (c->ic_id == id) {
		held = id;
		c->ic_id = NULL;
	}
	mutex_exit(&c->ic_lock);
	if (held != NULL)
		rdk_cm_rele(held);
	rdk_ibconn_rele(c);
}

static uint16_t
rdk_cm_roce_pdata_max(rdk_cm_dev_t *cd, enum rdk_cm_msg msg)
{
	_NOTE(ARGUNUSED(cd));
	switch (msg) {
	case RDK_CM_MSG_REQ:
		return (IBCM_CMA_REQ_PDATA);
	case RDK_CM_MSG_REP:
		return (IBCM_REP_PDATA);
	case RDK_CM_MSG_REJ:
		return (IBCM_REJ_PDATA);
	default:
		return (0);
	}
}

const rdk_cm_tport_t rdk_cm_roce_tport = {
	.ct_listen = rdk_cm_roce_listen,
	.ct_unlisten = rdk_cm_roce_unlisten,
	.ct_connect = rdk_cm_roce_connect,
	.ct_accept = rdk_cm_roce_accept,
	.ct_reject = rdk_cm_roce_reject,
	.ct_disconnect = rdk_cm_roce_disconnect,
	.ct_release = rdk_cm_roce_release,
	.ct_pdata_max = rdk_cm_roce_pdata_max
};

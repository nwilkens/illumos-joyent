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
 * The operations of the offload core (t4_rdma.h).  The client never writes a
 * work request itself: each operation checks that the IDs, L2T entry, port
 * and addresses it names belong to the calling client and are in range, and
 * t4nex builds the CPL.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <inet/ip.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "common/t4_regs.h"
#include "common/t4_regs_values.h"
#include "t4_ofld.h"

/* The current client generation, or EIO if the caller is stale. */
int
t4_ofld_gen(t4_ofld_t *of, uint32_t *genp)
{
	int rc = 0;

	mutex_enter(&of->of_lock);
	if (!t4_ofld_client_ok(of))
		rc = EIO;
	else
		*genp = of->of_client_gen;
	mutex_exit(&of->of_lock);
	return (rc);
}

static boolean_t
t4_ofld_port_up(t4_ofld_t *of, uint8_t port)
{
	return (port < of->of_nports &&
	    (of->of_port[port].op_pi->flags & TPF_OPEN) != 0);
}

static boolean_t
t4_ofld_v4_unicast(const in6_addr_t *a)
{
	ipaddr_t v4;

	if (!IN6_IS_ADDR_V4MAPPED(a))
		return (B_FALSE);
	IN6_V4MAPPED_TO_IPADDR(a, v4);
	return (v4 != INADDR_ANY && v4 != INADDR_BROADCAST &&
	    !CLASSD(v4));
}

static boolean_t
t4_ofld_v6_unicast(const in6_addr_t *a)
{
	return (!IN6_IS_ADDR_UNSPECIFIED(a) && !IN6_IS_ADDR_MULTICAST(a) &&
	    !IN6_IS_ADDR_V4MAPPED(a) && !IN6_IS_ADDR_LOOPBACK(a));
}

/* The compressed filter tuple of a connection's hash lookup. */
static uint64_t
t4_ofld_ntuple(t4_ofld_t *of, const struct port_info *pi, uint16_t vlan)
{
	const struct tp_params *tp = &of->of_sc->params.tp;
	uint64_t ntuple = 0;

	if (tp->vlan_shift >= 0 && vlan != CPL_L2T_VLAN_NONE)
		ntuple |= (uint64_t)(F_FT_VLAN_VLD | vlan) << tp->vlan_shift;
	if (tp->port_shift >= 0)
		ntuple |= (uint64_t)pi->lport << tp->port_shift;
	if (tp->protocol_shift >= 0)
		ntuple |= (uint64_t)IPPROTO_TCP << tp->protocol_shift;
	if (tp->vnic_shift >= 0 && (tp->ingress_config & F_VNIC) != 0) {
		ntuple |= (uint64_t)(V_FT_VNID_ID_VF(pi->vin) |
		    V_FT_VNID_ID_PF(of->of_sc->pf) |
		    V_FT_VNID_ID_VLD(pi->vivld)) << tp->vnic_shift;
	}
	return (ntuple);
}

static int
t4_ofld_opts_ok(const t4_rdma_tcp_opts_t *o)
{
	if ((o->trt_rcv_win >> 10) > M_RCV_BUFSIZ || o->trt_rcv_win < 1024 ||
	    o->trt_mtu_idx >= NMTUS || o->trt_cong > CONG_ALG_HIGHSPEED ||
	    (o->trt_ulp_mode != ULP_MODE_NONE &&
	    o->trt_ulp_mode != ULP_MODE_TCPDDP))
		return (EINVAL);
	return (0);
}

static uint_t
t4_ofld_wscale(uint32_t win)
{
	uint_t ws = 0;

	while (ws < 14 && (65535U << ws) < win)
		ws++;
	return (ws);
}

static uint64_t
t4_ofld_opt0(const t4_ofld_port_t *op, const t4_rdma_tcp_opts_t *o,
    uint32_t l2t)
{
	const uint_t ws = t4_ofld_wscale(o->trt_rcv_win);

	return (F_KEEP_ALIVE | F_DELACK | V_WND_SCALE(ws) |
	    V_MSS_IDX(o->trt_mtu_idx) | V_L2T_IDX(l2t) |
	    V_TX_CHAN(op->op_pi->tx_chan) | V_SMAC_SEL(op->op_pi->smt_idx) |
	    V_DSCP(o->trt_tos >> 2) | V_ULP_MODE(o->trt_ulp_mode) |
	    V_RCV_BUFSIZ(o->trt_rcv_win >> 10));
}

static uint32_t
t4_ofld_opt2(const t4_ofld_port_t *op, const t4_rdma_tcp_opts_t *o)
{
	const struct adapter *sc = op->op_ofld->of_sc;
	/*
	 * The TX modulation queue of the port's channel, as FreeBSD sets it.
	 * RX channel 0 as Linux: e12 stopped answering on the network when a
	 * port 1 connection had RX channel 1.
	 */
	uint32_t opt2 = V_RX_CHANNEL(0) | F_RSS_QUEUE_VALID |
	    V_RSS_QUEUE(op->op_ofld->of_rxq.iq.tsi_abs_id) |
	    V_TX_QUEUE(sc->params.tp.tx_modq[op->op_pi->tx_chan]) |
	    F_T5_OPT_2_VALID | V_CONG_CNTRL(o->trt_cong) | F_T5_ISS;

	if (o->trt_timestamps)
		opt2 |= F_TSTAMPS_EN;
	if (o->trt_sack)
		opt2 |= F_SACK_EN;
	if (o->trt_ecn)
		opt2 |= F_CCTRL_ECN;
	if (t4_ofld_wscale(o->trt_rcv_win) != 0)
		opt2 |= F_WND_SCALE_EN;
	return (opt2);
}

static uint32_t
t4_ofld_isn(const t4_rdma_tcp_opts_t *o)
{
	uint32_t isn;

	(void) random_get_pseudo_bytes((uint8_t *)&isn, sizeof (isn));
	isn = (isn & ~7U) - 1;
	return (o->trt_p2p_iss ? isn + 4 : isn);
}

int
t4_ofld_listen(t4_ofld_t *of, const t4_rdma_listen_t *l)
{
	union {
		struct cpl_pass_open_req	v4;
		struct cpl_pass_open_req6	v6;
	} req;
	t4_ofld_port_t *op;
	t4_tid_ent_t *e;
	const boolean_t v6 = l->trl_family == AF_INET6;
	size_t len;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (l->trl_port >= of->of_nports || l->trl_lport == 0 ||
	    (l->trl_family != AF_INET && l->trl_family != AF_INET6))
		return (EINVAL);
	if (v6) {
		if (!IN6_IS_ADDR_UNSPECIFIED(&l->trl_laddr) &&
		    !t4_clip_held(of, &l->trl_laddr))
			return (EINVAL);
	} else if (!IN6_IS_ADDR_V4MAPPED_ANY(&l->trl_laddr) &&
	    !t4_ofld_v4_unicast(&l->trl_laddr)) {
		return (EINVAL);
	}
	op = &of->of_port[l->trl_port];

	bzero(&req, sizeof (req));
	if (v6) {
		len = sizeof (req.v6);
		t4_ofld_init_tp_wr(&req.v6, len, 0);
		OPCODE_TID(&req.v6) = BE_32(MK_OPCODE_TID(CPL_PASS_OPEN_REQ6,
		    l->trl_stid));
		req.v6.local_port = l->trl_lport;
		bcopy(&l->trl_laddr.s6_addr[0], &req.v6.local_ip_hi, 8);
		bcopy(&l->trl_laddr.s6_addr[8], &req.v6.local_ip_lo, 8);
		req.v6.opt0 = BE_64(V_TX_CHAN(op->op_pi->tx_chan));
		req.v6.opt1 = BE_64(V_CONN_POLICY(CPL_CONN_POLICY_ASK) |
		    F_SYN_RSS_ENABLE |
		    V_SYN_RSS_QUEUE(of->of_rxq.iq.tsi_abs_id));
	} else {
		len = sizeof (req.v4);
		t4_ofld_init_tp_wr(&req.v4, len, 0);
		OPCODE_TID(&req.v4) = BE_32(MK_OPCODE_TID(CPL_PASS_OPEN_REQ,
		    l->trl_stid));
		req.v4.local_port = l->trl_lport;
		bcopy(&l->trl_laddr.s6_addr[12], &req.v4.local_ip, 4);
		req.v4.opt0 = BE_64(V_TX_CHAN(op->op_pi->tx_chan));
		req.v4.opt1 = BE_64(V_CONN_POLICY(CPL_CONN_POLICY_ASK) |
		    F_SYN_RSS_ENABLE |
		    V_SYN_RSS_QUEUE(of->of_rxq.iq.tsi_abs_id));
	}

	/* Sent under td_lock so that a concurrent free sees the request. */
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_STID, l->trl_stid, gen);
	if (e == NULL || (e->te_flags & TEF_STID_BUSY) != 0 ||
	    ((e->te_flags & TEF_V6) != 0) != v6) {
		rc = EINVAL;
	} else if ((rc = t4_ofld_wr_send(of, &op->op_ctrlq, &req,
	    roundup(len, 16))) == 0) {
		e->te_flags |= TEF_LISTEN | TEF_OPEN;
		e->te_port = l->trl_port;
	}
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_unlisten(t4_ofld_t *of, uint32_t stid)
{
	t4_tid_ent_t *e;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_STID, stid, gen);
	if (e == NULL || (e->te_flags & TEF_LISTEN) == 0 ||
	    (e->te_flags & TEF_UNLISTEN) != 0) {
		mutex_exit(&of->of_tids.td_lock);
		return (EINVAL);
	}
	rc = t4_ofld_send_unlisten(of, e->te_port, stid,
	    (e->te_flags & TEF_V6) != 0);
	if (rc == 0)
		e->te_flags |= TEF_UNLISTEN;
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

void
t4_ofld_stid_free(t4_ofld_t *of, uint32_t stid)
{
	t4_tid_ent_t *e;
	uint32_t gen;

	if (t4_ofld_gen(of, &gen) != 0)
		return;
	mutex_enter(&of->of_tids.td_lock);
	if ((e = t4_tid_owned(of, T4_TID_STID, stid, gen)) != NULL) {
		t4_tid_wait_idle(of, e);
		if ((e->te_flags & TEF_STID_BUSY) == 0) {
			t4_tid_free_locked(of, T4_TID_STID, stid);
		} else {
			/* The last reply for the server frees it. */
			e->te_state = TTS_ORPHAN;
			t4_ofld_orphan_unlisten_locked(of, e, stid);
		}
	}
	mutex_exit(&of->of_tids.td_lock);
}

void
t4_ofld_atid_free(t4_ofld_t *of, uint32_t atid)
{
	t4_tid_ent_t *e;
	uint32_t gen;

	if (t4_ofld_gen(of, &gen) != 0)
		return;
	mutex_enter(&of->of_tids.td_lock);
	if ((e = t4_tid_owned(of, T4_TID_ATID, atid, gen)) != NULL) {
		t4_tid_wait_idle(of, e);
		/* An open in flight still gets a reply, which frees it. */
		if ((e->te_flags & TEF_OPEN) != 0)
			e->te_state = TTS_ORPHAN;
		else
			t4_tid_free_locked(of, T4_TID_ATID, atid);
	}
	mutex_exit(&of->of_tids.td_lock);
}

int
t4_ofld_act_open(t4_ofld_t *of, const t4_rdma_act_open_t *a)
{
	union {
		struct cpl_t6_act_open_req	v4;
		struct cpl_t6_act_open_req6	v6;
	} req;
	struct cpl_t5_act_open_req *r4 = (void *)&req.v4;
	struct cpl_t5_act_open_req6 *r6 = (void *)&req.v6;
	const boolean_t t6 = t4_cver_ge(of->of_sc, CHELSIO_T6);
	const boolean_t v6 = a->trao_family == AF_INET6;
	t4_ofld_port_t *op;
	t4_tid_ent_t *e;
	uint16_t vlan;
	uint64_t opt0, params;
	uint32_t gen, opt2, tidq;
	size_t len;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (!t4_ofld_port_up(of, a->trao_port))
		return (ENETDOWN);
	if (a->trao_lport == 0 || a->trao_fport == 0 ||
	    (rc = t4_ofld_opts_ok(&a->trao_opts)) != 0)
		return (EINVAL);
	if (v6) {
		if (!t4_ofld_v6_unicast(&a->trao_faddr) ||
		    !t4_ofld_v6_unicast(&a->trao_laddr) ||
		    !t4_clip_held(of, &a->trao_laddr))
			return (EINVAL);
	} else if (a->trao_family != AF_INET ||
	    !t4_ofld_v4_unicast(&a->trao_faddr) ||
	    !t4_ofld_v4_unicast(&a->trao_laddr)) {
		return (EINVAL);
	}
	if (!t4_l2t_held(of, a->trao_l2t, a->trao_port, &vlan))
		return (EINVAL);
	op = &of->of_port[a->trao_port];

	opt0 = t4_ofld_opt0(op, &a->trao_opts, a->trao_l2t);
	opt2 = t4_ofld_opt2(op, &a->trao_opts);
	params = V_FILTER_TUPLE(t4_ofld_ntuple(of, op->op_pi, vlan));
	tidq = V_TID_QID(of->of_rxq.iq.tsi_abs_id) | a->trao_atid;

	bzero(&req, sizeof (req));
	if (v6) {
		len = t6 ? sizeof (req.v6) : sizeof (*r6);
		t4_ofld_init_tp_wr(r6, len, 0);
		OPCODE_TID(r6) = BE_32(MK_OPCODE_TID(CPL_ACT_OPEN_REQ6, tidq));
		r6->local_port = a->trao_lport;
		r6->peer_port = a->trao_fport;
		bcopy(&a->trao_laddr.s6_addr[0], &r6->local_ip_hi, 8);
		bcopy(&a->trao_laddr.s6_addr[8], &r6->local_ip_lo, 8);
		bcopy(&a->trao_faddr.s6_addr[0], &r6->peer_ip_hi, 8);
		bcopy(&a->trao_faddr.s6_addr[8], &r6->peer_ip_lo, 8);
		r6->opt0 = BE_64(opt0);
		r6->rsvd = BE_32(t4_ofld_isn(&a->trao_opts));
		r6->opt2 = BE_32(opt2);
		r6->params = BE_64(params);
	} else {
		len = t6 ? sizeof (req.v4) : sizeof (*r4);
		t4_ofld_init_tp_wr(r4, len, 0);
		OPCODE_TID(r4) = BE_32(MK_OPCODE_TID(CPL_ACT_OPEN_REQ, tidq));
		r4->local_port = a->trao_lport;
		r4->peer_port = a->trao_fport;
		bcopy(&a->trao_laddr.s6_addr[12], &r4->local_ip, 4);
		bcopy(&a->trao_faddr.s6_addr[12], &r4->peer_ip, 4);
		r4->opt0 = BE_64(opt0);
		r4->rsvd = BE_32(t4_ofld_isn(&a->trao_opts));
		r4->opt2 = BE_32(opt2);
		r4->params = BE_64(params);
	}

	/* Sent under td_lock so that a concurrent free sees the request. */
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_ATID, a->trao_atid, gen);
	if (e == NULL || (e->te_flags & TEF_OPEN) != 0) {
		rc = EINVAL;
	} else if ((rc = t4_ofld_wr_send(of, &op->op_ctrlq, &req,
	    roundup(len, 16))) == 0) {
		e->te_flags |= TEF_OPEN;
		e->te_port = a->trao_port;
		e->te_rxq = of->of_rxq.iq.tsi_abs_id;
	}
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_accept(t4_ofld_t *of, const t4_rdma_accept_t *a)
{
	struct cpl_t5_pass_accept_rpl rpl;
	t4_ofld_port_t *op;
	t4_tid_ent_t *e;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (!t4_ofld_port_up(of, a->trac_port))
		return (ENETDOWN);
	if ((rc = t4_ofld_opts_ok(&a->trac_opts)) != 0 ||
	    !t4_l2t_held(of, a->trac_l2t, a->trac_port, NULL))
		return (EINVAL);
	op = &of->of_port[a->trac_port];

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_HW, a->trac_tid, gen);
	if (e == NULL || (e->te_flags & TEF_EMBRYO) == 0 ||
	    (e->te_flags & TEF_RELEASING) != 0 ||
	    e->te_port != a->trac_port) {
		mutex_exit(&of->of_tids.td_lock);
		return (EINVAL);
	}

	bzero(&rpl, sizeof (rpl));
	t4_ofld_init_tp_wr(&rpl, sizeof (rpl), a->trac_tid);
	OPCODE_TID(&rpl) = BE_32(MK_OPCODE_TID(CPL_PASS_ACCEPT_RPL,
	    a->trac_tid));
	rpl.opt0 = BE_64(t4_ofld_opt0(op, &a->trac_opts, a->trac_l2t));
	rpl.opt2 = BE_32(t4_ofld_opt2(op, &a->trac_opts));
	rpl.iss = BE_32(t4_ofld_isn(&a->trac_opts));
	rc = t4_ofld_wr_send(of, &op->op_ctrlq, &rpl,
	    roundup(sizeof (rpl), 16));
	if (rc == 0) {
		e->te_flags &= ~TEF_EMBRYO;
		of->of_tids.td_embryos--;
		e->te_rxq = of->of_rxq.iq.tsi_abs_id;
	}
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

/*
 * The hwtid entry for an operation on a connection, with td_lock held on
 * success.  An embryonic connection takes only accept or release.
 */
static t4_tid_ent_t *
t4_ofld_conn(t4_ofld_t *of, uint32_t tid, int *rcp)
{
	t4_tid_ent_t *e;
	uint32_t gen;

	if ((*rcp = t4_ofld_gen(of, &gen)) != 0)
		return (NULL);
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_HW, tid, gen);
	if (e == NULL ||
	    (e->te_flags & (TEF_EMBRYO | TEF_RELEASING)) != 0) {
		mutex_exit(&of->of_tids.td_lock);
		*rcp = EINVAL;
		return (NULL);
	}
	return (e);
}

int
t4_ofld_flowc(t4_ofld_t *of, uint32_t tid, const t4_rdma_flowc_t *f)
{
	t4_tid_ent_t *e;
	int rc;

	if (f == NULL || f->trf_mss == 0 || f->trf_rcv_scale > 14)
		return (EINVAL);
	if ((e = t4_ofld_conn(of, tid, &rc)) == NULL)
		return (rc);
	if ((e->te_flags & TEF_FLOWC) != 0) {
		rc = EALREADY;
	} else if ((rc = t4_ofld_send_flowc(of, e->te_port, tid, f)) == 0) {
		e->te_flags |= TEF_FLOWC;
	}
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_close_con(t4_ofld_t *of, uint32_t tid)
{
	struct cpl_close_con_req req;
	t4_tid_ent_t *e;
	int rc;

	if ((e = t4_ofld_conn(of, tid, &rc)) == NULL)
		return (rc);
	if ((e->te_flags & TEF_FLOWC) == 0) {
		mutex_exit(&of->of_tids.td_lock);
		return (EINVAL);
	}
	bzero(&req, sizeof (req));
	t4_ofld_init_tp_wr(&req, sizeof (req), tid);
	OPCODE_TID(&req) = BE_32(MK_OPCODE_TID(CPL_CLOSE_CON_REQ, tid));
	rc = t4_ofld_wr_send(of, &of->of_port[e->te_port].op_txq, &req,
	    roundup(sizeof (req), 16));
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_abort(t4_ofld_t *of, uint32_t tid, boolean_t rst)
{
	t4_tid_ent_t *e;
	int rc;

	if ((e = t4_ofld_conn(of, tid, &rc)) == NULL)
		return (rc);
	if ((e->te_flags & TEF_FLOWC) == 0) {
		if ((rc = t4_ofld_send_flowc(of, e->te_port, tid, NULL)) != 0) {
			mutex_exit(&of->of_tids.td_lock);
			return (rc);
		}
		e->te_flags |= TEF_FLOWC;
	}
	if ((rc = t4_ofld_send_abort(of, e->te_port, tid, rst)) == 0)
		e->te_flags |= TEF_ABORT;
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_abort_rpl(t4_ofld_t *of, uint32_t tid, boolean_t rst)
{
	t4_tid_ent_t *e;
	int rc;

	if ((e = t4_ofld_conn(of, tid, &rc)) == NULL)
		return (rc);
	rc = t4_ofld_send_abort_rpl(of, e->te_port, tid, rst);
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_set_tcb_field(t4_ofld_t *of, uint32_t tid, uint16_t word,
    uint64_t mask, uint64_t val)
{
	struct cpl_set_tcb_field req;
	t4_ofld_port_t *op;
	t4_tid_ent_t *e;
	int rc;

	if (word > M_WORD)
		return (EINVAL);
	if ((e = t4_ofld_conn(of, tid, &rc)) == NULL)
		return (rc);
	op = &of->of_port[e->te_port];
	bzero(&req, sizeof (req));
	t4_ofld_init_tp_wr(&req, sizeof (req), tid);
	OPCODE_TID(&req) = BE_32(MK_OPCODE_TID(CPL_SET_TCB_FIELD, tid));
	req.reply_ctrl = BE_16(V_NO_REPLY(0) |
	    V_QUEUENO(of->of_rxq.iq.tsi_abs_id));
	req.word_cookie = BE_16(V_WORD(word) | V_COOKIE(0));
	req.mask = BE_64(mask);
	req.val = BE_64(val);
	rc = t4_ofld_wr_send(of, &op->op_ctrlq, &req,
	    roundup(sizeof (req), 16));
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_rx_credits(t4_ofld_t *of, uint32_t tid, uint32_t credits)
{
	struct cpl_rx_data_ack req;
	t4_tid_ent_t *e;
	int rc;

	if (credits == 0 || credits > M_RX_CREDITS)
		return (EINVAL);
	if ((e = t4_ofld_conn(of, tid, &rc)) == NULL)
		return (rc);
	if ((e->te_flags & TEF_FLOWC) == 0) {
		mutex_exit(&of->of_tids.td_lock);
		return (EINVAL);
	}
	bzero(&req, sizeof (req));
	t4_ofld_init_tp_wr(&req, sizeof (req), tid);
	OPCODE_TID(&req) = BE_32(MK_OPCODE_TID(CPL_RX_DATA_ACK, tid));
	req.credit_dack = BE_32(F_RX_DACK_CHANGE | V_RX_DACK_MODE(1) |
	    V_RX_CREDITS(credits));
	rc = t4_ofld_wr_send(of, &of->of_port[e->te_port].op_txq, &req,
	    roundup(sizeof (req), 16));
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

CTASSERT(T4_OFLD_TX_IMM_MAX <= M_FW_WR_IMMDLEN);

int
t4_ofld_tx_data(t4_ofld_t *of, uint32_t tid, const void *buf, size_t len)
{
	struct {
		struct fw_ofld_tx_data_wr	hdr;
		uint8_t				data[T4_OFLD_TX_IMM_MAX];
	} wr;
	t4_tid_ent_t *e;
	size_t wrlen;
	int rc;

	if (buf == NULL || len == 0 || len > T4_OFLD_TX_IMM_MAX)
		return (EINVAL);
	if ((e = t4_ofld_conn(of, tid, &rc)) == NULL)
		return (rc);
	if ((e->te_flags & TEF_FLOWC) == 0) {
		mutex_exit(&of->of_tids.td_lock);
		return (EINVAL);
	}
	bzero(&wr, sizeof (wr));
	wrlen = roundup(sizeof (wr.hdr) + len, 16);
	wr.hdr.op_to_immdlen = BE_32(V_FW_WR_OP(FW_OFLD_TX_DATA_WR) |
	    F_FW_WR_COMPL | V_FW_WR_IMMDLEN(len));
	wr.hdr.flowid_len16 = BE_32(V_FW_WR_FLOWID(tid) |
	    V_FW_WR_LEN16(wrlen / 16));
	wr.hdr.plen = BE_32(len);
	wr.hdr.lsodisable_to_flags = BE_32(V_TX_ULP_MODE(ULP_MODE_NONE) |
	    F_TX_FLUSH | F_TX_SHOVE);
	bcopy(buf, wr.data, len);
	rc = t4_ofld_wr_send(of, &of->of_port[e->te_port].op_txq, &wr, wrlen);
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

int
t4_ofld_tid_bind(t4_ofld_t *of, uint32_t tid, void *ctx)
{
	t4_tid_ent_t *e;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_HW, tid, gen);
	if (e == NULL || (e->te_flags & TEF_RELEASING) != 0)
		rc = EINVAL;
	else
		e->te_ctx = ctx;
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

/*
 * Give a connection TID back to the chip.  The entry refuses new CPL holds
 * while the release is on its way; if the chip hands the TID out again before
 * the entry is freed, t4_hwtid_claim() takes it over and te_seq tells.
 */
int
t4_ofld_tid_release(t4_ofld_t *of, uint32_t tid)
{
	t4_tid_ent_t *e;
	uint32_t gen, seq;
	uint8_t port;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_HW, tid, gen);
	if (e == NULL || (e->te_flags & TEF_RELEASING) != 0) {
		mutex_exit(&of->of_tids.td_lock);
		return (EINVAL);
	}
	e->te_flags |= TEF_RELEASING;
	t4_tid_wait_idle(of, e);
	seq = e->te_seq;
	port = e->te_port;
	mutex_exit(&of->of_tids.td_lock);

	rc = t4_ofld_send_tid_release(of, port, tid);

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_HW, tid);
	if (e != NULL && e->te_seq == seq &&
	    (e->te_flags & TEF_RELEASING) != 0) {
		if (rc == 0)
			t4_tid_free_locked(of, T4_TID_HW, tid);
		else
			e->te_flags &= ~TEF_RELEASING;
	}
	mutex_exit(&of->of_tids.td_lock);
	return (rc);
}

/*
 * Write TPT or PBL entries: len bytes at the adapter memory address addr,
 * which must lie inside the STAG or PBL region.  Inline ULP_TX memory writes
 * of up to 96 bytes each go on port 0's control queue; the last one asks for
 * a completion.  Thread context.
 */
int
t4_ofld_tpt_write(t4_ofld_t *of, uint32_t addr, const void *buf, size_t len)
{
	const t4_rdma_vres_t *vr = &of->of_vres;
	const uint8_t *src = buf;
	t4_sge_eq_t *eq = &of->of_port[0].op_ctrlq;
	struct {
		struct ulp_mem_io	mem;
		struct ulptx_idata	idata;
		uint8_t			data[T4_TPT_INLINE_MAX];
	} wr;
	uint64_t end, cookie = 0;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if (buf == NULL || len == 0 || len > T4_TPT_MAX_LEN ||
	    (len % T4_TPT_UNIT) != 0 || (addr % T4_TPT_UNIT) != 0)
		return (EINVAL);
	end = (uint64_t)addr + len;
	if (!((addr >= vr->trv_stag.trr_start && end <=
	    (uint64_t)vr->trv_stag.trr_start + vr->trv_stag.trr_size) ||
	    (addr >= vr->trv_pbl.trr_start && end <=
	    (uint64_t)vr->trv_pbl.trr_start + vr->trv_pbl.trr_size)))
		return (ERANGE);

	while (len != 0) {
		const size_t n = MIN(len, T4_TPT_INLINE_MAX);
		const size_t wrlen = roundup(sizeof (wr.mem) +
		    sizeof (wr.idata) + n, 16);
		const boolean_t last = n == len;

		bzero(&wr, sizeof (wr));
		wr.mem.wr.wr_hi = BE_32(V_FW_WR_OP(FW_ULPTX_WR) |
		    (last ? F_FW_WR_COMPL : 0));
		wr.mem.wr.wr_mid = BE_32(V_FW_WR_LEN16(wrlen / 16));
		if (last) {
			if ((rc = t4_ofld_waiter_get(of, &cookie)) != 0)
				return (rc);
			wr.mem.wr.wr_lo = BE_64(cookie);
		}
		wr.mem.cmd = BE_32(V_ULPTX_CMD(ULP_TX_MEM_WRITE) |
		    F_T5_ULP_MEMIO_IMM);
		wr.mem.dlen = BE_32(V_ULP_MEMIO_DATA_LEN(n / T4_TPT_UNIT));
		wr.mem.len16 = BE_32(howmany(wrlen - sizeof (wr.mem.wr), 16));
		wr.mem.lock_addr = BE_32(V_ULP_MEMIO_ADDR(addr / T4_TPT_UNIT));
		wr.idata.cmd_more = BE_32(V_ULPTX_CMD(ULP_TX_SC_IMM));
		wr.idata.len = BE_32(n);
		bcopy(src, wr.data, n);

		if ((rc = t4_ofld_wr_send(of, eq, &wr, wrlen)) != 0) {
			if (last)
				t4_ofld_waiter_put(of, cookie);
			return (rc);
		}
		T4_OFLD_STAT(of, os_tpt_write);
		addr += n;
		src += n;
		len -= n;
	}
	return (t4_ofld_waiter_wait(of, cookie));
}

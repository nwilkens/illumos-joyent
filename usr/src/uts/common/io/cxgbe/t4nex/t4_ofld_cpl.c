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
 * CPL dispatch for the offload queues.  A table indexed by opcode gives each
 * CPL t4nex accepts a minimum length and a class; everything else is counted
 * and dropped.  The class says which ID in the message names its owner.  The
 * ID is bounded, looked up, and held for the client's handler; a CPL for an
 * ID the client does not own is dropped, or, when the chip is holding a
 * connection nobody owns, used to tear that connection down.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/strsun.h>
#include <sys/atomic.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "t4_ofld.h"

typedef enum t4_cpl_class {
	TCC_NONE = 0,
	TCC_L2T,	/* t4nex's own replies */
	TCC_STID,	/* the TID field is a server TID */
	TCC_PASS_ACCEPT,
	TCC_ACT_OPEN_RPL,
	TCC_ACT_EST,
	TCC_HWTID,	/* the TID field is a connection TID */
	TCC_FW,		/* firmware message */
	TCC_DROP	/* known but unused here */
} t4_cpl_class_t;

typedef struct t4_cpl_desc {
	uint8_t		tcd_class;
	uint8_t		tcd_minlen;
} t4_cpl_desc_t;

#define	TCD(op, cls, type)	[op] = { cls, sizeof (type) }

static const t4_cpl_desc_t t4_cpl_table[NUM_CPL_CMDS] = {
	TCD(CPL_L2T_WRITE_RPL, TCC_L2T, struct cpl_l2t_write_rpl),
	TCD(CPL_SMT_WRITE_RPL, TCC_DROP, struct cpl_smt_write_rpl),
	TCD(CPL_SGE_EGR_UPDATE, TCC_DROP, struct cpl_sge_egr_update),
	TCD(CPL_PASS_OPEN_RPL, TCC_STID, struct cpl_pass_open_rpl),
	TCD(CPL_CLOSE_LISTSRV_RPL, TCC_STID, struct cpl_close_listsvr_rpl),
	TCD(CPL_PASS_ACCEPT_REQ, TCC_PASS_ACCEPT, struct cpl_pass_accept_req),
	TCD(CPL_ACT_OPEN_RPL, TCC_ACT_OPEN_RPL, struct cpl_act_open_rpl),
	TCD(CPL_ACT_ESTABLISH, TCC_ACT_EST, struct cpl_act_establish),
	TCD(CPL_PASS_ESTABLISH, TCC_HWTID, struct cpl_pass_establish),
	TCD(CPL_PEER_CLOSE, TCC_HWTID, struct cpl_peer_close),
	TCD(CPL_CLOSE_CON_RPL, TCC_HWTID, struct cpl_close_con_rpl),
	TCD(CPL_ABORT_REQ_RSS, TCC_HWTID, struct cpl_abort_req_rss),
	TCD(CPL_ABORT_RPL_RSS, TCC_HWTID, struct cpl_abort_rpl_rss),
	TCD(CPL_RX_DATA, TCC_HWTID, struct cpl_rx_data),
	TCD(CPL_FW4_ACK, TCC_HWTID, struct cpl_fw4_ack),
	TCD(CPL_SET_TCB_RPL, TCC_HWTID, struct cpl_set_tcb_rpl),
	TCD(CPL_GET_TCB_RPL, TCC_HWTID, struct cpl_get_tcb_rpl),
	TCD(CPL_RDMA_TERMINATE, TCC_HWTID, struct cpl_rdma_terminate),
	TCD(CPL_FW4_MSG, TCC_FW, struct cpl_fw4_msg),
	TCD(CPL_FW6_MSG, TCC_FW, struct cpl_fw6_msg),
};

/* ABORT_REQ_RSS statuses that are advice, not an abort. */
static boolean_t
t4_cpl_neg_advice(uint8_t status)
{
	return (status == CPL_ERR_RTX_NEG_ADVICE ||
	    status == CPL_ERR_PERSIST_NEG_ADVICE ||
	    status == CPL_ERR_KEEPALV_NEG_ADVICE);
}

void
t4_ofld_init_tp_wr(void *wr, size_t len, uint32_t tid)
{
	struct work_request_hdr *w = wr;

	ASSERT3U(len - sizeof (*w), <=, M_FW_WR_IMMDLEN);
	w->wr_hi = BE_32(V_FW_WR_OP(FW_TP_WR) |
	    V_FW_WR_IMMDLEN(len - sizeof (*w)));
	w->wr_mid = BE_32(V_FW_WR_LEN16(howmany(len, 16)) |
	    V_FW_WR_FLOWID(tid));
	w->wr_lo = 0;
}

static t4_ofld_port_t *
t4_ofld_port(t4_ofld_t *of, uint8_t port)
{
	VERIFY3U(port, <, of->of_nports);
	return (&of->of_port[port]);
}

int
t4_ofld_send_tid_release(t4_ofld_t *of, uint8_t port, uint32_t tid)
{
	struct cpl_tid_release req;

	bzero(&req, sizeof (req));
	t4_ofld_init_tp_wr(&req, sizeof (req), tid);
	OPCODE_TID(&req) = BE_32(MK_OPCODE_TID(CPL_TID_RELEASE, tid));
	return (t4_ofld_wr_send(of, &t4_ofld_port(of, port)->op_ctrlq, &req,
	    roundup(sizeof (req), 16)));
}

int
t4_ofld_send_abort(t4_ofld_t *of, uint8_t port, uint32_t tid, boolean_t rst)
{
	struct cpl_abort_req req;

	bzero(&req, sizeof (req));
	t4_ofld_init_tp_wr(&req, sizeof (req), tid);
	OPCODE_TID(&req) = BE_32(MK_OPCODE_TID(CPL_ABORT_REQ, tid));
	req.cmd = rst ? CPL_ABORT_SEND_RST : CPL_ABORT_NO_RST;
	return (t4_ofld_wr_send(of, &t4_ofld_port(of, port)->op_txq, &req,
	    roundup(sizeof (req), 16)));
}

int
t4_ofld_send_abort_rpl(t4_ofld_t *of, uint8_t port, uint32_t tid,
    boolean_t rst)
{
	struct cpl_abort_rpl rpl;

	bzero(&rpl, sizeof (rpl));
	t4_ofld_init_tp_wr(&rpl, sizeof (rpl), tid);
	OPCODE_TID(&rpl) = BE_32(MK_OPCODE_TID(CPL_ABORT_RPL, tid));
	rpl.cmd = rst ? CPL_ABORT_SEND_RST : CPL_ABORT_NO_RST;
	return (t4_ofld_wr_send(of, &t4_ofld_port(of, port)->op_txq, &rpl,
	    roundup(sizeof (rpl), 16)));
}

/*
 * The firmware needs a FLOWC before any other work request on a connection,
 * an abort included.  Without flowc, only the fields the firmware needs to
 * route the connection are sent.
 */
int
t4_ofld_send_flowc(t4_ofld_t *of, uint8_t port, uint32_t tid,
    const t4_rdma_flowc_t *flowc)
{
	t4_ofld_port_t *op = t4_ofld_port(of, port);
	struct {
		struct fw_flowc_wr	hdr;
		struct fw_flowc_mnemval	mv[10];
	} wr;
	uint_t n = 0;
	size_t len;

	bzero(&wr, sizeof (wr));
#define	FLOWC(m, v)	do {					\
	wr.mv[n].mnemonic = FW_FLOWC_MNEM_##m;			\
	wr.mv[n].val = BE_32(v);				\
	n++;							\
} while (0)
	FLOWC(PFNVFN, V_FW_PFVF_CMD_PFN(of->of_sc->pf));
	FLOWC(CH, op->op_pi->tx_chan);
	FLOWC(PORT, op->op_pi->tx_chan);
	FLOWC(IQID, of->of_rxq.iq.tsi_abs_id);
	if (flowc != NULL) {
		FLOWC(SNDNXT, flowc->trf_snd_nxt);
		FLOWC(RCVNXT, flowc->trf_rcv_nxt);
		FLOWC(SNDBUF, flowc->trf_sndbuf);
		FLOWC(MSS, flowc->trf_mss);
		FLOWC(RCV_SCALE, flowc->trf_rcv_scale);
	}
#undef	FLOWC
	len = roundup(sizeof (wr.hdr) + n * sizeof (wr.mv[0]), 16);
	wr.hdr.op_to_nparams = BE_32(V_FW_WR_OP(FW_FLOWC_WR) |
	    V_FW_FLOWC_WR_NPARAMS(n));
	wr.hdr.flowid_len16 = BE_32(V_FW_WR_LEN16(len / 16) |
	    V_FW_WR_FLOWID(tid));
	return (t4_ofld_wr_send(of, &op->op_txq, &wr, len));
}

int
t4_ofld_send_unlisten(t4_ofld_t *of, uint8_t port, uint32_t stid,
    boolean_t v6)
{
	t4_ofld_port_t *op = t4_ofld_port(of, port);
	struct cpl_close_listsvr_req req;

	bzero(&req, sizeof (req));
	t4_ofld_init_tp_wr(&req, sizeof (req), 0);
	OPCODE_TID(&req) = BE_32(MK_OPCODE_TID(CPL_CLOSE_LISTSRV_REQ, stid));
	req.reply_ctrl = BE_16(V_NO_REPLY(0) | V_LISTSVR_IPV6(v6 ? 1 : 0) |
	    V_QUEUENO(of->of_rxq.iq.tsi_abs_id));
	return (t4_ofld_wr_send(of, &op->op_ctrlq, &req,
	    roundup(sizeof (req), 16)));
}

/*
 * Work request completion waiters.  A cookie names a slot and its
 * generation, never an address: the firmware echoes the cookie back and is
 * not trusted.
 */
int
t4_ofld_waiter_get(t4_ofld_t *of, uint64_t *cookiep)
{
	mutex_enter(&of->of_wlock);
	for (uint_t i = 0; i < T4_OFLD_NWAITERS; i++) {
		t4_ofld_waiter_t *w = &of->of_waiter[i];

		if (w->ow_state != TWS_FREE)
			continue;
		w->ow_state = TWS_BUSY;
		w->ow_gen = ++of->of_wgen;
		w->ow_status = 0;
		mutex_exit(&of->of_wlock);
		*cookiep = T4_OFLD_COOKIE_PARENT | ((uint64_t)w->ow_gen << 8) |
		    i;
		return (0);
	}
	mutex_exit(&of->of_wlock);
	return (EAGAIN);
}

static t4_ofld_waiter_t *
t4_ofld_waiter_find(t4_ofld_t *of, uint64_t cookie)
{
	const uint_t slot = cookie & 0xff;
	const uint32_t gen = (uint32_t)(cookie >> 8);

	ASSERT(MUTEX_HELD(&of->of_wlock));
	if ((cookie & T4_OFLD_COOKIE_PARENT) == 0 ||
	    slot >= T4_OFLD_NWAITERS || of->of_waiter[slot].ow_gen != gen)
		return (NULL);
	return (&of->of_waiter[slot]);
}

/*
 * Wait for the completion of the work request that carried cookie.  A slot
 * that times out stays out of use until its reply arrives, so a late reply
 * cannot complete somebody else's request.
 */
int
t4_ofld_waiter_wait(t4_ofld_t *of, uint64_t cookie)
{
	const clock_t deadline = ddi_get_lbolt() +
	    drv_usectohz(T4_OFLD_WR_TIMEOUT_US);
	t4_ofld_waiter_t *w;
	int rc;

	mutex_enter(&of->of_wlock);
	w = t4_ofld_waiter_find(of, cookie);
	VERIFY(w != NULL);
	while (w->ow_state == TWS_BUSY) {
		if (cv_timedwait(&of->of_wcv, &of->of_wlock, deadline) == -1)
			break;
	}
	if (w->ow_state == TWS_DONE) {
		rc = w->ow_status;
		w->ow_state = TWS_FREE;
	} else {
		w->ow_state = TWS_ABANDONED;
		rc = ETIMEDOUT;
	}
	mutex_exit(&of->of_wlock);
	return (rc);
}

/* Release a waiter whose request was never sent. */
void
t4_ofld_waiter_put(t4_ofld_t *of, uint64_t cookie)
{
	t4_ofld_waiter_t *w;

	mutex_enter(&of->of_wlock);
	if ((w = t4_ofld_waiter_find(of, cookie)) != NULL)
		w->ow_state = TWS_FREE;
	mutex_exit(&of->of_wlock);
}

static void
t4_ofld_wr_rpl(t4_ofld_t *of, const struct cpl_fw6_msg *cpl)
{
	const uint64_t cookie = BE_64(cpl->data[1]);
	const int status = (BE_64(cpl->data[0]) >> 8) & 0xff;
	t4_ofld_waiter_t *w;

	mutex_enter(&of->of_wlock);
	w = t4_ofld_waiter_find(of, cookie);
	if (w == NULL || w->ow_state == TWS_FREE || w->ow_state == TWS_DONE) {
		mutex_exit(&of->of_wlock);
		T4_OFLD_STAT(of, os_wr_badcookie);
		return;
	}
	if (w->ow_state == TWS_ABANDONED) {
		w->ow_state = TWS_FREE;
	} else {
		w->ow_state = TWS_DONE;
		w->ow_status = status == 0 ? 0 : EIO;
		cv_broadcast(&of->of_wcv);
	}
	mutex_exit(&of->of_wlock);
}

/*
 * Enter the client for one CPL.  Returns B_FALSE when there is no client to
 * deliver to; *genp is then 0, which no entry is owned by.
 */
static boolean_t
t4_ofld_cl_enter(t4_ofld_t *of, uint32_t *genp,
    const t4_rdma_client_t **clp, void **argp)
{
	mutex_enter(&of->of_lock);
	if (!t4_ofld_client_ok(of)) {
		mutex_exit(&of->of_lock);
		*genp = 0;
		return (B_FALSE);
	}
	*genp = of->of_client_gen;
	*clp = of->of_client;
	*argp = of->of_client_arg;
	of->of_cb_busy++;
	mutex_exit(&of->of_lock);
	return (B_TRUE);
}

static void
t4_ofld_cl_exit(t4_ofld_t *of)
{
	mutex_enter(&of->of_lock);
	if (--of->of_cb_busy == 0)
		cv_broadcast(&of->of_cv);
	mutex_exit(&of->of_lock);
}

static void
t4_ofld_cl_call(const t4_rdma_client_t *cl, void *arg, uint8_t port,
    t4_rdma_queue_t q, uint8_t opcode, uint32_t tid, uint32_t ltid, void *ctx,
    mblk_t *mp)
{
	t4_rdma_cpl_t cpl;

	cpl.trc_opcode = opcode;
	cpl.trc_port = port;
	cpl.trc_queue = q;
	cpl.trc_tid = tid;
	cpl.trc_ltid = ltid;
	cpl.trc_ctx = ctx;
	cpl.trc_mp = mp;
	cl->trcl_cpl(arg, &cpl);
}

/* Whether an entry belongs to a client that has left. */
static boolean_t
t4_ofld_orphaned(const t4_tid_ent_t *e, uint32_t gen)
{
	if ((e->te_flags & TEF_RELEASING) != 0)
		return (B_FALSE);
	return (e->te_state == TTS_ORPHAN ||
	    (e->te_state == TTS_OWNED && e->te_owner != gen));
}

/* Give a TID nobody will own back to the chip. */
static void
t4_ofld_refuse_tid(t4_ofld_t *of, uint8_t port, uint16_t rxq, uint32_t tid)
{
	if (t4_hwtid_claim(of, tid, TTS_ORPHAN, 0, port, rxq, TEF_EMBRYO,
	    NULL) != 0) {
		T4_OFLD_STAT(of, os_cpl_badid);
		return;
	}
	mutex_enter(&of->of_tids.td_lock);
	t4_ofld_orphan_release_locked(of, port, tid);
	mutex_exit(&of->of_tids.td_lock);
}

static void
t4_ofld_cpl_stid(t4_ofld_t *of, t4_rdma_queue_t q, uint8_t opcode,
    mblk_t *mp)
{
	const uint32_t stid = GET_TID((const struct cpl_pass_open_rpl *)
	    mp->b_rptr);
	const uint8_t status = opcode == CPL_PASS_OPEN_RPL ?
	    ((const struct cpl_pass_open_rpl *)mp->b_rptr)->status :
	    ((const struct cpl_close_listsvr_rpl *)mp->b_rptr)->status;
	const t4_rdma_client_t *cl;
	t4_tid_ent_t *e;
	uint32_t gen;
	void *arg, *ctx = NULL;
	boolean_t live;
	uint8_t port;

	live = t4_ofld_cl_enter(of, &gen, &cl, &arg);

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_STID, stid);
	if (e == NULL || e->te_state == TTS_FREE) {
		mutex_exit(&of->of_tids.td_lock);
		T4_OFLD_STAT(of, os_cpl_badid);
		goto drop;
	}
	/* Each reply must answer the request t4nex has outstanding. */
	const uint16_t want = opcode == CPL_PASS_OPEN_RPL ? TEF_OPEN :
	    TEF_UNLISTEN;
	if ((e->te_flags & want) == 0) {
		mutex_exit(&of->of_tids.td_lock);
		T4_OFLD_STAT(of, os_cpl_mismatch);
		goto drop;
	}
	e->te_flags &= ~want;
	if (opcode == CPL_PASS_OPEN_RPL ? status != CPL_ERR_NONE :
	    status == CPL_ERR_NONE)
		e->te_flags &= ~TEF_LISTEN;

	if (t4_ofld_orphaned(e, gen)) {
		e->te_state = TTS_ORPHAN;
		if ((e->te_flags & TEF_STID_BUSY) == 0)
			t4_tid_free_locked(of, T4_TID_STID, stid);
		else if (opcode == CPL_CLOSE_LISTSRV_RPL)
			t4_ofld_retry_arm_locked(of);
		else
			t4_ofld_orphan_unlisten_locked(of, e, stid);
		mutex_exit(&of->of_tids.td_lock);
		goto drop;
	}
	port = e->te_port;
	mutex_exit(&of->of_tids.td_lock);

	if (!live || t4_tid_hold(of, T4_TID_STID, stid, gen, UINT16_MAX,
	    &ctx) != 0) {
		T4_OFLD_STAT(of, os_cpl_stale);
		goto drop;
	}
	t4_ofld_cl_call(cl, arg, port, q, opcode, T4_RDMA_TID_NONE, stid, ctx,
	    mp);
	t4_tid_rele(of, T4_TID_STID, stid);
	t4_ofld_cl_exit(of);
	return;
drop:
	freemsg(mp);
	if (live)
		t4_ofld_cl_exit(of);
}

/*
 * The port a SYN arrived on, if it was addressed to that port's own MAC.  A
 * SYN seen only because the port is promiscuous, or sent to a VNIC's MAC,
 * gets no offloaded connection.
 */
static int
t4_ofld_syn_port(t4_ofld_t *of, const struct cpl_pass_accept_req *cpl)
{
	const uint16_t l2info = BE_16(cpl->l2info);
	const uint_t port = G_SYN_INTF(l2info);

	if (port >= of->of_nports || (l2info & F_SYN_XACT_MATCH) == 0 ||
	    G_SYN_MAC_IDX(l2info) !=
	    (uint_t)of->of_port[port].op_pi->xact_addr_filt)
		return (-1);
	return ((int)port);
}

static void
t4_ofld_cpl_pass_accept(t4_ofld_t *of, t4_rdma_queue_t q, mblk_t *mp)
{
	const struct cpl_pass_accept_req *cpl = (const void *)mp->b_rptr;
	const uint32_t tid = GET_TID(cpl);
	const uint32_t stid = G_PASS_OPEN_TID(BE_32(cpl->tos_stid));
	const uint16_t rxq = of->of_rxq.iq.tsi_abs_id;
	const int sport = t4_ofld_syn_port(of, cpl);
	const t4_rdma_client_t *cl;
	t4_tid_ent_t *se;
	uint32_t gen;
	void *arg, *ctx;
	boolean_t live;
	uint8_t port;
	int rc;

	live = t4_ofld_cl_enter(of, &gen, &cl, &arg);

	mutex_enter(&of->of_tids.td_lock);
	se = t4_tid_ent(of, T4_TID_STID, stid);
	if (se == NULL || !live || se->te_state != TTS_OWNED ||
	    se->te_owner != gen || (se->te_flags & TEF_LISTEN) == 0 ||
	    sport < 0 || se->te_port != sport) {
		/* Nobody takes this SYN. */
		mutex_exit(&of->of_tids.td_lock);
		T4_OFLD_STAT(of, os_cpl_stale);
		t4_ofld_refuse_tid(of, sport >= 0 ? sport : 0, rxq, tid);
		goto drop;
	}
	port = se->te_port;
	mutex_exit(&of->of_tids.td_lock);

	if ((rc = t4_hwtid_claim(of, tid, TTS_OWNED, gen, port, rxq,
	    TEF_EMBRYO, NULL)) != 0) {
		if (rc == EAGAIN)
			T4_OFLD_STAT(of, os_syn_refused);
		else
			T4_OFLD_STAT(of, os_cpl_badid);
		if (rc == EAGAIN)
			t4_ofld_refuse_tid(of, port, rxq, tid);
		goto drop;
	}
	if (t4_tid_hold(of, T4_TID_STID, stid, gen, UINT16_MAX, &ctx) != 0) {
		mutex_enter(&of->of_tids.td_lock);
		t4_ofld_orphan_release_locked(of, port, tid);
		mutex_exit(&of->of_tids.td_lock);
		goto drop;
	}
	t4_ofld_cl_call(cl, arg, port, q, CPL_PASS_ACCEPT_REQ, tid, stid, ctx,
	    mp);
	t4_tid_rele(of, T4_TID_STID, stid);
	t4_ofld_cl_exit(of);
	return;
drop:
	freemsg(mp);
	if (live)
		t4_ofld_cl_exit(of);
}

static void
t4_ofld_cpl_act_open_rpl(t4_ofld_t *of, t4_rdma_queue_t q, mblk_t *mp)
{
	const struct cpl_act_open_rpl *cpl = (const void *)mp->b_rptr;
	const uint32_t as = BE_32(cpl->atid_status);
	const uint32_t atid = G_TID_TID(G_AOPEN_ATID(as));
	const uint8_t status = G_AOPEN_STATUS(as);
	const t4_rdma_client_t *cl;
	t4_tid_ent_t *e;
	uint32_t gen;
	void *arg, *ctx;
	boolean_t live;
	uint8_t port;

	live = t4_ofld_cl_enter(of, &gen, &cl, &arg);

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_ATID, atid);
	if (e == NULL || e->te_state == TTS_FREE ||
	    (e->te_flags & TEF_OPEN) == 0) {
		mutex_exit(&of->of_tids.td_lock);
		T4_OFLD_STAT(of, os_cpl_badid);
		goto drop;
	}
	port = e->te_port;
	e->te_flags &= ~TEF_OPEN;
	mutex_exit(&of->of_tids.td_lock);

	/* A failed open can still leave a TID in the chip; free it here. */
	if (status != CPL_ERR_NONE && act_open_has_tid(status) &&
	    t4_hwtid_claim(of, GET_TID(cpl), TTS_ORPHAN, 0, port,
	    of->of_rxq.iq.tsi_abs_id, 0, NULL) == 0) {
		mutex_enter(&of->of_tids.td_lock);
		t4_ofld_orphan_release_locked(of, port, GET_TID(cpl));
		mutex_exit(&of->of_tids.td_lock);
	}

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_ATID, atid);
	if (t4_ofld_orphaned(e, gen)) {
		t4_tid_free_locked(of, T4_TID_ATID, atid);
		mutex_exit(&of->of_tids.td_lock);
		goto drop;
	}
	mutex_exit(&of->of_tids.td_lock);

	if (!live || t4_tid_hold(of, T4_TID_ATID, atid, gen,
	    of->of_rxq.iq.tsi_abs_id, &ctx) != 0) {
		T4_OFLD_STAT(of, os_cpl_stale);
		goto drop;
	}
	t4_ofld_cl_call(cl, arg, port, q, CPL_ACT_OPEN_RPL, T4_RDMA_TID_NONE,
	    atid, ctx, mp);
	t4_tid_rele(of, T4_TID_ATID, atid);
	t4_ofld_cl_exit(of);
	return;
drop:
	freemsg(mp);
	if (live)
		t4_ofld_cl_exit(of);
}

static void
t4_ofld_cpl_act_est(t4_ofld_t *of, t4_rdma_queue_t q, mblk_t *mp)
{
	const struct cpl_act_establish *cpl = (const void *)mp->b_rptr;
	const uint32_t tid = GET_TID(cpl);
	const uint32_t atid = G_TID_TID(G_PASS_OPEN_TID(BE_32(cpl->tos_atid)));
	const uint16_t rxq = of->of_rxq.iq.tsi_abs_id;
	const t4_rdma_client_t *cl;
	t4_tid_ent_t *e;
	uint32_t gen;
	void *arg, *ctx;
	boolean_t live, orphan;
	uint8_t port;

	live = t4_ofld_cl_enter(of, &gen, &cl, &arg);

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_ATID, atid);
	if (e == NULL || e->te_state == TTS_FREE ||
	    (e->te_flags & TEF_OPEN) == 0) {
		mutex_exit(&of->of_tids.td_lock);
		T4_OFLD_STAT(of, os_cpl_badid);
		goto drop;
	}
	port = e->te_port;
	e->te_flags &= ~TEF_OPEN;
	orphan = t4_ofld_orphaned(e, gen);
	if (orphan)
		t4_tid_free_locked(of, T4_TID_ATID, atid);
	mutex_exit(&of->of_tids.td_lock);

	if (orphan) {
		if (t4_hwtid_claim(of, tid, TTS_ORPHAN, 0, port, rxq, 0,
		    NULL) == 0) {
			mutex_enter(&of->of_tids.td_lock);
			e = t4_tid_ent(of, T4_TID_HW, tid);
			t4_ofld_orphan_abort_locked(of, e, tid);
			mutex_exit(&of->of_tids.td_lock);
		} else {
			T4_OFLD_STAT(of, os_cpl_badid);
		}
		goto drop;
	}

	if (t4_hwtid_claim(of, tid, TTS_OWNED, gen, port, rxq, 0,
	    NULL) != 0) {
		T4_OFLD_STAT(of, os_cpl_badid);
		goto drop;
	}
	if (t4_tid_hold(of, T4_TID_ATID, atid, gen, rxq, &ctx) != 0) {
		mutex_enter(&of->of_tids.td_lock);
		t4_ofld_orphan_abort_locked(of, t4_tid_ent(of, T4_TID_HW, tid),
		    tid);
		mutex_exit(&of->of_tids.td_lock);
		goto drop;
	}
	t4_ofld_cl_call(cl, arg, port, q, CPL_ACT_ESTABLISH, tid, atid, ctx,
	    mp);
	t4_tid_rele(of, T4_TID_ATID, atid);
	t4_ofld_cl_exit(of);
	return;
drop:
	freemsg(mp);
	if (live)
		t4_ofld_cl_exit(of);
}

static void
t4_ofld_cpl_hwtid(t4_ofld_t *of, t4_rdma_queue_t q, uint8_t opcode,
    mblk_t *mp)
{
	const uint32_t tid = GET_TID((const struct cpl_peer_close *)
	    mp->b_rptr);
	const uint16_t rxq = of->of_rxq.iq.tsi_abs_id;
	const t4_rdma_client_t *cl;
	t4_tid_ent_t *e;
	uint32_t gen;
	void *arg, *ctx;
	boolean_t live;
	uint8_t port;
	int rc;

	live = t4_ofld_cl_enter(of, &gen, &cl, &arg);

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_HW, tid);
	if (e == NULL || e->te_state == TTS_FREE) {
		mutex_exit(&of->of_tids.td_lock);
		if (e == NULL)
			T4_OFLD_STAT(of, os_cpl_badid);
		else
			T4_OFLD_STAT(of, os_cpl_stale);
		goto drop;
	}
	if (t4_ofld_orphaned(e, gen)) {
		if (opcode == CPL_ABORT_RPL_RSS ||
		    (e->te_flags & TEF_RELPEND) != 0) {
			t4_ofld_orphan_release_locked(of, e->te_port, tid);
		} else if (opcode == CPL_ABORT_REQ_RSS &&
		    !t4_cpl_neg_advice(((const struct cpl_abort_req_rss *)
		    mp->b_rptr)->status)) {
			(void) t4_ofld_send_abort_rpl(of, e->te_port, tid,
			    B_FALSE);
			if ((e->te_flags & TEF_ABORT) == 0)
				t4_ofld_orphan_release_locked(of, e->te_port,
				    tid);
		} else {
			t4_ofld_orphan_abort_locked(of, e, tid);
		}
		mutex_exit(&of->of_tids.td_lock);
		goto drop;
	}
	port = e->te_port;
	mutex_exit(&of->of_tids.td_lock);

	if (opcode == CPL_ABORT_RPL_RSS || (opcode == CPL_ABORT_REQ_RSS &&
	    !t4_cpl_neg_advice(((const struct cpl_abort_req_rss *)
	    mp->b_rptr)->status)))
		t4_ofld_ri_gone(of, tid);

	if ((rc = t4_tid_hold(of, T4_TID_HW, tid, gen, rxq, &ctx)) != 0) {
		if (rc == EXDEV)
			T4_OFLD_STAT(of, os_cpl_wrongq);
		else
			T4_OFLD_STAT(of, os_cpl_stale);
		goto drop;
	}
	t4_ofld_cl_call(cl, arg, port, q, opcode, tid, T4_RDMA_TID_NONE, ctx,
	    mp);
	t4_tid_rele(of, T4_TID_HW, tid);
	t4_ofld_cl_exit(of);
	return;
drop:
	freemsg(mp);
	if (live)
		t4_ofld_cl_exit(of);
}

static void t4_ofld_cpl_dispatch_one(t4_ofld_t *, t4_rdma_queue_t,
    uint8_t, mblk_t *, boolean_t);

static void
t4_ofld_cpl_fw(t4_ofld_t *of, t4_rdma_queue_t q, uint8_t opcode,
    mblk_t *mp, boolean_t nested)
{
	const struct cpl_fw6_msg *cpl = (const void *)mp->b_rptr;
	const t4_rdma_client_t *cl;
	uint32_t gen;
	void *arg;

	switch (cpl->type) {
	case FW6_TYPE_WR_RPL:
		if (opcode != CPL_FW6_MSG || MBLKL(mp) < sizeof (*cpl))
			break;
		t4_ofld_wr_rpl(of, cpl);
		break;
	case FW6_TYPE_RSSCPL: {
		const struct rss_header *rss2 = (const void *)&cpl->data[0];
		const uint8_t op2 = rss2->opcode;

		/* The CPL follows the inner RSS header. */
		if (nested || op2 == CPL_FW4_MSG || op2 == CPL_FW6_MSG ||
		    MBLKL(mp) <= offsetof(struct cpl_fw6_msg, data[1]))
			break;
		mp->b_rptr += offsetof(struct cpl_fw6_msg, data[1]);
		t4_ofld_cpl_dispatch_one(of, q, op2, mp, B_TRUE);
		return;
	}
	case FW6_TYPE_CQE:
		if (opcode != CPL_FW6_MSG || q != T4_RDMA_Q_CIQ) {
			T4_OFLD_STAT(of, os_cpl_wrongq);
			break;
		}
		if (!t4_ofld_cl_enter(of, &gen, &cl, &arg)) {
			T4_OFLD_STAT(of, os_cpl_stale);
			break;
		}
		t4_ofld_cl_call(cl, arg, 0, q, opcode, T4_RDMA_TID_NONE,
		    T4_RDMA_TID_NONE, NULL, mp);
		t4_ofld_cl_exit(of);
		return;
	default:
		T4_OFLD_STAT(of, os_cpl_unknown);
		break;
	}
	freemsg(mp);
}

static void
t4_ofld_cpl_dispatch_one(t4_ofld_t *of, t4_rdma_queue_t q,
    uint8_t opcode, mblk_t *mp, boolean_t nested)
{
	const t4_cpl_desc_t *d;

	if (opcode >= NUM_CPL_CMDS ||
	    (d = &t4_cpl_table[opcode])->tcd_class == TCC_NONE) {
		T4_OFLD_STAT(of, os_cpl_unknown);
		freemsg(mp);
		return;
	}
	if (MBLKL(mp) < d->tcd_minlen && !pullupmsg(mp, d->tcd_minlen)) {
		T4_OFLD_STAT(of, os_cpl_short);
		freemsg(mp);
		return;
	}
	if (mp->b_rptr[0] != opcode) {
		T4_OFLD_STAT(of, os_cpl_mismatch);
		freemsg(mp);
		return;
	}
	if (q != T4_RDMA_Q_RX && d->tcd_class != TCC_FW) {
		T4_OFLD_STAT(of, os_cpl_wrongq);
		freemsg(mp);
		return;
	}

	switch (d->tcd_class) {
	case TCC_L2T:
		t4_l2t_write_rpl(of, (const void *)mp->b_rptr);
		freemsg(mp);
		break;
	case TCC_STID:
		t4_ofld_cpl_stid(of, q, opcode, mp);
		break;
	case TCC_PASS_ACCEPT:
		t4_ofld_cpl_pass_accept(of, q, mp);
		break;
	case TCC_ACT_OPEN_RPL:
		t4_ofld_cpl_act_open_rpl(of, q, mp);
		break;
	case TCC_ACT_EST:
		t4_ofld_cpl_act_est(of, q, mp);
		break;
	case TCC_HWTID:
		t4_ofld_cpl_hwtid(of, q, opcode, mp);
		break;
	case TCC_FW:
		t4_ofld_cpl_fw(of, q, opcode, mp, nested);
		break;
	default:
		freemsg(mp);
		break;
	}
}

/*
 * Hand one CPL from an offload queue to its owner.  Interrupt context, no
 * t4nex lock held; mp starts with the CPL and is consumed.
 */
void
t4_ofld_cpl_dispatch(t4_ofld_t *of, t4_rdma_queue_t q, uint8_t opcode,
    mblk_t *mp)
{
	t4_ofld_cpl_dispatch_one(of, q, opcode, mp, B_FALSE);
}

/*
 * Tell the client which RDMA CQs have new entries.  The IDs come from the
 * device; one outside the RDMA CQ range is dropped.
 */
void
t4_ofld_cq_notify(t4_ofld_t *of, const uint32_t *cqs, uint_t n)
{
	const t4_rdma_range_t *r = &of->of_vres.trv_cq;
	const t4_rdma_client_t *cl;
	uint32_t gen;
	void *arg;

	if (!t4_ofld_cl_enter(of, &gen, &cl, &arg)) {
		T4_OFLD_STAT(of, os_cpl_stale);
		return;
	}
	for (uint_t i = 0; i < n; i++) {
		if (cqs[i] < r->trr_start ||
		    cqs[i] - r->trr_start >= r->trr_size) {
			T4_OFLD_STAT(of, os_cq_badid);
			continue;
		}
		T4_OFLD_STAT(of, os_cq_notify);
		cl->trcl_cq(arg, cqs[i]);
	}
	t4_ofld_cl_exit(of);
}

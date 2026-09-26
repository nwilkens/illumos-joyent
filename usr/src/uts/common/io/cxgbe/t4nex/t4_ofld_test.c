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
 * A test client of the offload core, driven by T4_IOCTL_OFLD_TEST, so that
 * the core can be exercised on hardware without an RDMA provider.  It uses
 * only the operations the RDMA child uses.  A listener it opens accepts every
 * SYN, answers FIN with FIN, and releases each connection when both sides
 * are closed.  CPLs are queued from interrupt context and handled on a
 * taskq, one at a time.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/strsun.h>
#include <sys/ethernet.h>
#include <sys/vlan.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "t4nex.h"
#include "t4_ofld.h"

#define	T4_OT_RCV_WIN		(256 * 1024)
#define	T4_OT_MAX_TIMEOUT_MS	30000
#define	T4_OT_QMAX		256

typedef struct t4_ot_conn {
	boolean_t	oc_used;
	uint32_t	oc_tid;
	uint32_t	oc_l2t;
	uint8_t		oc_port;
	uint32_t	oc_flags;
	uint32_t	oc_snd_isn;
	uint32_t	oc_rcv_isn;
	uint64_t	oc_rx_bytes;
	uint64_t	oc_tx_bytes;
} t4_ot_conn_t;

typedef struct t4_ot {
	t4_ofld_t	*ot_of;
	t4_rdma_client_t ot_client;
	ddi_taskq_t	*ot_tq;

	kmutex_t	ot_qlock;	/* interrupt priority */
	mblk_t		*ot_qhead;
	mblk_t		*ot_qtail;
	uint_t		ot_qlen;
	boolean_t	ot_qrun;

	kmutex_t	ot_lock;
	kcondvar_t	ot_cv;
	boolean_t	ot_busy;
	uint8_t		ot_want_op;
	uint32_t	ot_want_id;
	boolean_t	ot_got;
	int		ot_got_status;
	uint32_t	ot_got_tid;
	uint32_t	ot_got_snd_isn;
	uint32_t	ot_got_rcv_isn;
	uint32_t	ot_listen_stid;
	uint8_t		ot_listen_port;
	uint32_t	ot_accepts;
	uint32_t	ot_refused;
	uint32_t	ot_events;
	t4_ot_conn_t	ot_conn[T4_OFLD_TEST_NCONN];
} t4_ot_t;

/* The CPL fields carried in front of each queued message. */
typedef struct t4_ot_msg {
	uint8_t		om_opcode;
	uint8_t		om_port;
	uint32_t	om_tid;
	uint32_t	om_ltid;
} t4_ot_msg_t;

static void t4_ot_task(void *);

static void
t4_ot_event(void *arg, const t4_rdma_event_t *ev)
{
	t4_ot_t *ot = arg;

	mutex_enter(&ot->ot_lock);
	ot->ot_events++;
	mutex_exit(&ot->ot_lock);
}

static void
t4_ot_cq(void *arg, uint32_t cq)
{
	_NOTE(ARGUNUSED(arg, cq));
}

/* A SYN the test cannot queue gets its TID back to the chip at once. */
static void
t4_ot_drop(t4_ot_t *ot, const t4_rdma_cpl_t *cpl)
{
	if (cpl->trc_opcode == CPL_PASS_ACCEPT_REQ)
		(void) t4_ofld_tid_release(ot->ot_of, cpl->trc_tid);
}

static void
t4_ot_cpl(void *arg, t4_rdma_cpl_t *cpl)
{
	t4_ot_t *ot = arg;
	t4_ot_msg_t *om;
	mblk_t *hdr;
	boolean_t run = B_FALSE;

	if ((hdr = allocb(sizeof (*om), BPRI_HI)) == NULL) {
		t4_ot_drop(ot, cpl);
		freemsg(cpl->trc_mp);
		return;
	}
	om = (t4_ot_msg_t *)hdr->b_wptr;
	om->om_opcode = cpl->trc_opcode;
	om->om_port = cpl->trc_port;
	om->om_tid = cpl->trc_tid;
	om->om_ltid = cpl->trc_ltid;
	hdr->b_wptr += sizeof (*om);
	hdr->b_cont = cpl->trc_mp;

	mutex_enter(&ot->ot_qlock);
	if (ot->ot_qlen >= T4_OT_QMAX) {
		mutex_exit(&ot->ot_qlock);
		t4_ot_drop(ot, cpl);
		freemsg(hdr);
		return;
	}
	if (ot->ot_qtail == NULL)
		ot->ot_qhead = hdr;
	else
		ot->ot_qtail->b_next = hdr;
	ot->ot_qtail = hdr;
	ot->ot_qlen++;
	if (!ot->ot_qrun) {
		ot->ot_qrun = B_TRUE;
		run = B_TRUE;
	}
	mutex_exit(&ot->ot_qlock);

	if (run && ddi_taskq_dispatch(ot->ot_tq, t4_ot_task, ot,
	    DDI_NOSLEEP) != DDI_SUCCESS) {
		mutex_enter(&ot->ot_qlock);
		ot->ot_qrun = B_FALSE;
		mutex_exit(&ot->ot_qlock);
	}
}

static t4_ot_conn_t *
t4_ot_conn(t4_ot_t *ot, uint32_t tid)
{
	ASSERT(MUTEX_HELD(&ot->ot_lock));
	for (uint_t i = 0; i < T4_OFLD_TEST_NCONN; i++) {
		if (ot->ot_conn[i].oc_used && ot->ot_conn[i].oc_tid == tid)
			return (&ot->ot_conn[i]);
	}
	return (NULL);
}

static t4_ot_conn_t *
t4_ot_conn_new(t4_ot_t *ot, uint32_t tid, uint8_t port, uint32_t l2t)
{
	ASSERT(MUTEX_HELD(&ot->ot_lock));
	for (uint_t i = 0; i < T4_OFLD_TEST_NCONN; i++) {
		t4_ot_conn_t *c = &ot->ot_conn[i];

		if (c->oc_used && (c->oc_flags & T4_OFLD_TCF_RELEASED) == 0)
			continue;
		bzero(c, sizeof (*c));
		c->oc_used = B_TRUE;
		c->oc_tid = tid;
		c->oc_port = port;
		c->oc_l2t = l2t;
		return (c);
	}
	return (NULL);
}

static void
t4_ot_release(t4_ot_t *ot, t4_ot_conn_t *c)
{
	ASSERT(MUTEX_HELD(&ot->ot_lock));
	if ((c->oc_flags & T4_OFLD_TCF_RELEASED) != 0)
		return;
	(void) t4_ofld_tid_release(ot->ot_of, c->oc_tid);
	t4_l2t_put(ot->ot_of, c->oc_l2t);
	c->oc_flags |= T4_OFLD_TCF_RELEASED;
}

static void
t4_ot_flowc(t4_ot_t *ot, t4_ot_conn_t *c)
{
	t4_ofld_t *of = ot->ot_of;
	const uint32_t mtu = of->of_port[c->oc_port].op_pi->mtu;
	t4_rdma_flowc_t f = {
		.trf_snd_nxt = c->oc_snd_isn,
		.trf_rcv_nxt = c->oc_rcv_isn,
		.trf_sndbuf = T4_OT_RCV_WIN,
		.trf_mss = mtu > 40 ? mtu - 40 : 536,
		.trf_rcv_scale = 3
	};

	(void) t4_ofld_flowc(of, c->oc_tid, &f);
}

/*
 * The peer's MAC for the accept, from the SYN headers after the CPL.  Every
 * length comes from the device and is bounded.
 */
static boolean_t
t4_ot_syn_mac(t4_ofld_t *of, mblk_t *mp, uint8_t *mac, uint16_t *vlan)
{
	const struct cpl_pass_accept_req *req;
	const size_t off = sizeof (*req);
	uint32_t hl;
	uint_t eth, ip, tcp;
	const uint8_t *p;

	if (msgdsize(mp) < off + sizeof (struct ether_header) ||
	    !pullupmsg(mp, -1))
		return (B_FALSE);
	req = (const void *)mp->b_rptr;
	hl = BE_32(req->hdr_len);
	if (t4_cver_ge(of->of_sc, CHELSIO_T6)) {
		eth = G_T6_ETH_HDR_LEN(hl);
		ip = G_T6_IP_HDR_LEN(hl);
		tcp = G_T6_TCP_HDR_LEN(hl);
	} else {
		eth = G_ETH_HDR_LEN(hl);
		ip = G_IP_HDR_LEN(hl);
		tcp = G_TCP_HDR_LEN(hl);
	}
	if (eth < sizeof (struct ether_header) ||
	    eth > sizeof (struct ether_vlan_header) ||
	    ip < sizeof (struct ip) || tcp < sizeof (struct tcphdr) ||
	    MBLKL(mp) < off + eth + ip + tcp)
		return (B_FALSE);
	p = mp->b_rptr + off;
	bcopy(p + ETHERADDRL, mac, ETHERADDRL);
	if (eth == sizeof (struct ether_vlan_header)) {
		*vlan = ((p[14] << 8) | p[15]) & VLAN_ID_MASK;
	} else {
		*vlan = CPL_L2T_VLAN_NONE;
	}
	return (B_TRUE);
}

static void
t4_ot_pass_accept(t4_ot_t *ot, const t4_ot_msg_t *om, mblk_t *mp)
{
	t4_ofld_t *of = ot->ot_of;
	t4_rdma_accept_t a;
	uint8_t mac[ETHERADDRL];
	uint16_t vlan;
	uint32_t l2t;
	t4_ot_conn_t *c;

	bzero(&a, sizeof (a));
	if (!t4_ot_syn_mac(of, mp, mac, &vlan) ||
	    t4_l2t_get(of, om->om_port, vlan, mac, &l2t) != 0) {
		(void) t4_ofld_tid_release(of, om->om_tid);
		ot->ot_refused++;
		return;
	}
	if ((c = t4_ot_conn_new(ot, om->om_tid, om->om_port, l2t)) == NULL) {
		t4_l2t_put(of, l2t);
		(void) t4_ofld_tid_release(of, om->om_tid);
		ot->ot_refused++;
		return;
	}
	c->oc_flags = T4_OFLD_TCF_PASSIVE;
	a.trac_tid = om->om_tid;
	a.trac_port = om->om_port;
	a.trac_l2t = l2t;
	a.trac_opts.trt_rcv_win = T4_OT_RCV_WIN;
	a.trac_opts.trt_mtu_idx = NMTUS - 1;
	a.trac_opts.trt_ulp_mode = ULP_MODE_NONE;
	while (a.trac_opts.trt_mtu_idx > 0 &&
	    of->of_sc->params.mtus[a.trac_opts.trt_mtu_idx] >
	    of->of_port[om->om_port].op_pi->mtu)
		a.trac_opts.trt_mtu_idx--;
	if (t4_ofld_accept(of, &a) != 0) {
		t4_ot_release(ot, c);
		ot->ot_refused++;
		return;
	}
	ot->ot_accepts++;
}

static void
t4_ot_handle(t4_ot_t *ot, const t4_ot_msg_t *om, mblk_t *mp)
{
	t4_ofld_t *of = ot->ot_of;
	t4_ot_conn_t *c = NULL;
	boolean_t wake = B_FALSE;

	mutex_enter(&ot->ot_lock);
	if (om->om_tid != T4_RDMA_TID_NONE)
		c = t4_ot_conn(ot, om->om_tid);

	switch (om->om_opcode) {
	case CPL_PASS_OPEN_RPL: {
		const struct cpl_pass_open_rpl *r = (const void *)mp->b_rptr;

		if (ot->ot_want_op == om->om_opcode &&
		    ot->ot_want_id == om->om_ltid) {
			ot->ot_got_status = r->status;
			wake = B_TRUE;
		}
		break;
	}
	case CPL_CLOSE_LISTSRV_RPL: {
		const struct cpl_close_listsvr_rpl *r =
		    (const void *)mp->b_rptr;

		if (ot->ot_want_op == om->om_opcode &&
		    ot->ot_want_id == om->om_ltid) {
			ot->ot_got_status = r->status;
			wake = B_TRUE;
		}
		break;
	}
	case CPL_PASS_ACCEPT_REQ:
		t4_ot_pass_accept(ot, om, mp);
		break;
	case CPL_PASS_ESTABLISH: {
		const struct cpl_pass_establish *r = (const void *)mp->b_rptr;

		if (c != NULL) {
			c->oc_flags |= T4_OFLD_TCF_EST;
			c->oc_snd_isn = BE_32(r->snd_isn);
			c->oc_rcv_isn = BE_32(r->rcv_isn);
			t4_ot_flowc(ot, c);
		}
		break;
	}
	case CPL_ACT_ESTABLISH: {
		const struct cpl_act_establish *r = (const void *)mp->b_rptr;

		if (ot->ot_want_op == om->om_opcode &&
		    ot->ot_want_id == om->om_ltid) {
			ot->ot_got_status = 0;
			ot->ot_got_tid = om->om_tid;
			ot->ot_got_snd_isn = BE_32(r->snd_isn);
			ot->ot_got_rcv_isn = BE_32(r->rcv_isn);
			wake = B_TRUE;
		} else {
			(void) t4_ofld_abort(of, om->om_tid, B_TRUE);
		}
		break;
	}
	case CPL_ACT_OPEN_RPL: {
		const struct cpl_act_open_rpl *r = (const void *)mp->b_rptr;

		if (ot->ot_want_op == CPL_ACT_ESTABLISH &&
		    ot->ot_want_id == om->om_ltid) {
			ot->ot_got_status = G_AOPEN_STATUS(BE_32(
			    r->atid_status));
			ot->ot_got_tid = T4_RDMA_TID_NONE;
			wake = B_TRUE;
		}
		break;
	}
	case CPL_RX_DATA: {
		const struct cpl_rx_data *r = (const void *)mp->b_rptr;
		const uint32_t len = BE_16(r->len);

		if (c != NULL && msgdsize(mp) >= sizeof (*r) + len) {
			c->oc_rx_bytes += len;
			if (len != 0)
				(void) t4_ofld_rx_credits(of, c->oc_tid, len);
		}
		break;
	}
	case CPL_PEER_CLOSE:
		if (c != NULL) {
			c->oc_flags |= T4_OFLD_TCF_PEER_FIN;
			if ((c->oc_flags & T4_OFLD_TCF_FIN_SENT) == 0 &&
			    t4_ofld_close_con(of, c->oc_tid) == 0)
				c->oc_flags |= T4_OFLD_TCF_FIN_SENT;
			if ((c->oc_flags & T4_OFLD_TCF_FIN_ACKED) != 0)
				t4_ot_release(ot, c);
		}
		wake = ot->ot_want_op == CPL_CLOSE_CON_RPL;
		break;
	case CPL_CLOSE_CON_RPL:
		if (c != NULL) {
			c->oc_flags |= T4_OFLD_TCF_FIN_ACKED;
			if ((c->oc_flags & T4_OFLD_TCF_PEER_FIN) != 0)
				t4_ot_release(ot, c);
		}
		wake = ot->ot_want_op == CPL_CLOSE_CON_RPL;
		break;
	case CPL_ABORT_REQ_RSS: {
		const struct cpl_abort_req_rss *r = (const void *)mp->b_rptr;

		if (r->status == CPL_ERR_RTX_NEG_ADVICE ||
		    r->status == CPL_ERR_PERSIST_NEG_ADVICE ||
		    r->status == CPL_ERR_KEEPALV_NEG_ADVICE)
			break;
		(void) t4_ofld_abort_rpl(of, om->om_tid, B_FALSE);
		if (c != NULL) {
			c->oc_flags |= T4_OFLD_TCF_ABORTED;
			t4_ot_release(ot, c);
		} else {
			(void) t4_ofld_tid_release(of, om->om_tid);
		}
		wake = B_TRUE;
		break;
	}
	case CPL_ABORT_RPL_RSS:
		if (c != NULL) {
			c->oc_flags |= T4_OFLD_TCF_ABORTED;
			t4_ot_release(ot, c);
		} else {
			(void) t4_ofld_tid_release(of, om->om_tid);
		}
		wake = ot->ot_want_op == CPL_ABORT_RPL_RSS;
		break;
	default:
		break;
	}
	if (wake) {
		ot->ot_got = B_TRUE;
		cv_broadcast(&ot->ot_cv);
	}
	mutex_exit(&ot->ot_lock);
}

static void
t4_ot_task(void *arg)
{
	t4_ot_t *ot = arg;
	mblk_t *hdr;

	for (;;) {
		mutex_enter(&ot->ot_qlock);
		if ((hdr = ot->ot_qhead) == NULL) {
			ot->ot_qtail = NULL;
			ot->ot_qrun = B_FALSE;
			mutex_exit(&ot->ot_qlock);
			return;
		}
		ot->ot_qhead = hdr->b_next;
		ot->ot_qlen--;
		mutex_exit(&ot->ot_qlock);

		hdr->b_next = NULL;
		t4_ot_handle(ot, (const t4_ot_msg_t *)hdr->b_rptr, hdr->b_cont);
		freemsg(hdr);
	}
}

/*
 * Wait for the reply the current operation expects.  ot_lock is held and
 * ot_want_op and ot_want_id were set before the request was sent.
 */
static int
t4_ot_wait(t4_ot_t *ot, uint32_t ms)
{
	const clock_t deadline = ddi_get_lbolt() + drv_usectohz(
	    (clock_t)MIN(MAX(ms, 1), T4_OT_MAX_TIMEOUT_MS) * 1000);

	ASSERT(MUTEX_HELD(&ot->ot_lock));
	while (!ot->ot_got) {
		if (cv_timedwait(&ot->ot_cv, &ot->ot_lock, deadline) == -1)
			return (ETIMEDOUT);
	}
	return (0);
}

static void
t4_ot_want(t4_ot_t *ot, uint8_t opcode, uint32_t id)
{
	ASSERT(MUTEX_HELD(&ot->ot_lock));
	ot->ot_want_op = opcode;
	ot->ot_want_id = id;
	ot->ot_got = B_FALSE;
	ot->ot_got_status = -1;
}

static int
t4_ot_open(t4_ofld_t *of)
{
	t4_ot_t *ot;
	int rc;

	if (of->of_test != NULL)
		return (EBUSY);
	ot = kmem_zalloc(sizeof (*ot), KM_SLEEP);
	ot->ot_of = of;
	ot->ot_client.trcl_event = t4_ot_event;
	ot->ot_client.trcl_cpl = t4_ot_cpl;
	ot->ot_client.trcl_cq = t4_ot_cq;
	ot->ot_listen_stid = T4_TID_NIL;
	mutex_init(&ot->ot_qlock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(of->of_sc->intr_pri));
	mutex_init(&ot->ot_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ot->ot_cv, NULL, CV_DRIVER, NULL);
	ot->ot_tq = ddi_taskq_create(of->of_sc->dip, "t4_ofld_test", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (ot->ot_tq == NULL) {
		rc = ENOMEM;
		goto fail;
	}
	if ((rc = t4_ofld_client_open(of, &ot->ot_client, ot, B_TRUE)) != 0)
		goto fail;
	of->of_test = ot;
	return (0);
fail:
	if (ot->ot_tq != NULL)
		ddi_taskq_destroy(ot->ot_tq);
	cv_destroy(&ot->ot_cv);
	mutex_destroy(&ot->ot_lock);
	mutex_destroy(&ot->ot_qlock);
	kmem_free(ot, sizeof (*ot));
	return (rc);
}

void
t4_ofld_test_fini(t4_ofld_t *of)
{
	t4_ot_t *ot = of->of_test;
	mblk_t *mp;

	if (ot == NULL)
		return;
	t4_ofld_client_close(of);
	ddi_taskq_wait(ot->ot_tq);
	ddi_taskq_destroy(ot->ot_tq);
	while ((mp = ot->ot_qhead) != NULL) {
		ot->ot_qhead = mp->b_next;
		mp->b_next = NULL;
		freemsg(mp);
	}
	cv_destroy(&ot->ot_cv);
	mutex_destroy(&ot->ot_lock);
	mutex_destroy(&ot->ot_qlock);
	kmem_free(ot, sizeof (*ot));
	of->of_test = NULL;
}

static int
t4_ot_listen(t4_ot_t *ot, t4_ofld_test_t *t)
{
	t4_ofld_t *of = ot->ot_of;
	t4_rdma_listen_t l;
	uint32_t gen, stid;
	int rc;

	if (ot->ot_listen_stid != T4_TID_NIL)
		return (EBUSY);
	mutex_enter(&of->of_lock);
	gen = of->of_client_gen;
	mutex_exit(&of->of_lock);
	if ((rc = t4_stid_alloc(of, gen, AF_INET, NULL, &stid)) != 0)
		return (rc);

	bzero(&l, sizeof (l));
	l.trl_port = t->tot_port;
	l.trl_family = AF_INET;
	IN6_IPADDR_TO_V4MAPPED(t->tot_laddr, &l.trl_laddr);
	l.trl_lport = t->tot_lport;
	l.trl_stid = stid;

	mutex_enter(&ot->ot_lock);
	t4_ot_want(ot, CPL_PASS_OPEN_RPL, stid);
	if ((rc = t4_ofld_listen(of, &l)) == 0)
		rc = t4_ot_wait(ot, t->tot_timeout_ms);
	t->tot_status = ot->ot_got_status;
	ot->ot_want_op = 0;
	if (rc == 0 && t->tot_status == CPL_ERR_NONE) {
		ot->ot_listen_stid = stid;
		ot->ot_listen_port = t->tot_port;
		t->tot_id = stid;
	}
	mutex_exit(&ot->ot_lock);

	if (ot->ot_listen_stid != stid) {
		t4_ofld_stid_free(of, stid);
		if (rc == 0)
			rc = EIO;
	}
	return (rc);
}

static int
t4_ot_unlisten(t4_ot_t *ot, t4_ofld_test_t *t)
{
	t4_ofld_t *of = ot->ot_of;
	const uint32_t stid = ot->ot_listen_stid;
	int rc;

	if (stid == T4_TID_NIL)
		return (ENOENT);
	mutex_enter(&ot->ot_lock);
	t4_ot_want(ot, CPL_CLOSE_LISTSRV_RPL, stid);
	if ((rc = t4_ofld_unlisten(of, stid)) == 0)
		rc = t4_ot_wait(ot, t->tot_timeout_ms);
	t->tot_status = ot->ot_got_status;
	ot->ot_want_op = 0;
	mutex_exit(&ot->ot_lock);

	t4_ofld_stid_free(of, stid);
	ot->ot_listen_stid = T4_TID_NIL;
	t->tot_id = stid;
	return (rc);
}

static int
t4_ot_connect(t4_ot_t *ot, t4_ofld_test_t *t)
{
	t4_ofld_t *of = ot->ot_of;
	t4_rdma_act_open_t a;
	t4_ot_conn_t *c;
	uint32_t gen, atid, l2t;
	int rc;

	if (t->tot_port >= of->of_nports)
		return (EINVAL);
	if ((rc = t4_l2t_get(of, t->tot_port, t->tot_vlan, t->tot_dmac,
	    &l2t)) != 0)
		return (rc);
	mutex_enter(&of->of_lock);
	gen = of->of_client_gen;
	mutex_exit(&of->of_lock);
	if ((rc = t4_atid_alloc(of, gen, NULL, &atid)) != 0) {
		t4_l2t_put(of, l2t);
		return (rc);
	}

	bzero(&a, sizeof (a));
	a.trao_port = t->tot_port;
	a.trao_family = AF_INET;
	IN6_IPADDR_TO_V4MAPPED(t->tot_laddr, &a.trao_laddr);
	IN6_IPADDR_TO_V4MAPPED(t->tot_faddr, &a.trao_faddr);
	a.trao_lport = t->tot_lport;
	a.trao_fport = t->tot_fport;
	a.trao_atid = atid;
	a.trao_l2t = l2t;
	a.trao_opts.trt_rcv_win = T4_OT_RCV_WIN;
	a.trao_opts.trt_ulp_mode = ULP_MODE_NONE;
	a.trao_opts.trt_mtu_idx = NMTUS - 1;
	while (a.trao_opts.trt_mtu_idx > 0 &&
	    of->of_sc->params.mtus[a.trao_opts.trt_mtu_idx] >
	    of->of_port[t->tot_port].op_pi->mtu)
		a.trao_opts.trt_mtu_idx--;

	mutex_enter(&ot->ot_lock);
	t4_ot_want(ot, CPL_ACT_ESTABLISH, atid);
	if ((rc = t4_ofld_act_open(of, &a)) == 0)
		rc = t4_ot_wait(ot, t->tot_timeout_ms);
	t->tot_status = ot->ot_got_status;
	ot->ot_want_op = 0;
	if (rc == 0 && ot->ot_got_tid != T4_RDMA_TID_NONE &&
	    t->tot_status == 0) {
		c = t4_ot_conn_new(ot, ot->ot_got_tid, t->tot_port, l2t);
		if (c == NULL) {
			(void) t4_ofld_abort(of, ot->ot_got_tid, B_TRUE);
			rc = ENOSPC;
		} else {
			c->oc_flags = T4_OFLD_TCF_EST;
			c->oc_snd_isn = ot->ot_got_snd_isn;
			c->oc_rcv_isn = ot->ot_got_rcv_isn;
			t4_ot_flowc(ot, c);
			t->tot_id = c->oc_tid;
			t->tot_snd_isn = c->oc_snd_isn;
			t->tot_rcv_isn = c->oc_rcv_isn;
			l2t = T4_TID_NIL;
		}
	} else if (rc == 0) {
		rc = EIO;
	}
	mutex_exit(&ot->ot_lock);

	/* The open is over one way or the other; a late reply frees it. */
	t4_ofld_atid_free(of, atid);
	if (l2t != T4_TID_NIL)
		t4_l2t_put(of, l2t);
	return (rc);
}

static int
t4_ot_send(t4_ot_t *ot, t4_ofld_test_t *t)
{
	uint8_t buf[T4_OFLD_TX_IMM_MAX];
	t4_ot_conn_t *c;
	int rc;

	if (t->tot_len == 0 || t->tot_len > sizeof (buf))
		return (EINVAL);
	for (uint_t i = 0; i < t->tot_len; i++)
		buf[i] = (uint8_t)("t4ofld"[i % 6] + i / 6);
	mutex_enter(&ot->ot_lock);
	if ((c = t4_ot_conn(ot, t->tot_id)) == NULL) {
		mutex_exit(&ot->ot_lock);
		return (ENOENT);
	}
	if ((rc = t4_ofld_tx_data(ot->ot_of, c->oc_tid, buf,
	    t->tot_len)) == 0)
		c->oc_tx_bytes += t->tot_len;
	mutex_exit(&ot->ot_lock);
	return (rc);
}

static int
t4_ot_disconnect(t4_ot_t *ot, t4_ofld_test_t *t, boolean_t abort)
{
	t4_ot_conn_t *c;
	int rc;

	mutex_enter(&ot->ot_lock);
	if ((c = t4_ot_conn(ot, t->tot_id)) == NULL ||
	    (c->oc_flags & T4_OFLD_TCF_RELEASED) != 0) {
		mutex_exit(&ot->ot_lock);
		return (ENOENT);
	}
	if (abort) {
		t4_ot_want(ot, CPL_ABORT_RPL_RSS, c->oc_tid);
		rc = t4_ofld_abort(ot->ot_of, c->oc_tid, B_TRUE);
	} else {
		t4_ot_want(ot, CPL_CLOSE_CON_RPL, c->oc_tid);
		rc = (c->oc_flags & T4_OFLD_TCF_FIN_SENT) != 0 ? 0 :
		    t4_ofld_close_con(ot->ot_of, c->oc_tid);
		if (rc == 0)
			c->oc_flags |= T4_OFLD_TCF_FIN_SENT;
	}
	while (rc == 0 && (c->oc_flags & T4_OFLD_TCF_RELEASED) == 0) {
		ot->ot_got = B_FALSE;
		rc = t4_ot_wait(ot, t->tot_timeout_ms);
	}
	ot->ot_want_op = 0;
	t->tot_status = (int32_t)c->oc_flags;
	mutex_exit(&ot->ot_lock);
	return (rc);
}

/*
 * Write tot_len zero bytes at byte offset tot_id of the STAG region: TPT
 * entries that name nothing.
 */
static int
t4_ot_tpt(t4_ot_t *ot, t4_ofld_test_t *t)
{
	const t4_rdma_range_t *r = &ot->ot_of->of_vres.trv_stag;
	uint8_t *buf;
	int rc;

	if (t->tot_len == 0 || t->tot_len > T4_TPT_MAX_LEN ||
	    t->tot_id > r->trr_size)
		return (EINVAL);
	buf = kmem_zalloc(t->tot_len, KM_SLEEP);
	rc = t4_ofld_tpt_write(ot->ot_of, r->trr_start + t->tot_id, buf,
	    t->tot_len);
	kmem_free(buf, t->tot_len);
	t->tot_status = rc;
	return (rc);
}

static void
t4_ot_status(t4_ot_t *ot, t4_ofld_test_t *t)
{
	uint_t n = 0;

	mutex_enter(&ot->ot_lock);
	for (uint_t i = 0; i < T4_OFLD_TEST_NCONN; i++) {
		const t4_ot_conn_t *c = &ot->ot_conn[i];

		if (!c->oc_used)
			continue;
		t->tot_conn[n].totc_tid = c->oc_tid;
		t->tot_conn[n].totc_flags = c->oc_flags;
		t->tot_conn[n].totc_snd_isn = c->oc_snd_isn;
		t->tot_conn[n].totc_rcv_isn = c->oc_rcv_isn;
		t->tot_conn[n].totc_rx_bytes = c->oc_rx_bytes;
		t->tot_conn[n].totc_tx_bytes = c->oc_tx_bytes;
		n++;
	}
	t->tot_nconn = n;
	t->tot_accepts = ot->ot_accepts;
	t->tot_refused = ot->ot_refused;
	t->tot_events = ot->ot_events;
	t->tot_id = ot->ot_listen_stid;
	mutex_exit(&ot->ot_lock);
}

int
t4_ofld_test_ioctl(struct adapter *sc, void *data, int mode)
{
	t4_ofld_t *of = sc->ofld;
	t4_ofld_test_t *t;
	t4_ot_t *ot;
	int rc = 0;

	if (of == NULL || !of->of_ready)
		return (ENOTSUP);

	t = kmem_zalloc(sizeof (*t), KM_SLEEP);
	if (ddi_copyin(data, t, sizeof (*t), mode) != 0) {
		kmem_free(t, sizeof (*t));
		return (EFAULT);
	}
	bzero(t->tot_conn, sizeof (t->tot_conn));

	mutex_enter(&of->of_cfg_lock);
	ot = of->of_test;
	if (t->tot_op == T4_OFLD_TEST_OPEN) {
		rc = t4_ot_open(of);
		goto out;
	}
	if (t->tot_op == T4_OFLD_TEST_CLOSE) {
		if (ot == NULL)
			rc = ENOENT;
		else
			t4_ofld_test_fini(of);
		goto out;
	}
	if (ot == NULL) {
		rc = ENOENT;
		goto out;
	}
	if (t->tot_port >= of->of_nports) {
		rc = EINVAL;
		goto out;
	}

	switch (t->tot_op) {
	case T4_OFLD_TEST_LISTEN:
		rc = t4_ot_listen(ot, t);
		break;
	case T4_OFLD_TEST_UNLISTEN:
		rc = t4_ot_unlisten(ot, t);
		break;
	case T4_OFLD_TEST_CONNECT:
		rc = t4_ot_connect(ot, t);
		break;
	case T4_OFLD_TEST_SEND:
		rc = t4_ot_send(ot, t);
		break;
	case T4_OFLD_TEST_DISCONNECT:
		rc = t4_ot_disconnect(ot, t, B_FALSE);
		break;
	case T4_OFLD_TEST_ABORT:
		rc = t4_ot_disconnect(ot, t, B_TRUE);
		break;
	case T4_OFLD_TEST_STATUS:
		t4_ot_status(ot, t);
		break;
	case T4_OFLD_TEST_TPT:
		rc = t4_ot_tpt(ot, t);
		break;
	default:
		rc = EINVAL;
		break;
	}
out:
	mutex_exit(&of->of_cfg_lock);
	if (ddi_copyout(t, data, sizeof (*t), mode) != 0 && rc == 0)
		rc = EFAULT;
	kmem_free(t, sizeof (*t));
	return (rc);
}

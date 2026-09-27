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
 * rdmat over the rdmak connection manager.  A session connects its QPs with
 * rdk_cm and exchanges rdmat_qpinfo_t as private data, which is remote input
 * and taken only at its exact size.  A listener either accepts into one QP
 * of the session or, with RDMAT_CM_AUTO, into a QP of its own per request,
 * which it destroys when the connection is over.
 *
 * Handlers run in rdmak's CM taskq and take only ts_cm_lock and tq_lock,
 * never ts_lock, which teardown holds while it destroys the IDs.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rdmat_impl.h"

#define	RDMAT_CM_DEF_MS		10000
#define	RDMAT_CM_MAX_CYCLES	100000

static uint32_t
rdmat_cm_bit(enum rdk_cm_event_type ev)
{
	return (1U << (uint_t)ev);
}

static void
rdmat_cm_qp_event(rdmat_qp_t *tq, const struct rdk_cm_event *ev)
{
	mutex_enter(&tq->tq_lock);
	tq->tq_cm_seen |= rdmat_cm_bit(ev->event);
	tq->tq_cm_last = ev->event;
	if (ev->status != 0)
		tq->tq_cm_status = ev->status;
	if (ev->event == RDK_CM_EVENT_REJECTED) {
		tq->tq_cm_reason = ev->reject_reason;
		tq->tq_cm_rej_len = MIN(ev->param.private_data_len,
		    RDMAT_CM_REJ_LEN);
		bcopy(ev->param.private_data, tq->tq_cm_rej,
		    tq->tq_cm_rej_len);
	}
	if (ev->event == RDK_CM_EVENT_ESTABLISHED && tq->tq_cmid != NULL &&
	    !tq->tq_cm_ok) {
		/* The active side learns the peer's buffer here. */
		if (ev->param.private_data_len >= sizeof (rdmat_qpinfo_t)) {
			bcopy(ev->param.private_data, &tq->tq_peer,
			    sizeof (tq->tq_peer));
			tq->tq_cm_ok = B_TRUE;
		}
	}
	cv_broadcast(&tq->tq_cv);
	mutex_exit(&tq->tq_lock);
}

static struct rdk_qp *
rdmat_auto_qp(rdmat_sess_t *ts)
{
	struct rdk_qp_init_attr init;
	struct rdk_qp *qp;
	rdmat_qp_t *tq = &ts->ts_qp[0];

	bzero(&init, sizeof (init));
	init.send_cq = tq->tq_scq;
	init.recv_cq = tq->tq_rcq;
	init.cap.max_send_wr = 4;
	init.cap.max_recv_wr = 4;
	init.cap.max_send_sge = 1;
	init.cap.max_recv_sge = 1;
	init.sq_sig_type = RDK_SIGNAL_REQ_WR;
	init.qp_type = RDK_QPT_RC;
	init.port_num = 1;
	if (rdk_create_qp(ts->ts_pd, &init, &qp) != 0)
		return (NULL);
	return (qp);
}

/* A request on a listener.  Returns nonzero to refuse it. */
static int
rdmat_cm_request(rdmat_listen_t *rl, rdk_cm_id_t *id,
    const struct rdk_cm_event *ev)
{
	rdmat_sess_t *ts = rl->rl_ctx.cc_sess;
	struct rdk_cm_conn_param p;
	rdmat_qpinfo_t mine;
	rdmat_auto_t *ra;
	rdmat_qp_t *tq;

	mutex_enter(&ts->ts_cm_lock);
	rl->rl_reqs++;
	mutex_exit(&ts->ts_cm_lock);
	if (rl->rl_reject) {
		mutex_enter(&ts->ts_cm_lock);
		rl->rl_rejects++;
		mutex_exit(&ts->ts_cm_lock);
		(void) rdk_cm_reject(id, RDMAT_CM_REJ_DATA, RDMAT_CM_REJ_LEN);
		return (1);
	}
	/* RoCE pads the private data to the message's fixed size. */
	if (ev->param.private_data_len < sizeof (rdmat_qpinfo_t))
		goto reject;

	bzero(&p, sizeof (p));
	p.responder_resources = ev->param.initiator_depth;
	p.initiator_depth = ev->param.responder_resources;
	if (rl->rl_auto) {
		ra = kmem_zalloc(sizeof (*ra), KM_SLEEP);
		ra->ra_ctx.cc_kind = RCK_AUTO;
		ra->ra_ctx.cc_sess = ts;
		ra->ra_id = id;
		if ((ra->ra_qp = rdmat_auto_qp(ts)) == NULL) {
			kmem_free(ra, sizeof (*ra));
			goto reject;
		}
		mutex_enter(&ts->ts_cm_lock);
		list_insert_tail(&ts->ts_autos, ra);
		mutex_exit(&ts->ts_cm_lock);
		rdk_cm_set_context(id, &ra->ra_ctx);
		rdmat_qp_info(&ts->ts_qp[0], &mine);
		p.qp = ra->ra_qp;
		p.private_data = &mine;
		p.private_data_len = sizeof (mine);
		if (rdk_cm_accept(id, &p) != 0) {
			/* Teardown owns the entry if it took it off first. */
			mutex_enter(&ts->ts_cm_lock);
			if (list_link_active(&ra->ra_node)) {
				list_remove(&ts->ts_autos, ra);
				mutex_exit(&ts->ts_cm_lock);
				rdk_destroy_qp(ra->ra_qp);
				kmem_free(ra, sizeof (*ra));
				goto reject;
			}
			mutex_exit(&ts->ts_cm_lock);
			return (0);
		}
		mutex_enter(&ts->ts_cm_lock);
		rl->rl_accepts++;
		mutex_exit(&ts->ts_cm_lock);
		return (0);
	}

	tq = &ts->ts_qp[rl->rl_qp];
	mutex_enter(&tq->tq_lock);
	if (tq->tq_cmid != NULL || tq->tq_connected) {
		mutex_exit(&tq->tq_lock);
		goto reject;
	}
	tq->tq_cmid = id;
	tq->tq_cm_seen = 0;
	tq->tq_cm_status = 0;
	bcopy(ev->param.private_data, &tq->tq_peer, sizeof (tq->tq_peer));
	tq->tq_cm_ok = B_TRUE;
	mutex_exit(&tq->tq_lock);
	rdk_cm_set_context(id, &tq->tq_cmctx);
	rdmat_qp_info(tq, &mine);
	p.qp = tq->tq_qp;
	p.private_data = &mine;
	p.private_data_len = sizeof (mine);
	if (rdk_cm_accept(id, &p) != 0) {
		mutex_enter(&tq->tq_lock);
		tq->tq_cmid = NULL;
		tq->tq_cm_ok = B_FALSE;
		mutex_exit(&tq->tq_lock);
		goto reject;
	}
	mutex_enter(&ts->ts_cm_lock);
	rl->rl_accepts++;
	mutex_exit(&ts->ts_cm_lock);
	return (0);
reject:
	mutex_enter(&ts->ts_cm_lock);
	rl->rl_rejects++;
	mutex_exit(&ts->ts_cm_lock);
	(void) rdk_cm_reject(id, NULL, 0);
	return (1);
}

/* An automatic QP's connection ended: free it with its ID. */
static int
rdmat_cm_auto_event(rdmat_auto_t *ra, const struct rdk_cm_event *ev)
{
	rdmat_sess_t *ts = ra->ra_ctx.cc_sess;

	switch (ev->event) {
	case RDK_CM_EVENT_TIMEWAIT_EXIT:
	case RDK_CM_EVENT_CONNECT_ERROR:
	case RDK_CM_EVENT_DEVICE_REMOVAL:
		break;
	default:
		return (0);
	}
	mutex_enter(&ts->ts_cm_lock);
	if (!list_link_active(&ra->ra_node)) {
		mutex_exit(&ts->ts_cm_lock);
		return (0);
	}
	list_remove(&ts->ts_autos, ra);
	mutex_exit(&ts->ts_cm_lock);
	rdk_destroy_qp(ra->ra_qp);
	kmem_free(ra, sizeof (*ra));
	return (1);
}

static int
rdmat_cm_handler(rdk_cm_id_t *id, void *ctx, const struct rdk_cm_event *ev)
{
	rdmat_cmctx_t *cc = ctx;

	switch (cc->cc_kind) {
	case RCK_LISTEN:
		if (ev->event == RDK_CM_EVENT_CONNECT_REQUEST)
			return (rdmat_cm_request((rdmat_listen_t *)cc, id, ev));
		return (0);
	case RCK_AUTO:
		return (rdmat_cm_auto_event((rdmat_auto_t *)cc, ev));
	case RCK_QP:
		rdmat_cm_qp_event((rdmat_qp_t *)((caddr_t)cc -
		    offsetof(rdmat_qp_t, tq_cmctx)), ev);
		return (0);
	default:
		return (0);
	}
}

/* Wait for any event of the mask on the QP's ID.  tq_lock is held. */
static int
rdmat_cm_wait(rdmat_sess_t *ts, rdmat_qp_t *tq, uint32_t mask,
    hrtime_t deadline)
{
	ASSERT(MUTEX_HELD(&tq->tq_lock));
	while ((tq->tq_cm_seen & mask) == 0) {
		if (ts->ts_dying)
			return (ENXIO);
		if (gethrtime() >= deadline)
			return (ETIMEDOUT);
		if (cv_reltimedwait_sig(&tq->tq_cv, &tq->tq_lock,
		    drv_usectohz(100000), TR_CLOCK_TICK) == 0)
			return (EINTR);
	}
	return (0);
}

static void
rdmat_sin(struct sockaddr_in *sin, uint32_t addr, uint16_t port)
{
	bzero(sin, sizeof (*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr;
	sin->sin_port = port;
}

static int
rdmat_cm_listen(rdmat_sess_t *ts, rdmat_cm_t *c)
{
	struct sockaddr_in sin, peers[RDMAT_CM_MAX_PEERS];
	rdmat_listen_t *rl;
	rdk_cm_acl_t *acl;
	uint32_t i, slot;
	int ret;

	if (c->rcm_npeers == 0 || c->rcm_npeers > RDMAT_CM_MAX_PEERS ||
	    c->rcm_lport == 0 || c->rcm_backlog == 0 ||
	    (c->rcm_flags & ~(RDMAT_CM_AUTO | RDMAT_CM_REJECT)) != 0 ||
	    ((c->rcm_flags & (RDMAT_CM_AUTO | RDMAT_CM_REJECT)) == 0 &&
	    c->rcm_qp >= ts->ts_nqp))
		return (EINVAL);
	for (slot = 0; slot < RDMAT_CM_MAX_LISTEN; slot++) {
		if (ts->ts_listen[slot] == NULL)
			break;
	}
	if (slot == RDMAT_CM_MAX_LISTEN)
		return (EBUSY);
	for (i = 0; i < c->rcm_npeers; i++)
		rdmat_sin(&peers[i], c->rcm_peers[i], 0);
	if ((ret = rdk_cm_acl_create(peers, c->rcm_npeers, &acl)) != 0)
		return (ret);

	rl = kmem_zalloc(sizeof (*rl), KM_SLEEP);
	rl->rl_ctx.cc_kind = RCK_LISTEN;
	rl->rl_ctx.cc_sess = ts;
	rl->rl_qp = c->rcm_qp;
	rl->rl_auto = (c->rcm_flags & RDMAT_CM_AUTO) != 0;
	rl->rl_reject = (c->rcm_flags & RDMAT_CM_REJECT) != 0;
	if ((ret = rdk_cm_create_id(ts->ts_cred, rdmat_cm_handler,
	    &rl->rl_ctx, RDK_PS_TCP, RDK_QPT_RC, &rl->rl_id)) != 0)
		goto fail;
	rdmat_sin(&sin, c->rcm_laddr, c->rcm_lport);
	if ((ret = rdk_cm_bind_addr(rl->rl_id, (struct sockaddr *)&sin)) != 0)
		goto fail;
	if (rdk_cm_device(rl->rl_id, NULL) != ts->ts_dev) {
		ret = EADDRNOTAVAIL;
		goto fail;
	}
	if ((ret = rdk_cm_listen(rl->rl_id, (int)c->rcm_backlog, acl)) != 0)
		goto fail;
	rdk_cm_acl_rele(acl);
	ts->ts_listen[slot] = rl;
	c->rcm_qp = slot;
	c->rcm_bound = c->rcm_lport;
	return (0);
fail:
	c->rcm_status = ret;
	if (rl->rl_id != NULL)
		(void) rdk_cm_destroy_id(rl->rl_id);
	kmem_free(rl, sizeof (*rl));
	rdk_cm_acl_rele(acl);
	return (ret);
}

static int
rdmat_cm_unlisten(rdmat_sess_t *ts, uint32_t slot)
{
	rdmat_listen_t *rl;

	if (slot >= RDMAT_CM_MAX_LISTEN || (rl = ts->ts_listen[slot]) == NULL)
		return (ENOENT);
	ts->ts_listen[slot] = NULL;
	(void) rdk_cm_destroy_id(rl->rl_id);
	kmem_free(rl, sizeof (*rl));
	return (0);
}

/* After ESTABLISHED: take the peer's info and bind the MRs. */
static int
rdmat_cm_finish(rdmat_qp_t *tq, rdmat_cm_t *c)
{
	int ret;

	mutex_enter(&tq->tq_lock);
	if (!tq->tq_cm_ok) {
		mutex_exit(&tq->tq_lock);
		return (EPROTO);
	}
	c->rcm_peer = tq->tq_peer;
	tq->tq_rqpn = tq->tq_peer.rqi_qpn;
	mutex_exit(&tq->tq_lock);
	if ((ret = rdmat_qp_register(tq)) != 0)
		return (ret);
	return (0);
}

static int
rdmat_cm_accept(rdmat_sess_t *ts, rdmat_cm_t *c, hrtime_t deadline)
{
	rdmat_qp_t *tq = &ts->ts_qp[c->rcm_qp];
	int ret;

	mutex_enter(&tq->tq_lock);
	while (tq->tq_cmid == NULL) {
		if (ts->ts_dying || gethrtime() >= deadline) {
			mutex_exit(&tq->tq_lock);
			return (ts->ts_dying ? ENXIO : ETIMEDOUT);
		}
		if (cv_reltimedwait_sig(&tq->tq_cv, &tq->tq_lock,
		    drv_usectohz(100000), TR_CLOCK_TICK) == 0) {
			mutex_exit(&tq->tq_lock);
			return (EINTR);
		}
	}
	ret = rdmat_cm_wait(ts, tq, rdmat_cm_bit(RDK_CM_EVENT_ESTABLISHED) |
	    rdmat_cm_bit(RDK_CM_EVENT_CONNECT_ERROR) |
	    rdmat_cm_bit(RDK_CM_EVENT_DEVICE_REMOVAL), deadline);
	c->rcm_event = tq->tq_cm_last;
	c->rcm_status = tq->tq_cm_status;
	if (ret == 0 && (tq->tq_cm_seen &
	    rdmat_cm_bit(RDK_CM_EVENT_ESTABLISHED)) == 0)
		ret = tq->tq_cm_status != 0 ? tq->tq_cm_status : ECONNRESET;
	mutex_exit(&tq->tq_lock);
	return (ret != 0 ? ret : rdmat_cm_finish(tq, c));
}

/* Resolve, connect and wait for the outcome. */
static int
rdmat_cm_connect1(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_cm_t *c,
    hrtime_t deadline)
{
	struct sockaddr_in src, dst;
	struct rdk_cm_conn_param p;
	struct rdk_cm_route rt;
	rdmat_qpinfo_t mine;
	rdk_cm_id_t *id;
	hrtime_t t0 = gethrtime(), dt;
	uint32_t done;
	int ret;

	if ((ret = rdk_cm_create_id(ts->ts_cred, rdmat_cm_handler,
	    &tq->tq_cmctx, RDK_PS_TCP, RDK_QPT_RC, &id)) != 0)
		return (ret);
	mutex_enter(&tq->tq_lock);
	tq->tq_cmid = id;
	tq->tq_cm_seen = 0;
	tq->tq_cm_status = 0;
	tq->tq_cm_reason = 0;
	tq->tq_cm_rej_len = 0;
	tq->tq_cm_ok = B_FALSE;
	mutex_exit(&tq->tq_lock);

	rdmat_sin(&src, c->rcm_laddr, 0);
	rdmat_sin(&dst, c->rcm_raddr, c->rcm_rport);
	if ((ret = rdk_cm_resolve_addr(id, (struct sockaddr *)&src,
	    (struct sockaddr *)&dst, 0)) != 0)
		return (ret);
	mutex_enter(&tq->tq_lock);
	done = rdmat_cm_bit(RDK_CM_EVENT_ADDR_RESOLVED) |
	    rdmat_cm_bit(RDK_CM_EVENT_ADDR_ERROR);
	if ((ret = rdmat_cm_wait(ts, tq, done, deadline)) == 0 &&
	    (tq->tq_cm_seen & rdmat_cm_bit(RDK_CM_EVENT_ADDR_RESOLVED)) == 0)
		ret = tq->tq_cm_status;
	mutex_exit(&tq->tq_lock);
	if (ret != 0)
		return (ret);
	if (rdk_cm_device(id, NULL) != ts->ts_dev)
		return (EADDRNOTAVAIL);
	if ((ret = rdk_cm_resolve_route(id, 0)) != 0)
		return (ret);
	mutex_enter(&tq->tq_lock);
	done = rdmat_cm_bit(RDK_CM_EVENT_ROUTE_RESOLVED) |
	    rdmat_cm_bit(RDK_CM_EVENT_ROUTE_ERROR);
	if ((ret = rdmat_cm_wait(ts, tq, done, deadline)) == 0 &&
	    (tq->tq_cm_seen & rdmat_cm_bit(RDK_CM_EVENT_ROUTE_RESOLVED)) == 0)
		ret = tq->tq_cm_status;
	mutex_exit(&tq->tq_lock);
	if (ret != 0)
		return (ret);
	if (rdk_cm_route(id, &rt) == 0)
		c->rcm_bound = rt.rcr_src.sin_port;

	rdmat_qp_info(tq, &mine);
	bzero(&p, sizeof (p));
	p.qp = tq->tq_qp;
	p.private_data = &mine;
	p.private_data_len = sizeof (mine);
	p.responder_resources = 8;
	p.initiator_depth = 8;
	p.timeout_ms = c->rcm_timeout_ms;
	if ((ret = rdk_cm_connect(id, &p)) != 0)
		return (ret);
	mutex_enter(&tq->tq_lock);
	done = rdmat_cm_bit(RDK_CM_EVENT_ESTABLISHED) |
	    rdmat_cm_bit(RDK_CM_EVENT_REJECTED) |
	    rdmat_cm_bit(RDK_CM_EVENT_UNREACHABLE) |
	    rdmat_cm_bit(RDK_CM_EVENT_CONNECT_ERROR) |
	    rdmat_cm_bit(RDK_CM_EVENT_DEVICE_REMOVAL);
	ret = rdmat_cm_wait(ts, tq, done, deadline + SEC2NSEC(2));
	c->rcm_event = tq->tq_cm_last;
	c->rcm_status = tq->tq_cm_status;
	c->rcm_reason = tq->tq_cm_reason;
	c->rcm_rej_len = tq->tq_cm_rej_len;
	bcopy(tq->tq_cm_rej, c->rcm_rej_data, sizeof (c->rcm_rej_data));
	if (ret == 0 && (tq->tq_cm_seen &
	    rdmat_cm_bit(RDK_CM_EVENT_ESTABLISHED)) == 0) {
		ret = (tq->tq_cm_seen & rdmat_cm_bit(RDK_CM_EVENT_REJECTED)) !=
		    0 ? ECONNREFUSED : tq->tq_cm_status != 0 ?
		    tq->tq_cm_status : ECONNRESET;
	}
	mutex_exit(&tq->tq_lock);
	if (ret == 0) {
		dt = gethrtime() - t0;
		c->rcm_conn_ns += (uint64_t)dt;
		if (c->rcm_conn_min_ns == 0 ||
		    (uint64_t)dt < c->rcm_conn_min_ns)
			c->rcm_conn_min_ns = (uint64_t)dt;
		c->rcm_conn_max_ns = MAX(c->rcm_conn_max_ns, (uint64_t)dt);
	}
	return (ret);
}

/* Forget the QP's ID after its connection, and make the QP anew. */
static int
rdmat_cm_reset(rdmat_sess_t *ts, rdmat_qp_t *tq)
{
	rdk_cm_id_t *id;

	mutex_enter(&tq->tq_lock);
	id = tq->tq_cmid;
	tq->tq_cmid = NULL;
	tq->tq_connected = B_FALSE;
	tq->tq_cm_ok = B_FALSE;
	mutex_exit(&tq->tq_lock);
	if (id != NULL)
		(void) rdk_cm_destroy_id(id);
	if (tq->tq_qp != NULL) {
		rdk_drain_qp(tq->tq_qp);
		rdk_destroy_qp(tq->tq_qp);
		tq->tq_qp = NULL;
	}
	return (rdmat_qp_make(ts, tq));
}

static int
rdmat_cm_disconnect(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_cm_t *c,
    hrtime_t deadline)
{
	rdk_cm_id_t *id;
	hrtime_t t0 = gethrtime();
	uint32_t until;
	int ret = 0;

	mutex_enter(&tq->tq_lock);
	id = tq->tq_cmid;
	mutex_exit(&tq->tq_lock);
	if (id == NULL)
		return (ENOTCONN);
	(void) rdk_cm_disconnect(id);
	until = rdmat_cm_bit(RDK_CM_EVENT_TIMEWAIT_EXIT) |
	    rdmat_cm_bit(RDK_CM_EVENT_DEVICE_REMOVAL);
	if ((c->rcm_flags & RDMAT_CM_FAST) != 0)
		until |= rdmat_cm_bit(RDK_CM_EVENT_DISCONNECTED);
	mutex_enter(&tq->tq_lock);
	if ((tq->tq_cm_seen & rdmat_cm_bit(RDK_CM_EVENT_ESTABLISHED)) != 0)
		ret = rdmat_cm_wait(ts, tq, until, deadline);
	c->rcm_event = tq->tq_cm_last;
	mutex_exit(&tq->tq_lock);
	c->rcm_disc_ns += (uint64_t)(gethrtime() - t0);
	if (ret == EINTR || ret == ENXIO)
		return (ret);
	c->rcm_status = ret;
	return (rdmat_cm_reset(ts, tq));
}

static int
rdmat_cm_cycle(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_cm_t *c)
{
	const uint32_t ms = c->rcm_timeout_ms != 0 ? c->rcm_timeout_ms :
	    RDMAT_CM_DEF_MS;
	hrtime_t t0 = gethrtime(), deadline;
	int ret = 0;

	if (c->rcm_count == 0 || c->rcm_count > RDMAT_CM_MAX_CYCLES ||
	    (c->rcm_flags & ~RDMAT_CM_FAST) != 0)
		return (EINVAL);
	c->rcm_conn_ns = c->rcm_conn_min_ns = c->rcm_conn_max_ns = 0;
	c->rcm_disc_ns = 0;
	for (c->rcm_done = 0; c->rcm_done < c->rcm_count; c->rcm_done++) {
		if (ts->ts_dying)
			return (ENXIO);
		deadline = gethrtime() + MSEC2NSEC(ms);
		if ((ret = rdmat_cm_connect1(ts, tq, c, deadline)) != 0) {
			(void) rdmat_cm_reset(ts, tq);
			break;
		}
		if ((ret = rdmat_cm_disconnect(ts, tq, c, deadline)) != 0 ||
		    c->rcm_status != 0) {
			ret = ret != 0 ? ret : c->rcm_status;
			break;
		}
	}
	c->rcm_ns = (uint64_t)(gethrtime() - t0);
	return (ret);
}

int
rdmat_cm(rdmat_sess_t *ts, rdmat_cm_t *c)
{
	const uint32_t ms = c->rcm_timeout_ms != 0 ? c->rcm_timeout_ms :
	    RDMAT_CM_DEF_MS;
	hrtime_t deadline;
	rdmat_listen_t *rl;
	rdmat_qp_t *tq = NULL;
	int ret;

	if (ts->ts_qpt != RDMAT_QPT_RC || ms > RDMAT_MAX_TIMEOUT_MS)
		return (EINVAL);
	if (!ts->ts_autos_init) {
		list_create(&ts->ts_autos, sizeof (rdmat_auto_t),
		    offsetof(rdmat_auto_t, ra_node));
		ts->ts_autos_init = B_TRUE;
	}
	if (c->rcm_op != RDMAT_CM_LISTEN && c->rcm_op != RDMAT_CM_UNLISTEN &&
	    c->rcm_op != RDMAT_CM_STATUS) {
		if (c->rcm_qp >= ts->ts_nqp)
			return (EINVAL);
		tq = &ts->ts_qp[c->rcm_qp];
		tq->tq_cmctx.cc_kind = RCK_QP;
		tq->tq_cmctx.cc_sess = ts;
	} else if (c->rcm_op == RDMAT_CM_LISTEN) {
		for (uint32_t i = 0; i < ts->ts_nqp; i++) {
			ts->ts_qp[i].tq_cmctx.cc_kind = RCK_QP;
			ts->ts_qp[i].tq_cmctx.cc_sess = ts;
		}
	}
	deadline = gethrtime() + MSEC2NSEC(ms);
	c->rcm_status = 0;

	switch (c->rcm_op) {
	case RDMAT_CM_LISTEN:
		return (rdmat_cm_listen(ts, c));
	case RDMAT_CM_UNLISTEN:
		return (rdmat_cm_unlisten(ts, c->rcm_qp));
	case RDMAT_CM_ACCEPT:
		if (tq->tq_connected)
			return (EISCONN);
		return (rdmat_cm_accept(ts, c, deadline));
	case RDMAT_CM_CONNECT:
		if (tq->tq_connected || tq->tq_cmid != NULL)
			return (EISCONN);
		c->rcm_conn_ns = c->rcm_conn_min_ns = c->rcm_conn_max_ns = 0;
		if ((ret = rdmat_cm_connect1(ts, tq, c, deadline)) != 0) {
			(void) rdmat_cm_reset(ts, tq);
			return (ret);
		}
		return (rdmat_cm_finish(tq, c));
	case RDMAT_CM_DISCONNECT:
		return (rdmat_cm_disconnect(ts, tq, c, deadline));
	case RDMAT_CM_CYCLE:
		if (tq->tq_connected || tq->tq_cmid != NULL)
			return (EISCONN);
		return (rdmat_cm_cycle(ts, tq, c));
	case RDMAT_CM_STATUS:
		if (c->rcm_qp >= RDMAT_CM_MAX_LISTEN ||
		    (rl = ts->ts_listen[c->rcm_qp]) == NULL)
			return (ENOENT);
		mutex_enter(&ts->ts_cm_lock);
		c->rcm_reqs = rl->rl_reqs;
		c->rcm_accepts = rl->rl_accepts;
		c->rcm_rejects = rl->rl_rejects;
		c->rcm_live = 0;
		if (ts->ts_autos_init) {
			for (rdmat_auto_t *ra = list_head(&ts->ts_autos);
			    ra != NULL; ra = list_next(&ts->ts_autos, ra))
				c->rcm_live++;
		}
		mutex_exit(&ts->ts_cm_lock);
		return (0);
	default:
		return (EINVAL);
	}
}

/* Before the QPs go: end every connection and listener of the session. */
void
rdmat_cm_teardown(rdmat_sess_t *ts)
{
	rdmat_auto_t *ra;
	rdk_cm_id_t *id;
	uint_t i;

	for (i = 0; i < RDMAT_CM_MAX_LISTEN; i++)
		(void) rdmat_cm_unlisten(ts, i);
	for (i = 0; i < ts->ts_nqp; i++) {
		rdmat_qp_t *tq = &ts->ts_qp[i];

		if (tq->tq_sess == NULL)
			continue;
		mutex_enter(&tq->tq_lock);
		id = tq->tq_cmid;
		tq->tq_cmid = NULL;
		mutex_exit(&tq->tq_lock);
		if (id != NULL)
			(void) rdk_cm_destroy_id(id);
	}
	if (!ts->ts_autos_init)
		return;
	for (;;) {
		mutex_enter(&ts->ts_cm_lock);
		ra = list_remove_head(&ts->ts_autos);
		mutex_exit(&ts->ts_cm_lock);
		if (ra == NULL)
			break;
		(void) rdk_cm_destroy_id(ra->ra_id);
		rdk_destroy_qp(ra->ra_qp);
		kmem_free(ra, sizeof (*ra));
	}
	list_destroy(&ts->ts_autos);
	ts->ts_autos_init = B_FALSE;
}

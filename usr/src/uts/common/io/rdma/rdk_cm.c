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
 * The connection manager: IDs, their operations and their events.  Written
 * for illumos; the event and operation names follow the Linux RDMA CM.
 *
 * Every event goes through the ID's queue and is delivered by one task at a
 * time, in order.  An operation records itself in rci_op with a generation;
 * whichever of the transport, a timer, a cancel or a removal ends it first
 * queues its one event, and the others find rci_op cleared.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/disp.h>
#include <sys/zone.h>
#include <sys/cred.h>
#include <sys/thread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"

kmutex_t rdk_cm_lock;
list_t rdk_cm_devs;
list_t rdk_cm_ids;
taskq_t *rdk_cm_taskq;
static uint_t rdk_cm_tsd;

/* How long connect and route resolution wait by default. */
uint_t rdk_cm_connect_ms = 30000;
uint_t rdk_cm_max_pending = 4096;
uint_t rdk_cm_max_conns = 4096;
static uint_t rdk_cm_conns;	/* rdk_cm_lock */

static void rdk_cm_task(void *);
static void rdk_cm_destroy_common(rdk_cm_id_t *, boolean_t);
static void rdk_cm_disarm(rdk_cm_id_t *);

static const char *const rdk_cm_event_names[] = {
	[RDK_CM_EVENT_ADDR_RESOLVED] = "address resolved",
	[RDK_CM_EVENT_ADDR_ERROR] = "address error",
	[RDK_CM_EVENT_ROUTE_RESOLVED] = "route resolved",
	[RDK_CM_EVENT_ROUTE_ERROR] = "route error",
	[RDK_CM_EVENT_CONNECT_REQUEST] = "connect request",
	[RDK_CM_EVENT_CONNECT_RESPONSE] = "connect response",
	[RDK_CM_EVENT_CONNECT_ERROR] = "connect error",
	[RDK_CM_EVENT_UNREACHABLE] = "unreachable",
	[RDK_CM_EVENT_REJECTED] = "rejected",
	[RDK_CM_EVENT_ESTABLISHED] = "established",
	[RDK_CM_EVENT_DISCONNECTED] = "disconnected",
	[RDK_CM_EVENT_DEVICE_REMOVAL] = "device removal",
	[RDK_CM_EVENT_ADDR_CHANGE] = "address change",
	[RDK_CM_EVENT_TIMEWAIT_EXIT] = "timewait exit"
};

const char *
rdk_cm_event_msg(enum rdk_cm_event_type ev)
{
	if ((uint_t)ev < ARRAY_SIZE(rdk_cm_event_names) &&
	    rdk_cm_event_names[ev] != NULL)
		return (rdk_cm_event_names[ev]);
	return ("unrecognized event");
}

void
rdk_cm_hold(rdk_cm_id_t *id)
{
	mutex_enter(&id->rci_lock);
	VERIFY3U(id->rci_refs, >, 0);
	id->rci_refs++;
	mutex_exit(&id->rci_lock);
}

static void
rdk_cm_free(rdk_cm_id_t *id)
{
	rdk_cm_qev_t *q;

	while ((q = list_remove_head(&id->rci_events)) != NULL)
		kmem_free(q, sizeof (*q));
	if (id->rci_resv != NULL)
		rdk_cm_resv_rele(id->rci_resv);
	if (id->rci_acl != NULL)
		rdk_cm_acl_rele(id->rci_acl);
	if (id->rci_dev != NULL)
		rdk_cm_dev_rele(id->rci_dev);
	crfree(id->rci_cred);
	list_destroy(&id->rci_events);
	cv_destroy(&id->rci_cv);
	mutex_destroy(&id->rci_lock);
	kmem_free(id, sizeof (*id));
}

void
rdk_cm_rele(rdk_cm_id_t *id)
{
	boolean_t last;

	mutex_enter(&id->rci_lock);
	VERIFY3U(id->rci_refs, >, 0);
	last = --id->rci_refs == 0;
	mutex_exit(&id->rci_lock);
	if (last)
		rdk_cm_free(id);
}

static rdk_cm_id_t *
rdk_cm_alloc(cred_t *cr, rdk_cm_handler_t handler, void *ctx,
    enum rdk_qp_type qpt)
{
	rdk_cm_id_t *id = kmem_zalloc(sizeof (*id), KM_SLEEP);

	mutex_init(&id->rci_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&id->rci_cv, NULL, CV_DRIVER, NULL);
	list_create(&id->rci_events, sizeof (rdk_cm_qev_t),
	    offsetof(rdk_cm_qev_t, q_node));
	id->rci_refs = 1;
	id->rci_handler = handler;
	id->rci_ctx = ctx;
	crhold(cr);
	id->rci_cred = cr;
	id->rci_qpt = qpt;
	id->rci_route.rcr_vlan = RDK_VLAN_NONE;
	id->rci_iw.iw_priv = id;
	id->rci_iw.iw_vlan = RDK_VLAN_NONE;
	return (id);
}

int
rdk_cm_create_id(cred_t *cr, rdk_cm_handler_t handler, void *ctx,
    enum rdk_port_space ps, enum rdk_qp_type qpt, rdk_cm_id_t **idp)
{
	rdk_cm_id_t *id;

	*idp = NULL;
	if (cr == NULL || handler == NULL)
		return (EINVAL);
	if (crgetzoneid(cr) != GLOBAL_ZONEID)
		return (EPERM);
	if (ps != RDK_PS_TCP || qpt != RDK_QPT_RC)
		return (ENOTSUP);
	id = rdk_cm_alloc(cr, handler, ctx, qpt);
	mutex_enter(&rdk_cm_lock);
	list_insert_tail(&rdk_cm_ids, id);
	mutex_exit(&rdk_cm_lock);
	*idp = id;
	return (0);
}

void
rdk_cm_set_context(rdk_cm_id_t *id, void *ctx)
{
	mutex_enter(&id->rci_lock);
	id->rci_ctx = ctx;
	mutex_exit(&id->rci_lock);
}

rdk_cm_qev_t *
rdk_cm_qev_alloc(enum rdk_cm_event_type ev, int status, const void *pdata,
    uint16_t len)
{
	rdk_cm_qev_t *q = kmem_zalloc(sizeof (*q), KM_SLEEP);

	q->q_ev.event = ev;
	q->q_ev.status = status;
	len = MIN(len, RDK_CM_PDATA_MAX);
	if (pdata != NULL && len != 0) {
		bcopy(pdata, q->q_pdata, len);
		q->q_ev.param.private_data = q->q_pdata;
		q->q_ev.param.private_data_len = len;
	}
	return (q);
}

/* Queue an event and make sure the task runs.  rci_lock is held. */
void
rdk_cm_queue_locked(rdk_cm_id_t *id, rdk_cm_qev_t *q)
{
	ASSERT(MUTEX_HELD(&id->rci_lock));
	if (id->rci_destroying || id->rci_state == RCS_REMOVED) {
		kmem_free(q, sizeof (*q));
		return;
	}
	if (q->q_ev.event == RDK_CM_EVENT_DEVICE_REMOVAL)
		id->rci_state = RCS_REMOVED;
	list_insert_tail(&id->rci_events, q);
	if (!id->rci_queued) {
		id->rci_queued = B_TRUE;
		id->rci_refs++;
		taskq_dispatch_ent(rdk_cm_taskq, rdk_cm_task, id, 0,
		    &id->rci_tqent);
	}
}

void
rdk_cm_queue(rdk_cm_id_t *id, enum rdk_cm_event_type ev, int status,
    const void *pdata, uint16_t len)
{
	rdk_cm_qev_t *q = rdk_cm_qev_alloc(ev, status, pdata, len);

	mutex_enter(&id->rci_lock);
	rdk_cm_queue_locked(id, q);
	mutex_exit(&id->rci_lock);
}

/* The ID whose handler this thread runs, and that ID's listener. */
typedef struct rdk_cm_cur {
	rdk_cm_id_t	*cc_id;
	rdk_cm_id_t	*cc_listener;
} rdk_cm_cur_t;

static boolean_t
rdk_cm_in_handler(rdk_cm_id_t *id)
{
	rdk_cm_cur_t *cc = tsd_get(rdk_cm_tsd);

	return (cc != NULL && (cc->cc_id == id || cc->cc_listener == id));
}

/*
 * A request's handler returned without a decision, or its listener is gone:
 * refuse the connection.
 */
static void
rdk_cm_refuse(rdk_cm_id_t *id)
{
	mutex_enter(&id->rci_lock);
	if (id->rci_decided) {
		mutex_exit(&id->rci_lock);
		return;
	}
	id->rci_decided = B_TRUE;
	mutex_exit(&id->rci_lock);
	(void) rdk_cm_iw_reject(id, NULL, 0);
}

static void
rdk_cm_task(void *arg)
{
	rdk_cm_id_t *id = arg;
	rdk_cm_id_t *listener;
	rdk_cm_qev_t *q;
	rdk_cm_cur_t cc;
	rdk_cm_handler_t handler;
	void *ctx;
	int ret;

	for (;;) {
		mutex_enter(&id->rci_lock);
		if (id->rci_destroying ||
		    (q = list_remove_head(&id->rci_events)) == NULL) {
			id->rci_queued = B_FALSE;
			cv_broadcast(&id->rci_cv);
			mutex_exit(&id->rci_lock);
			rdk_cm_rele(id);
			return;
		}
		id->rci_cb_thread = curthread;
		handler = id->rci_handler;
		ctx = id->rci_ctx;
		listener = id->rci_listener;
		mutex_exit(&id->rci_lock);

		ret = 0;
		if (q->q_ev.event == RDK_CM_EVENT_CONNECT_REQUEST) {
			mutex_enter(&listener->rci_lock);
			if (listener->rci_destroying ||
			    listener->rci_state != RCS_LISTEN)
				ret = 1;
			mutex_exit(&listener->rci_lock);
			q->q_ev.listen_id = listener;
		}
		if (ret == 0) {
			cc.cc_id = id;
			cc.cc_listener = listener;
			(void) tsd_set(rdk_cm_tsd, &cc);
			ret = handler(id, ctx, &q->q_ev);
			(void) tsd_set(rdk_cm_tsd, NULL);
		}
		if (q->q_ev.event == RDK_CM_EVENT_CONNECT_REQUEST) {
			if (ret != 0 || !id->rci_decided) {
				rdk_cm_refuse(id);
				ret = 1;
			}
		}
		kmem_free(q, sizeof (*q));

		mutex_enter(&id->rci_lock);
		id->rci_cb_thread = NULL;
		if (ret != 0) {
			boolean_t mine = !id->rci_destroying;

			/*
			 * The handler gave the ID back; this task drops the
			 * base reference too, unless rdk_cm_destroy_id()
			 * claimed the ID first.
			 */
			id->rci_destroying = B_TRUE;
			id->rci_queued = B_FALSE;
			cv_broadcast(&id->rci_cv);
			mutex_exit(&id->rci_lock);
			if (mine) {
				rdk_cm_destroy_common(id, B_TRUE);
				rdk_cm_rele(id);
			}
			rdk_cm_rele(id);
			return;
		}
		mutex_exit(&id->rci_lock);
	}
}

/*
 * End the pending operation with its event.  B_FALSE if another path ended
 * it first.  rci_lock is held.
 */
static boolean_t
rdk_cm_op_end_locked(rdk_cm_id_t *id, rdk_cm_op_t op, uint32_t gen)
{
	ASSERT(MUTEX_HELD(&id->rci_lock));
	if (id->rci_op != op || id->rci_opgen != gen)
		return (B_FALSE);
	id->rci_op = RCO_NONE;
	return (B_TRUE);
}

static void
rdk_cm_admit_release(rdk_cm_id_t *id)
{
	void *admit;

	mutex_enter(&id->rci_lock);
	admit = id->rci_admit;
	id->rci_admit = NULL;
	mutex_exit(&id->rci_lock);
	if (admit != NULL)
		rdk_iw_cm_unadmit(admit);
}

static void
rdk_cm_destroy_common(rdk_cm_id_t *id, boolean_t from_task)
{
	rdk_cm_state_t state;
	rdk_cm_qev_t *q;
	boolean_t listening, undecided;

	mutex_enter(&id->rci_lock);
	id->rci_destroying = B_TRUE;
	id->rci_op = RCO_NONE;
	while ((q = list_remove_head(&id->rci_events)) != NULL)
		kmem_free(q, sizeof (*q));
	if (!from_task) {
		while (id->rci_queued)
			cv_wait(&id->rci_cv, &id->rci_lock);
	}
	state = id->rci_state;
	listening = id->rci_iw_listen;
	undecided = state == RCS_REQ && !id->rci_decided;
	if (undecided)
		id->rci_decided = B_TRUE;
	mutex_exit(&id->rci_lock);

	rdk_cm_disarm(id);
	rdk_cm_resolve_cancel(id);
	if (listening)
		rdk_cm_iw_unlisten(id);
	if (undecided)
		(void) rdk_cm_iw_reject(id, NULL, 0);
	rdk_cm_iw_disconnect(id, B_TRUE);
	rdk_cm_iw_wait_final(id);
	rdk_cm_iw_release(id);
	rdk_cm_admit_release(id);

	mutex_enter(&rdk_cm_lock);
	list_remove(&rdk_cm_ids, id);
	mutex_exit(&rdk_cm_lock);
	if (id->rci_listener != NULL) {
		rdk_cm_rele(id->rci_listener);
		id->rci_listener = NULL;
	}
}

int
rdk_cm_destroy_id(rdk_cm_id_t *id)
{
	if (id == NULL)
		return (EINVAL);
	if (rdk_cm_in_handler(id))
		return (EDEADLK);
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	id->rci_destroying = B_TRUE;
	mutex_exit(&id->rci_lock);
	rdk_cm_destroy_common(id, B_FALSE);
	rdk_cm_rele(id);
	return (0);
}

/*
 * Addresses.
 */
static int
rdk_cm_sin(const struct sockaddr *sa, const struct sockaddr_in **sinp)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

	if (sa == NULL)
		return (EINVAL);
	if (sa->sa_family != AF_INET)
		return (EAFNOSUPPORT);
	*sinp = sin;
	return (0);
}

/* Bind the ID to its device and reservation.  rci_lock is not held. */
static int
rdk_cm_bind_dev(rdk_cm_id_t *id, rdk_cm_dev_t *cd, uint32_t port,
    uint_t ifindex, rdk_cm_resv_t *resv, const struct rdk_cm_route *rt,
    rdk_cm_state_t from, rdk_cm_state_t to)
{
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != from) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	id->rci_dev = cd;
	id->rci_port = port;
	id->rci_ifindex = ifindex;
	id->rci_resv = resv;
	id->rci_route = *rt;
	id->rci_route.rcr_src.sin_port = resv->rr_addr.sin_port;
	id->rci_iw.iw_laddr = id->rci_route.rcr_src;
	id->rci_iw.iw_port = port;
	id->rci_iw.iw_mtu = rt->rcr_mtu;
	id->rci_state = to;
	mutex_exit(&id->rci_lock);
	return (0);
}

int
rdk_cm_bind_addr(rdk_cm_id_t *id, const struct sockaddr *sa)
{
	const struct sockaddr_in *sin;
	struct rdk_cm_route rt;
	rdk_cm_resv_t *resv;
	rdk_cm_dev_t *cd;
	uint32_t port;
	uint_t ifindex;
	int ret;

	if ((ret = rdk_cm_sin(sa, &sin)) != 0)
		return (ret);
	if (!rdk_cm_unicast(sin->sin_addr.s_addr))
		return (EADDRNOTAVAIL);
	bzero(&rt, sizeof (rt));
	if ((ret = rdk_cm_local_dev(id->rci_cred, sin->sin_addr.s_addr, &cd,
	    &port, &ifindex, &rt)) != 0)
		return (ret);
	if ((ret = rdk_cm_resv_get(id->rci_cred, sin, &resv)) != 0) {
		rdk_cm_dev_rele(cd);
		return (ret);
	}
	if ((ret = rdk_cm_bind_dev(id, cd, port, ifindex, resv, &rt, RCS_IDLE,
	    RCS_BOUND)) != 0) {
		rdk_cm_resv_rele(resv);
		rdk_cm_dev_rele(cd);
	}
	return (ret);
}

static uint32_t
rdk_cm_timeout(uint32_t ms, uint32_t def)
{
	return (ms == 0 ? def : ms);
}

/*
 * Address resolution finds the source address, egress interface and
 * device, and reserves an ephemeral port on the source address.  It needs
 * no network traffic, so ADDR_RESOLVED or ADDR_ERROR is queued at once.
 */
int
rdk_cm_resolve_addr(rdk_cm_id_t *id, const struct sockaddr *src,
    const struct sockaddr *dst, uint32_t ms)
{
	const struct sockaddr_in *ssin = NULL, *dsin;
	struct sockaddr_in local;
	struct rdk_cm_route rt;
	rdk_cm_resv_t *resv;
	rdk_cm_dev_t *cd;
	ipaddr_t srcaddr = INADDR_ANY, nh;
	uint32_t port;
	uint_t ifindex;
	int ret;

	if (ms > RDK_CM_TIMEOUT_MAX_MS)
		return (EINVAL);
	if ((ret = rdk_cm_sin(dst, &dsin)) != 0)
		return (ret);
	if (src != NULL) {
		if ((ret = rdk_cm_sin(src, &ssin)) != 0)
			return (ret);
		srcaddr = ssin->sin_addr.s_addr;
		if (srcaddr != INADDR_ANY && !rdk_cm_unicast(srcaddr))
			return (EADDRNOTAVAIL);
	}
	if (!rdk_cm_unicast(dsin->sin_addr.s_addr) || dsin->sin_port == 0)
		return (EINVAL);

	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_IDLE) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	id->rci_state = RCS_ADDR_QUERY;
	mutex_exit(&id->rci_lock);

	bzero(&rt, sizeof (rt));
	ret = rdk_cm_route_lookup(id->rci_cred, dsin->sin_addr.s_addr,
	    &srcaddr, &cd, &port, &ifindex, &rt, &nh);
	if (ret == 0) {
		bzero(&local, sizeof (local));
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = srcaddr;
		local.sin_port = ssin != NULL ? ssin->sin_port : 0;
		if ((ret = rdk_cm_resv_get(id->rci_cred, &local, &resv)) != 0)
			rdk_cm_dev_rele(cd);
	}
	if (ret == 0) {
		rt.rcr_dst = *dsin;
		if ((ret = rdk_cm_bind_dev(id, cd, port, ifindex, resv, &rt,
		    RCS_ADDR_QUERY, RCS_ADDR_RESOLVED)) != 0) {
			rdk_cm_resv_rele(resv);
			rdk_cm_dev_rele(cd);
			return (ret);
		}
		mutex_enter(&id->rci_lock);
		id->rci_iw.iw_raddr = *dsin;
		id->rci_nexthop = nh;
		mutex_exit(&id->rci_lock);
		rdk_cm_queue(id, RDK_CM_EVENT_ADDR_RESOLVED, 0, NULL, 0);
		return (0);
	}
	mutex_enter(&id->rci_lock);
	if (id->rci_state == RCS_ADDR_QUERY)
		id->rci_state = RCS_IDLE;
	mutex_exit(&id->rci_lock);
	rdk_cm_queue(id, RDK_CM_EVENT_ADDR_ERROR, ret, NULL, 0);
	return (0);
}

static void
rdk_cm_timer_task(void *arg)
{
	rdk_cm_id_t *id = arg;
	rdk_cm_op_t op;
	boolean_t abort = B_FALSE, route = B_FALSE;

	mutex_enter(&id->rci_lock);
	op = id->rci_op;
	if (!id->rci_destroying && op != RCO_NONE && !id->rci_canceled &&
	    id->rci_opgen == id->rci_timer_gen) {
		if (op == RCO_ROUTE) {
			id->rci_op = RCO_NONE;
			id->rci_state = RCS_ADDR_RESOLVED;
			rdk_cm_queue_locked(id, rdk_cm_qev_alloc(
			    RDK_CM_EVENT_ROUTE_ERROR, ETIMEDOUT, NULL, 0));
			route = B_TRUE;
		} else {
			id->rci_canceled = B_TRUE;
			id->rci_cancel_err = ETIMEDOUT;
			abort = B_TRUE;
		}
	}
	mutex_exit(&id->rci_lock);
	if (route)
		rdk_cm_resolve_cancel(id);
	if (abort)
		rdk_cm_iw_disconnect(id, B_TRUE);
	rdk_cm_rele(id);
}

/* The deadline fired; its hold on the ID passes to the task. */
static void
rdk_cm_timer(void *arg)
{
	rdk_cm_id_t *id = arg;
	boolean_t rele = B_FALSE;

	mutex_enter(&id->rci_lock);
	id->rci_timer = 0;
	if (taskq_dispatch(rdk_cm_taskq, rdk_cm_timer_task, id,
	    TQ_NOSLEEP) == TASKQID_INVALID) {
		if (!id->rci_destroying) {
			id->rci_timer = timeout(rdk_cm_timer, id,
			    drv_usectohz(10 * MILLISEC));
		} else {
			rele = B_TRUE;
		}
	}
	mutex_exit(&id->rci_lock);
	if (rele)
		rdk_cm_rele(id);
}

/* Arm the deadline of the current operation.  rci_lock is held. */
static void
rdk_cm_arm(rdk_cm_id_t *id, uint32_t ms)
{
	ASSERT(MUTEX_HELD(&id->rci_lock));
	id->rci_refs++;
	id->rci_timer_gen = id->rci_opgen;
	id->rci_timer = timeout(rdk_cm_timer, id,
	    drv_usectohz((clock_t)ms * MILLISEC));
}

/* Stop the deadline; drops its hold if it had not fired. */
static void
rdk_cm_disarm(rdk_cm_id_t *id)
{
	timeout_id_t tid;

	mutex_enter(&id->rci_lock);
	tid = id->rci_timer;
	id->rci_timer = 0;
	mutex_exit(&id->rci_lock);
	if (tid != 0 && untimeout(tid) != -1)
		rdk_cm_rele(id);
}

int
rdk_cm_resolve_route(rdk_cm_id_t *id, uint32_t ms)
{
	ipaddr_t dst;
	uint32_t gen;

	if (ms > RDK_CM_TIMEOUT_MAX_MS)
		return (EINVAL);
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_ADDR_RESOLVED ||
	    id->rci_op != RCO_NONE) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	id->rci_state = RCS_ROUTE_QUERY;
	id->rci_op = RCO_ROUTE;
	gen = ++id->rci_opgen;
	id->rci_canceled = B_FALSE;
	dst = id->rci_nexthop;
	rdk_cm_arm(id, rdk_cm_timeout(ms, rdk_cm_connect_ms));
	mutex_exit(&id->rci_lock);

	return (rdk_cm_nexthop(id, dst, gen));
}

/* The neighbor answered, or failed; rdk_cm_addr.c calls this. */
void
rdk_cm_route_done(rdk_cm_id_t *id, uint32_t gen, int err, const uint8_t *mac)
{
	boolean_t ended;

	mutex_enter(&id->rci_lock);
	ended = rdk_cm_op_end_locked(id, RCO_ROUTE, gen);
	if (ended) {
		if (err == 0) {
			bcopy(mac, id->rci_route.rcr_dmac, ETHERADDRL);
			bcopy(mac, id->rci_iw.iw_nh_mac, ETHERADDRL);
			id->rci_state = RCS_ROUTE_RESOLVED;
		} else {
			id->rci_state = RCS_ADDR_RESOLVED;
		}
		rdk_cm_queue_locked(id, rdk_cm_qev_alloc(err == 0 ?
		    RDK_CM_EVENT_ROUTE_RESOLVED : RDK_CM_EVENT_ROUTE_ERROR, err,
		    NULL, 0));
	}
	mutex_exit(&id->rci_lock);
	if (ended)
		rdk_cm_disarm(id);
}

int
rdk_cm_acl_create(const struct sockaddr_in *peers, uint32_t n,
    rdk_cm_acl_t **aclp)
{
	rdk_cm_acl_t *acl;
	uint32_t i, j;
	ipaddr_t a;

	*aclp = NULL;
	if (peers == NULL || n == 0 || n > RDK_CM_ACL_MAX)
		return (EINVAL);
	acl = kmem_zalloc(sizeof (*acl) + n * sizeof (ipaddr_t), KM_SLEEP);
	for (i = 0; i < n; i++) {
		if (peers[i].sin_family != AF_INET ||
		    !rdk_cm_unicast(peers[i].sin_addr.s_addr)) {
			kmem_free(acl, sizeof (*acl) + n * sizeof (ipaddr_t));
			return (EINVAL);
		}
		/* Insertion sort by host order value; n is small. */
		a = peers[i].sin_addr.s_addr;
		for (j = acl->rca_n; j > 0 &&
		    ntohl(acl->rca_addr[j - 1]) > ntohl(a); j--)
			acl->rca_addr[j] = acl->rca_addr[j - 1];
		acl->rca_addr[j] = a;
		acl->rca_n++;
	}
	acl->rca_refs = 1;
	*aclp = acl;
	return (0);
}

void
rdk_cm_acl_hold(rdk_cm_acl_t *acl)
{
	atomic_inc_32(&acl->rca_refs);
}

void
rdk_cm_acl_rele(rdk_cm_acl_t *acl)
{
	if (acl != NULL && atomic_dec_32_nv(&acl->rca_refs) == 0) {
		kmem_free(acl, sizeof (*acl) +
		    acl->rca_n * sizeof (ipaddr_t));
	}
}

boolean_t
rdk_cm_acl_allows(const rdk_cm_acl_t *acl, ipaddr_t peer)
{
	uint32_t lo = 0, hi = acl->rca_n, mid;
	const uint32_t want = ntohl(peer);

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (ntohl(acl->rca_addr[mid]) == want)
			return (B_TRUE);
		if (ntohl(acl->rca_addr[mid]) < want)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (B_FALSE);
}

int
rdk_cm_listen(rdk_cm_id_t *id, int backlog, rdk_cm_acl_t *acl)
{
	int ret;

	if (acl == NULL || backlog <= 0 || backlog > RDK_CM_BACKLOG_MAX)
		return (EINVAL);
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_BOUND ||
	    id->rci_dev == NULL || id->rci_resv->rr_addr.sin_port == 0 ||
	    id->rci_dev->rcd_iw == NULL) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	rdk_cm_acl_hold(acl);
	id->rci_acl = acl;
	id->rci_backlog = (uint32_t)backlog;
	id->rci_state = RCS_LISTEN;
	mutex_exit(&id->rci_lock);

	rdk_cm_resv_used(id->rci_resv);
	if ((ret = rdk_cm_iw_listen(id)) != 0) {
		mutex_enter(&id->rci_lock);
		id->rci_state = RCS_BOUND;
		id->rci_acl = NULL;
		mutex_exit(&id->rci_lock);
		rdk_cm_acl_rele(acl);
	}
	return (ret);
}

static int
rdk_cm_param_ok(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p,
    enum rdk_cm_msg msg)
{
	if (p == NULL || p->qp == NULL || p->qp->device != id->rci_dev->rcd_dev ||
	    p->qp->qp_type != RDK_QPT_RC ||
	    p->timeout_ms > RDK_CM_TIMEOUT_MAX_MS ||
	    p->private_data_len > rdk_cm_pdata_max(id, msg) ||
	    (p->private_data_len != 0 && p->private_data == NULL))
		return (EINVAL);
	return (0);
}

static int
rdk_cm_conn_quota(void)
{
	int ret = 0;

	mutex_enter(&rdk_cm_lock);
	if (rdk_cm_conns >= rdk_cm_max_conns)
		ret = ENOBUFS;
	else
		rdk_cm_conns++;
	mutex_exit(&rdk_cm_lock);
	return (ret);
}

void
rdk_cm_conn_unquota(void)
{
	mutex_enter(&rdk_cm_lock);
	VERIFY3U(rdk_cm_conns, >, 0);
	rdk_cm_conns--;
	mutex_exit(&rdk_cm_lock);
}

int
rdk_cm_connect(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	uint32_t gen;
	int ret;

	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_ROUTE_RESOLVED ||
	    id->rci_op != RCO_NONE || id->rci_dev->rcd_iw == NULL) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	mutex_exit(&id->rci_lock);
	if ((ret = rdk_cm_param_ok(id, p, RDK_CM_MSG_REQ)) != 0)
		return (ret);
	if ((ret = rdk_cm_conn_quota()) != 0)
		return (ret);

	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_ROUTE_RESOLVED) {
		mutex_exit(&id->rci_lock);
		rdk_cm_conn_unquota();
		return (EINVAL);
	}
	id->rci_state = RCS_CONNECT;
	id->rci_op = RCO_CONNECT;
	gen = ++id->rci_opgen;
	id->rci_canceled = B_FALSE;
	mutex_exit(&id->rci_lock);

	rdk_cm_resv_used(id->rci_resv);
	if ((ret = rdk_cm_iw_connect(id, p)) != 0) {
		mutex_enter(&id->rci_lock);
		(void) rdk_cm_op_end_locked(id, RCO_CONNECT, gen);
		id->rci_state = RCS_ROUTE_RESOLVED;
		mutex_exit(&id->rci_lock);
		rdk_cm_conn_unquota();
		return (ret);
	}
	mutex_enter(&id->rci_lock);
	if (id->rci_op == RCO_CONNECT && id->rci_opgen == gen)
		rdk_cm_arm(id, rdk_cm_timeout(p->timeout_ms,
		    rdk_cm_connect_ms));
	mutex_exit(&id->rci_lock);
	return (0);
}

int
rdk_cm_accept(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	uint32_t gen;
	int ret;

	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_REQ ||
	    id->rci_decided) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	mutex_exit(&id->rci_lock);
	if ((ret = rdk_cm_param_ok(id, p, RDK_CM_MSG_REP)) != 0)
		return (ret);
	if ((ret = rdk_cm_conn_quota()) != 0)
		return (ret);

	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_decided) {
		mutex_exit(&id->rci_lock);
		rdk_cm_conn_unquota();
		return (EINVAL);
	}
	id->rci_decided = B_TRUE;
	id->rci_state = RCS_ACCEPT;
	id->rci_op = RCO_ACCEPT;
	gen = ++id->rci_opgen;
	id->rci_canceled = B_FALSE;
	mutex_exit(&id->rci_lock);

	rdk_cm_admit_release(id);
	if ((ret = rdk_cm_iw_accept(id, p)) != 0) {
		/* The provider refused the connection; nothing is live. */
		mutex_enter(&id->rci_lock);
		(void) rdk_cm_op_end_locked(id, RCO_ACCEPT, gen);
		id->rci_state = RCS_DONE;
		mutex_exit(&id->rci_lock);
		rdk_cm_conn_unquota();
	}
	return (ret);
}

int
rdk_cm_reject(rdk_cm_id_t *id, const void *pdata, uint16_t len)
{
	int ret;

	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_REQ ||
	    id->rci_decided) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	if (len > rdk_cm_pdata_max(id, RDK_CM_MSG_REJ) ||
	    (len != 0 && pdata == NULL)) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	id->rci_decided = B_TRUE;
	id->rci_state = RCS_DONE;
	mutex_exit(&id->rci_lock);
	ret = rdk_cm_iw_reject(id, pdata, len);
	rdk_cm_admit_release(id);
	return (ret);
}

int
rdk_cm_disconnect(rdk_cm_id_t *id)
{
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || (id->rci_state != RCS_ESTABLISHED &&
	    id->rci_state != RCS_DISCONNECT)) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	mutex_exit(&id->rci_lock);
	rdk_cm_iw_disconnect(id, B_FALSE);
	return (0);
}

int
rdk_cm_cancel(rdk_cm_id_t *id)
{
	rdk_cm_op_t op;

	mutex_enter(&id->rci_lock);
	op = id->rci_op;
	if (id->rci_destroying || op == RCO_NONE || id->rci_canceled) {
		mutex_exit(&id->rci_lock);
		return (EALREADY);
	}
	if (op == RCO_ROUTE) {
		id->rci_op = RCO_NONE;
		id->rci_state = RCS_ADDR_RESOLVED;
		rdk_cm_queue_locked(id, rdk_cm_qev_alloc(
		    RDK_CM_EVENT_ROUTE_ERROR, ECANCELED, NULL, 0));
		mutex_exit(&id->rci_lock);
		rdk_cm_disarm(id);
		rdk_cm_resolve_cancel(id);
		return (0);
	}
	id->rci_canceled = B_TRUE;
	id->rci_cancel_err = ECANCELED;
	mutex_exit(&id->rci_lock);
	rdk_cm_iw_disconnect(id, B_TRUE);
	return (0);
}

/*
 * The transport ended a connect or accept, or the connection.  Called by
 * rdk_cm_iw.c with the event already mapped; queues what the consumer is
 * owed.  final: the provider let go of the ID.
 */
void
rdk_cm_conn_event(rdk_cm_id_t *id, enum rdk_iw_event_type type, int status,
    const void *pdata, uint16_t len, uint32_t ird, uint32_t ord)
{
	rdk_cm_qev_t *q = NULL, *q2 = NULL;
	enum rdk_cm_event_type ev;
	boolean_t disarm = B_FALSE;
	int err;

	mutex_enter(&id->rci_lock);
	switch (type) {
	case RDK_IW_EVENT_CONNECT_REPLY:
	case RDK_IW_EVENT_ESTABLISHED:
		if (id->rci_op != RCO_CONNECT && id->rci_op != RCO_ACCEPT)
			break;
		disarm = B_TRUE;
		id->rci_op = RCO_NONE;
		if (id->rci_canceled) {
			err = id->rci_cancel_err;
			ev = err == ETIMEDOUT ? RDK_CM_EVENT_UNREACHABLE :
			    RDK_CM_EVENT_CONNECT_ERROR;
			q = rdk_cm_qev_alloc(ev, err, NULL, 0);
			id->rci_state = RCS_DONE;
			break;
		}
		if (status == 0) {
			q = rdk_cm_qev_alloc(RDK_CM_EVENT_ESTABLISHED, 0,
			    pdata, len);
			q->q_ev.param.responder_resources = (uint8_t)MIN(ird,
			    UINT8_MAX);
			q->q_ev.param.initiator_depth = (uint8_t)MIN(ord,
			    UINT8_MAX);
			id->rci_state = RCS_ESTABLISHED;
			break;
		}
		if (status == ECONNREFUSED) {
			ev = RDK_CM_EVENT_REJECTED;
		} else if (status == ETIMEDOUT || status == EHOSTUNREACH ||
		    status == ENETUNREACH) {
			ev = RDK_CM_EVENT_UNREACHABLE;
		} else {
			ev = RDK_CM_EVENT_CONNECT_ERROR;
		}
		q = rdk_cm_qev_alloc(ev, status, pdata, len);
		id->rci_state = RCS_DONE;
		break;
	case RDK_IW_EVENT_DISCONNECT:
		if (id->rci_state != RCS_ESTABLISHED)
			break;
		q = rdk_cm_qev_alloc(RDK_CM_EVENT_DISCONNECTED, status, NULL,
		    0);
		id->rci_state = RCS_DISCONNECT;
		break;
	case RDK_IW_EVENT_CLOSE:
		if (id->rci_op == RCO_CONNECT || id->rci_op == RCO_ACCEPT) {
			disarm = B_TRUE;
			id->rci_op = RCO_NONE;
			err = id->rci_canceled ? id->rci_cancel_err :
			    (status != 0 ? status : ECONNRESET);
			ev = id->rci_canceled && err == ETIMEDOUT ?
			    RDK_CM_EVENT_UNREACHABLE :
			    RDK_CM_EVENT_CONNECT_ERROR;
			q = rdk_cm_qev_alloc(ev, err, NULL, 0);
			id->rci_state = RCS_DONE;
			break;
		}
		if (id->rci_state == RCS_ESTABLISHED) {
			q = rdk_cm_qev_alloc(RDK_CM_EVENT_DISCONNECTED, status,
			    NULL, 0);
			id->rci_state = RCS_DISCONNECT;
		}
		if (id->rci_state == RCS_DISCONNECT) {
			q2 = rdk_cm_qev_alloc(RDK_CM_EVENT_TIMEWAIT_EXIT, 0,
			    NULL, 0);
			id->rci_state = RCS_DONE;
		}
		break;
	default:
		break;
	}
	if (q != NULL)
		rdk_cm_queue_locked(id, q);
	if (q2 != NULL)
		rdk_cm_queue_locked(id, q2);
	mutex_exit(&id->rci_lock);
	if (disarm)
		rdk_cm_disarm(id);
}

struct rdk_device *
rdk_cm_device(rdk_cm_id_t *id, uint32_t *portp)
{
	struct rdk_device *dev = NULL;

	mutex_enter(&id->rci_lock);
	if (id->rci_dev != NULL) {
		dev = id->rci_dev->rcd_dev;
		if (portp != NULL)
			*portp = id->rci_port;
	}
	mutex_exit(&id->rci_lock);
	return (dev);
}

int
rdk_cm_route(rdk_cm_id_t *id, struct rdk_cm_route *rt)
{
	int ret = 0;

	mutex_enter(&id->rci_lock);
	if (id->rci_dev == NULL)
		ret = ENXIO;
	else
		*rt = id->rci_route;
	mutex_exit(&id->rci_lock);
	return (ret);
}

uint16_t
rdk_cm_pdata_max(rdk_cm_id_t *id, enum rdk_cm_msg msg)
{
	uint16_t max = 0;

	_NOTE(ARGUNUSED(msg));
	mutex_enter(&id->rci_lock);
	if (id->rci_dev != NULL && id->rci_dev->rcd_iw != NULL)
		max = MIN(id->rci_dev->rcd_iw->iw_max_pdata, RDK_CM_PDATA_MAX);
	mutex_exit(&id->rci_lock);
	return (max);
}

/* An address on the interface changed; the IDs bound to it are told. */
void
rdk_cm_addr_change(uint_t ifindex)
{
	rdk_cm_id_t *id;

	mutex_enter(&rdk_cm_lock);
	for (id = list_head(&rdk_cm_ids); id != NULL;
	    id = list_next(&rdk_cm_ids, id)) {
		mutex_enter(&id->rci_lock);
		if (id->rci_dev != NULL && id->rci_ifindex == ifindex &&
		    !id->rci_destroying && id->rci_state != RCS_REMOVED) {
			rdk_cm_queue_locked(id, rdk_cm_qev_alloc(
			    RDK_CM_EVENT_ADDR_CHANGE, 0, NULL, 0));
		}
		mutex_exit(&id->rci_lock);
	}
	mutex_exit(&rdk_cm_lock);
}

/*
 * Devices.  rdk_cm is a client of every device: removal tells each ID on
 * the device and waits until the consumers have destroyed them.
 */
static int
rdk_cm_client_add(struct rdk_device *dev)
{
	uint8_t mac[RDK_CM_MAX_PORTS][ETHERADDRL];
	struct rdk_port_attr pa;
	rdk_cm_dev_t *cd;
	uint32_t n, i;

	n = MIN(dev->rd_phys_port_cnt, RDK_CM_MAX_PORTS);
	for (i = 0; i < n; i++) {
		if (rdk_query_port(dev, i + 1, &pa) != 0)
			bzero(pa.mac, ETHERADDRL);
		bcopy(pa.mac, mac[i], ETHERADDRL);
	}
	mutex_enter(&rdk_cm_lock);
	for (cd = list_head(&rdk_cm_devs); cd != NULL;
	    cd = list_next(&rdk_cm_devs, cd)) {
		if (cd->rcd_dev == dev) {
			cd->rcd_nports = n;
			bcopy(mac, cd->rcd_mac, sizeof (mac));
			cd->rcd_added = B_TRUE;
			break;
		}
	}
	mutex_exit(&rdk_cm_lock);
	return (0);
}

static void
rdk_cm_client_remove(struct rdk_device *dev, void *arg)
{
	rdk_cm_dev_t *cd;
	rdk_cm_id_t *id;
	uint_t waited = 0;

	_NOTE(ARGUNUSED(arg));
	mutex_enter(&rdk_cm_lock);
	for (cd = list_head(&rdk_cm_devs); cd != NULL;
	    cd = list_next(&rdk_cm_devs, cd)) {
		if (cd->rcd_dev == dev)
			break;
	}
	if (cd == NULL) {
		mutex_exit(&rdk_cm_lock);
		return;
	}
	cd->rcd_removing = B_TRUE;
	for (id = list_head(&rdk_cm_ids); id != NULL;
	    id = list_next(&rdk_cm_ids, id)) {
		mutex_enter(&id->rci_lock);
		if (id->rci_dev == cd && !id->rci_removal) {
			id->rci_removal = B_TRUE;
			id->rci_op = RCO_NONE;
			rdk_cm_queue_locked(id, rdk_cm_qev_alloc(
			    RDK_CM_EVENT_DEVICE_REMOVAL, 0, NULL, 0));
		}
		mutex_exit(&id->rci_lock);
	}
	while (cd->rcd_ids != 0) {
		mutex_exit(&rdk_cm_lock);
		delay(drv_usectohz(100 * MILLISEC));
		if (++waited % 100 == 0) {
			dev_err(dev->rd_dip, CE_WARN, "!waiting for %u RDMA CM "
			    "IDs to be destroyed", cd->rcd_ids);
		}
		mutex_enter(&rdk_cm_lock);
	}
	cd->rcd_added = B_FALSE;
	mutex_exit(&rdk_cm_lock);
}

static struct rdk_client rdk_cm_client = {
	.name = "rdk_cm",
	.add = rdk_cm_client_add,
	.remove = rdk_cm_client_remove
};

int
rdk_cm_init(void)
{
	int ret;

	mutex_init(&rdk_cm_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&rdk_cm_devs, sizeof (rdk_cm_dev_t),
	    offsetof(rdk_cm_dev_t, rcd_node));
	list_create(&rdk_cm_ids, sizeof (rdk_cm_id_t),
	    offsetof(rdk_cm_id_t, rci_node));
	tsd_create(&rdk_cm_tsd, NULL);
	rdk_cm_iw_init();
	rdk_cm_taskq = taskq_create("rdk_cm", MAX(MIN(ncpus, 16), 4),
	    minclsyspri, 4, INT_MAX, TASKQ_PREPOPULATE);
	if ((ret = rdk_cm_addr_init()) != 0)
		goto fail;
	if ((ret = rdk_register_client(&rdk_cm_client)) != 0) {
		(void) rdk_cm_addr_fini();
		goto fail;
	}
	return (0);
fail:
	taskq_destroy(rdk_cm_taskq);
	rdk_cm_iw_fini();
	tsd_destroy(&rdk_cm_tsd);
	list_destroy(&rdk_cm_ids);
	list_destroy(&rdk_cm_devs);
	mutex_destroy(&rdk_cm_lock);
	return (ret);
}

int
rdk_cm_fini(void)
{
	int ret;

	mutex_enter(&rdk_cm_lock);
	if (!list_is_empty(&rdk_cm_ids) || !list_is_empty(&rdk_cm_devs)) {
		mutex_exit(&rdk_cm_lock);
		return (EBUSY);
	}
	mutex_exit(&rdk_cm_lock);
	if ((ret = rdk_cm_addr_fini()) != 0)
		return (ret);
	rdk_unregister_client(&rdk_cm_client);
	taskq_destroy(rdk_cm_taskq);
	rdk_cm_iw_fini();
	tsd_destroy(&rdk_cm_tsd);
	list_destroy(&rdk_cm_ids);
	list_destroy(&rdk_cm_devs);
	mutex_destroy(&rdk_cm_lock);
	return (0);
}

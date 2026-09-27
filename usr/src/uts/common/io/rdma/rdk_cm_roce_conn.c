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
 * IB CM connections: the ID tables, references, the QP link, timers and
 * devices.  Local communication IDs are random, and every later message
 * must come from the addresses the connection was made with and carry its
 * IDs, so that an off-path sender cannot guess its way into a connection.
 *
 * The QP belongs to the consumer.  A connection uses it only through a
 * lease taken under the QP's cm_lock, and rdk_destroy_qp() unlinks the
 * connection and waits for the leases before the provider frees the QP.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/random.h>

#include "rdk_impl.h"
#include "rdk_cm_roce.h"

kmutex_t rdk_ibcm_lock;
static avl_tree_t rdk_ibcm_lids;
static avl_tree_t rdk_ibcm_rids;
static avl_tree_t rdk_ibcm_qpns;
static list_t rdk_ibcm_conns;
static uint_t rdk_ibcm_nconns;
static kcondvar_t rdk_ibcm_cv;
uint64_t rdk_ibcm_hi_tid;

/* Connections, timewait ones included, the host keeps at once. */
uint_t rdk_ibcm_max_conns = 65536;
/* How long the passive side may take to find its way back to a peer. */
uint_t rdk_cm_roce_resolve_ms = 5000;
/* Passive requests whose way back is being resolved. */
volatile uint_t rdk_cm_roce_resolving;

static void rdk_ibconn_timer_task(void *);

static int
rdk_ibcm_lid_cmp(const void *a, const void *b)
{
	const rdk_ibconn_t *x = a, *y = b;

	if (x->ic_lid != y->ic_lid)
		return (x->ic_lid < y->ic_lid ? -1 : 1);
	return (0);
}

static int
rdk_ibcm_rid_cmp(const void *a, const void *b)
{
	const rdk_ibconn_t *x = a, *y = b;

	if (x->ic_rguid != y->ic_rguid)
		return (x->ic_rguid < y->ic_rguid ? -1 : 1);
	if (x->ic_rid != y->ic_rid)
		return (x->ic_rid < y->ic_rid ? -1 : 1);
	return (0);
}

static int
rdk_ibcm_qpn_cmp(const void *a, const void *b)
{
	const rdk_ibconn_t *x = a, *y = b;

	if (x->ic_rguid != y->ic_rguid)
		return (x->ic_rguid < y->ic_rguid ? -1 : 1);
	if (x->ic_rqpn != y->ic_rqpn)
		return (x->ic_rqpn < y->ic_rqpn ? -1 : 1);
	return (0);
}

uint32_t
rdk_ibcm_clamp_ms(uint32_t ms)
{
	return (MAX(MIN(ms, RDK_CM_TIMEOUT_MAX_MS), 1));
}

/* The port's GSI agent, held. */
rdk_gsi_t *
rdk_cm_roce_gsi(rdk_cm_dev_t *cd, uint32_t port)
{
	rdk_gsi_t *g = NULL;

	mutex_enter(&rdk_cm_lock);
	if (port >= 1 && port <= RDK_CM_MAX_PORTS && !cd->rcd_removing)
		g = cd->rcd_gsi[port - 1];
	if (g != NULL)
		rdk_gsi_hold(g);
	mutex_exit(&rdk_cm_lock);
	return (g);
}

/* The RoCE path MTU for an IP MTU, within the port's; 0 if none fits. */
uint8_t
rdk_cm_roce_mtu(struct rdk_device *dev, uint32_t port, uint32_t ip_mtu)
{
	struct rdk_port_attr pa;
	enum rdk_mtu m;

	if ((m = rdk_roce_mtu((int)MIN(ip_mtu, INT_MAX))) == 0)
		return (0);
	if (rdk_query_port(dev, port, &pa) == 0 && pa.active_mtu != 0 &&
	    m > pa.active_mtu)
		m = pa.active_mtu;
	return ((uint8_t)m);
}

rdk_ibconn_t *
rdk_ibconn_alloc(rdk_cm_dev_t *cd, uint32_t port, boolean_t active)
{
	rdk_ibconn_t *c;
	rdk_gsi_t *g;

	if ((g = rdk_cm_roce_gsi(cd, port)) == NULL)
		return (NULL);
	c = kmem_zalloc(sizeof (*c), KM_SLEEP);
	mutex_init(&c->ic_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&c->ic_cv, NULL, CV_DRIVER, NULL);
	c->ic_refs = 1;
	c->ic_cd = cd;
	c->ic_gsi = g;
	c->ic_port = port;
	c->ic_fsm.f_active = active;
	(void) random_get_pseudo_bytes((uint8_t *)&c->ic_spsn,
	    sizeof (c->ic_spsn));
	c->ic_spsn &= 0xffffff;
	return (c);
}

void
rdk_ibconn_hold(rdk_ibconn_t *c)
{
	atomic_inc_32(&c->ic_refs);
}

static void
rdk_ibconn_free(rdk_ibconn_t *c)
{
	VERIFY(!c->ic_in_l && !c->ic_in_r && !c->ic_in_q);
	VERIFY3P(c->ic_id, ==, NULL);
	VERIFY3P(c->ic_qp, ==, NULL);
	VERIFY3P(c->ic_arp, ==, NULL);
	if (c->ic_admit != NULL)
		rdk_cm_unadmit(c->ic_admit);
	if (c->ic_listener != NULL)
		rdk_cm_rele(c->ic_listener);
	rdk_put_gid_attr(c->ic_sgid);
	rdk_gsi_rele(c->ic_gsi);
	cv_destroy(&c->ic_cv);
	mutex_destroy(&c->ic_lock);
	kmem_free(c, sizeof (*c));
}

void
rdk_ibconn_rele(rdk_ibconn_t *c)
{
	if (atomic_dec_32_nv(&c->ic_refs) == 0)
		rdk_ibconn_free(c);
}

/*
 * Give the connection a random local ID no live one has, and count it.
 * ENOBUFS at the limit.
 */
int
rdk_ibconn_insert(rdk_ibconn_t *c)
{
	avl_index_t where;

	mutex_enter(&rdk_ibcm_lock);
	if (rdk_ibcm_nconns >= rdk_ibcm_max_conns) {
		mutex_exit(&rdk_ibcm_lock);
		return (ENOBUFS);
	}
	do {
		(void) random_get_pseudo_bytes((uint8_t *)&c->ic_lid,
		    sizeof (c->ic_lid));
	} while (c->ic_lid == 0 ||
	    avl_find(&rdk_ibcm_lids, c, &where) != NULL);
	avl_insert(&rdk_ibcm_lids, c, where);
	c->ic_in_l = B_TRUE;
	list_insert_tail(&rdk_ibcm_conns, c);
	rdk_ibcm_nconns++;
	c->ic_cd->rcd_conns++;
	mutex_exit(&rdk_ibcm_lock);
	return (0);
}

/*
 * Enter the remote IDs, for duplicate and stale detection (Linux
 * cm_insert_remote_id() and cm_insert_remote_qpn()).  EEXIST: *otherp is
 * the held connection that has them already.  ENOENT: the connection has
 * ended and left the tables, and must not enter them again.
 */
int
rdk_ibconn_insert_remote(rdk_ibconn_t *c, rdk_ibconn_t **otherp)
{
	rdk_ibconn_t *o;
	avl_index_t wr, wq;

	*otherp = NULL;
	mutex_enter(&rdk_ibcm_lock);
	if (!c->ic_in_l) {
		mutex_exit(&rdk_ibcm_lock);
		return (ENOENT);
	}
	if ((o = avl_find(&rdk_ibcm_rids, c, &wr)) == NULL &&
	    (o = avl_find(&rdk_ibcm_qpns, c, &wq)) == NULL) {
		avl_insert(&rdk_ibcm_rids, c, wr);
		avl_insert(&rdk_ibcm_qpns, c, wq);
		c->ic_in_r = c->ic_in_q = B_TRUE;
		mutex_exit(&rdk_ibcm_lock);
		return (0);
	}
	rdk_ibconn_hold(o);
	mutex_exit(&rdk_ibcm_lock);
	*otherp = o;
	return (EEXIST);
}

void
rdk_ibconn_drop_remote(rdk_ibconn_t *c)
{
	mutex_enter(&rdk_ibcm_lock);
	if (c->ic_in_r)
		avl_remove(&rdk_ibcm_rids, c);
	if (c->ic_in_q)
		avl_remove(&rdk_ibcm_qpns, c);
	c->ic_in_r = c->ic_in_q = B_FALSE;
	mutex_exit(&rdk_ibcm_lock);
}

/* Out of every table; the caller then drops the base reference. */
void
rdk_ibconn_unlink(rdk_ibconn_t *c)
{
	mutex_enter(&rdk_ibcm_lock);
	if (c->ic_in_r)
		avl_remove(&rdk_ibcm_rids, c);
	if (c->ic_in_q)
		avl_remove(&rdk_ibcm_qpns, c);
	c->ic_in_r = c->ic_in_q = B_FALSE;
	if (c->ic_in_l) {
		avl_remove(&rdk_ibcm_lids, c);
		list_remove(&rdk_ibcm_conns, c);
		c->ic_in_l = B_FALSE;
		VERIFY3U(rdk_ibcm_nconns, >, 0);
		rdk_ibcm_nconns--;
		VERIFY3U(c->ic_cd->rcd_conns, >, 0);
		c->ic_cd->rcd_conns--;
		cv_broadcast(&rdk_ibcm_cv);
	}
	mutex_exit(&rdk_ibcm_lock);
}

rdk_ibconn_t *
rdk_ibconn_find(uint32_t lid)
{
	rdk_ibconn_t key, *c;

	key.ic_lid = lid;
	mutex_enter(&rdk_ibcm_lock);
	if ((c = avl_find(&rdk_ibcm_lids, &key, NULL)) != NULL)
		rdk_ibconn_hold(c);
	mutex_exit(&rdk_ibcm_lock);
	return (c);
}

rdk_ibconn_t *
rdk_ibconn_find_remote(uint64_t guid, uint32_t rid)
{
	rdk_ibconn_t key, *c;

	key.ic_rguid = guid;
	key.ic_rid = rid;
	mutex_enter(&rdk_ibcm_lock);
	if ((c = avl_find(&rdk_ibcm_rids, &key, NULL)) != NULL)
		rdk_ibconn_hold(c);
	mutex_exit(&rdk_ibcm_lock);
	return (c);
}

rdk_ibconn_t *
rdk_ibconn_find_qpn(uint64_t guid, uint32_t qpn)
{
	rdk_ibconn_t key, *c;

	key.ic_rguid = guid;
	key.ic_rqpn = qpn;
	mutex_enter(&rdk_ibcm_lock);
	if ((c = avl_find(&rdk_ibcm_qpns, &key, NULL)) != NULL)
		rdk_ibconn_hold(c);
	mutex_exit(&rdk_ibcm_lock);
	return (c);
}

/*
 * The QP link.  Lock order: ic_lock, then the QP's cm_lock.
 */
int
rdk_cm_qp_attach(rdk_ibconn_t *c, struct rdk_qp *qp)
{
	int ret = 0;

	mutex_enter(&qp->cm_lock);
	if (qp->cm_dying || qp->cm_link != NULL)
		ret = EBUSY;
	else
		qp->cm_link = c;
	mutex_exit(&qp->cm_lock);
	if (ret != 0)
		return (ret);
	rdk_ibconn_hold(c);
	mutex_enter(&c->ic_lock);
	c->ic_qp = qp;
	c->ic_lqpn = qp->qp_num;
	mutex_exit(&c->ic_lock);
	return (0);
}

/* The QP for one use; NULL once it is unlinked. */
struct rdk_qp *
rdk_cm_qp_lease(rdk_ibconn_t *c)
{
	struct rdk_qp *qp;

	mutex_enter(&c->ic_lock);
	if ((qp = c->ic_qp) != NULL) {
		mutex_enter(&qp->cm_lock);
		qp->cm_leases++;
		mutex_exit(&qp->cm_lock);
	}
	mutex_exit(&c->ic_lock);
	return (qp);
}

void
rdk_cm_qp_unlease(struct rdk_qp *qp)
{
	mutex_enter(&qp->cm_lock);
	VERIFY3U(qp->cm_leases, >, 0);
	if (--qp->cm_leases == 0)
		cv_broadcast(&qp->cm_cv);
	mutex_exit(&qp->cm_lock);
}

/* The connection lets go of its QP. */
void
rdk_cm_qp_detach(rdk_ibconn_t *c)
{
	struct rdk_qp *qp;
	boolean_t owned = B_FALSE;

	if ((qp = rdk_cm_qp_lease(c)) == NULL)
		return;
	mutex_enter(&c->ic_lock);
	if (c->ic_qp == qp)
		c->ic_qp = NULL;
	mutex_exit(&c->ic_lock);
	mutex_enter(&qp->cm_lock);
	if (qp->cm_link == c) {
		qp->cm_link = NULL;
		owned = B_TRUE;
	}
	VERIFY3U(qp->cm_leases, >, 0);
	if (--qp->cm_leases == 0)
		cv_broadcast(&qp->cm_cv);
	mutex_exit(&qp->cm_lock);
	if (owned)
		rdk_ibconn_rele(c);
}

static void
rdk_ibconn_qp_gone_task(void *arg)
{
	rdk_ibconn_t *c = arg;

	rdk_ibconn_input(c, IBCI_QP_GONE);
	rdk_ibconn_rele(c);
}

/*
 * The consumer destroys the QP: unlink it and wait for the leases.  The
 * connection is aborted afterwards, from a task.
 */
void
rdk_cm_roce_qp_gone(struct rdk_qp *qp)
{
	rdk_ibconn_t *c;

	mutex_enter(&qp->cm_lock);
	qp->cm_dying = B_TRUE;
	c = qp->cm_link;
	qp->cm_link = NULL;
	mutex_exit(&qp->cm_lock);
	if (c == NULL)
		return;
	mutex_enter(&c->ic_lock);
	if (c->ic_qp == qp)
		c->ic_qp = NULL;
	c->ic_qp_gone = B_TRUE;
	mutex_exit(&c->ic_lock);
	mutex_enter(&qp->cm_lock);
	while (qp->cm_leases != 0)
		cv_wait(&qp->cm_cv, &qp->cm_lock);
	mutex_exit(&qp->cm_lock);
	/* The link's reference passes to the task. */
	if (taskq_dispatch(rdk_cm_taskq, rdk_ibconn_qp_gone_task, c,
	    TQ_SLEEP) == TASKQID_INVALID)
		rdk_ibconn_qp_gone_task(c);
}

/*
 * Timers.  ic_deadline is when the current timer is due; a timer task
 * acts only if it is, so a timer that was replaced or stopped while its
 * callback ran does nothing.  Each armed timer holds a reference.  A
 * callout may fire up to a tick before its time in hrtime, so it is set a
 * tick later than the deadline.
 */
static void
rdk_ibconn_timer(void *arg)
{
	rdk_ibconn_t *c = arg;

	if (taskq_dispatch(rdk_cm_taskq, rdk_ibconn_timer_task, c,
	    TQ_NOSLEEP) != TASKQID_INVALID)
		return;
	mutex_enter(&c->ic_lock);
	c->ic_timer = timeout(rdk_ibconn_timer, c,
	    drv_usectohz(10 * MILLISEC));
	mutex_exit(&c->ic_lock);
}

/* Arm (ms > 0) or stop (0) the timer; the old one is returned.  Locked. */
timeout_id_t
rdk_ibconn_timer_set(rdk_ibconn_t *c, uint32_t ms)
{
	timeout_id_t old = c->ic_timer;

	ASSERT(MUTEX_HELD(&c->ic_lock));
	c->ic_timer = 0;
	c->ic_deadline = 0;
	if (ms == 0)
		return (old);
	ms = rdk_ibcm_clamp_ms(ms);
	c->ic_deadline = gethrtime() + MSEC2NSEC(ms);
	rdk_ibconn_hold(c);
	c->ic_timer = timeout(rdk_ibconn_timer, c,
	    drv_usectohz((clock_t)ms * MILLISEC) + 1);
	return (old);
}

/* Stop a replaced timer; its reference goes if it will never run. */
void
rdk_ibconn_timer_cancel(rdk_ibconn_t *c, timeout_id_t tid)
{
	if (tid != 0 && untimeout(tid) != -1)
		rdk_ibconn_rele(c);
}

/* Arm from outside a step: the deadline of passive path resolution. */
void
rdk_ibconn_arm(rdk_ibconn_t *c, uint32_t ms)
{
	timeout_id_t old;

	mutex_enter(&c->ic_lock);
	old = rdk_ibconn_timer_set(c, ms);
	c->ic_resolving = ms != 0;
	mutex_exit(&c->ic_lock);
	rdk_ibconn_timer_cancel(c, old);
}

static void
rdk_ibconn_timer_task(void *arg)
{
	rdk_ibconn_t *c = arg;
	hrtime_t now = gethrtime();
	boolean_t due = B_FALSE, resolving = B_FALSE;

	mutex_enter(&c->ic_lock);
	if (c->ic_deadline != 0 && now >= c->ic_deadline) {
		due = B_TRUE;
		resolving = c->ic_resolving;
		c->ic_deadline = 0;
		c->ic_timer = 0;
		c->ic_resolving = B_FALSE;
	}
	mutex_exit(&c->ic_lock);
	if (due && resolving)
		rdk_cm_roce_resolve_timeout(c);
	else if (due)
		rdk_ibconn_input(c, IBCI_TIMER);
	rdk_ibconn_rele(c);
}

/*
 * Devices.
 */
int
rdk_cm_roce_dev_add(rdk_cm_dev_t *cd)
{
	rdk_gsi_t *g;
	uint32_t i;
	int ret, first = 0;

	for (i = 0; i < cd->rcd_nports; i++) {
		if ((ret = rdk_gsi_create(cd->rcd_dev, i + 1, &g)) != 0) {
			if (first == 0)
				first = ret;
			continue;
		}
		mutex_enter(&rdk_cm_lock);
		cd->rcd_gsi[i] = g;
		mutex_exit(&rdk_cm_lock);
	}
	return (first);
}

/*
 * The device's IDs are gone.  End the connections that remain (timewait,
 * or a request being resolved), then the GSI agents.
 */
void
rdk_cm_roce_dev_remove(rdk_cm_dev_t *cd)
{
	rdk_ibconn_t *c, **v;
	rdk_gsi_t *g;
	uint32_t i, n = 0, max;

	mutex_enter(&rdk_ibcm_lock);
	max = cd->rcd_conns;
	mutex_exit(&rdk_ibcm_lock);
	v = max != 0 ? kmem_alloc(max * sizeof (*v), KM_SLEEP) : NULL;
	mutex_enter(&rdk_ibcm_lock);
	for (c = list_head(&rdk_ibcm_conns); c != NULL && n < max;
	    c = list_next(&rdk_ibcm_conns, c)) {
		if (c->ic_cd == cd) {
			rdk_ibconn_hold(c);
			v[n++] = c;
		}
	}
	mutex_exit(&rdk_ibcm_lock);
	for (i = 0; i < n; i++) {
		rdk_ibconn_input(v[i], IBCI_FLUSH);
		rdk_ibconn_rele(v[i]);
	}
	if (v != NULL)
		kmem_free(v, max * sizeof (*v));

	/* A connection made while the list was walked ends by itself. */
	mutex_enter(&rdk_ibcm_lock);
	while (cd->rcd_conns != 0)
		cv_wait(&rdk_ibcm_cv, &rdk_ibcm_lock);
	mutex_exit(&rdk_ibcm_lock);

	for (i = 0; i < RDK_CM_MAX_PORTS; i++) {
		mutex_enter(&rdk_cm_lock);
		g = cd->rcd_gsi[i];
		cd->rcd_gsi[i] = NULL;
		mutex_exit(&rdk_cm_lock);
		if (g != NULL)
			rdk_gsi_destroy(g);
	}
}

/* A GID was withdrawn: the GSI agent lets go of AHs that use it. */
void
rdk_cm_roce_gid_withdrawn(struct rdk_device *dev, uint32_t port,
    uint16_t idx)
{
	rdk_cm_dev_t *cd;
	rdk_gsi_t *g = NULL;

	mutex_enter(&rdk_cm_lock);
	if ((cd = rdk_cm_dev_find_locked(dev)) != NULL && cd->rcd_roce &&
	    port >= 1 && port <= RDK_CM_MAX_PORTS &&
	    (g = cd->rcd_gsi[port - 1]) != NULL)
		rdk_gsi_hold(g);
	mutex_exit(&rdk_cm_lock);
	if (g != NULL) {
		rdk_gsi_gid_withdrawn(g, idx);
		rdk_gsi_rele(g);
	}
}

int
rdk_cm_roce_init(void)
{
	uint32_t hi;
	int ret;

	if ((ret = rdk_gsi_init()) != 0)
		return (ret);
	mutex_init(&rdk_ibcm_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&rdk_ibcm_cv, NULL, CV_DRIVER, NULL);
	avl_create(&rdk_ibcm_lids, rdk_ibcm_lid_cmp, sizeof (rdk_ibconn_t),
	    offsetof(rdk_ibconn_t, ic_lnode));
	avl_create(&rdk_ibcm_rids, rdk_ibcm_rid_cmp, sizeof (rdk_ibconn_t),
	    offsetof(rdk_ibconn_t, ic_rnode));
	avl_create(&rdk_ibcm_qpns, rdk_ibcm_qpn_cmp, sizeof (rdk_ibconn_t),
	    offsetof(rdk_ibconn_t, ic_qnode));
	list_create(&rdk_ibcm_conns, sizeof (rdk_ibconn_t),
	    offsetof(rdk_ibconn_t, ic_node));
	(void) random_get_pseudo_bytes((uint8_t *)&hi, sizeof (hi));
	rdk_ibcm_hi_tid = (uint64_t)hi << 32;
	return (0);
}

int
rdk_cm_roce_fini(void)
{
	mutex_enter(&rdk_ibcm_lock);
	if (rdk_ibcm_nconns != 0) {
		mutex_exit(&rdk_ibcm_lock);
		return (EBUSY);
	}
	mutex_exit(&rdk_ibcm_lock);
	list_destroy(&rdk_ibcm_conns);
	avl_destroy(&rdk_ibcm_qpns);
	avl_destroy(&rdk_ibcm_rids);
	avl_destroy(&rdk_ibcm_lids);
	cv_destroy(&rdk_ibcm_cv);
	mutex_destroy(&rdk_ibcm_lock);
	rdk_gsi_fini();
	return (0);
}

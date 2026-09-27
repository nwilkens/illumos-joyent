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
 * ports' GSI agents, compatible with the Linux RDMA CM.
 *
 * A connection's state machine (rdk_ibcm_fsm.c) decides; this file keeps
 * the tables, the timers and the QP, and runs the actions.  Local
 * communication IDs are random, and every later message must come from the
 * addresses the connection was made with and carry its IDs, so that an
 * off-path sender cannot guess its way into a connection.
 *
 * The QP belongs to the consumer.  A connection uses it only through a
 * lease taken under the QP's cm_lock, and rdk_destroy_qp() unlinks the
 * connection and waits for the leases before the provider frees the QP.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <netinet/in.h>

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
/* The RNR NAK timer a QP asks of its peer: 12 is 0.64 ms. */
uint_t rdk_ibcm_min_rnr_timer = 12;
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

/*
 * The RoCE path MTU for an IP MTU: the IP, UDP, BTH, largest extended
 * header and ICRC take 96 bytes (Linux iboe_get_mtu()).  0 if none fits.
 */
uint8_t
rdk_cm_roce_mtu(struct rdk_device *dev, uint32_t port, uint32_t ip_mtu)
{
	struct rdk_port_attr pa;
	enum rdk_mtu m;

	if (ip_mtu < 256 + 96)
		return (0);
	m = rdk_mtu_int_to_enum((int)(ip_mtu - 96));
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
 * cm_insert_remote_id() and cm_insert_remote_qpn()).  On B_FALSE *otherp
 * is the held connection that has them already.
 */
boolean_t
rdk_ibconn_insert_remote(rdk_ibconn_t *c, rdk_ibconn_t **otherp)
{
	rdk_ibconn_t *o;
	avl_index_t wr, wq;

	*otherp = NULL;
	mutex_enter(&rdk_ibcm_lock);
	if ((o = avl_find(&rdk_ibcm_rids, c, &wr)) == NULL &&
	    (o = avl_find(&rdk_ibcm_qpns, c, &wq)) == NULL) {
		avl_insert(&rdk_ibcm_rids, c, wr);
		avl_insert(&rdk_ibcm_qpns, c, wq);
		c->ic_in_r = c->ic_in_q = B_TRUE;
		mutex_exit(&rdk_ibcm_lock);
		return (B_TRUE);
	}
	rdk_ibconn_hold(o);
	mutex_exit(&rdk_ibcm_lock);
	*otherp = o;
	return (B_FALSE);
}

static void
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
static int
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
static struct rdk_qp *
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

static void
rdk_cm_qp_unlease(struct rdk_qp *qp)
{
	mutex_enter(&qp->cm_lock);
	VERIFY3U(qp->cm_leases, >, 0);
	if (--qp->cm_leases == 0)
		cv_broadcast(&qp->cm_cv);
	mutex_exit(&qp->cm_lock);
}

/* The connection lets go of its QP. */
static void
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
static timeout_id_t
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
static void
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

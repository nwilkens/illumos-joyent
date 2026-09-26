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
 * The iWARP backend of the connection manager: the provider operations,
 * the events providers deliver, and listener admission.  Written for
 * illumos after the contract of the Linux iw_cm_verbs.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/disp.h>
#include <sys/socket.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"

static kcondvar_t rdk_cm_iw_cv;
static uint_t rdk_cm_pending;	/* rdk_cm_lock: admissions held */

/* How often a wait for a provider's final event warns. */
#define	RDK_CM_WAIT_WARN_SEC	10

static rdk_cm_dev_t *
rdk_cm_dev_find_locked(struct rdk_device *dev)
{
	rdk_cm_dev_t *cd;

	ASSERT(MUTEX_HELD(&rdk_cm_lock));
	for (cd = list_head(&rdk_cm_devs); cd != NULL;
	    cd = list_next(&rdk_cm_devs, cd)) {
		if (cd->rcd_dev == dev)
			return (cd);
	}
	return (NULL);
}

int
rdk_iw_cm_attach(struct rdk_device *dev, const struct rdk_iw_cm_ops *ops)
{
	rdk_cm_dev_t *cd;

	if (dev == NULL || ops == NULL || ops->iw_version != RDK_ABI_VERSION ||
	    ops->iw_connect == NULL || ops->iw_accept == NULL ||
	    ops->iw_reject == NULL || ops->iw_create_listen == NULL ||
	    ops->iw_destroy_listen == NULL || ops->iw_disconnect == NULL ||
	    ops->iw_release == NULL ||
	    ops->iw_max_pdata > RDK_CM_PDATA_MAX)
		return (EINVAL);
	cd = kmem_zalloc(sizeof (*cd), KM_SLEEP);
	cd->rcd_dev = dev;
	cd->rcd_iw = ops;
	mutex_enter(&rdk_cm_lock);
	if (rdk_cm_dev_find_locked(dev) != NULL) {
		mutex_exit(&rdk_cm_lock);
		kmem_free(cd, sizeof (*cd));
		return (EEXIST);
	}
	list_insert_tail(&rdk_cm_devs, cd);
	mutex_exit(&rdk_cm_lock);
	return (0);
}

void
rdk_iw_cm_detach(struct rdk_device *dev)
{
	rdk_cm_dev_t *cd;

	mutex_enter(&rdk_cm_lock);
	if ((cd = rdk_cm_dev_find_locked(dev)) == NULL) {
		mutex_exit(&rdk_cm_lock);
		return;
	}
	cd->rcd_removing = B_TRUE;
	while (cd->rcd_ops != 0 || cd->rcd_ids != 0)
		cv_wait(&rdk_cm_iw_cv, &rdk_cm_lock);
	list_remove(&rdk_cm_devs, cd);
	mutex_exit(&rdk_cm_lock);
	kmem_free(cd, sizeof (*cd));
}

void
rdk_cm_dev_rele(rdk_cm_dev_t *cd)
{
	mutex_enter(&rdk_cm_lock);
	VERIFY3U(cd->rcd_ids, >, 0);
	if (--cd->rcd_ids == 0)
		cv_broadcast(&rdk_cm_iw_cv);
	mutex_exit(&rdk_cm_lock);
}

/*
 * Hold the device's operations and the ID's provider state for one call;
 * NULL when either is going.
 */
static const struct rdk_iw_cm_ops *
rdk_cm_ops_enter(rdk_cm_id_t *id)
{
	rdk_cm_dev_t *cd = id->rci_dev;
	const struct rdk_iw_cm_ops *ops = NULL;

	if (cd == NULL)
		return (NULL);
	mutex_enter(&rdk_cm_lock);
	if (cd->rcd_iw != NULL) {
		cd->rcd_ops++;
		ops = cd->rcd_iw;
	}
	mutex_exit(&rdk_cm_lock);
	if (ops == NULL)
		return (NULL);
	mutex_enter(&id->rci_lock);
	if (id->rci_iw_gone) {
		mutex_exit(&id->rci_lock);
		mutex_enter(&rdk_cm_lock);
		if (--cd->rcd_ops == 0)
			cv_broadcast(&rdk_cm_iw_cv);
		mutex_exit(&rdk_cm_lock);
		return (NULL);
	}
	id->rci_iw_calls++;
	mutex_exit(&id->rci_lock);
	return (ops);
}

static void
rdk_cm_ops_exit(rdk_cm_id_t *id)
{
	rdk_cm_dev_t *cd = id->rci_dev;

	mutex_enter(&id->rci_lock);
	VERIFY3U(id->rci_iw_calls, >, 0);
	if (--id->rci_iw_calls == 0)
		cv_broadcast(&id->rci_cv);
	mutex_exit(&id->rci_lock);
	mutex_enter(&rdk_cm_lock);
	VERIFY3U(cd->rcd_ops, >, 0);
	if (--cd->rcd_ops == 0)
		cv_broadcast(&rdk_cm_iw_cv);
	mutex_exit(&rdk_cm_lock);
}

/*
 * The ID is being destroyed and the provider has delivered its final
 * event: once no call runs, give the provider its state back.
 */
void
rdk_cm_iw_release(rdk_cm_id_t *id)
{
	const struct rdk_iw_cm_ops *ops;
	boolean_t owned;

	if ((ops = rdk_cm_ops_enter(id)) == NULL)
		return;
	mutex_enter(&id->rci_lock);
	while (id->rci_iw_calls != 1)
		cv_wait(&id->rci_cv, &id->rci_lock);
	id->rci_iw_gone = B_TRUE;
	owned = id->rci_iw_owned;
	id->rci_iw_owned = B_FALSE;
	mutex_exit(&id->rci_lock);
	if (owned)
		ops->iw_release(id->rci_dev->rcd_dev, &id->rci_iw);
	rdk_cm_ops_exit(id);
}

int
rdk_cm_iw_listen(rdk_cm_id_t *id)
{
	const struct rdk_iw_cm_ops *ops;
	int ret;

	if ((ops = rdk_cm_ops_enter(id)) == NULL)
		return (ENXIO);
	ret = ops->iw_create_listen(id->rci_dev->rcd_dev, &id->rci_iw);
	if (ret == 0) {
		mutex_enter(&id->rci_lock);
		id->rci_iw_listen = B_TRUE;
		mutex_exit(&id->rci_lock);
	}
	rdk_cm_ops_exit(id);
	return (ret);
}

void
rdk_cm_iw_unlisten(rdk_cm_id_t *id)
{
	const struct rdk_iw_cm_ops *ops;

	if ((ops = rdk_cm_ops_enter(id)) == NULL)
		return;
	ops->iw_destroy_listen(id->rci_dev->rcd_dev, &id->rci_iw);
	mutex_enter(&id->rci_lock);
	id->rci_iw_listen = B_FALSE;
	mutex_exit(&id->rci_lock);
	rdk_cm_ops_exit(id);
}

static void
rdk_cm_iw_param(const struct rdk_cm_conn_param *p, struct rdk_iw_conn_param *ip)
{
	bzero(ip, sizeof (*ip));
	ip->qp = p->qp;
	ip->pdata = p->private_data;
	ip->pdata_len = p->private_data_len;
	ip->ird = p->responder_resources;
	ip->ord = p->initiator_depth;
}

/*
 * The provider may deliver its final event before the call returns, so the
 * reference is marked before the call and taken back if the call fails.
 */
static int
rdk_cm_iw_open(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p,
    boolean_t active)
{
	const struct rdk_iw_cm_ops *ops;
	struct rdk_iw_conn_param ip;
	int ret;

	if ((ops = rdk_cm_ops_enter(id)) == NULL)
		return (ENXIO);
	rdk_cm_iw_param(p, &ip);
	mutex_enter(&id->rci_lock);
	id->rci_iw_ref = B_TRUE;
	mutex_exit(&id->rci_lock);
	if (active)
		ret = ops->iw_connect(id->rci_dev->rcd_dev, &id->rci_iw, &ip);
	else
		ret = ops->iw_accept(id->rci_dev->rcd_dev, &id->rci_iw, &ip);
	mutex_enter(&id->rci_lock);
	if (ret != 0) {
		id->rci_iw_ref = B_FALSE;
		cv_broadcast(&id->rci_cv);
	} else if (active) {
		id->rci_iw_owned = B_TRUE;
	}
	mutex_exit(&id->rci_lock);
	rdk_cm_ops_exit(id);
	return (ret);
}

int
rdk_cm_iw_connect(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	return (rdk_cm_iw_open(id, p, B_TRUE));
}

int
rdk_cm_iw_accept(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	return (rdk_cm_iw_open(id, p, B_FALSE));
}

int
rdk_cm_iw_reject(rdk_cm_id_t *id, const void *pdata, uint16_t len)
{
	const struct rdk_iw_cm_ops *ops;
	int ret;

	if ((ops = rdk_cm_ops_enter(id)) == NULL)
		return (ENXIO);
	ret = ops->iw_reject(id->rci_dev->rcd_dev, &id->rci_iw, pdata, len);
	rdk_cm_ops_exit(id);
	return (ret);
}

void
rdk_cm_iw_disconnect(rdk_cm_id_t *id, boolean_t abrupt)
{
	const struct rdk_iw_cm_ops *ops;
	boolean_t live;

	if ((ops = rdk_cm_ops_enter(id)) == NULL)
		return;
	mutex_enter(&id->rci_lock);
	live = id->rci_iw_ref;
	mutex_exit(&id->rci_lock);
	if (live) {
		(void) ops->iw_disconnect(id->rci_dev->rcd_dev, &id->rci_iw,
		    abrupt);
	}
	rdk_cm_ops_exit(id);
}

/* Wait until the provider has let go of the ID. */
void
rdk_cm_iw_wait_final(rdk_cm_id_t *id)
{
	mutex_enter(&id->rci_lock);
	while (id->rci_iw_ref) {
		if (cv_reltimedwait(&id->rci_cv, &id->rci_lock,
		    SEC_TO_TICK(RDK_CM_WAIT_WARN_SEC), TR_SEC) == -1 &&
		    id->rci_iw_ref) {
			cmn_err(CE_WARN, "!rdk_cm: waiting for the provider "
			    "to release a connection");
		}
	}
	mutex_exit(&id->rci_lock);
}

static boolean_t
rdk_cm_same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return (a->sin_family == AF_INET && a->sin_addr.s_addr ==
	    b->sin_addr.s_addr && a->sin_port == b->sin_port);
}

/* A new ID for a request on a listener. */
static int
rdk_cm_iw_request(rdk_cm_id_t *listener, const struct rdk_iw_cm_event *ev)
{
	rdk_cm_admit_t *ra = ev->ev_admit;
	rdk_cm_id_t *id;
	rdk_cm_qev_t *q;
	uint16_t len;

	if (ra == NULL || ra->ra_listener != listener || ra->ra_done ||
	    ra->ra_peer != ev->ev_raddr.sin_addr.s_addr ||
	    ev->ev_raddr.sin_family != AF_INET || ev->ev_raddr.sin_port == 0 ||
	    ev->ev_child == NULL)
		return (EINVAL);
	len = ev->ev_pdata_len;
	if (len > rdk_cm_pdata_max(listener, RDK_CM_MSG_REQ) ||
	    (len != 0 && ev->ev_pdata == NULL))
		return (EINVAL);
	q = rdk_cm_qev_alloc(RDK_CM_EVENT_CONNECT_REQUEST, 0, ev->ev_pdata,
	    len);
	q->q_ev.param.responder_resources = (uint8_t)MIN(ev->ev_ird,
	    UINT8_MAX);
	q->q_ev.param.initiator_depth = (uint8_t)MIN(ev->ev_ord, UINT8_MAX);
	id = kmem_zalloc(sizeof (*id), KM_SLEEP);

	mutex_enter(&rdk_cm_lock);
	mutex_enter(&listener->rci_lock);
	if (listener->rci_destroying || listener->rci_state != RCS_LISTEN ||
	    listener->rci_removal || listener->rci_dev->rcd_removing ||
	    !rdk_cm_same_addr(&ev->ev_laddr, &listener->rci_iw.iw_laddr) ||
	    ev->ev_port != listener->rci_port) {
		mutex_exit(&listener->rci_lock);
		mutex_exit(&rdk_cm_lock);
		kmem_free(q, sizeof (*q));
		kmem_free(id, sizeof (*id));
		return (ECONNREFUSED);
	}
	mutex_init(&id->rci_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&id->rci_cv, NULL, CV_DRIVER, NULL);
	list_create(&id->rci_events, sizeof (rdk_cm_qev_t),
	    offsetof(rdk_cm_qev_t, q_node));
	id->rci_refs = 1;
	id->rci_handler = listener->rci_handler;
	id->rci_ctx = listener->rci_ctx;
	crhold(listener->rci_cred);
	id->rci_cred = listener->rci_cred;
	id->rci_qpt = listener->rci_qpt;
	id->rci_dev = listener->rci_dev;
	id->rci_dev->rcd_ids++;
	id->rci_port = listener->rci_port;
	id->rci_ifindex = listener->rci_ifindex;
	id->rci_resv = listener->rci_resv;
	rdk_cm_resv_hold(id->rci_resv);
	id->rci_route = listener->rci_route;
	id->rci_route.rcr_dst = ev->ev_raddr;
	id->rci_iw.iw_priv = id;
	id->rci_iw.iw_laddr = ev->ev_laddr;
	id->rci_iw.iw_raddr = ev->ev_raddr;
	id->rci_iw.iw_port = ev->ev_port;
	id->rci_iw.iw_vlan = RDK_VLAN_NONE;
	id->rci_iw.iw_mtu = listener->rci_iw.iw_mtu;
	id->rci_iw.iw_provider = ev->ev_child;
	id->rci_iw_owned = B_TRUE;
	id->rci_state = RCS_REQ;
	id->rci_listener = listener;
	listener->rci_refs++;
	id->rci_admit = ra;
	mutex_exit(&listener->rci_lock);
	list_insert_tail(&rdk_cm_ids, id);
	mutex_exit(&rdk_cm_lock);

	mutex_enter(&id->rci_lock);
	rdk_cm_queue_locked(id, q);
	mutex_exit(&id->rci_lock);
	return (0);
}

int
rdk_iw_cm_event(struct rdk_iw_cm_id *iwid, const struct rdk_iw_cm_event *ev)
{
	rdk_cm_id_t *id;
	boolean_t final;

	if (iwid == NULL || ev == NULL || (id = iwid->iw_priv) == NULL)
		return (EINVAL);
	if (ev->ev_type == RDK_IW_EVENT_CONNECT_REQUEST)
		return (rdk_cm_iw_request(id, ev));
	if (ev->ev_pdata_len > RDK_CM_PDATA_MAX ||
	    (ev->ev_pdata_len != 0 && ev->ev_pdata == NULL))
		return (EINVAL);

	final = ev->ev_type == RDK_IW_EVENT_CLOSE ||
	    (ev->ev_type == RDK_IW_EVENT_CONNECT_REPLY && ev->ev_status != 0);
	mutex_enter(&id->rci_lock);
	if (!id->rci_iw_ref) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	mutex_exit(&id->rci_lock);

	rdk_cm_conn_event(id, ev->ev_type, ev->ev_status, ev->ev_pdata,
	    ev->ev_pdata_len, ev->ev_ird, ev->ev_ord);
	if (final) {
		rdk_cm_conn_unquota();
		/* The ID may be freed as soon as rci_lock is dropped. */
		mutex_enter(&id->rci_lock);
		id->rci_iw_ref = B_FALSE;
		cv_broadcast(&id->rci_cv);
		mutex_exit(&id->rci_lock);
	}
	return (0);
}

int
rdk_iw_cm_admit(struct rdk_iw_cm_id *iwid, const struct sockaddr_in *peer,
    void **admitp)
{
	rdk_cm_id_t *id;
	rdk_cm_admit_t *ra;
	int ret = 0;

	*admitp = NULL;
	if (iwid == NULL || (id = iwid->iw_priv) == NULL || peer == NULL ||
	    peer->sin_family != AF_INET)
		return (EINVAL);
	if (!rdk_cm_unicast(peer->sin_addr.s_addr))
		return (EACCES);
	if ((ra = kmem_zalloc(sizeof (*ra), KM_NOSLEEP)) == NULL)
		return (ENOMEM);

	mutex_enter(&rdk_cm_lock);
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_LISTEN ||
	    id->rci_removal || id->rci_acl == NULL)
		ret = ECONNREFUSED;
	else if (!rdk_cm_acl_allows(id->rci_acl, peer->sin_addr.s_addr))
		ret = EACCES;
	else if (id->rci_pending >= id->rci_backlog ||
	    rdk_cm_pending >= rdk_cm_max_pending)
		ret = ENOBUFS;
	if (ret == 0) {
		id->rci_pending++;
		id->rci_refs++;
		rdk_cm_pending++;
		ra->ra_listener = id;
		ra->ra_peer = peer->sin_addr.s_addr;
	}
	mutex_exit(&id->rci_lock);
	mutex_exit(&rdk_cm_lock);
	if (ret != 0) {
		kmem_free(ra, sizeof (*ra));
		return (ret);
	}
	*admitp = ra;
	return (0);
}

void
rdk_iw_cm_unadmit(void *arg)
{
	rdk_cm_admit_t *ra = arg;
	rdk_cm_id_t *id;

	if (ra == NULL)
		return;
	id = ra->ra_listener;
	mutex_enter(&rdk_cm_lock);
	mutex_enter(&id->rci_lock);
	VERIFY3U(id->rci_pending, >, 0);
	id->rci_pending--;
	VERIFY3U(rdk_cm_pending, >, 0);
	rdk_cm_pending--;
	ra->ra_done = B_TRUE;
	mutex_exit(&id->rci_lock);
	mutex_exit(&rdk_cm_lock);
	kmem_free(ra, sizeof (*ra));
	rdk_cm_rele(id);
}

void
rdk_cm_iw_init(void)
{
	cv_init(&rdk_cm_iw_cv, NULL, CV_DRIVER, NULL);
}

void
rdk_cm_iw_fini(void)
{
	cv_destroy(&rdk_cm_iw_cv);
}

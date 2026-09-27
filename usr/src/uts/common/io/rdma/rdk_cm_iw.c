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
	cd->rcd_tp = &rdk_cm_iw_tport;
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

boolean_t
rdk_device_iwarp(struct rdk_device *dev)
{
	rdk_cm_dev_t *cd;
	boolean_t ret;

	mutex_enter(&rdk_cm_lock);
	cd = rdk_cm_dev_find_locked(dev);
	ret = cd != NULL && cd->rcd_iw != NULL;
	mutex_exit(&rdk_cm_lock);
	return (ret);
}

void
rdk_iw_cm_detach(struct rdk_device *dev)
{
	rdk_cm_dev_t *cd;

	mutex_enter(&rdk_cm_lock);
	if ((cd = rdk_cm_dev_find_locked(dev)) == NULL || cd->rcd_iw == NULL) {
		mutex_exit(&rdk_cm_lock);
		return;
	}
	cd->rcd_removing = B_TRUE;
	while (cd->rcd_ops != 0 || cd->rcd_ids != 0)
		cv_wait(&rdk_cm_dev_cv, &rdk_cm_lock);
	list_remove(&rdk_cm_devs, cd);
	mutex_exit(&rdk_cm_lock);
	kmem_free(cd, sizeof (*cd));
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
			cv_broadcast(&rdk_cm_dev_cv);
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
		cv_broadcast(&rdk_cm_dev_cv);
	mutex_exit(&rdk_cm_lock);
}

/*
 * The ID is being destroyed and the provider has delivered its final
 * event: once no call runs, give the provider its state back.
 */
static void
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

static int
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

static void
rdk_cm_iw_unlisten(rdk_cm_id_t *id)
{
	const struct rdk_iw_cm_ops *ops;
	boolean_t listening;

	mutex_enter(&id->rci_lock);
	listening = id->rci_iw_listen;
	mutex_exit(&id->rci_lock);
	if (!listening || (ops = rdk_cm_ops_enter(id)) == NULL)
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
	id->rci_tp_ref = B_TRUE;
	mutex_exit(&id->rci_lock);
	if (active)
		ret = ops->iw_connect(id->rci_dev->rcd_dev, &id->rci_iw, &ip);
	else
		ret = ops->iw_accept(id->rci_dev->rcd_dev, &id->rci_iw, &ip);
	mutex_enter(&id->rci_lock);
	if (ret != 0) {
		id->rci_tp_ref = B_FALSE;
		cv_broadcast(&id->rci_cv);
	} else if (active) {
		id->rci_iw_owned = B_TRUE;
	}
	mutex_exit(&id->rci_lock);
	rdk_cm_ops_exit(id);
	return (ret);
}

static int
rdk_cm_iw_connect(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	return (rdk_cm_iw_open(id, p, B_TRUE));
}

static int
rdk_cm_iw_accept(rdk_cm_id_t *id, const struct rdk_cm_conn_param *p)
{
	return (rdk_cm_iw_open(id, p, B_FALSE));
}

static int
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

static void
rdk_cm_iw_disconnect(rdk_cm_id_t *id, boolean_t abrupt)
{
	const struct rdk_iw_cm_ops *ops;
	boolean_t live;

	if ((ops = rdk_cm_ops_enter(id)) == NULL)
		return;
	mutex_enter(&id->rci_lock);
	live = id->rci_tp_ref;
	mutex_exit(&id->rci_lock);
	if (live) {
		(void) ops->iw_disconnect(id->rci_dev->rcd_dev, &id->rci_iw,
		    abrupt);
	}
	rdk_cm_ops_exit(id);
}

static uint16_t
rdk_cm_iw_pdata_max(rdk_cm_dev_t *cd, enum rdk_cm_msg msg)
{
	_NOTE(ARGUNUSED(msg));
	return (cd->rcd_iw != NULL ? cd->rcd_iw->iw_max_pdata : 0);
}

const rdk_cm_tport_t rdk_cm_iw_tport = {
	.ct_listen = rdk_cm_iw_listen,
	.ct_unlisten = rdk_cm_iw_unlisten,
	.ct_connect = rdk_cm_iw_connect,
	.ct_accept = rdk_cm_iw_accept,
	.ct_reject = rdk_cm_iw_reject,
	.ct_disconnect = rdk_cm_iw_disconnect,
	.ct_release = rdk_cm_iw_release,
	.ct_pdata_max = rdk_cm_iw_pdata_max
};

/* A new ID for a request on a listener. */
static int
rdk_cm_iw_request(rdk_cm_id_t *listener, const struct rdk_iw_cm_event *ev)
{
	rdk_cm_child_t cc;
	rdk_cm_id_t *child;

	if (ev->ev_child == NULL)
		return (EINVAL);
	bzero(&cc, sizeof (cc));
	cc.cc_laddr = ev->ev_laddr;
	cc.cc_raddr = ev->ev_raddr;
	cc.cc_port = ev->ev_port;
	cc.cc_pdata = ev->ev_pdata;
	cc.cc_pdata_len = ev->ev_pdata_len;
	cc.cc_ird = ev->ev_ird;
	cc.cc_ord = ev->ev_ord;
	cc.cc_admit = ev->ev_admit;
	cc.cc_iw_provider = ev->ev_child;
	return (rdk_cm_child_new(listener, &cc, &child));
}

int
rdk_iw_cm_event(struct rdk_iw_cm_id *iwid, const struct rdk_iw_cm_event *ev)
{
	rdk_cm_id_t *id;
	rdk_cm_tev_t type;
	boolean_t final;
	int status;

	if (iwid == NULL || ev == NULL || (id = iwid->iw_priv) == NULL)
		return (EINVAL);
	if (ev->ev_type == RDK_IW_EVENT_CONNECT_REQUEST)
		return (rdk_cm_iw_request(id, ev));
	if (ev->ev_pdata_len > RDK_CM_PDATA_MAX ||
	    (ev->ev_pdata_len != 0 && ev->ev_pdata == NULL))
		return (EINVAL);
	switch (ev->ev_type) {
	case RDK_IW_EVENT_CONNECT_REPLY:
		type = RCT_REPLY;
		break;
	case RDK_IW_EVENT_ESTABLISHED:
		type = RCT_ESTABLISHED;
		break;
	case RDK_IW_EVENT_DISCONNECT:
		type = RCT_DISCONNECT;
		break;
	case RDK_IW_EVENT_CLOSE:
		type = RCT_CLOSE;
		break;
	default:
		return (EINVAL);
	}
	status = ev->ev_status;
	final = type == RCT_CLOSE || (type == RCT_REPLY && status != 0);
	mutex_enter(&id->rci_lock);
	if (!id->rci_tp_ref) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	mutex_exit(&id->rci_lock);

	rdk_cm_conn_event(id, type, status, 0, ev->ev_pdata, ev->ev_pdata_len,
	    ev->ev_ird, ev->ev_ord);
	if (final)
		rdk_cm_final(id);
	return (0);
}

int
rdk_iw_cm_admit(struct rdk_iw_cm_id *iwid, const struct sockaddr_in *peer,
    void **admitp)
{
	rdk_cm_id_t *id;

	*admitp = NULL;
	if (iwid == NULL || (id = iwid->iw_priv) == NULL || peer == NULL ||
	    peer->sin_family != AF_INET)
		return (EINVAL);
	return (rdk_cm_admit(id, peer->sin_addr.s_addr, admitp));
}

void
rdk_iw_cm_unadmit(void *arg)
{
	rdk_cm_unadmit(arg);
}

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
 * Listeners, their allow-lists and admission, the IDs of the requests they
 * receive, and the CM's view of the devices.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"

static uint_t rdk_cm_pending;	/* rdk_cm_lock: admissions held */

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
	const rdk_cm_tport_t *tp;
	int ret;

	if (acl == NULL || backlog <= 0 || backlog > RDK_CM_BACKLOG_MAX)
		return (EINVAL);
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_BOUND ||
	    id->rci_dev == NULL || id->rci_resv->rr_addr.sin_port == 0) {
		mutex_exit(&id->rci_lock);
		return (EINVAL);
	}
	tp = id->rci_dev->rcd_tp;
	rdk_cm_acl_hold(acl);
	id->rci_acl = acl;
	id->rci_backlog = (uint32_t)backlog;
	id->rci_state = RCS_LISTEN;
	mutex_exit(&id->rci_lock);

	rdk_cm_resv_used(id->rci_resv);
	if ((ret = tp->ct_listen(id)) != 0) {
		mutex_enter(&id->rci_lock);
		id->rci_state = RCS_BOUND;
		id->rci_acl = NULL;
		mutex_exit(&id->rci_lock);
		rdk_cm_acl_rele(acl);
	}
	return (ret);
}

/*
 * A listener's admission check, before the transport answers a request:
 * the peer is on the allow-list and the backlog has room.  Any context that
 * can take an adaptive mutex.
 */
int
rdk_cm_admit(rdk_cm_id_t *id, ipaddr_t peer, void **admitp)
{
	rdk_cm_admit_t *ra;
	int ret = 0;

	*admitp = NULL;
	if (!rdk_cm_unicast(peer))
		return (EACCES);
	if ((ra = kmem_zalloc(sizeof (*ra), KM_NOSLEEP)) == NULL)
		return (ENOMEM);

	mutex_enter(&rdk_cm_lock);
	mutex_enter(&id->rci_lock);
	if (id->rci_destroying || id->rci_state != RCS_LISTEN ||
	    id->rci_removal || id->rci_acl == NULL)
		ret = ECONNREFUSED;
	else if (!rdk_cm_acl_allows(id->rci_acl, peer))
		ret = EACCES;
	else if (id->rci_pending >= id->rci_backlog ||
	    rdk_cm_pending >= rdk_cm_max_pending)
		ret = ENOBUFS;
	if (ret == 0) {
		id->rci_pending++;
		id->rci_refs++;
		rdk_cm_pending++;
		ra->ra_listener = id;
		ra->ra_peer = peer;
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
rdk_cm_unadmit(void *arg)
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

static boolean_t
rdk_cm_same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return (a->sin_family == AF_INET && a->sin_addr.s_addr ==
	    b->sin_addr.s_addr && a->sin_port == b->sin_port);
}

/*
 * Make the ID of a request the listener admitted, and queue its
 * CONNECT_REQUEST.  The caller holds the listener.  On success the child
 * owns cc_admit and the caller's hold on cc_conn.
 */
int
rdk_cm_child_new(rdk_cm_id_t *listener, const rdk_cm_child_t *cc,
    rdk_cm_id_t **childp)
{
	rdk_cm_admit_t *ra = cc->cc_admit;
	rdk_cm_resv_t *resv = listener->rci_resv;
	rdk_cm_handler_t handler;
	rdk_cm_id_t *id;
	rdk_cm_qev_t *q;
	uint16_t len;
	void *ctx;

	*childp = NULL;
	if (ra == NULL || ra->ra_listener != listener || ra->ra_done ||
	    ra->ra_peer != cc->cc_raddr.sin_addr.s_addr ||
	    cc->cc_raddr.sin_family != AF_INET || cc->cc_raddr.sin_port == 0 ||
	    resv == NULL)
		return (EINVAL);
	len = cc->cc_pdata_len;
	if (len > rdk_cm_pdata_max(listener, RDK_CM_MSG_REQ) ||
	    (len != 0 && cc->cc_pdata == NULL))
		return (EINVAL);
	q = rdk_cm_qev_alloc(RDK_CM_EVENT_CONNECT_REQUEST, 0, cc->cc_pdata,
	    len);
	q->q_ev.param.responder_resources = (uint8_t)MIN(cc->cc_ird,
	    UINT8_MAX);
	q->q_ev.param.initiator_depth = (uint8_t)MIN(cc->cc_ord, UINT8_MAX);

	mutex_enter(&listener->rci_lock);
	handler = listener->rci_handler;
	ctx = listener->rci_ctx;
	mutex_exit(&listener->rci_lock);
	id = rdk_cm_alloc(listener->rci_cred, handler, ctx,
	    listener->rci_qpt);
	/* The reservation lock comes before an ID's lock. */
	rdk_cm_resv_hold(resv);

	mutex_enter(&rdk_cm_lock);
	mutex_enter(&listener->rci_lock);
	if (listener->rci_destroying || listener->rci_state != RCS_LISTEN ||
	    listener->rci_removal || listener->rci_dev->rcd_removing ||
	    !rdk_cm_same_addr(&cc->cc_laddr, &listener->rci_route.rcr_src) ||
	    cc->cc_port != listener->rci_port) {
		mutex_exit(&listener->rci_lock);
		mutex_exit(&rdk_cm_lock);
		kmem_free(q, sizeof (*q));
		rdk_cm_resv_rele(resv);
		rdk_cm_rele(id);
		return (ECONNREFUSED);
	}
	id->rci_dev = listener->rci_dev;
	id->rci_dev->rcd_ids++;
	id->rci_port = listener->rci_port;
	id->rci_ifindex = listener->rci_ifindex;
	id->rci_ttl = listener->rci_ttl;
	id->rci_resv = resv;
	id->rci_route = listener->rci_route;
	id->rci_route.rcr_dst = cc->cc_raddr;
	if (cc->cc_dmac != NULL)
		bcopy(cc->cc_dmac, id->rci_route.rcr_dmac, ETHERADDRL);
	if (cc->cc_mtu != 0)
		id->rci_route.rcr_mtu = cc->cc_mtu;
	id->rci_iw.iw_laddr = cc->cc_laddr;
	id->rci_iw.iw_raddr = cc->cc_raddr;
	id->rci_iw.iw_port = cc->cc_port;
	id->rci_iw.iw_mtu = listener->rci_iw.iw_mtu;
	if (cc->cc_iw_provider != NULL) {
		id->rci_iw.iw_provider = cc->cc_iw_provider;
		id->rci_iw_owned = B_TRUE;
	}
	id->rci_conn = cc->cc_conn;
	id->rci_state = RCS_REQ;
	id->rci_listener = listener;
	listener->rci_refs++;
	id->rci_admit = ra;
	mutex_exit(&listener->rci_lock);
	list_insert_tail(&rdk_cm_ids, id);
	mutex_enter(&id->rci_lock);
	rdk_cm_queue_locked(id, q);
	mutex_exit(&id->rci_lock);
	mutex_exit(&rdk_cm_lock);
	*childp = id;
	return (0);
}

/*
 * Devices.  rdk_cm is a client of every device: removal tells each ID on
 * the device and waits until the consumers have destroyed them.  A device
 * with no iWARP CM attached is a RoCE device.
 */
rdk_cm_dev_t *
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

void
rdk_cm_dev_rele(rdk_cm_dev_t *cd)
{
	mutex_enter(&rdk_cm_lock);
	VERIFY3U(cd->rcd_ids, >, 0);
	if (--cd->rcd_ids == 0)
		cv_broadcast(&rdk_cm_dev_cv);
	mutex_exit(&rdk_cm_lock);
}

static int
rdk_cm_client_add(struct rdk_device *dev)
{
	uint8_t mac[RDK_CM_MAX_PORTS][ETHERADDRL];
	struct rdk_port_attr pa;
	rdk_cm_dev_t *cd, *ncd;
	uint32_t n, i;
	int ret;

	n = MIN(dev->rd_phys_port_cnt, RDK_CM_MAX_PORTS);
	bzero(mac, sizeof (mac));
	for (i = 0; i < n; i++) {
		if (rdk_query_port(dev, i + 1, &pa) == 0)
			bcopy(pa.mac, mac[i], ETHERADDRL);
	}
	ncd = kmem_zalloc(sizeof (*ncd), KM_SLEEP);
	mutex_enter(&rdk_cm_lock);
	if ((cd = rdk_cm_dev_find_locked(dev)) == NULL) {
		cd = ncd;
		ncd = NULL;
		cd->rcd_dev = dev;
		cd->rcd_roce = B_TRUE;
		cd->rcd_tp = &rdk_cm_roce_tport;
		list_insert_tail(&rdk_cm_devs, cd);
	}
	cd->rcd_nports = n;
	bcopy(mac, cd->rcd_mac, sizeof (mac));
	mutex_exit(&rdk_cm_lock);
	if (ncd != NULL)
		kmem_free(ncd, sizeof (*ncd));

	if (cd->rcd_roce && (ret = rdk_cm_roce_dev_add(cd)) != 0) {
		dev_err(dev->rd_dip, CE_WARN, "!RoCE CM unavailable on %s: %d",
		    dev->rd_name, ret);
	}
	mutex_enter(&rdk_cm_lock);
	cd->rcd_added = B_TRUE;
	mutex_exit(&rdk_cm_lock);
	rdk_cm_gid_dev_added();
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
	if ((cd = rdk_cm_dev_find_locked(dev)) == NULL) {
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
		if (cv_reltimedwait(&rdk_cm_dev_cv, &rdk_cm_lock,
		    SEC_TO_TICK(10), TR_SEC) == -1 && ++waited % 6 == 0) {
			dev_err(dev->rd_dip, CE_WARN, "!waiting for %u RDMA CM "
			    "IDs to be destroyed", cd->rcd_ids);
		}
	}
	mutex_exit(&rdk_cm_lock);

	if (cd->rcd_roce)
		rdk_cm_roce_dev_remove(cd);
	rdk_cm_gid_dev_removed(dev);

	mutex_enter(&rdk_cm_lock);
	cd->rcd_added = B_FALSE;
	if (cd->rcd_roce) {
		list_remove(&rdk_cm_devs, cd);
		mutex_exit(&rdk_cm_lock);
		kmem_free(cd, sizeof (*cd));
		return;
	}
	mutex_exit(&rdk_cm_lock);
}

static struct rdk_client rdk_cm_client = {
	.name = "rdk_cm",
	.add = rdk_cm_client_add,
	.remove = rdk_cm_client_remove
};

int
rdk_cm_client_init(void)
{
	return (rdk_register_client(&rdk_cm_client));
}

void
rdk_cm_client_fini(void)
{
	rdk_unregister_client(&rdk_cm_client);
}

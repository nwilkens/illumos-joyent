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
 * Connection setup for the RDMA transport.  A listener binds one IPv4
 * address and port and takes requests only from the peers of its ACL.  A
 * CONNECT_REQUEST is checked in this order, cheapest first: the private
 * data (nvmf_rdma_req_parse()), a live admin queue of the same peer for an
 * I/O queue's CNTLID, the queue size against the device, and the peer's
 * and the global allowances.  Only then is the queue built, its RECVs
 * posted and the queue adopted by nvmft; the accept follows.  The Fabrics
 * Connect that nvmft checks stays the authority; these checks shed load.
 *
 * A queue must see a successful Connect within nvmf_rdma_connect_ms of
 * ESTABLISHED.
 *
 * A listener's CM context is a slot that is never freed: a CONNECT_REQUEST
 * handler may still run after the listener ID is destroyed, and then finds
 * the slot empty.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/socket.h>
#include <sys/byteorder.h>
#include <netinet/in.h>

#include "nvmf_rdma_impl.h"

extern int nvmft_adopt_qpair(struct nvmf_transport_ops *,
    struct nvmf_qpair *, uint16_t, uint16_t);

uint_t nvmf_rdma_connect_ms = 5000;
uint_t nvmf_rdma_backlog = 64;
uint_t nvmf_rdma_peer_unconnected = 8;
uint_t nvmf_rdma_peer_queues = 256;
uint64_t nvmf_rdma_max_mem = 1024ULL * 1024 * 1024;
uint_t nvmf_rdma_max_ord = 16;

typedef struct nr_lslot {
	nr_kind_t	ls_kind;
	kmutex_t	ls_lock;
	nr_listener_t	*ls_nl;
	boolean_t	ls_busy;	/* being filled or emptied */
} nr_lslot_t;

static kmutex_t nr_cm_lock;
static list_t nr_devs;
static uint_t nr_nlisteners;
static uint64_t nr_mem;
static boolean_t nr_client_registered;
static nr_lslot_t nr_lslots[NVMF_RDMA_MAX_LISTENERS];

static int nr_client_add(struct rdk_device *);
static void nr_client_remove(struct rdk_device *, void *);

static struct rdk_client nr_client = {
	.name = "nvmf_rdma",
	.add = nr_client_add,
	.remove = nr_client_remove
};

static int
nr_client_add(struct rdk_device *dev)
{
	nr_dev_t *nd;
	int ret;

	nd = kmem_zalloc(sizeof (*nd), KM_SLEEP);
	nd->nd_dev = dev;
	nd->nd_iwarp = rdk_device_iwarp(dev);
	mutex_init(&nd->nd_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&nd->nd_cv, NULL, CV_DRIVER, NULL);
	list_create(&nd->nd_qlist, sizeof (nr_queue_t),
	    offsetof(nr_queue_t, nq_dnode));
	if ((ret = rdk_alloc_pd(dev, 0, &nd->nd_pd)) != 0) {
		list_destroy(&nd->nd_qlist);
		cv_destroy(&nd->nd_cv);
		mutex_destroy(&nd->nd_lock);
		kmem_free(nd, sizeof (*nd));
		return (ret);
	}
	(void) nr_pool_init(nd);
	rdk_set_client_data(dev, &nr_client, nd);
	mutex_enter(&nr_cm_lock);
	list_insert_tail(&nr_devs, nd);
	mutex_exit(&nr_cm_lock);
	return (0);
}

static void
nr_listener_rele(nr_listener_t *nl)
{
	nr_peer_t *p;
	boolean_t last;

	mutex_enter(&nl->nl_lock);
	ASSERT3U(nl->nl_refs, >, 0);
	last = --nl->nl_refs == 0;
	mutex_exit(&nl->nl_lock);
	if (!last)
		return;
	while ((p = list_remove_head(&nl->nl_peers)) != NULL)
		kmem_free(p, sizeof (*p));
	list_destroy(&nl->nl_peers);
	list_destroy(&nl->nl_queues);
	mutex_destroy(&nl->nl_lock);
	kmem_free(nl, sizeof (*nl));
}

/* Empty a listener's slot and destroy its ID; thread context. */
static void
nr_listener_stop(nr_listener_t *nl)
{
	nr_lslot_t *ls = &nr_lslots[nl->nl_id];
	nr_dev_t *nd = nl->nl_dev;

	mutex_enter(&ls->ls_lock);
	ASSERT3P(ls->ls_nl, ==, nl);
	ls->ls_nl = NULL;
	mutex_exit(&ls->ls_lock);
	(void) rdk_cm_destroy_id(nl->nl_cmid);
	nl->nl_cmid = NULL;

	mutex_enter(&nd->nd_lock);
	nd->nd_listeners--;
	cv_broadcast(&nd->nd_cv);
	mutex_exit(&nd->nd_lock);
	mutex_enter(&nr_cm_lock);
	nr_nlisteners--;
	mutex_exit(&nr_cm_lock);
	nr_listener_rele(nl);
}

/*
 * The device is going: stop its listeners, fail its queues and wait for
 * their teardowns, then for STMF to give back the pool's buffers.
 */
static void
nr_client_remove(struct rdk_device *dev, void *arg)
{
	nr_dev_t *nd = arg;
	nr_listener_t *nl;
	nr_queue_t *q;
	uint_t i;

	_NOTE(ARGUNUSED(dev));
	mutex_enter(&nr_cm_lock);
	list_remove(&nr_devs, nd);
	mutex_exit(&nr_cm_lock);
	mutex_enter(&nd->nd_lock);
	nd->nd_removing = B_TRUE;
	mutex_exit(&nd->nd_lock);

	for (i = 0; i < NVMF_RDMA_MAX_LISTENERS; i++) {
		nr_lslot_t *ls = &nr_lslots[i];

		mutex_enter(&ls->ls_lock);
		nl = ls->ls_nl;
		if (nl == NULL || ls->ls_busy || nl->nl_dev != nd) {
			mutex_exit(&ls->ls_lock);
			continue;
		}
		ls->ls_busy = B_TRUE;
		mutex_exit(&ls->ls_lock);
		nr_listener_stop(nl);
		mutex_enter(&ls->ls_lock);
		ls->ls_busy = B_FALSE;
		mutex_exit(&ls->ls_lock);
	}

	/* An unlisten that raced us finishes before the device goes. */
	mutex_enter(&nd->nd_lock);
	while (nd->nd_listeners != 0)
		cv_wait(&nd->nd_cv, &nd->nd_lock);
	mutex_exit(&nd->nd_lock);

	mutex_enter(&nd->nd_lock);
	while (!list_is_empty(&nd->nd_qlist) || nd->nd_building != 0) {
		for (q = list_head(&nd->nd_qlist); q != NULL;
		    q = list_next(&nd->nd_qlist, q))
			nr_queue_fail(q, ENXIO);
		(void) cv_reltimedwait(&nd->nd_cv, &nd->nd_lock,
		    SEC_TO_TICK(10), TR_SEC);
	}
	mutex_exit(&nd->nd_lock);

	nr_pool_fini(nd);
	rdk_dealloc_pd(nd->nd_pd);
	list_destroy(&nd->nd_qlist);
	cv_destroy(&nd->nd_cv);
	mutex_destroy(&nd->nd_lock);
	kmem_free(nd, sizeof (*nd));
}

/* The peer's entry, made on first use; nl_lock is held. */
static nr_peer_t *
nr_peer_get_locked(nr_listener_t *nl, ipaddr_t addr)
{
	nr_peer_t *p;

	ASSERT(MUTEX_HELD(&nl->nl_lock));
	for (p = list_head(&nl->nl_peers); p != NULL;
	    p = list_next(&nl->nl_peers, p)) {
		if (p->np_addr == addr)
			return (p);
	}
	if ((p = kmem_zalloc(sizeof (*p), KM_NOSLEEP)) == NULL)
		return (NULL);
	p->np_addr = addr;
	list_insert_tail(&nl->nl_peers, p);
	return (p);
}

/* An admin queue of this listener and peer that connected with cntlid. */
static boolean_t
nr_cntlid_live(nr_listener_t *nl, ipaddr_t peer, uint16_t cntlid)
{
	boolean_t live = B_FALSE;
	nr_queue_t *q;

	mutex_enter(&nl->nl_lock);
	for (q = list_head(&nl->nl_queues); q != NULL && !live;
	    q = list_next(&nl->nl_queues, q)) {
		if (q->nq_qid != 0 || q->nq_peer != peer)
			continue;
		mutex_enter(&q->nq_lock);
		live = q->nq_state == NR_Q_LIVE && q->nq_connected &&
		    q->nq_cntlid == cntlid;
		mutex_exit(&q->nq_lock);
	}
	mutex_exit(&nl->nl_lock);
	return (live);
}

/*
 * Charge a new queue to its peer and to the global memory allowance.
 * Returns NULL, with nothing charged, when an allowance is used up.
 */
static nr_peer_t *
nr_admit(nr_listener_t *nl, ipaddr_t addr, uint64_t bytes)
{
	nr_peer_t *p;

	mutex_enter(&nl->nl_lock);
	p = nr_peer_get_locked(nl, addr);
	if (p == NULL || p->np_queues >= nvmf_rdma_peer_queues ||
	    p->np_unconnected >= nvmf_rdma_peer_unconnected) {
		mutex_exit(&nl->nl_lock);
		return (NULL);
	}
	mutex_enter(&nr_cm_lock);
	if (bytes > nvmf_rdma_max_mem || nr_mem > nvmf_rdma_max_mem - bytes) {
		mutex_exit(&nr_cm_lock);
		mutex_exit(&nl->nl_lock);
		return (NULL);
	}
	nr_mem += bytes;
	mutex_exit(&nr_cm_lock);
	p->np_queues++;
	p->np_unconnected++;
	mutex_exit(&nl->nl_lock);
	return (p);
}

static void
nr_unadmit(nr_listener_t *nl, nr_peer_t *p, boolean_t connected,
    uint64_t bytes)
{
	mutex_enter(&nl->nl_lock);
	ASSERT3U(p->np_queues, >, 0);
	p->np_queues--;
	if (!connected) {
		ASSERT3U(p->np_unconnected, >, 0);
		p->np_unconnected--;
	}
	mutex_exit(&nl->nl_lock);
	mutex_enter(&nr_cm_lock);
	ASSERT3U(nr_mem, >=, bytes);
	nr_mem -= bytes;
	mutex_exit(&nr_cm_lock);
}

static int
nr_reject(rdk_cm_id_t *id, nvmf_rdma_rej_t sts)
{
	uint8_t rej[NVMF_RDMA_REJ_LEN];

	nvmf_rdma_rej_build(rej, sts);
	(void) rdk_cm_reject(id, rej, sizeof (rej));
	return (1);
}

/*
 * A CONNECT_REQUEST on the listener nl.  Returns nonzero to have rdk_cm
 * destroy the new ID, which is then refused.
 */
static int
nr_cm_request(nr_listener_t *nl, rdk_cm_id_t *id,
    const struct rdk_cm_event *ev)
{
	nr_dev_t *nd = nl->nl_dev;
	struct rdk_device *dev = nd->nd_dev;
	struct rdk_cm_conn_param p;
	struct rdk_cm_route rt;
	struct rdk_rw_attr rwa;
	struct rdk_rw_limits rwl;
	nvmf_rdma_devlim_t dl;
	nvmf_rdma_sizes_t sz;
	nvmf_rdma_req_t req;
	nvmf_rdma_rej_t sts;
	uint8_t rep[NVMF_RDMA_REP_LEN];
	nr_peer_t *peer;
	nr_queue_t *q;
	ipaddr_t addr;
	uint32_t vec, nvec;
	int err = 0;

	if (rdk_cm_device(id, NULL) != dev || rdk_cm_route(id, &rt) != 0)
		return (nr_reject(id, NVMF_RDMA_REJ_NO_RESOURCES));
	addr = rt.rcr_dst.sin_addr.s_addr;

	sts = nvmf_rdma_req_parse(ev->param.private_data,
	    ev->param.private_data_len, ev->param.responder_resources,
	    &nl->nl_lim, &req);
	if (sts != NVMF_RDMA_OK)
		return (nr_reject(id, sts));
	if (req.nrq_qid != 0 && req.nrq_cntlid != 0 &&
	    !nr_cntlid_live(nl, addr, req.nrq_cntlid))
		return (nr_reject(id, NVMF_RDMA_REJ_INVALID_CNTLID));

	if (nr_rw_attr(nd, &rwa) == 0 || rdk_rw_limits(dev, &rwa, &rwl) != 0)
		return (nr_reject(id, NVMF_RDMA_REJ_NO_RESOURCES));
	dl.ndl_max_qp_wr = (uint32_t)MAX(dev->rd_attr.max_qp_wr, 0);
	dl.ndl_max_cqe = (uint32_t)MAX(dev->rd_attr.max_cqe, 0);
	dl.ndl_rw_wrs = rwl.rwl_wrs;
	sts = nvmf_rdma_size_queue((uint32_t)req.nrq_hsqsize + 1,
	    req.nrq_qid == 0 ? 0 : nl->nl_icd, &dl, &sz);
	if (sts != NVMF_RDMA_OK)
		return (nr_reject(id, sts));

	mutex_enter(&nd->nd_lock);
	if (nd->nd_removing) {
		mutex_exit(&nd->nd_lock);
		return (nr_reject(id, NVMF_RDMA_REJ_NO_RESOURCES));
	}
	nd->nd_building++;
	mutex_exit(&nd->nd_lock);
	if ((peer = nr_admit(nl, addr, sz.nrs_bytes)) == NULL) {
		sts = NVMF_RDMA_REJ_NO_RESOURCES;
		goto unbuild;
	}

	/* Spread the queues of one host over the vectors. */
	nvec = MAX(dev->rd_num_comp_vectors, 1);
	vec = (ntohl(addr) * 31 + req.nrq_cntlid * 7 + req.nrq_qid) % nvec;
	q = nr_queue_create(nd, &sz, req.nrq_qid, nl->nl_icd, vec, &err);
	if (q == NULL) {
		nr_unadmit(nl, peer, B_FALSE, sz.nrs_bytes);
		sts = NVMF_RDMA_REJ_NO_RESOURCES;
		goto unbuild;
	}
	mutex_enter(&nl->nl_lock);
	nl->nl_refs++;
	list_insert_tail(&nl->nl_queues, q);
	mutex_exit(&nl->nl_lock);
	q->nq_listener = nl;
	q->nq_peer = addr;
	q->nq_peer_ent = peer;
	q->nq_charge = sz.nrs_bytes;
	mutex_enter(&nd->nd_lock);
	list_insert_tail(&nd->nd_qlist, q);
	nd->nd_building--;
	cv_broadcast(&nd->nd_cv);
	mutex_exit(&nd->nd_lock);

	rdk_cm_set_context(id, q);
	if (nr_queue_post_ring(q) != 0 ||
	    nvmft_adopt_qpair(&nvmf_rdma_ops, &q->nq_nq, req.nrq_qid,
	    req.nrq_hsqsize) != 0) {
		nr_queue_destroy_unadopted(q);
		return (nr_reject(id, NVMF_RDMA_REJ_NO_RESOURCES));
	}

	/* nvmft owns the queue now; a failure below goes through teardown. */
	mutex_enter(&q->nq_lock);
	q->nq_adopted = B_TRUE;
	q->nq_cmid = id;
	mutex_exit(&q->nq_lock);

	nvmf_rdma_rep_build(rep, (uint16_t)sz.nrs_depth);
	bzero(&p, sizeof (p));
	p.private_data = rep;
	p.private_data_len = sizeof (rep);
	/* Hosts never read the target; iWARP's MPA wants one for its RTR. */
	p.responder_resources = nd->nd_iwarp ? 1 : 0;
	p.initiator_depth = (uint8_t)MIN(MIN(ev->param.responder_resources,
	    (uint32_t)MAX(dev->rd_attr.max_qp_init_rd_atom, 1)),
	    nvmf_rdma_max_ord);
	p.retry_count = 7;
	p.rnr_retry_count = 7;
	p.qp = q->nq_qp;
	if (rdk_cm_accept(id, &p) != 0)
		nr_queue_fail(q, ECONNABORTED);
	return (0);

unbuild:
	mutex_enter(&nd->nd_lock);
	nd->nd_building--;
	cv_broadcast(&nd->nd_cv);
	mutex_exit(&nd->nd_lock);
	return (nr_reject(id, sts));
}

void
nr_queue_deadline(void *arg)
{
	nr_queue_t *q = arg;

	nr_queue_fail(q, ETIMEDOUT);
}

static void
nr_cm_queue_event(nr_queue_t *q, const struct rdk_cm_event *ev)
{
	switch (ev->event) {
	case RDK_CM_EVENT_ESTABLISHED:
		nr_queue_established(q);
		mutex_enter(&q->nq_lock);
		if (!q->nq_connected && q->nq_state == NR_Q_LIVE) {
			q->nq_deadline = timeout(nr_queue_deadline, q,
			    drv_usectohz((clock_t)nvmf_rdma_connect_ms *
			    MILLISEC));
		}
		mutex_exit(&q->nq_lock);
		break;
	case RDK_CM_EVENT_DISCONNECTED:
		nr_queue_fail(q, ECONNRESET);
		break;
	case RDK_CM_EVENT_CONNECT_ERROR:
	case RDK_CM_EVENT_UNREACHABLE:
	case RDK_CM_EVENT_REJECTED:
		nr_queue_fail(q, ECONNABORTED);
		break;
	case RDK_CM_EVENT_DEVICE_REMOVAL:
		nr_queue_fail(q, ENXIO);
		break;
	default:
		break;
	}
}

/*
 * Every CM event of our IDs.  Queue IDs are destroyed by the queue's
 * teardown, never by returning nonzero.
 */
static int
nr_cm_handler(rdk_cm_id_t *id, void *ctx, const struct rdk_cm_event *ev)
{
	nr_kind_t kind = *(nr_kind_t *)ctx;
	nr_lslot_t *ls;
	nr_listener_t *nl;
	int ret;

	if (kind == NR_KIND_QUEUE) {
		nr_cm_queue_event(ctx, ev);
		return (0);
	}
	ASSERT3U(kind, ==, NR_KIND_LISTENER);
	if (ev->event != RDK_CM_EVENT_CONNECT_REQUEST)
		return (0);
	ls = ctx;
	mutex_enter(&ls->ls_lock);
	if ((nl = ls->ls_nl) != NULL) {
		mutex_enter(&nl->nl_lock);
		nl->nl_refs++;
		mutex_exit(&nl->nl_lock);
	}
	mutex_exit(&ls->ls_lock);
	if (nl == NULL)
		return (nr_reject(id, NVMF_RDMA_REJ_NO_RESOURCES));
	ret = nr_cm_request(nl, id, ev);
	nr_listener_rele(nl);
	return (ret);
}

/* The Connect succeeded: the queue no longer counts as unconnected. */
void
nr_queue_connected(nr_queue_t *q)
{
	nr_listener_t *nl = q->nq_listener;
	timeout_id_t tid;

	mutex_enter(&q->nq_lock);
	tid = q->nq_deadline;
	q->nq_deadline = 0;
	mutex_exit(&q->nq_lock);
	if (tid != 0)
		(void) untimeout(tid);
	if (nl == NULL)
		return;
	mutex_enter(&nl->nl_lock);
	if (!q->nq_counted) {
		q->nq_counted = B_TRUE;
		ASSERT3U(q->nq_peer_ent->np_unconnected, >, 0);
		q->nq_peer_ent->np_unconnected--;
	}
	mutex_exit(&nl->nl_lock);
}

/* The end of a queue's teardown: give back what the queue was charged. */
void
nr_queue_detach(nr_queue_t *q)
{
	nr_listener_t *nl = q->nq_listener;
	nr_dev_t *nd = q->nq_dev;
	timeout_id_t tid;
	boolean_t counted;

	mutex_enter(&q->nq_lock);
	tid = q->nq_deadline;
	q->nq_deadline = 0;
	mutex_exit(&q->nq_lock);
	if (tid != 0)
		(void) untimeout(tid);
	if (nl == NULL)
		return;

	mutex_enter(&nl->nl_lock);
	list_remove(&nl->nl_queues, q);
	counted = q->nq_counted;
	q->nq_counted = B_TRUE;
	mutex_exit(&nl->nl_lock);
	nr_unadmit(nl, q->nq_peer_ent, counted, q->nq_charge);
	q->nq_listener = NULL;
	nr_listener_rele(nl);

	mutex_enter(&nd->nd_lock);
	list_remove(&nd->nd_qlist, q);
	cv_broadcast(&nd->nd_cv);
	mutex_exit(&nd->nd_lock);
}

/*
 * Listen on addr:port for the peers of the ACL.  The address picks the
 * device.
 */
int
nr_listen(cred_t *cr, const struct sockaddr_in *addr,
    const struct sockaddr_in *peers, uint_t npeers,
    const nvmf_rdma_limits_t *lim, uint32_t icd, uint32_t *idp)
{
	struct rdk_device *dev;
	nr_listener_t *nl;
	nr_lslot_t *ls = NULL;
	rdk_cm_acl_t *acl;
	nr_dev_t *nd;
	uint_t i;
	int ret;

	if (npeers == 0 || npeers > NVMF_RDMA_MAX_PEERS || icd >
	    NVMF_RDMA_MAX_ICD || (icd % 16) != 0 || lim->nrl_io_entries < 2 ||
	    lim->nrl_io_entries > NVMF_RDMA_MAX_ENTRIES ||
	    lim->nrl_admin_entries < 2 ||
	    lim->nrl_admin_entries > NVMF_RDMA_ADMIN_ENTRIES)
		return (EINVAL);
	if ((ret = rdk_cm_acl_create(peers, npeers, &acl)) != 0)
		return (ret);

	nl = kmem_zalloc(sizeof (*nl), KM_SLEEP);
	nl->nl_kind = NR_KIND_LISTENER;
	nl->nl_addr = *addr;
	nl->nl_lim = *lim;
	nl->nl_icd = icd;
	nl->nl_refs = 1;
	mutex_init(&nl->nl_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&nl->nl_peers, sizeof (nr_peer_t),
	    offsetof(nr_peer_t, np_node));
	list_create(&nl->nl_queues, sizeof (nr_queue_t),
	    offsetof(nr_queue_t, nq_lnode));

	mutex_enter(&nr_cm_lock);
	for (i = 0; i < NVMF_RDMA_MAX_LISTENERS; i++) {
		mutex_enter(&nr_lslots[i].ls_lock);
		if (nr_lslots[i].ls_nl == NULL && !nr_lslots[i].ls_busy) {
			ls = &nr_lslots[i];
			ls->ls_busy = B_TRUE;
			mutex_exit(&ls->ls_lock);
			break;
		}
		mutex_exit(&nr_lslots[i].ls_lock);
	}
	mutex_exit(&nr_cm_lock);
	if (ls == NULL) {
		ret = ENOSPC;
		goto fail;
	}
	nl->nl_id = i;

	if ((ret = rdk_cm_create_id(cr, nr_cm_handler, ls, RDK_PS_TCP,
	    RDK_QPT_RC, &nl->nl_cmid)) != 0)
		goto fail;
	if ((ret = rdk_cm_bind_addr(nl->nl_cmid,
	    (const struct sockaddr *)addr)) != 0)
		goto fail;
	dev = rdk_cm_device(nl->nl_cmid, NULL);
	mutex_enter(&nr_cm_lock);
	for (nd = list_head(&nr_devs); nd != NULL;
	    nd = list_next(&nr_devs, nd)) {
		if (nd->nd_dev == dev)
			break;
	}
	if (nd != NULL) {
		mutex_enter(&nd->nd_lock);
		if (nd->nd_removing) {
			mutex_exit(&nd->nd_lock);
			nd = NULL;
		} else {
			nd->nd_listeners++;
			mutex_exit(&nd->nd_lock);
		}
	}
	if (nd == NULL) {
		mutex_exit(&nr_cm_lock);
		ret = ENODEV;
		goto fail;
	}
	nr_nlisteners++;
	mutex_exit(&nr_cm_lock);
	nl->nl_dev = nd;

	/* Fill the slot first: a request may come as soon as we listen. */
	mutex_enter(&ls->ls_lock);
	ls->ls_nl = nl;
	ls->ls_busy = B_FALSE;
	mutex_exit(&ls->ls_lock);
	if ((ret = nr_pool_prime(nd)) != 0 ||
	    (ret = rdk_cm_listen(nl->nl_cmid, (int)nvmf_rdma_backlog,
	    acl)) != 0) {
		nr_listener_stop(nl);
		rdk_cm_acl_rele(acl);
		return (ret);
	}
	rdk_cm_acl_rele(acl);
	*idp = nl->nl_id;
	return (0);

fail:
	if (nl->nl_cmid != NULL)
		(void) rdk_cm_destroy_id(nl->nl_cmid);
	if (ls != NULL) {
		mutex_enter(&ls->ls_lock);
		ls->ls_busy = B_FALSE;
		mutex_exit(&ls->ls_lock);
	}
	rdk_cm_acl_rele(acl);
	nl->nl_refs = 0;
	list_destroy(&nl->nl_peers);
	list_destroy(&nl->nl_queues);
	mutex_destroy(&nl->nl_lock);
	kmem_free(nl, sizeof (*nl));
	return (ret);
}

/* Stop listening; the listener's queues go on. */
int
nr_unlisten(uint32_t id)
{
	nr_lslot_t *ls;
	nr_listener_t *nl;

	if (id >= NVMF_RDMA_MAX_LISTENERS)
		return (ENOENT);
	ls = &nr_lslots[id];
	mutex_enter(&nr_cm_lock);
	mutex_enter(&ls->ls_lock);
	nl = ls->ls_nl;
	if (nl == NULL || ls->ls_busy) {
		mutex_exit(&ls->ls_lock);
		mutex_exit(&nr_cm_lock);
		return (ENOENT);
	}
	ls->ls_busy = B_TRUE;
	mutex_exit(&ls->ls_lock);
	mutex_exit(&nr_cm_lock);

	nr_listener_stop(nl);
	mutex_enter(&ls->ls_lock);
	ls->ls_busy = B_FALSE;
	mutex_exit(&ls->ls_lock);
	return (0);
}

/* A listener or a queue remains. */
boolean_t
nr_cm_busy(void)
{
	boolean_t busy;
	nr_dev_t *nd;

	mutex_enter(&nr_cm_lock);
	busy = nr_nlisteners != 0 || nr_mem != 0;
	for (nd = list_head(&nr_devs); nd != NULL && !busy;
	    nd = list_next(&nr_devs, nd)) {
		mutex_enter(&nd->nd_lock);
		busy = !list_is_empty(&nd->nd_qlist) || nd->nd_building != 0;
		mutex_exit(&nd->nd_lock);
	}
	mutex_exit(&nr_cm_lock);
	return (busy);
}

int
nr_cm_init(void)
{
	uint_t i;
	int ret;

	mutex_init(&nr_cm_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&nr_devs, sizeof (nr_dev_t), offsetof(nr_dev_t, nd_node));
	for (i = 0; i < NVMF_RDMA_MAX_LISTENERS; i++) {
		nr_lslots[i].ls_kind = NR_KIND_LISTENER;
		mutex_init(&nr_lslots[i].ls_lock, NULL, MUTEX_DRIVER, NULL);
	}
	if ((ret = rdk_register_client(&nr_client)) != 0) {
		nr_cm_fini();
		return (ret);
	}
	nr_client_registered = B_TRUE;
	return (0);
}

/* Only once nr_cm_busy() is false. */
void
nr_cm_fini(void)
{
	uint_t i;

	if (nr_client_registered) {
		rdk_unregister_client(&nr_client);
		nr_client_registered = B_FALSE;
	}
	for (i = 0; i < NVMF_RDMA_MAX_LISTENERS; i++)
		mutex_destroy(&nr_lslots[i].ls_lock);
	list_destroy(&nr_devs);
	mutex_destroy(&nr_cm_lock);
}

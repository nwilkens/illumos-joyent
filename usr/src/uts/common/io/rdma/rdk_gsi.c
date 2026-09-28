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
 * The GSI agent of a RoCE device port: QP1, which carries the IB CM's
 * MADs over UD.
 *
 * Received MADs land in a fixed ring of buffers.  The completion handler,
 * in the provider's thread, checks the completion and the IPv4 header,
 * charges a token bucket, copies the MAD out and posts the buffer again at
 * once, so a flood costs a bounded amount of work and memory and never
 * starves the ring.  The copies go to the CM taskq.  Sends take a slot of
 * a fixed pool and an address handle from a small cache keyed by the path,
 * so replies to a peer do not each cost a firmware command.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/kstat.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"
#include "rdk_gsi.h"

/* MADs per second a port takes in, and stateless replies it sends. */
uint_t rdk_gsi_rx_rate = 20000;
uint_t rdk_gsi_reply_rate = 1000;
/* Received MADs waiting for the CM, per port. */
uint_t rdk_gsi_rx_max = 1024;

#define	RDK_GSI_BURST_SEC	8	/* a bucket holds rate/8 tokens */

static kmem_cache_t *rdk_gsi_rx_cache;
static const uint8_t rdk_gsi_zero_mac[ETHERADDRL];

static void rdk_gsi_recv_done(struct rdk_cq *, struct rdk_wc *);
static void rdk_gsi_send_done(struct rdk_cq *, struct rdk_wc *);

void
rdk_gsi_hold(rdk_gsi_t *g)
{
	mutex_enter(&g->rg_lock);
	g->rg_refs++;
	mutex_exit(&g->rg_lock);
}

void
rdk_gsi_rele(rdk_gsi_t *g)
{
	mutex_enter(&g->rg_lock);
	VERIFY3U(g->rg_refs, >, 0);
	if (--g->rg_refs == 0)
		cv_broadcast(&g->rg_cv);
	mutex_exit(&g->rg_lock);
}

/*
 * A token bucket in units of 1/NANOSEC token, so that the refill is exact.
 * rg_lock is held.
 */
static boolean_t
rdk_gsi_take(uint64_t *tokens, hrtime_t *last, uint_t rate)
{
	const uint64_t one = NANOSEC;
	const uint64_t cap = MAX((uint64_t)rate / RDK_GSI_BURST_SEC, 1) * one;
	hrtime_t now = gethrtime();
	uint64_t add;

	if (rate == 0)
		return (B_FALSE);
	add = (uint64_t)MAX(now - *last, 0);
	*last = now;
	if (add > cap / rate + 1)
		*tokens = cap;
	else
		*tokens = MIN(*tokens + add * rate, cap);
	if (*tokens < one)
		return (B_FALSE);
	*tokens -= one;
	return (B_TRUE);
}

/* A stateless reply may go out: none is owed to a sender we do not know. */
boolean_t
rdk_gsi_reply_ok(rdk_gsi_t *g)
{
	boolean_t ok;

	mutex_enter(&g->rg_lock);
	ok = rdk_gsi_take(&g->rg_reply_tokens, &g->rg_reply_last,
	    rdk_gsi_reply_rate);
	if (!ok)
		g->rg_stats.gst_reply_rate.value.ui64++;
	mutex_exit(&g->rg_lock);
	return (ok);
}

static int
rdk_gsi_post_recv(rdk_gsi_t *g, rdk_gsi_recv_t *gr)
{
	struct rdk_recv_wr wr;
	struct rdk_sge sge;
	int ret;

	sge.addr = g->rg_rbuf.rdb_pa + (uint64_t)gr->gr_idx * RDK_GSI_RSLOT;
	sge.length = IBCM_GRH_LEN + IBCM_MAD_LEN;
	sge.lkey = g->rg_pd->local_dma_lkey;
	bzero(&wr, sizeof (wr));
	wr.wr_cqe = &gr->gr_cqe;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	mutex_enter(&g->rg_lock);
	g->rg_rx_posted++;
	mutex_exit(&g->rg_lock);
	if ((ret = rdk_post_recv(g->rg_qp, &wr, NULL)) != 0) {
		mutex_enter(&g->rg_lock);
		g->rg_rx_posted--;
		cv_broadcast(&g->rg_cv);
		mutex_exit(&g->rg_lock);
	}
	return (ret);
}

static void
rdk_gsi_rx_task(void *arg)
{
	rdk_gsi_rx_t *rx = arg;
	rdk_gsi_t *g = rx->rx_gsi;

	rdk_cm_roce_recv(rx);
	kmem_cache_free(rdk_gsi_rx_cache, rx);
	mutex_enter(&g->rg_lock);
	VERIFY3U(g->rg_rx_out, >, 0);
	if (--g->rg_rx_out == 0)
		cv_broadcast(&g->rg_cv);
	mutex_exit(&g->rg_lock);
}

/*
 * A receive completed, in the provider's thread.  Anything but a well
 * formed RoCEv2 IPv4 MAD from a QP1 is dropped without a word.
 */
static void
rdk_gsi_recv_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	rdk_gsi_recv_t *gr = (rdk_gsi_recv_t *)wc->wr_cqe;
	rdk_gsi_t *g = gr->gr_gsi;
	const uint8_t *buf;
	rdk_gsi_rx_t *rx = NULL;
	rdk_ibcm_ip4_t ip;
	boolean_t repost;

	_NOTE(ARGUNUSED(cq));
	mutex_enter(&g->rg_lock);
	VERIFY3U(g->rg_rx_posted, >, 0);
	g->rg_rx_posted--;
	repost = !g->rg_dying;
	if (wc->status != RDK_WC_SUCCESS) {
		g->rg_stats.gst_rx_error.value.ui64++;
		if (wc->status == RDK_WC_WR_FLUSH_ERR)
			repost = B_FALSE;
		goto out;
	}
	g->rg_stats.gst_rx.value.ui64++;
	if (wc->opcode != RDK_WC_RECV ||
	    wc->byte_len != IBCM_GRH_LEN + IBCM_MAD_LEN ||
	    (wc->wc_flags & (RDK_WC_GRH | RDK_WC_WITH_NETWORK_HDR_TYPE)) !=
	    (RDK_WC_GRH | RDK_WC_WITH_NETWORK_HDR_TYPE) ||
	    (wc->wc_flags & RDK_WC_WITH_VLAN) != 0 ||
	    wc->network_hdr_type != RDK_NETWORK_IPV4 || wc->src_qp != 1) {
		g->rg_stats.gst_rx_bad.value.ui64++;
		goto out;
	}
	buf = (const uint8_t *)g->rg_rbuf.rdb_va +
	    (size_t)gr->gr_idx * RDK_GSI_RSLOT;
	if (rdk_ibcm_parse_ip4(buf, &ip) != 0) {
		g->rg_stats.gst_rx_bad.value.ui64++;
		goto out;
	}
	if (!rdk_gsi_take(&g->rg_rx_tokens, &g->rg_rx_last,
	    rdk_gsi_rx_rate)) {
		g->rg_stats.gst_rx_rate.value.ui64++;
		goto out;
	}
	if (g->rg_rx_out >= rdk_gsi_rx_max || g->rg_dying) {
		g->rg_stats.gst_rx_queue.value.ui64++;
		goto out;
	}
	if ((rx = kmem_cache_alloc(rdk_gsi_rx_cache, KM_NOSLEEP)) == NULL) {
		g->rg_stats.gst_rx_nomem.value.ui64++;
		goto out;
	}
	g->rg_rx_out++;
	rx->rx_gsi = g;
	rx->rx_ip = ip;
	rx->rx_has_smac = (wc->wc_flags & RDK_WC_WITH_SMAC) != 0 &&
	    (wc->smac[0] & 0x01) == 0 &&
	    bcmp(wc->smac, rdk_gsi_zero_mac, ETHERADDRL) != 0;
	if (rx->rx_has_smac)
		bcopy(wc->smac, rx->rx_smac, ETHERADDRL);
	bcopy(buf + IBCM_GRH_LEN, rx->rx_mad, IBCM_MAD_LEN);
out:
	if (!repost)
		cv_broadcast(&g->rg_cv);
	mutex_exit(&g->rg_lock);
	if (repost && rdk_gsi_post_recv(g, gr) != 0)
		g->rg_stats.gst_rx_error.value.ui64++;
	if (rx != NULL) {
		taskq_dispatch_ent(rdk_cm_taskq, rdk_gsi_rx_task, rx, 0,
		    &rx->rx_tqent);
	}
}

/*
 * Address handles.  An entry is keyed by the local GID's index and value,
 * so that a slot reused for another address never matches.
 */
static boolean_t
rdk_gsi_ah_match(const rdk_gsi_ah_t *a, const rdk_gsi_path_t *p)
{
	return (!a->ga_stale && a->ga_sgid_index == p->gp_sgid->index &&
	    bcmp(&a->ga_sgid, &p->gp_sgid->gid, sizeof (rdk_gid_t)) == 0 &&
	    bcmp(&a->ga_dgid, &p->gp_dgid, sizeof (rdk_gid_t)) == 0 &&
	    bcmp(a->ga_dmac, p->gp_dmac, ETHERADDRL) == 0 &&
	    a->ga_hop == p->gp_hop && a->ga_tclass == p->gp_tclass);
}

static void
rdk_gsi_ah_free(rdk_gsi_ah_t *a)
{
	rdk_destroy_ah(a->ga_ah);
	kmem_free(a, sizeof (*a));
}

/* Take the unused entries out: stale ones, or the oldest beyond max. */
static void
rdk_gsi_ah_reap(rdk_gsi_t *g, uint32_t max)
{
	rdk_gsi_ah_t *a, *next;
	list_t dead;

	list_create(&dead, sizeof (rdk_gsi_ah_t), offsetof(rdk_gsi_ah_t,
	    ga_node));
	mutex_enter(&g->rg_lock);
	for (a = list_head(&g->rg_ahs); a != NULL; a = next) {
		next = list_next(&g->rg_ahs, a);
		if (a->ga_refs != 0 || (!a->ga_stale && g->rg_nah <= max))
			continue;
		list_remove(&g->rg_ahs, a);
		a->ga_cached = B_FALSE;
		g->rg_nah--;
		g->rg_stats.gst_ah_evict.value.ui64++;
		list_insert_tail(&dead, a);
	}
	mutex_exit(&g->rg_lock);
	while ((a = list_remove_head(&dead)) != NULL)
		rdk_gsi_ah_free(a);
	list_destroy(&dead);
}

static void
rdk_gsi_reap_task(void *arg)
{
	rdk_gsi_t *g = arg;

	rdk_gsi_ah_reap(g, g->rg_ah_max);
	mutex_enter(&g->rg_lock);
	g->rg_reap_queued = B_FALSE;
	cv_broadcast(&g->rg_cv);
	mutex_exit(&g->rg_lock);
}

/* An AH for the path with a reference for one send.  Thread context. */
static int
rdk_gsi_ah_get(rdk_gsi_t *g, const rdk_gsi_path_t *p, rdk_gsi_ah_t **ap)
{
	struct rdk_ah_attr attr;
	rdk_gsi_ah_t *a, *n;
	uint64_t gen;
	int ret;

	mutex_enter(&g->rg_lock);
	gen = g->rg_withdraw_gen;
	for (a = list_head(&g->rg_ahs); a != NULL;
	    a = list_next(&g->rg_ahs, a)) {
		if (rdk_gsi_ah_match(a, p)) {
			a->ga_refs++;
			list_remove(&g->rg_ahs, a);
			list_insert_tail(&g->rg_ahs, a);
			g->rg_stats.gst_ah_hit.value.ui64++;
			mutex_exit(&g->rg_lock);
			*ap = a;
			return (0);
		}
	}
	mutex_exit(&g->rg_lock);

	n = kmem_zalloc(sizeof (*n), KM_SLEEP);
	bzero(&attr, sizeof (attr));
	attr.type = RDK_AH_ATTR_TYPE_ROCE;
	attr.ah_flags = RDK_AH_GRH;
	attr.port_num = g->rg_port;
	attr.grh.sgid_index = (uint8_t)p->gp_sgid->index;
	attr.grh.dgid = p->gp_dgid;
	attr.grh.hop_limit = p->gp_hop;
	attr.grh.traffic_class = p->gp_tclass;
	bcopy(p->gp_dmac, attr.roce.dmac, ETHERADDRL);
	if ((ret = rdk_create_ah(g->rg_pd, &attr, &n->ga_ah)) != 0) {
		kmem_free(n, sizeof (*n));
		return (ret);
	}
	n->ga_sgid_index = p->gp_sgid->index;
	n->ga_sgid = p->gp_sgid->gid;
	n->ga_dgid = p->gp_dgid;
	bcopy(p->gp_dmac, n->ga_dmac, ETHERADDRL);
	n->ga_hop = p->gp_hop;
	n->ga_tclass = p->gp_tclass;
	n->ga_refs = 1;
	n->ga_cached = B_TRUE;
	mutex_enter(&g->rg_lock);
	/* A withdrawal while the AH was made may have missed it. */
	if (g->rg_withdraw_gen != gen)
		n->ga_stale = B_TRUE;
	g->rg_stats.gst_ah_create.value.ui64++;
	list_insert_tail(&g->rg_ahs, n);
	g->rg_nah++;
	mutex_exit(&g->rg_lock);
	if (g->rg_nah > g->rg_ah_max)
		rdk_gsi_ah_reap(g, g->rg_ah_max);
	*ap = n;
	return (0);
}

/* Drop a send's AH reference; any context. */
static void
rdk_gsi_ah_put(rdk_gsi_t *g, rdk_gsi_ah_t *a)
{
	boolean_t reap;

	mutex_enter(&g->rg_lock);
	VERIFY3U(a->ga_refs, >, 0);
	a->ga_refs--;
	reap = a->ga_refs == 0 && (a->ga_stale || g->rg_nah > g->rg_ah_max) &&
	    !g->rg_reap_queued && !g->rg_dying;
	if (reap) {
		g->rg_reap_queued = B_TRUE;
		taskq_dispatch_ent(rdk_cm_taskq, rdk_gsi_reap_task, g, 0,
		    &g->rg_reap_ent);
	}
	mutex_exit(&g->rg_lock);
}

/* The GID at idx was withdrawn: its cached AHs go as their sends end. */
void
rdk_gsi_gid_withdrawn(rdk_gsi_t *g, uint16_t idx)
{
	rdk_gsi_ah_t *a;

	mutex_enter(&g->rg_lock);
	g->rg_withdraw_gen++;
	for (a = list_head(&g->rg_ahs); a != NULL;
	    a = list_next(&g->rg_ahs, a)) {
		if (a->ga_sgid_index == idx)
			a->ga_stale = B_TRUE;
	}
	mutex_exit(&g->rg_lock);
	rdk_gsi_ah_reap(g, g->rg_ah_max);
}

static void
rdk_gsi_send_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	rdk_gsi_send_t *gs = (rdk_gsi_send_t *)wc->wr_cqe;
	rdk_gsi_t *g = gs->gs_gsi;
	rdk_gsi_ah_t *a = gs->gs_ah;

	_NOTE(ARGUNUSED(cq));
	gs->gs_ah = NULL;
	rdk_gsi_ah_put(g, a);
	mutex_enter(&g->rg_lock);
	if (wc->status != RDK_WC_SUCCESS)
		g->rg_stats.gst_tx_error.value.ui64++;
	list_insert_tail(&g->rg_sfree, gs);
	VERIFY3U(g->rg_tx_out, >, 0);
	if (--g->rg_tx_out == 0)
		cv_broadcast(&g->rg_cv);
	mutex_exit(&g->rg_lock);
}

/* Send a MAD to the peer's QP1.  Thread context. */
int
rdk_gsi_send(rdk_gsi_t *g, const rdk_gsi_path_t *p, const uint8_t *mad)
{
	struct rdk_ud_wr wr;
	struct rdk_sge sge;
	rdk_gsi_send_t *gs;
	rdk_gsi_ah_t *a;
	int ret;

	mutex_enter(&g->rg_lock);
	if (g->rg_dying) {
		mutex_exit(&g->rg_lock);
		return (ENXIO);
	}
	if ((gs = list_remove_head(&g->rg_sfree)) == NULL) {
		g->rg_stats.gst_tx_nobufs.value.ui64++;
		mutex_exit(&g->rg_lock);
		return (ENOBUFS);
	}
	g->rg_tx_out++;
	mutex_exit(&g->rg_lock);

	if ((ret = rdk_gsi_ah_get(g, p, &a)) != 0)
		goto fail;
	gs->gs_ah = a;
	bcopy(mad, (uint8_t *)g->rg_sbuf.rdb_va + (size_t)gs->gs_idx *
	    IBCM_MAD_LEN, IBCM_MAD_LEN);
	sge.addr = g->rg_sbuf.rdb_pa + (uint64_t)gs->gs_idx * IBCM_MAD_LEN;
	sge.length = IBCM_MAD_LEN;
	sge.lkey = g->rg_pd->local_dma_lkey;
	bzero(&wr, sizeof (wr));
	wr.wr.wr_cqe = &gs->gs_cqe;
	wr.wr.sg_list = &sge;
	wr.wr.num_sge = 1;
	wr.wr.opcode = RDK_WR_SEND;
	wr.wr.send_flags = RDK_SEND_SIGNALED;
	wr.ah = a->ga_ah;
	wr.remote_qpn = 1;
	wr.remote_qkey = IBCM_QP1_QKEY;
	wr.pkey_index = 0;
	wr.port_num = g->rg_port;
	if ((ret = rdk_post_send(g->rg_qp, &wr.wr, NULL)) != 0) {
		gs->gs_ah = NULL;
		rdk_gsi_ah_put(g, a);
		goto fail;
	}
	mutex_enter(&g->rg_lock);
	g->rg_stats.gst_tx.value.ui64++;
	mutex_exit(&g->rg_lock);
	return (0);
fail:
	mutex_enter(&g->rg_lock);
	g->rg_stats.gst_tx_error.value.ui64++;
	list_insert_tail(&g->rg_sfree, gs);
	if (--g->rg_tx_out == 0)
		cv_broadcast(&g->rg_cv);
	mutex_exit(&g->rg_lock);
	return (ret);
}

static int
rdk_gsi_modify(rdk_gsi_t *g)
{
	struct rdk_qp_attr a;
	int ret;

	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_INIT;
	a.pkey_index = 0;
	a.qkey = IBCM_QP1_QKEY;
	if ((ret = rdk_modify_qp(g->rg_qp, &a, RDK_QP_STATE |
	    RDK_QP_PKEY_INDEX | RDK_QP_QKEY)) != 0)
		return (ret);
	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_RTR;
	if ((ret = rdk_modify_qp(g->rg_qp, &a, RDK_QP_STATE)) != 0)
		return (ret);
	bzero(&a, sizeof (a));
	a.qp_state = RDK_QPS_RTS;
	a.sq_psn = 0;
	return (rdk_modify_qp(g->rg_qp, &a, RDK_QP_STATE | RDK_QP_SQ_PSN));
}

static void
rdk_gsi_kstat_init(rdk_gsi_t *g)
{
	rdk_gsi_stats_t *s = &g->rg_stats;
	char name[KSTAT_STRLEN];

	(void) snprintf(name, sizeof (name), "%s_gsi%u", g->rg_dev->rd_name,
	    g->rg_port);
	kstat_named_init(&s->gst_rx, "rx", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_rx_bad, "rx_bad", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_rx_rate, "rx_rate_drop", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_rx_queue, "rx_queue_drop",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_rx_nomem, "rx_nomem", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_rx_error, "rx_error", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_tx, "tx", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_tx_error, "tx_error", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_tx_nobufs, "tx_nobufs", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_reply_rate, "reply_rate_drop",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_ah_hit, "ah_hit", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_ah_create, "ah_create", KSTAT_DATA_UINT64);
	kstat_named_init(&s->gst_ah_evict, "ah_evict", KSTAT_DATA_UINT64);
	g->rg_ksp = kstat_create("rdmak", 0, name, "net", KSTAT_TYPE_NAMED,
	    sizeof (*s) / sizeof (kstat_named_t), KSTAT_FLAG_VIRTUAL);
	if (g->rg_ksp != NULL) {
		g->rg_ksp->ks_data = s;
		g->rg_ksp->ks_lock = &g->rg_lock;
		kstat_install(g->rg_ksp);
	}
}

/* Stop, flush and free.  The caller holds the last reference. */
void
rdk_gsi_destroy(rdk_gsi_t *g)
{
	struct rdk_qp_attr a;
	rdk_gsi_ah_t *ah;

	mutex_enter(&g->rg_lock);
	g->rg_dying = B_TRUE;
	while (g->rg_refs != 0)
		cv_wait(&g->rg_cv, &g->rg_lock);
	mutex_exit(&g->rg_lock);
	if (g->rg_qp != NULL) {
		bzero(&a, sizeof (a));
		a.qp_state = RDK_QPS_ERR;
		(void) rdk_modify_qp(g->rg_qp, &a, RDK_QP_STATE);
		mutex_enter(&g->rg_lock);
		while (g->rg_rx_posted != 0 || g->rg_tx_out != 0) {
			if (cv_reltimedwait(&g->rg_cv, &g->rg_lock,
			    SEC_TO_TICK(10), TR_SEC) == -1) {
				dev_err(g->rg_dev->rd_dip, CE_WARN, "!GSI "
				    "flush waits: %u receives, %u sends",
				    g->rg_rx_posted, g->rg_tx_out);
			}
		}
		mutex_exit(&g->rg_lock);
		rdk_destroy_qp(g->rg_qp);
	}
	mutex_enter(&g->rg_lock);
	while (g->rg_rx_out != 0 || g->rg_reap_queued)
		cv_wait(&g->rg_cv, &g->rg_lock);
	mutex_exit(&g->rg_lock);
	if (g->rg_ksp != NULL)
		kstat_delete(g->rg_ksp);
	while ((ah = list_remove_head(&g->rg_ahs)) != NULL) {
		VERIFY0(ah->ga_refs);
		rdk_gsi_ah_free(ah);
	}
	if (g->rg_scq != NULL)
		rdk_free_cq(g->rg_scq);
	if (g->rg_rcq != NULL)
		rdk_free_cq(g->rg_rcq);
	rdk_dma_buf_free(g->rg_dev, &g->rg_rbuf);
	rdk_dma_buf_free(g->rg_dev, &g->rg_sbuf);
	if (g->rg_pd != NULL)
		rdk_dealloc_pd(g->rg_pd);
	list_destroy(&g->rg_ahs);
	list_destroy(&g->rg_sfree);
	cv_destroy(&g->rg_cv);
	mutex_destroy(&g->rg_lock);
	kmem_free(g, sizeof (*g));
}

int
rdk_gsi_create(struct rdk_device *dev, uint32_t port, rdk_gsi_t **gp)
{
	struct rdk_qp_init_attr init;
	rdk_gsi_t *g;
	uint32_t i;
	int ret;

	*gp = NULL;
	g = kmem_zalloc(sizeof (*g), KM_SLEEP);
	mutex_init(&g->rg_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&g->rg_cv, NULL, CV_DRIVER, NULL);
	list_create(&g->rg_sfree, sizeof (rdk_gsi_send_t),
	    offsetof(rdk_gsi_send_t, gs_node));
	list_create(&g->rg_ahs, sizeof (rdk_gsi_ah_t),
	    offsetof(rdk_gsi_ah_t, ga_node));
	g->rg_dev = dev;
	g->rg_port = port;
	g->rg_ah_max = MAX(MIN(RDK_GSI_AH_MAX,
	    (uint32_t)MAX(dev->rd_attr.max_ah, 0) / 4), 4);
	g->rg_rx_last = g->rg_reply_last = gethrtime();

	if ((ret = rdk_alloc_pd(dev, 0, &g->rg_pd)) != 0)
		goto fail;
	if ((ret = rdk_alloc_cq(dev, g, RDK_GSI_NSEND + 16, 0,
	    RDK_POLL_TASKQ, &g->rg_scq)) != 0)
		goto fail;
	if ((ret = rdk_alloc_cq(dev, g, RDK_GSI_NRECV + 16, 0,
	    RDK_POLL_TASKQ, &g->rg_rcq)) != 0)
		goto fail;
	if ((ret = rdk_dma_buf_alloc(dev, RDK_GSI_NRECV * RDK_GSI_RSLOT,
	    &g->rg_rbuf)) != 0)
		goto fail;
	if ((ret = rdk_dma_buf_alloc(dev, RDK_GSI_NSEND * IBCM_MAD_LEN,
	    &g->rg_sbuf)) != 0)
		goto fail;

	bzero(&init, sizeof (init));
	init.send_cq = g->rg_scq;
	init.recv_cq = g->rg_rcq;
	init.cap.max_send_wr = RDK_GSI_NSEND;
	init.cap.max_recv_wr = RDK_GSI_NRECV;
	init.cap.max_send_sge = 1;
	init.cap.max_recv_sge = 1;
	init.sq_sig_type = RDK_SIGNAL_REQ_WR;
	init.qp_type = RDK_QPT_GSI;
	init.port_num = port;
	if ((ret = rdk_create_qp(g->rg_pd, &init, &g->rg_qp)) != 0)
		goto fail;
	if ((ret = rdk_gsi_modify(g)) != 0)
		goto fail;

	for (i = 0; i < RDK_GSI_NSEND; i++) {
		g->rg_send[i].gs_cqe.done = rdk_gsi_send_done;
		g->rg_send[i].gs_gsi = g;
		g->rg_send[i].gs_idx = i;
		list_insert_tail(&g->rg_sfree, &g->rg_send[i]);
	}
	for (i = 0; i < RDK_GSI_NRECV; i++) {
		g->rg_recv[i].gr_cqe.done = rdk_gsi_recv_done;
		g->rg_recv[i].gr_gsi = g;
		g->rg_recv[i].gr_idx = i;
		if ((ret = rdk_gsi_post_recv(g, &g->rg_recv[i])) != 0)
			goto fail;
	}
	rdk_gsi_kstat_init(g);
	*gp = g;
	return (0);
fail:
	rdk_gsi_destroy(g);
	return (ret);
}

int
rdk_gsi_init(void)
{
	rdk_gsi_rx_cache = kmem_cache_create("rdk_gsi_rx",
	    sizeof (rdk_gsi_rx_t), 64, NULL, NULL, NULL, NULL, NULL, 0);
	return (rdk_gsi_rx_cache != NULL ? 0 : ENOMEM);
}

void
rdk_gsi_fini(void)
{
	kmem_cache_destroy(rdk_gsi_rx_cache);
}

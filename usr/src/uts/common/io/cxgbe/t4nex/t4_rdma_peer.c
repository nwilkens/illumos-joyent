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
 * The operations vector (t4_rdma_ops_t) the child calls.  Each entry point
 * checks its peer, pins the client and copies the request before t4_ofld_ops.c
 * checks it.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/disp.h>
#include <netinet/in.h>

#include "common/common.h"
#include "t4_ofld.h"

static t4_ofld_t *
t4_rdma_peer_ofld(t4_rdma_peer_t *peer)
{
	VERIFY(peer != NULL && peer->trp_hdr.trp_ops == &t4_rdma_ops);
	return (peer->trp_ofld);
}

/*
 * Pin the child's client for one operation, so that close cannot sweep its
 * IDs while the operation is still sending work requests for them.
 */
static t4_ofld_t *
t4_rdma_op_enter(t4_rdma_peer_t *peer)
{
	t4_ofld_t *of = t4_rdma_peer_ofld(peer);

	mutex_enter(&of->of_lock);
	if (!t4_ofld_client_ok(of) || of->of_client_test) {
		mutex_exit(&of->of_lock);
		return (NULL);
	}
	of->of_op_busy++;
	mutex_exit(&of->of_lock);
	return (of);
}

static void
t4_rdma_op_exit(t4_ofld_t *of)
{
	mutex_enter(&of->of_lock);
	if (--of->of_op_busy == 0)
		cv_broadcast(&of->of_cv);
	mutex_exit(&of->of_lock);
}

static int
t4_rdma_op_open(t4_rdma_peer_t *peer, const t4_rdma_client_t *client,
    void *arg, t4_rdma_info_t *info)
{
	t4_ofld_t *of = t4_rdma_peer_ofld(peer);
	int rc;

	if (info == NULL)
		return (EINVAL);
	if ((rc = t4_ofld_client_open(of, client, arg, B_FALSE)) != 0)
		return (rc);
	t4_ofld_info(of, info);
	mutex_enter(&of->of_lock);
	info->tri_generation = of->of_client_gen;
	mutex_exit(&of->of_lock);
	return (0);
}

/* Close waits for the callbacks, so a callback cannot close. */
static int
t4_rdma_op_close(t4_rdma_peer_t *peer)
{
	t4_ofld_t *of = t4_rdma_peer_ofld(peer);

	if (servicing_interrupt())
		return (EDEADLK);
	mutex_enter(&of->of_lock);
	const boolean_t mine = of->of_client != NULL && !of->of_client_test;
	const boolean_t cb = of->of_ev_thread == curthread;
	mutex_exit(&of->of_lock);
	if (cb)
		return (EDEADLK);
	if (mine)
		t4_ofld_client_close(of);
	return (0);
}

static int
t4_rdma_op_atid_alloc(t4_rdma_peer_t *peer, void *ctx, uint32_t *atidp)
{
	t4_ofld_t *of;
	uint32_t gen;
	int rc;

	if (atidp == NULL)
		return (EINVAL);
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	if ((rc = t4_ofld_gen(of, &gen)) == 0)
		rc = t4_atid_alloc(of, gen, ctx, atidp);
	t4_rdma_op_exit(of);
	return (rc);
}

static void
t4_rdma_op_atid_free(t4_rdma_peer_t *peer, uint32_t atid)
{
	t4_ofld_t *of;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return;
	t4_ofld_atid_free(of, atid);
	t4_rdma_op_exit(of);
}

static int
t4_rdma_op_stid_alloc(t4_rdma_peer_t *peer, sa_family_t family, void *ctx,
    uint32_t *stidp)
{
	t4_ofld_t *of;
	uint32_t gen;
	int rc;

	if (stidp == NULL)
		return (EINVAL);
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	if ((rc = t4_ofld_gen(of, &gen)) == 0)
		rc = t4_stid_alloc(of, gen, family, ctx, stidp);
	t4_rdma_op_exit(of);
	return (rc);
}

static void
t4_rdma_op_stid_free(t4_rdma_peer_t *peer, uint32_t stid)
{
	t4_ofld_t *of;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return;
	t4_ofld_stid_free(of, stid);
	t4_rdma_op_exit(of);
}

static int
t4_rdma_op_tid_bind(t4_rdma_peer_t *peer, uint32_t tid, void *ctx)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_tid_bind(of, tid, ctx);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_tid_release(t4_rdma_peer_t *peer, uint32_t tid)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_tid_release(of, tid);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_l2t_get(t4_rdma_peer_t *peer, uint8_t port, uint16_t vlan,
    const uint8_t *dmac, uint32_t *idxp)
{
	uint8_t mac[ETHERADDRL];
	t4_ofld_t *of;
	int rc;

	if (idxp == NULL || dmac == NULL)
		return (EINVAL);
	bcopy(dmac, mac, sizeof (mac));
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_l2t_get(of, port, vlan, mac, idxp);
	t4_rdma_op_exit(of);
	return (rc);
}

static void
t4_rdma_op_l2t_put(t4_rdma_peer_t *peer, uint32_t idx)
{
	t4_ofld_t *of;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return;
	t4_l2t_put(of, idx);
	t4_rdma_op_exit(of);
}

static int
t4_rdma_op_clip_get(t4_rdma_peer_t *peer, const in6_addr_t *addr)
{
	in6_addr_t a;
	t4_ofld_t *of;
	int rc;

	if (addr == NULL)
		return (EINVAL);
	a = *addr;
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_clip_get(of, &a);
	t4_rdma_op_exit(of);
	return (rc);
}

static void
t4_rdma_op_clip_put(t4_rdma_peer_t *peer, const in6_addr_t *addr)
{
	in6_addr_t a;
	t4_ofld_t *of;

	if (addr == NULL)
		return;
	a = *addr;
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return;
	t4_clip_put(of, &a);
	t4_rdma_op_exit(of);
}

/* Requests are copied before they are checked; the child may change them. */
static int
t4_rdma_op_listen(t4_rdma_peer_t *peer, const t4_rdma_listen_t *l)
{
	t4_rdma_listen_t req;
	t4_ofld_t *of;
	int rc;

	if (l == NULL)
		return (EINVAL);
	req = *l;
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_listen(of, &req);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_unlisten(t4_rdma_peer_t *peer, uint32_t stid)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_unlisten(of, stid);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_act_open(t4_rdma_peer_t *peer, const t4_rdma_act_open_t *a)
{
	t4_rdma_act_open_t req;
	t4_ofld_t *of;
	int rc;

	if (a == NULL)
		return (EINVAL);
	req = *a;
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_act_open(of, &req);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_accept(t4_rdma_peer_t *peer, const t4_rdma_accept_t *a)
{
	t4_rdma_accept_t req;
	t4_ofld_t *of;
	int rc;

	if (a == NULL)
		return (EINVAL);
	req = *a;
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_accept(of, &req);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_flowc(t4_rdma_peer_t *peer, uint32_t tid, const t4_rdma_flowc_t *f)
{
	t4_rdma_flowc_t req;
	t4_ofld_t *of;
	int rc;

	if (f == NULL)
		return (EINVAL);
	req = *f;
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_flowc(of, tid, &req);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_close_con(t4_rdma_peer_t *peer, uint32_t tid)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_close_con(of, tid);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_abort(t4_rdma_peer_t *peer, uint32_t tid, boolean_t rst)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_abort(of, tid, rst);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_abort_rpl(t4_rdma_peer_t *peer, uint32_t tid, boolean_t rst)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_abort_rpl(of, tid, rst);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_set_tcb_field(t4_rdma_peer_t *peer, uint32_t tid, uint16_t word,
    uint64_t mask, uint64_t val)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_set_tcb_field(of, tid, word, mask, val);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_rx_credits(t4_rdma_peer_t *peer, uint32_t tid, uint32_t credits)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_rx_credits(of, tid, credits);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_tx_data(t4_rdma_peer_t *peer, uint32_t tid, const void *buf,
    size_t len)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_tx_data(of, tid, buf, len);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_tpt_write(t4_rdma_peer_t *peer, uint32_t addr, const void *buf,
    size_t len)
{
	t4_ofld_t *of;
	int rc;

	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_tpt_write(of, addr, buf, len);
	t4_rdma_op_exit(of);
	return (rc);
}

static int
t4_rdma_op_dma_alloc(t4_rdma_peer_t *peer, size_t len, size_t align,
    t4_rdma_dma_t **dmap)
{
	t4_ofld_t *of;
	int rc;

	if (dmap == NULL)
		return (EINVAL);
	*dmap = NULL;
	if ((of = t4_rdma_op_enter(peer)) == NULL)
		return (EIO);
	rc = t4_ofld_dma_alloc(of, len, align, dmap);
	t4_rdma_op_exit(of);
	return (rc);
}

static void
t4_rdma_op_dma_free(t4_rdma_peer_t *peer, t4_rdma_dma_t *dma,
    boolean_t quiesced)
{
	t4_ofld_dma_free(t4_rdma_peer_ofld(peer), dma, quiesced);
}

/* t4nex cannot reset the adapter without a full detach and attach. */
static int
t4_rdma_op_reset(t4_rdma_peer_t *peer)
{
	(void) t4_rdma_peer_ofld(peer);
	return (ENOTSUP);
}

static boolean_t
t4_rdma_op_stopped(t4_rdma_peer_t *peer)
{
	t4_ofld_t *of = t4_rdma_peer_ofld(peer);
	boolean_t ok;

	mutex_enter(&of->of_lock);
	ok = t4_ofld_client_ok(of);
	mutex_exit(&of->of_lock);
	return (!ok);
}

const t4_rdma_ops_t t4_rdma_ops = {
	.tro_open = t4_rdma_op_open,
	.tro_close = t4_rdma_op_close,
	.tro_atid_alloc = t4_rdma_op_atid_alloc,
	.tro_atid_free = t4_rdma_op_atid_free,
	.tro_stid_alloc = t4_rdma_op_stid_alloc,
	.tro_stid_free = t4_rdma_op_stid_free,
	.tro_tid_bind = t4_rdma_op_tid_bind,
	.tro_tid_release = t4_rdma_op_tid_release,
	.tro_l2t_get = t4_rdma_op_l2t_get,
	.tro_l2t_put = t4_rdma_op_l2t_put,
	.tro_clip_get = t4_rdma_op_clip_get,
	.tro_clip_put = t4_rdma_op_clip_put,
	.tro_listen = t4_rdma_op_listen,
	.tro_unlisten = t4_rdma_op_unlisten,
	.tro_act_open = t4_rdma_op_act_open,
	.tro_accept = t4_rdma_op_accept,
	.tro_flowc = t4_rdma_op_flowc,
	.tro_close_con = t4_rdma_op_close_con,
	.tro_abort = t4_rdma_op_abort,
	.tro_abort_rpl = t4_rdma_op_abort_rpl,
	.tro_set_tcb_field = t4_rdma_op_set_tcb_field,
	.tro_rx_credits = t4_rdma_op_rx_credits,
	.tro_tx_data = t4_rdma_op_tx_data,
	.tro_tpt_write = t4_rdma_op_tpt_write,
	.tro_dma_alloc = t4_rdma_op_dma_alloc,
	.tro_dma_free = t4_rdma_op_dma_free,
	.tro_reset = t4_rdma_op_reset,
	.tro_stopped = t4_rdma_op_stopped
};

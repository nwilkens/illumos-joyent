/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 * A copy of the CDDL is available at http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * A start must not wait on buffers a peer can hold up the stack.  The actual
 * pool functions and ice_rx_start() replace such a pool, free all of it but
 * the loaned blocks at once, free each of those as it returns, and stop
 * loaning while the instance holds too many of them.
 */
#include "rx_test.h"

/* Memory a pool of nrcb blocks for a ring of size slots takes. */
static size_t
pool_kmem(unsigned nrcb, unsigned size)
{
	return (sizeof (ice_rx_pool_t) + nrcb * (sizeof (ice_rx_ctrl_block_t) +
	    sizeof (ice_rx_ctrl_block_t *)) + size *
	    sizeof (ice_rx_ctrl_block_t *));
}

/* Loan the buffer in slot idx up the stack, as a large frame would. */
static mblk_t *
loan(ice_rx_ring_t *r, uint16_t idx)
{
	mblk_t *mp;

	mutex_enter(&r->irxr_lock);
	mp = ice_rx_bind(r, idx, r->irxr_rcbs[idx], 1500);
	mutex_exit(&r->irxr_lock);
	assert(mp != NULL);
	return (mp);
}

/* What a restart does: close the ring, start it on a new pool, post it. */
static boolean_t
restart(ice_rx_ring_t *r)
{
	ice_rx_pool_t *old = r->irxr_pool;
	uint_t i;

	mutex_enter(&r->irxr_lock);
	r->irxr_shutdown = B_TRUE;
	r->irxr_started = B_FALSE;
	mutex_exit(&r->irxr_lock);
	if (!ice_rx_start(r->irxr_ice))
		return (B_FALSE);
	mutex_enter(&r->irxr_lock);
	assert(r->irxr_pool != old && r->irxr_nloaned == 0);
	assert(r->irxr_nfree == r->irxr_nrcb);
	for (i = 0; i < r->irxr_size; i++)
		ice_rx_reset_desc(r, i, ice_rcb_alloc(r, B_FALSE));
	r->irxr_head = 0;
	r->irxr_tail = r->irxr_size - 1;
	r->irxr_shutdown = B_FALSE;
	r->irxr_started = B_TRUE;
	mutex_exit(&r->irxr_lock);
	return (B_TRUE);
}

static void
stop(ice_rx_ring_t *r)
{
	r->irxr_shutdown = B_TRUE;
	r->irxr_started = B_FALSE;
}

static void
set_aside(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held[3];
	size_t pool, kmem;
	uint_t i, nrcb, nfree;

	setup(&ring, &ice);
	nrcb = ring.irxr_nrcb;
	pool = pool_kmem(nrcb, ring.irxr_size);
	assert(live_kmem == pool);
	for (i = 0; i < 3; i++)
		held[i] = loan(&ring, (uint16_t)i);
	assert(ring.irxr_nloaned == 3);

	/* Only the three loaned blocks and the pool record stay allocated. */
	assert(restart(&ring));
	assert(ice.ice_rx_orphan_loans == 3);
	assert(ring.irxr_stats.icrxs_orphan_loans.value.ui64 == 3);
	assert(ring.irxr_stats.icrxs_orphan_pools.value.ui64 == 1);
	assert(live_dma == 1 + 3 + nrcb);
	kmem = pool + sizeof (ice_rx_pool_t) + 3 * sizeof (ice_rx_ctrl_block_t);
	assert(live_kmem == kmem);
	nfree = ring.irxr_nfree;

	/* A returning loan is freed at once, not put on the new free list. */
	freemsg(held[0]);
	assert(ice.ice_rx_orphan_loans == 2 && live_dma == 1 + 2 + nrcb);
	assert(live_kmem == kmem - sizeof (ice_rx_ctrl_block_t));
	assert(ring.irxr_nfree == nfree && ring.irxr_nloaned == 0);

	/* A new-pool loan still returns to the new pool. */
	held[0] = loan(&ring, 5);
	assert(ring.irxr_nloaned == 1);
	freemsg(held[0]);
	assert(ring.irxr_nloaned == 0 && ring.irxr_nfree == nfree);

	/* The last return frees the record too. */
	freemsg(held[1]);
	freemsg(held[2]);
	assert(ice.ice_rx_orphan_loans == 0 && live_dma == 1 + nrcb);
	assert(ring.irxr_stats.icrxs_orphan_loans.value.ui64 == 0);
	assert(live_kmem == pool);
	stop(&ring);
	teardown(&ring);
}

/* A loan that returns while the start still frees the rest keeps the pool. */
static void
return_during_sweep(void)
{
	ice_rx_ring_t ring;
	ice_rx_pool_t *old;
	ice_t ice;
	mblk_t *held;
	size_t pool;

	setup(&ring, &ice);
	pool = pool_kmem(ring.irxr_nrcb, ring.irxr_size);
	held = loan(&ring, 0);
	mutex_enter(&ring.irxr_lock);
	old = ice_rx_pool_swap(&ring, ice_rx_pool_alloc(&ring));
	ice_rx_pool_orphan(&ring, old);
	mutex_exit(&ring.irxr_lock);
	assert(old->irp_refs == 2);
	freemsg(held);
	assert(old->irp_refs == 1 && ice.ice_rx_orphan_loans == 0);
	ice_rx_pool_retire(old);
	assert(live_kmem == pool);
	teardown(&ring);
}

/* A peer that keeps a loan across every restart never stops a start. */
static void
unbounded_restarts(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held[40];
	uint_t i;

	setup(&ring, &ice);
	for (i = 0; i < 40; i++) {
		held[i] = loan(&ring, 0);
		assert(restart(&ring));
	}
	assert(ice.ice_rx_orphan_loans == 40);
	assert(ring.irxr_stats.icrxs_orphan_pools.value.ui64 == 40);
	assert(live_dma == 1 + 40 + ring.irxr_nrcb);
	for (i = 0; i < 40; i++)
		freemsg(held[i]);
	assert(ice.ice_rx_orphan_loans == 0);
	assert(live_kmem == pool_kmem(ring.irxr_nrcb, ring.irxr_size));
	stop(&ring);
	teardown(&ring);
}

/* Deliver one 1500-byte frame through the poll entry point. */
static mblk_t *
receive(ice_rx_ring_t *r)
{
	static unsigned char frame[1500];
	mblk_t *mp;

	post(r, r->irxr_head, frame, sizeof (frame), B_TRUE, B_FALSE);
	mp = ice_ring_rx_poll(r, 65536);
	assert(mp != NULL && mp->b_next == NULL && mp->b_cont == NULL);
	assert(MBLKL(mp) == sizeof (frame));
	return (mp);
}

/*
 * At the budget the rings copy instead of loaning, and a start still
 * succeeds; below the low-water mark they loan again.
 */
static void
budget(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t **held, *mp;
	uint_t n = 0, per, nrcb;
	ice_rxq_stat_t *st = &ring.irxr_stats;

	setup(&ring, &ice);
	per = ring.irxr_nreserve;
	nrcb = ring.irxr_nrcb;
	held = calloc(ICE_RX_ORPHAN_BUDGET + 2 * per, sizeof (*held));
	assert(held != NULL);

	/* Below the budget a large frame is loaned. */
	mp = receive(&ring);
	assert(ring.irxr_nloaned == 1);
	assert(st->icrxs_copy_mode_enter.value.ui64 == 0);
	held[n++] = mp;
	while (ice.ice_rx_orphan_loans < ICE_RX_ORPHAN_BUDGET) {
		while (ring.irxr_nloaned < per)
			held[n++] = loan(&ring, 0);
		assert(restart(&ring));
	}
	assert(ice.ice_rx_orphan_loans == n);

	/* At the budget the next drain copies, and restarts still succeed. */
	mp = receive(&ring);
	assert(ring.irxr_copy_only && ring.irxr_nloaned == 0);
	assert(st->icrxs_copy_mode_enter.value.ui64 == 1);
	assert(st->icrxs_copy_mode_segs.value.ui64 == 1);
	assert(mp->b_datap->frtn == NULL);
	freemsg(mp);
	assert(restart(&ring) && restart(&ring));
	assert(live_dma == 1 + n + nrcb);

	/* Copy mode holds down to the low-water mark. */
	while (ice.ice_rx_orphan_loans > ICE_RX_ORPHAN_LOWAT)
		freemsg(held[--n]);
	mp = receive(&ring);
	assert(ring.irxr_copy_only && ring.irxr_nloaned == 0);
	assert(st->icrxs_copy_mode_exit.value.ui64 == 0);
	freemsg(mp);

	/* Below it, loaning resumes. */
	freemsg(held[--n]);
	mp = receive(&ring);
	assert(!ring.irxr_copy_only && ring.irxr_nloaned == 1);
	assert(st->icrxs_copy_mode_exit.value.ui64 == 1);
	assert(mp->b_datap->frtn != NULL);
	freemsg(mp);

	while (n > 0)
		freemsg(held[--n]);
	free(held);
	assert(ice.ice_rx_orphan_loans == 0 && ring.irxr_nloaned == 0);
	stop(&ring);
	teardown(&ring);
}

static mblk_t *returning[2];

static void
return_one(void)
{
	uint_t i;

	for (i = 0; i < 2; i++) {
		if (returning[i] != NULL) {
			freemsg(returning[i]);
			returning[i] = NULL;
			return;
		}
	}
}

/* Detach waits, within the loan deadline, for the loans of replaced pools. */
static void
detach_wait(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held;
	size_t kmem;
	uint_t dma;

	setup(&ring, &ice);
	held = loan(&ring, 0);
	assert(restart(&ring));
	stop(&ring);
	kmem = live_kmem;
	dma = live_dma;

	/* A loan that never returns fails the wait at the deadline. */
	assert(!ice_rx_orphans_drain(&ice));
	assert(lbolt >= drv_usectohz(ICE_RX_LOAN_WAIT_US));
	assert(lbolt < drv_usectohz(ICE_RX_LOAN_WAIT_US) +
	    drv_usectohz(ICE_RX_ORPHAN_POLL_US) + 1);
	assert(ice.ice_rx_orphan_loans == 1);
	assert(live_kmem == kmem && live_dma == dma);

	/* Loans that come back during the wait let it succeed. */
	returning[0] = held;
	returning[1] = loan(&ring, 1);
	assert(restart(&ring));
	stop(&ring);
	assert(ice.ice_rx_orphan_loans == 2);
	lbolt = 0;
	on_delay = return_one;
	assert(ice_rx_orphans_drain(&ice));
	assert(ice.ice_rx_orphan_loans == 0 && returning[1] == NULL);
	assert(lbolt < drv_usectohz(ICE_RX_LOAN_WAIT_US));
	on_delay = NULL;
	teardown(&ring);
}

int
main(void)
{
	set_aside();
	return_during_sweep();
	unbounded_restarts();
	budget();
	detach_wait();
	puts("RX orphan: replaced pools keep only their loans, free each as it "
	    "returns, never stop a start, and copy past the budget");
	return (0);
}

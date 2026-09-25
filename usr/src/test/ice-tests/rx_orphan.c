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
 * the loaned blocks at once, queue each of those for the reap taskq as it
 * returns, and stop loaning while the instance holds too many of them.  The
 * harness fails any free of memory or DMA made inside a free routine.
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

	/*
	 * A returning loan is queued for the reaper, not put on the new free
	 * list, and freed when the reaper runs.
	 */
	freemsg(held[0]);
	assert(ice.ice_rx_orphan_loans == 2 && live_dma == 1 + 3 + nrcb);
	assert(ice.ice_rx_reap != NULL && dispatches == 1);
	assert(ring.irxr_nfree == nfree && ring.irxr_nloaned == 0);
	run_taskq();
	assert(ice.ice_rx_reap == NULL && live_dma == 1 + 2 + nrcb);
	assert(live_kmem == kmem - sizeof (ice_rx_ctrl_block_t));

	/* A new-pool loan still returns to the new pool. */
	held[0] = loan(&ring, 5);
	assert(ring.irxr_nloaned == 1);
	freemsg(held[0]);
	assert(returned(&ring) == 1 && ring.irxr_nloaned == 1);
	harvest(&ring);
	assert(ring.irxr_nloaned == 0 && ring.irxr_nfree == nfree);

	/* Returns before the reaper runs share one dispatch. */
	freemsg(held[1]);
	freemsg(held[2]);
	assert(dispatches == 2);
	run_taskq();
	/* The last return frees the record too. */
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
	run_taskq();
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
	run_taskq();
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

#define	NR	16
static ice_rx_ring_t rings[NR];

/* An instance of NR rings, each started on its own pool. */
static void
setup_rings(ice_t *ice)
{
	ddi_dma_attr_t attr = 0;
	ddi_device_acc_attr_t acc = 0;
	uint_t i;

	setup(&rings[0], ice);
	ice->ice_num_rxr = NR;
	ice->ice_rxr = rings;
	for (i = 1; i < NR; i++) {
		ice_rx_ring_t *r = &rings[i];

		memset(r, 0, sizeof (*r));
		r->irxr_ice = ice;
		r->irxr_index = i;
		r->irxr_size = rings[0].irxr_size;
		r->irxr_dbuf = ICE_RX_BUF_SIZE;
		assert(ice_dma_alloc(ice, &r->irxr_desc_dma, &attr, &acc,
		    B_TRUE, r->irxr_size * sizeof (*r->irxr_descs), B_TRUE));
		r->irxr_descs = (void *)r->irxr_desc_dma.idb_va;
		post_pool(r);
	}
}

static void
teardown_rings(ice_t *ice)
{
	uint_t i;

	run_taskq();
	assert(ice->ice_rx_orphan_loans == 0 && ice->ice_rx_reap == NULL);
	for (i = 0; i < NR; i++) {
		harvest(&rings[i]);
		assert(rings[i].irxr_nloaned == 0);
		stop(&rings[i]);
		ice_rx_pool_release(&rings[i]);
		ice_dma_free(&rings[i].irxr_desc_dma);
	}
	assert(live_mblks == 0 && live_dma == 0 && live_kmem == 0);
}

static void
restart_all(void)
{
	uint_t i, j;

	for (i = 0; i < NR; i++)
		stop(&rings[i]);
	assert(ice_rx_start(rings[0].irxr_ice));
	for (i = 0; i < NR; i++) {
		ice_rx_ring_t *r = &rings[i];

		mutex_enter(&r->irxr_lock);
		for (j = 0; j < r->irxr_size; j++)
			ice_rx_reset_desc(r, j, ice_rcb_alloc(r, B_FALSE));
		r->irxr_head = 0;
		r->irxr_tail = r->irxr_size - 1;
		r->irxr_shutdown = B_FALSE;
		r->irxr_started = B_TRUE;
		mutex_exit(&r->irxr_lock);
	}
}

static mblk_t *
receive_on(ice_rx_ring_t *r)
{
	active_ring = r;
	return (receive(r));
}

/*
 * A restart adds at most every ring's reserve to the count, so the rings copy
 * from that far below ICE_RX_ORPHAN_MAX: with every ring's reserve out one
 * below that point, a restart takes the count to the limit and no further,
 * and restarts still succeed.  Below half that point loaning resumes.
 */
static void
limit(void)
{
	ice_t ice;
	mblk_t **held, *mp;
	uint_t n = 0, i, per, stop_at;
	ice_rxq_stat_t *st = &rings[0].irxr_stats;

	setup_rings(&ice);
	per = rings[0].irxr_nreserve;
	stop_at = ICE_RX_ORPHAN_MAX - per * NR;
	assert(per == 1024 && stop_at == 8192);
	held = calloc(ICE_RX_ORPHAN_MAX + 1, sizeof (*held));
	assert(held != NULL);

	for (i = 0; n < stop_at - 1; i++) {
		while (rings[i].irxr_nloaned < per && n < stop_at - 1)
			held[n++] = loan(&rings[i], 0);
	}
	restart_all();
	assert(ice.ice_rx_orphan_loans == stop_at - 1);

	/* One below the stop point a large frame is still loaned. */
	held[n++] = mp = receive_on(&rings[0]);
	assert(mp->b_datap->frtn != NULL && rings[0].irxr_nloaned == 1);
	assert(st->icrxs_copy_mode_enter.value.ui64 == 0);

	for (i = 0; i < NR; i++) {
		while (rings[i].irxr_nloaned < per)
			held[n++] = loan(&rings[i], 0);
	}
	restart_all();
	assert(ice.ice_rx_orphan_loans == n && n == ICE_RX_ORPHAN_MAX - 1);

	/* Past the stop point the next drain copies. */
	mp = receive_on(&rings[0]);
	assert(rings[0].irxr_copy_only && rings[0].irxr_nloaned == 0);
	assert(st->icrxs_copy_mode_enter.value.ui64 == 1);
	assert(st->icrxs_copy_mode_segs.value.ui64 == 1);
	assert(mp->b_datap->frtn == NULL);
	freemsg(mp);
	restart_all();
	restart_all();
	assert(ice.ice_rx_orphan_loans == ICE_RX_ORPHAN_MAX - 1);

	/* Copy mode holds down to half the stop point. */
	while (ice.ice_rx_orphan_loans > stop_at / 2)
		freemsg(held[--n]);
	mp = receive_on(&rings[0]);
	assert(rings[0].irxr_copy_only && rings[0].irxr_nloaned == 0);
	assert(st->icrxs_copy_mode_exit.value.ui64 == 0);
	freemsg(mp);

	/* Below it, loaning resumes. */
	freemsg(held[--n]);
	mp = receive_on(&rings[0]);
	assert(!rings[0].irxr_copy_only && rings[0].irxr_nloaned == 1);
	assert(st->icrxs_copy_mode_exit.value.ui64 == 1);
	assert(mp->b_datap->frtn != NULL);
	freemsg(mp);

	while (n > 0)
		freemsg(held[--n]);
	free(held);
	teardown_rings(&ice);
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

/* A reap that cannot be dispatched waits for the next return or start. */
static void
reap_undispatched(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held[2];
	uint_t dma;

	setup(&ring, &ice);
	held[0] = loan(&ring, 0);
	held[1] = loan(&ring, 1);
	assert(restart(&ring));
	dma = live_dma;
	dispatch_fail = 1;
	freemsg(held[0]);
	assert(dispatches == 0 && ice.ice_rx_reap_queued == 0);
	assert(ice.ice_rx_reap != NULL && live_dma == dma);
	dispatch_fail = 0;
	freemsg(held[1]);
	assert(dispatches == 1);
	run_taskq();
	assert(ice.ice_rx_reap == NULL && live_dma == dma - 2);

	held[0] = loan(&ring, 0);
	assert(restart(&ring));
	dma = live_dma;
	dispatch_fail = 1;
	freemsg(held[0]);
	dispatch_fail = 0;
	assert(ice.ice_rx_reap != NULL && live_dma == dma);
	assert(restart(&ring));
	assert(ice.ice_rx_reap == NULL && live_dma == dma - 1);
	stop(&ring);
	teardown(&ring);
}

static ice_rx_ring_t *race_ring;

/*
 * Another CPU restarts the ring between the return's test of irxr_pool and
 * its push.  The free routine's own allocation is where it pauses.
 */
static void
restart_in_race(void)
{
	int saved = in_free_routine;

	in_free_routine = 0;
	assert(restart(race_ring));
	in_free_routine = saved;
}

/*
 * A loan whose pool was replaced after the return tested it still reaches
 * the reap: the harvest finds it in the ring's return list.  The harvest runs
 * under the ring lock, so it queues the block without a taskq dispatch.
 */
static void
return_races_restart(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held;
	uint_t dma;

	setup(&ring, &ice);
	held = loan(&ring, 0);
	race_ring = &ring;
	on_desballoc = restart_in_race;
	freemsg(held);
	assert(on_desballoc == NULL);
	/* The restart counted the loan as its old pool's. */
	assert(returned(&ring) == 1 && ice.ice_rx_orphan_loans == 1);
	assert(ring.irxr_nloaned == 0 && ring.irxr_nfree == ring.irxr_nrcb -
	    ring.irxr_size);
	dma = live_dma;
	harvest(&ring);
	assert(ice.ice_rx_orphan_loans == 0 && dispatches == 0);
	assert(ring.irxr_stats.icrxs_orphan_loans.value.ui64 == 0);
	assert(ring.irxr_nfree == ring.irxr_nrcb - ring.irxr_size);
	assert(ice.ice_rx_reap != NULL && live_dma == dma);
	/* The next start frees it. */
	assert(restart(&ring));
	assert(ice.ice_rx_reap == NULL && live_dma == dma - 1);
	stop(&ring);
	teardown(&ring);
}

static mblk_t *late[2];
static ice_rx_ring_t *late_ring;

/* The return tested irxr_shutdown before the quiesce set it. */
static void
return_late(void)
{
	uint_t i;

	for (i = 0; i < 2; i++) {
		if (late[i] != NULL) {
			late_ring->irxr_shutdown = B_FALSE;
			freemsg(late[i]);
			late_ring->irxr_shutdown = B_TRUE;
			late[i] = NULL;
			return;
		}
	}
}

/*
 * A quiesce takes back loans already in the return list at once, and polls
 * for one that lands there while it waits, since that return sends no
 * wakeup.  A loan returned after the ring closed takes the ring lock.
 */
static void
quiesce_polls(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held[3];

	setup(&ring, &ice);
	held[0] = loan(&ring, 0);
	held[1] = loan(&ring, 1);
	held[2] = loan(&ring, 2);
	freemsg(held[0]);
	assert(returned(&ring) == 1 && ring.irxr_nloaned == 3);

	late[0] = held[1];
	late[1] = NULL;
	late_ring = &ring;
	on_delay = return_late;
	lbolt = 0;
	/* The third loan never returns: the wait runs to the deadline. */
	assert(!ice_rx_quiesce(&ice));
	assert(ring.irxr_shutdown && ring.irxr_returned == NULL);
	assert(ring.irxr_nloaned == 1 && late[0] == NULL);
	assert(lbolt >= drv_usectohz(ICE_RX_LOAN_WAIT_US));

	/* Returned after the close, the last loan goes to the free stack. */
	on_delay = NULL;
	freemsg(held[2]);
	assert(ring.irxr_returned == NULL && ring.irxr_nloaned == 0);
	assert(ring.irxr_nfree == ring.irxr_nrcb - ring.irxr_size);

	/* Every loan in the list: the quiesce takes them without waiting. */
	ring.irxr_shutdown = B_FALSE;
	ring.irxr_started = B_TRUE;
	held[0] = loan(&ring, 3);
	freemsg(held[0]);
	lbolt = 0;
	assert(ice_rx_quiesce(&ice));
	assert(lbolt == 0 && ring.irxr_nloaned == 0);
	teardown(&ring);
}

/*
 * With the reserve on loan, a loan waiting in the return list is taken back
 * before a new loan is refused.
 */
static void
reserve_harvest(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t **held, *mp;
	uint_t i, n;

	setup(&ring, &ice);
	n = ring.irxr_nreserve;
	held = calloc(n, sizeof (*held));
	assert(held != NULL);
	for (i = 0; i < n; i++)
		held[i] = loan(&ring, (uint16_t)(i % ring.irxr_size));
	assert(ring.irxr_nloaned == n);
	mutex_enter(&ring.irxr_lock);
	assert(ice_rx_bind(&ring, 0, ring.irxr_rcbs[0], 1500) == NULL);
	mutex_exit(&ring.irxr_lock);
	assert(ring.irxr_stats.icrxs_no_rcb.value.ui64 == 1);

	freemsg(held[0]);
	assert(returned(&ring) == 1 && ring.irxr_nloaned == n);
	mutex_enter(&ring.irxr_lock);
	mp = ice_rx_bind(&ring, 0, ring.irxr_rcbs[0], 1500);
	mutex_exit(&ring.irxr_lock);
	assert(mp != NULL && ring.irxr_returned == NULL);
	assert(ring.irxr_nloaned == n);
	held[0] = mp;
	for (i = 0; i < n; i++)
		freemsg(held[i]);
	free(held);
	harvest(&ring);
	teardown(&ring);
}

int
main(void)
{
	set_aside();
	reserve_harvest();
	return_races_restart();
	quiesce_polls();
	return_during_sweep();
	unbounded_restarts();
	limit();
	detach_wait();
	reap_undispatched();
	puts("RX orphan: loans return without the ring lock, replaced pools "
	    "keep only their loans, reap each as it returns, never stop a "
	    "start, and stay within ICE_RX_ORPHAN_MAX");
	return (0);
}

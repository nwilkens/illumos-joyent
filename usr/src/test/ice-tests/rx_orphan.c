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
 * pool functions set such a pool aside, route its returning loans away from
 * the new pool, and free it only once the last loan is back.
 */
#include "rx_test.h"

static uint_t
orphans(const ice_rx_ring_t *r)
{
	const ice_rx_orphan_t *o;
	uint_t n = 0;

	for (o = r->irxr_orphans; o != NULL; o = o->iro_next)
		n++;
	return (n);
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
	ice_rx_ctrl_block_t *area = r->irxr_rcb_area;
	uint_t i;

	mutex_enter(&r->irxr_lock);
	r->irxr_shutdown = B_TRUE;
	r->irxr_started = B_FALSE;
	mutex_exit(&r->irxr_lock);
	if (!ice_rx_start(r->irxr_ice))
		return (B_FALSE);
	mutex_enter(&r->irxr_lock);
	assert(r->irxr_rcb_area != area && r->irxr_nloaned == 0);
	assert(r->irxr_nfree == r->irxr_nrcb);
	for (i = 0; i < r->irxr_size; i++)
		ice_rx_reset_desc(r, i, ice_rcb_alloc(r, B_FALSE));
	r->irxr_shutdown = B_FALSE;
	mutex_exit(&r->irxr_lock);
	return (B_TRUE);
}

static void
reap(ice_rx_ring_t *r)
{
	(void) ice_rx_orphans_reap(r);
}

static void
set_aside(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held[3];
	uint_t i, pool, nfree;

	setup(&ring, &ice);
	pool = ring.irxr_nrcb;
	for (i = 0; i < 3; i++)
		held[i] = loan(&ring, (uint16_t)i);
	assert(ring.irxr_nloaned == 3);

	/* Only the three loaned buffers of the old pool stay allocated. */
	assert(restart(&ring));
	assert(orphans(&ring) == 1 && ring.irxr_orphans->iro_nloaned == 3);
	assert(live_dma == 1 + 3 + pool);
	nfree = ring.irxr_nfree;

	/* A returning loan goes to its own pool, not the new free list. */
	broadcasts = 0;
	freemsg(held[0]);
	assert(ring.irxr_orphans->iro_nloaned == 2 && broadcasts == 1);
	assert(ring.irxr_nfree == nfree && ring.irxr_nloaned == 0);
	reap(&ring);
	assert(orphans(&ring) == 1 && live_dma == 1 + 3 + pool);

	/* A new-pool loan still returns to the new pool. */
	held[0] = loan(&ring, 5);
	assert(ring.irxr_nloaned == 1);
	freemsg(held[0]);
	assert(ring.irxr_nloaned == 0 && ring.irxr_nfree == nfree);

	freemsg(held[1]);
	freemsg(held[2]);
	assert(ring.irxr_orphans->iro_nloaned == 0);
	reap(&ring);
	assert(orphans(&ring) == 0 && live_dma == 1 + pool);
	teardown(&ring);
}

/* A peer that keeps a loan per restart cannot grow the set without bound. */
static void
bounded(void)
{
	ice_rx_ring_t ring;
	ice_t ice;
	mblk_t *held[ICE_RX_ORPHANS_MAX + 1];
	ice_rx_ctrl_block_t *area;
	uint_t i;

	setup(&ring, &ice);
	for (i = 0; i < ICE_RX_ORPHANS_MAX; i++) {
		held[i] = loan(&ring, 0);
		assert(restart(&ring));
	}
	assert(orphans(&ring) == ICE_RX_ORPHANS_MAX);
	held[i] = loan(&ring, 0);
	area = ring.irxr_rcb_area;
	assert(!restart(&ring));
	assert(ring.irxr_rcb_area == area && ring.irxr_nloaned == 1);

	for (i = 0; i <= ICE_RX_ORPHANS_MAX; i++)
		freemsg(held[i]);
	reap(&ring);
	assert(orphans(&ring) == 0 && ring.irxr_nloaned == 0);
	ring.irxr_shutdown = B_FALSE;
	teardown(&ring);
}

int
main(void)
{
	set_aside();
	bounded();
	puts("RX orphan: set-aside pools keep only their loans, take their "
	    "returns and stay bounded");
	return (0);
}

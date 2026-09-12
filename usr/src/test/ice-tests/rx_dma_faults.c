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

#include "rx_test.h"

typedef enum {
	FIRST, FOLLOWUP, JUMBO_SECOND, LIMIT_PEEK, REPOST, DATA_SECOND, ACCESS
} fault_site_t;

static void
fault(fault_site_t site, boolean_t intr, boolean_t dd, boolean_t loan,
    boolean_t sync)
{
	ice_rx_ring_t ring;
	ice_t ice;
	unsigned char data[2048] = { 0 };
	size_t length = loan ? 512 : 128;
	ddi_dma_handle_t d;
	uint_t nfree;

	setup(&ring, &ice);
	d = ring.irxr_desc_dma.idb_dma_handle;
	if (site == FIRST) {
		if (dd)
			post(&ring, 0, data, length, B_TRUE, B_FALSE);
	} else if (site == JUMBO_SECOND || site == DATA_SECOND) {
		post(&ring, 0, data, 2048, B_FALSE, B_FALSE);
		if (dd || site == DATA_SECOND)
			post(&ring, 1, data, length, B_TRUE, B_FALSE);
	} else {
		post(&ring, 0, data, length, B_TRUE, B_FALSE);
		if (dd && site != REPOST && site != ACCESS)
			post(&ring, 1, data, length, B_TRUE, B_FALSE);
	}
	if (site == LIMIT_PEEK)
		ice.ice_rx_limit_per_intr = 1;
	if (site == ACCESS) {
		acc_fail = 1;
	} else {
		unsigned fail_after =
		    (site == FIRST || site == DATA_SECOND) ? 1 : 2;

		if (site == DATA_SECOND)
			d = ring.irxr_rcbs[1]->ircb_dma.idb_dma_handle;
		if (sync) {
			d->fail_sync = 1;
			d->fail_sync_after = fail_after;
		} else {
			d->fail_check = 1;
			d->fail_check_after = fail_after;
		}
		if (site == REPOST) {
			/* Two valid reads (packet, DD-clear), then repost. */
			d->fail_direction = DDI_DMA_SYNC_FORDEV;
			d->fail_check_after = 3;
		}
	}
	nfree = ring.irxr_nfree;
	if (intr) {
		assert(!ice_rx_ring_intr(&ring));
		assert(delivered == 0 && !ring.irxr_intr_busy);
	} else {
		assert(ice_ring_rx_poll(&ring, 65536) == NULL);
	}
	assert((ice.ice_state & ICE_STATE_ERROR) != 0);
	assert(impacts == 1 && bad_wb_reads == 0);
	assert(doorbells == (site == ACCESS ? 1U : 0U));
	assert(ring.irxr_stats.icrxs_packets.value.ui64 == 0);
	assert(ring.irxr_stats.icrxs_bytes.value.ui64 == 0);
	assert(ring.irxr_stats.icrxs_intr_limit.value.ui64 == 0);
	assert(ring.irxr_nloaned == 0 && ring.irxr_nfree == nfree);
	if (site == FIRST) {
		assert(ring.irxr_head == 0 && wb_reads == 0 && barriers == 0);
		/* A sync error still checks the associated DMA handle. */
		assert(d->checks == 1);
	}
	if (site == JUMBO_SECOND)
		assert(ring.irxr_head == 0);
	teardown(&ring);
}

static void
normal(boolean_t intr, boolean_t loan)
{
	ice_rx_ring_t ring;
	ice_t ice;
	unsigned char data[512] = { 0 };

	setup(&ring, &ice);
	post(&ring, 0, data, loan ? 512 : 128, B_TRUE, B_FALSE);
	if (intr) {
		assert(!ice_rx_ring_intr(&ring));
		assert(delivered == 1);
	} else {
		mblk_t *mp = ice_ring_rx_poll(&ring, 65536);

		assert(mp != NULL);
		freemsgchain(mp);
	}
	assert(ice.ice_state == 0 && impacts == 0 && bad_wb_reads == 0);
	assert(doorbells == 1 && ring.irxr_stats.icrxs_packets.value.ui64 == 1);
	assert(ring.irxr_nloaned == 0);
	teardown(&ring);
}

int
main(void)
{
	unsigned cases = 0;
	unsigned intr, loan, dd, sync;
	fault_site_t site;

	for (intr = 0; intr < 2; intr++) {
		for (loan = 0; loan < 2; loan++) {
			normal(intr, loan);
			cases++;
			for (site = FIRST; site <= ACCESS; site++) {
				if (site == LIMIT_PEEK && !intr)
					continue;
				for (dd = 0; dd < 2; dd++) {
					for (sync = 0; sync < 2; sync++) {
						fault(site, intr, dd, loan,
						    sync);
						cases++;
					}
				}
			}
		}
	}
	(void) printf("RX DMA: %u fault and healthy delivery cases passed\n",
	    cases);
	return (0);
}

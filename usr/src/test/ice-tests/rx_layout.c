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

static void
layout(size_t length, boolean_t tagged, boolean_t force_copy)
{
	ice_rx_ring_t ring;
	ice_t ice;
	unsigned char data[9216], expected[9220], actual[9220];
	ice_rx_ctrl_block_t *posted[5];
	unsigned nsegs = 0, i;
	uint_t nfree;
	uint32_t total = 0;
	boolean_t defer;
	mblk_t *mp, *m;
	size_t offset, l2len, copied = 0;

	setup(&ring, &ice);
	for (offset = 0; offset < length; offset++)
		data[offset] = (unsigned char)offset;
	data[12] = 0x86;
	data[13] = 0xdd;
	for (offset = 0; offset < length; nsegs++) {
		size_t n = length - offset;

		if (n > ICE_RX_BUF_SIZE)
			n = ICE_RX_BUF_SIZE;
		post(&ring, nsegs, data + offset, n, offset + n == length,
		    tagged && offset + n == length);
		offset += n;
	}
	for (i = 0; i < nsegs; i++)
		posted[i] = ring.irxr_rcbs[i];
	nfree = ring.irxr_nfree;
	if (force_copy)
		ring.irxr_nreserve = 0;
	mutex_enter(&ring.irxr_lock);
	mp = ice_ring_rx_frame(&ring, &total, &defer);
	mutex_exit(&ring.irxr_lock);
	assert(mp != NULL && !defer && checksum_head == mp);
	for (i = 0; i < nsegs; i++) {
		ddi_dma_handle_t d = posted[i]->ircb_dma.idb_dma_handle;

		assert(d->last_offset == 6);
		assert(d->last_length == (i + 1 < nsegs ? ICE_RX_BUF_SIZE :
		    length - i * ICE_RX_BUF_SIZE));
	}
	l2len = tagged ? 18 : 14;
	assert(((uintptr_t)(mp->b_rptr + l2len) & 3) == 0);
	/* Includes a full IPv6 base header and a maximum-size TCP header. */
	assert(MBLKL(mp) >= l2len + 40 + 60);
	assert(total == length + (tagged ? 4 : 0));
	assert(ring.irxr_head == nsegs);
	if (tagged) {
		memcpy(expected, data, 12);
		expected[12] = 0x81;
		expected[13] = 0;
		expected[14] = 0xab;
		expected[15] = 0xcd;
		memcpy(expected + 16, data + 12, length - 12);
	} else {
		memcpy(expected, data, length);
	}
	for (m = mp; m != NULL; m = m->b_cont) {
		memcpy(actual + copied, m->b_rptr, MBLKL(m));
		copied += MBLKL(m);
	}
	assert(copied == total && memcmp(actual, expected, total) == 0);
	if (length < ICE_RX_COPY_THRESHOLD || force_copy) {
		assert(ring.irxr_nloaned == 0 && ring.irxr_nfree == nfree);
	} else {
		assert(ring.irxr_nloaned == nsegs);
	}
	freemsg(mp);
	assert(ring.irxr_nloaned == 0 && ring.irxr_nfree == nfree);
	/* The posted address plus the full DBUF fits every allocation. */
	for (i = 0; i < ring.irxr_size; i++) {
		ice_rx_ctrl_block_t *r = ring.irxr_rcbs[i];
		uintptr_t posted_addr = ring.irxr_descs[i].read.pkt_addr;

		assert(posted_addr >= (uintptr_t)r->ircb_dma.idb_va);
		assert(posted_addr + ICE_RX_BUF_SIZE <=
		    (uintptr_t)r->ircb_dma.idb_va + r->ircb_dma.idb_len);
		assert(r->ircb_dma.idb_dma_handle->base ==
		    (unsigned char *)r->ircb_dma.idb_va);
	}
	teardown(&ring);
}

int
main(void)
{
	const size_t sizes[] = { 128, 1500, 2048, 9216 };
	unsigned i, vlan, copy;

	for (i = 0; i < sizeof (sizes) / sizeof (*sizes); i++) {
		for (vlan = 0; vlan < 2; vlan++) {
			for (copy = 0; copy < 2; copy++)
				layout(sizes[i], vlan, copy);
		}
	}
	puts("RX layout: 16 copy/loan, VLAN and jumbo cases passed");
	return (0);
}

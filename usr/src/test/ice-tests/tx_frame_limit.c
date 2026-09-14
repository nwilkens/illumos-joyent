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
 * Execute the production TX frame admission rule.  MAC does not bound a
 * client's frame against the link SDU, and E810 reports a packet above its
 * programmed maximum as a malicious-driver event for the whole function.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int boolean_t;
#define	B_TRUE	1
#define	B_FALSE	0

struct ether_vlan_header {
	uint8_t bytes[18];
};

#include "tx_frame_limit_body.h"

static boolean_t
plain(uint32_t mtu, size_t msglen)
{
	ice_tx_ctx_t ctx = { 0 };

	return (ice_tx_frame_fits(mtu, &ctx, msglen));
}

static boolean_t
lso(uint32_t mtu, uint32_t hdrlen, uint32_t mss)
{
	ice_tx_ctx_t ctx = { 0 };

	ctx.itc_use_ctx = B_TRUE;
	ctx.itc_hdrlen = hdrlen;
	ctx.itc_mss = mss;
	/* The aggregate LSO length never decides admission. */
	ctx.itc_tsolen = 64 * 1024;
	return (ice_tx_frame_fits(mtu, &ctx, 64 * 1024 + hdrlen));
}

int
main(void)
{
	/* Default MTU: a tagged maximum frame fits, one more byte does not. */
	assert(plain(1500, 1518));
	assert(!plain(1500, 1519));
	assert(plain(1500, 60));

	/* Jumbo MTU tracks the configured value, not the hardware maximum. */
	assert(plain(9000, 9018));
	assert(!plain(9000, 9019));
	assert(!plain(9000, 9728));
	assert(!plain(9000, UINT16_MAX));

	/* LSO: each segment is header plus MSS; the seed length is irrelevant. */
	assert(lso(1500, 54, 1464));
	assert(!lso(1500, 54, 1465));
	assert(lso(9000, 74, 8944));
	assert(!lso(9000, 74, 8945));
	/* The descriptor's own MSS maximum exceeds the frame at any MTU. */
	assert(!lso(9000, 54, 9668));

	(void) printf("PASS: ice tx frame admission\n");
	return (0);
}

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
 * Run the NVMe/TCP transport's data range check against a model that uses
 * 128-bit arithmetic, on the R2T that wraps a 32-bit sum and on random input.
 */
#include "tcp_limits.h"

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint64_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (rng);
}

static boolean_t
model(uint32_t off, uint32_t len, uint32_t base, size_t total)
{
	unsigned __int128 end = (unsigned __int128)off + len;

	return (off >= base && end <= (unsigned __int128)base + total);
}

static uint32_t
pick(void)
{
	static const uint32_t edge[] = { 0, 1, 4096, 8192, 0x7fffffff,
	    0x80000000, 0xfffff001, 0xfffffffe, 0xffffffff };
	uint64_t r = next();

	if ((r & 3) == 0)
		return ((uint32_t)(r >> 8));
	if ((r & 3) == 1)
		return ((uint32_t)(r >> 8) & 0xffff);
	return (edge[(r >> 8) % ARRAY_SIZE(edge)]);
}

int
main(void)
{
	unsigned i;

	/* An 8K write; the second R2T asks for 4 GiB less 4095 bytes. */
	assert(nvmf_tcp_range_ok(0, 4096, 0, 8192));
	assert(!nvmf_tcp_range_ok(4096, 0xfffff001, 0, 8192));
	assert(nvmf_tcp_range_ok(4096, 4096, 0, 8192));
	assert(!nvmf_tcp_range_ok(4096, 4097, 0, 8192));
	assert(nvmf_tcp_range_ok(8192, 0, 0, 8192));
	assert(!nvmf_tcp_range_ok(0, 1, 1, 8192));
	assert(nvmf_tcp_range_ok(0xfffffffe, 1, 0xfffffff0, 0x20));

	for (i = 0; i < 2000000; i++) {
		uint32_t off = pick(), len = pick(), base = pick();
		size_t total = pick();

		assert(nvmf_tcp_range_ok(off, len, base, total) ==
		    model(off, len, base, total));
	}
	printf("tcp limits passed\n");
	return (0);
}

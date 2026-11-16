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
 * Check the handoff limits and that the receive limit is the longest PDU a
 * peer can build from the largest allowed data.
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

/* The length of a PDU with this header that carries len bytes of data. */
static uint64_t
pdu_len(unsigned hlen, uint32_t len, unsigned pda, boolean_t hd, boolean_t dd)
{
	uint64_t plen = hlen + (hd ? 4 : 0);

	if (len == 0)
		return (plen);
	while (plen % pda != 0)
		plen++;
	return (plen + len + (dd ? 4 : 0));
}

static void
limits(void)
{
	static const uint32_t datas[] = { 0, 1, 4096, 16384, 262144, 1 << 20,
	    0xfffffff0 };
	static const unsigned hdrs[] = { 72, 24 };
	unsigned pda, hd, dd, d, h;

	assert(nvmf_tcp_params_ok(4, 4, 0, 4096, 0, 2));
	assert(nvmf_tcp_params_ok(128, 128, 0xfffffffe, 0xfffffffc,
	    0xffffffff, 65536));
	assert(!nvmf_tcp_params_ok(0, 4, 0, 4096, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 0, 0, 4096, 0, 2));
	assert(!nvmf_tcp_params_ok(256, 4, 0, 4096, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 132, 0, 4096, 0, 2));
	assert(!nvmf_tcp_params_ok(6, 4, 0, 4096, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 4, 0xffffffff, 4096, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 4, 0, 0, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 4, 0, 4092, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 4, 0, 4098, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 4, 0, 0x100000000ULL, 0, 2));
	assert(!nvmf_tcp_params_ok(4, 4, 0, 4096, 0x100000000ULL, 2));
	assert(!nvmf_tcp_params_ok(4, 4, 0, 4096, 0, 1));
	assert(!nvmf_tcp_params_ok(4, 4, 0, 4096, 0, 65537));

	for (pda = 4; pda <= 128; pda += 4) {
		for (hd = 0; hd < 2; hd++) {
			for (dd = 0; dd < 2; dd++) {
				for (d = 0; d < ARRAY_SIZE(datas); d++) {
					uint32_t lim = nvmf_tcp_rx_limit(
					    datas[d], pda, hd, dd);
					uint64_t most = 152;

					for (h = 0; h < ARRAY_SIZE(hdrs); h++) {
						uint64_t l = pdu_len(hdrs[h],
						    datas[d], pda, hd, dd);

						most = MAX(most, l);
					}
					assert(lim == MIN(most, UINT32_MAX));
				}
			}
		}
	}
}

int
main(void)
{
	unsigned i;

	limits();

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

/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Runs the firmware range checks from t4_ofld.c and the work request
 * completion waiters from t4_ofld_cpl.c, extracted by ofld_units.py.
 */

#include "units.h"

static void
test_ranges(void)
{
	t4_rdma_range_t r, a, b;

	CHECK(t4_ofld_range(0x100, 0x1ff, 1ULL << 32, 32, &r));
	CHECK(r.trr_start == 0x100 && r.trr_size == 0x100);
	CHECK(!t4_ofld_range(0, UINT32_MAX, 1ULL << 32, 1, &r));
	CHECK(t4_ofld_range(1, UINT32_MAX, 1ULL << 32, 1, &r));
	CHECK(r.trr_size == UINT32_MAX);
	CHECK(!t4_ofld_range(0x200, 0x1ff, 1ULL << 32, 1, &r));
	CHECK(!t4_ofld_range(0x10, 0x1000, 0x1000, 1, &r));
	CHECK(t4_ofld_range(0x10, 0xfff, 0x1000, 1, &r));
	CHECK(!t4_ofld_range(0x108, 0x1ff, 1ULL << 32, 32, &r));
	CHECK(t4_ofld_range(0x100, 0x100, 0x101, 1, &r) && r.trr_size == 1);
	CHECK(!t4_ofld_range(0, UINT16_MAX + 1U, UINT16_MAX + 1ULL, 1, &r));

	a.trr_start = 100; a.trr_size = 10;
	b.trr_start = 110; b.trr_size = 5;
	CHECK(!t4_ofld_overlap(&a, &b) && !t4_ofld_overlap(&b, &a));
	b.trr_start = 109;
	CHECK(t4_ofld_overlap(&a, &b) && t4_ofld_overlap(&b, &a));
	a.trr_start = UINT32_MAX - 1; a.trr_size = 2;
	b.trr_start = 0; b.trr_size = 1;
	CHECK(!t4_ofld_overlap(&a, &b));
}

static void
reply(t4_ofld_t *of, uint64_t cookie, uint8_t status)
{
	struct cpl_fw6_msg m;

	memset(&m, 0, sizeof (m));
	m.type = FW6_TYPE_WR_RPL;
	m.data[0] = BE_64((uint64_t)status << 8);
	m.data[1] = BE_64(cookie);
	t4_ofld_wr_rpl(of, &m);
}

/* Replies name a slot and its generation; forgeries change nothing. */
static void
test_waiters(void)
{
	t4_ofld_t *of = calloc(1, sizeof (*of));
	uint64_t c[T4_OFLD_NWAITERS], extra;
	t4_ofld_waiter_t *w;

	for (uint_t i = 0; i < T4_OFLD_NWAITERS; i++) {
		CHECK(t4_ofld_waiter_get(of, &c[i]) == 0);
		CHECK((c[i] & T4_OFLD_COOKIE_PARENT) != 0);
		CHECK((c[i] & 0xff) == i);
	}
	CHECK(t4_ofld_waiter_get(of, &extra) == EAGAIN);

	w = &of->of_waiter[3];
	reply(of, c[3] & ~T4_OFLD_COOKIE_PARENT, 0);
	reply(of, c[3] + (1ULL << 8), 0);
	reply(of, (c[3] & ~0xffULL) | 0x40, 0);
	CHECK(w->ow_state == TWS_BUSY);
	CHECK(of->of_stats.os_wr_badcookie == 3);

	reply(of, c[3], 5);
	CHECK(w->ow_state == TWS_DONE && w->ow_status == EIO);
	reply(of, c[3], 0);
	CHECK(w->ow_status == EIO && of->of_stats.os_wr_badcookie == 4);

	/* A reply for an abandoned request frees the slot for later use. */
	of->of_waiter[4].ow_state = TWS_ABANDONED;
	reply(of, c[4], 0);
	CHECK(of->of_waiter[4].ow_state == TWS_FREE);
	CHECK(t4_ofld_waiter_get(of, &extra) == 0 && (extra & 0xff) == 4);
	CHECK(extra != c[4]);
	reply(of, c[4], 0);
	CHECK(of->of_waiter[4].ow_state == TWS_BUSY);

	t4_ofld_waiter_put(of, c[5]);
	CHECK(of->of_waiter[5].ow_state == TWS_FREE);
	reply(of, c[5], 0);
	CHECK(of->of_waiter[5].ow_state == TWS_FREE);
	free(of);
}

int
main(void)
{
	test_ranges();
	test_waiters();
	(void) printf("offload units: ranges and waiters passed\n");
	return (0);
}

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
 * Copyright 2026 MNX Cloud, Inc.
 */

/*
 * The device asks for pages with a signed count it chooses, at boot and at
 * runtime. The driver must survive any count, keep to a total page limit,
 * and keep its page lock balanced when it cannot allocate.
 */

#include "pages_test.h"

#define	LIMIT		6000

static void
int_min(void)
{
	page_request(0, 0x80000000U);
	if (take_calls != 1)
		stub_fail("INT32_MIN request made %" PRIu64 " take calls",
		    take_calls);
}

static void
runtime_limit(void)
{
	mlx.mlx_npages_max = LIMIT;
	for (int i = 0; i < 5; i++)
		page_request(0, INT32_MAX);
	if (mlx.mlx_npages > LIMIT)
		stub_fail("gave %u pages, over the limit of %u",
		    mlx.mlx_npages, LIMIT);
	if (alloc_fail_calls == 0)
		stub_fail("refused request not reported to the device");
}

static void
boot_limit(void)
{
	mlx.mlx_npages_max = LIMIT;
	query_reply = INT32_MAX;
	if (mlxcx_init_pages(&mlx, MLXCX_QUERY_PAGES_OPMOD_BOOT))
		stub_fail("boot accepted a request for INT32_MAX pages");
	if (mlx.mlx_npages > LIMIT)
		stub_fail("gave %u pages, over the limit of %u",
		    mlx.mlx_npages, LIMIT);
}

static void
alloc_fail(void)
{
	stub_dma_fail = B_TRUE;
	page_request(0, 16);
	if (alloc_fail_calls != 1)
		stub_fail("allocation failure not reported to the device");
	if (mlx.mlx_npages != 0)
		stub_fail("page count changed");
}

static void
rejected(void)
{
	int64_t live = stub_dma_live;

	give_policy = GIVE_REJECT;
	page_request(0, 16);
	if (alloc_fail_calls != 1)
		stub_fail("refused gift not reported to the device");
	if (mlx.mlx_npages != 0 || stub_dma_live != live)
		stub_fail("refused pages not freed");
}

static const char *const names[] = {
	"int-min", "runtime-limit", "boot-limit", "alloc-fail", "rejected",
	NULL
};
static void (*const funcs[])(void) = {
	int_min, runtime_limit, boot_limit, alloc_fail, rejected
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

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
 * Hardware may hand back a page we never gave it, or the same page twice.
 * The driver must skip those without a panic and without freeing anything
 * the device still holds, and teardown must end even if hardware never gives
 * back a page we know.
 */

#include "pages_test.h"

#define	UNKNOWN_PA	0x7000000000ULL

static void
boot_pages(int32_t n)
{
	query_reply = n;
	if (!mlxcx_init_pages(&mlx, MLXCX_QUERY_PAGES_OPMOD_BOOT))
		stub_fail("boot pages failed");
	if (mlx.mlx_npages != (uint_t)n)
		stub_fail("gave %u pages, not %d", mlx.mlx_npages, n);
}

static void
teardown_unknown(void)
{
	boot_pages(4);
	take_script[take_script_n++] = UNKNOWN_PA;
	mlxcx_teardown_pages(&mlx);
	if (dev_npages != 0 || mlx.mlx_npages != 0)
		stub_fail("teardown left %u pages", mlx.mlx_npages);
}

static void
teardown_duplicate(void)
{
	boot_pages(4);
	take_script[take_script_n++] = known_pa(0);
	take_script[take_script_n++] = known_pa(0);
	mlxcx_teardown_pages(&mlx);
	if (mlx.mlx_npages != 0)
		stub_fail("teardown left %u pages", mlx.mlx_npages);
}

static void
teardown_stuck(void)
{
	boot_pages(4);
	take_script[take_script_n++] = UNKNOWN_PA;
	take_script_repeat = B_TRUE;
	mlxcx_teardown_pages(&mlx);
	if (mlx.mlx_npages != 4)
		stub_fail("teardown freed pages the device still holds");
}

static void
take_unknown(void)
{
	boot_pages(4);
	take_script[take_script_n++] = UNKNOWN_PA;
	take_script[take_script_n++] = known_pa(1);
	page_request(0, (uint32_t)-2);
	if (mlx.mlx_npages != 3)
		stub_fail("%u pages left, expected 3", mlx.mlx_npages);
}

static void
take_empty(void)
{
	page_request(0, (uint32_t)-5);
	if (mlx.mlx_npages != 0)
		stub_fail("page count changed");
}

static const char *const names[] = {
	"teardown-unknown", "teardown-duplicate", "teardown-stuck",
	"take-unknown", "take-empty", NULL
};
static void (*const funcs[])(void) = {
	teardown_unknown, teardown_duplicate, teardown_stuck, take_unknown,
	take_empty
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

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
 * If a MANAGE_PAGES(GIVE) command times out, hardware may already hold the
 * pages. The driver must keep them, not free them, so a later return or
 * teardown can reclaim them.
 */

#include "pages_test.h"

static void
runtime(void)
{
	give_policy = GIVE_TIMEOUT;
	page_request(0, 16);
	if (mlx.mlx_npages != 16)
		stub_fail("kept %u pages, not 16", mlx.mlx_npages);
	give_policy = GIVE_ACCEPT;
	mlxcx_teardown_pages(&mlx);
	if (mlx.mlx_npages != 0 || dev_npages != 0)
		stub_fail("teardown did not reclaim the kept pages");
}

static void
boot(void)
{
	give_policy = GIVE_TIMEOUT;
	query_reply = 16;
	if (mlxcx_init_pages(&mlx, MLXCX_QUERY_PAGES_OPMOD_BOOT))
		stub_fail("boot pages succeeded after a timeout");
	if (mlx.mlx_npages != 16)
		stub_fail("kept %u pages, not 16", mlx.mlx_npages);
}

static const char *const names[] = { "runtime", "boot", NULL };
static void (*const funcs[])(void) = { runtime, boot };

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

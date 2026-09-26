/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/* The offload test ioctl layout; ioctl_abi.py supplies ofld_test.h. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ofld_test.h"

_Static_assert(offsetof(t4_ofld_test_t, tot_conn) == 72, "conn offset");
_Static_assert(sizeof (t4_ofld_test_conn_t) == 32, "conn size");
_Static_assert(offsetof(t4_ofld_test_conn_t, totc_rx_bytes) == 16, "rx");
_Static_assert(sizeof (t4_ofld_test_t) == 72 + 32 * T4_OFLD_TEST_NCONN,
    "size");

int
main(void)
{
	(void) printf("ioctl ABI: layout checked\n");
	return (0);
}

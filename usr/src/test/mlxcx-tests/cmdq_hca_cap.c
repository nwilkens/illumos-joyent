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
 * QUERY_HCA_CAP must report failure when the device fails the command, or
 * attach goes on with capability data the device never wrote.
 */

#include "cmdq_test.h"

static uint8_t reply_status;
static uint8_t reply_delivery;
static boolean_t hang;

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) ms; (void) seq;
	if (!hang)
		model_complete_in(slot, 1);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	(void) slot; (void) ms;
	out[0] = reply_status;
	*delivery = reply_delivery;
}

static boolean_t
query(void)
{
	mlxcx_hca_cap_t cap;

	model_attach(B_FALSE);
	return (mlxcx_cmd_query_hca_cap(model_mlxp, MLXCX_HCA_CAP_GENERAL,
	    MLXCX_HCA_CAP_MODE_CURRENT, &cap));
}

static void
bad_status(void)
{
	reply_status = MLXCX_CMD_R_BAD_PARAM;
	if (query())
		stub_fail("failed QUERY_HCA_CAP reported success");
}

static void
bad_delivery(void)
{
	reply_delivery = 0x10;
	if (query())
		stub_fail("undelivered QUERY_HCA_CAP reported success");
}

static void
timeout(void)
{
	hang = B_TRUE;
	if (query())
		stub_fail("timed out QUERY_HCA_CAP reported success");
}

static void
good(void)
{
	if (!query())
		stub_fail("good QUERY_HCA_CAP failed");
}

static const char *const names[] = {
	"bad-status", "bad-delivery", "timeout", "good", NULL
};
static void (*const funcs[])(void) = {
	bad_status, bad_delivery, timeout, good
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

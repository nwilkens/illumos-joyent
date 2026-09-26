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
 * Firmware can hold every ordinary command until it gets pages. A
 * MANAGE_PAGES command must still find a slot then, so the last slot is
 * kept for it, and other commands must never take that slot.
 */

#include "cmdq_test.h"

#define	NREQ		8

static boolean_t hang;
static int last_slot = -1;

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) seq;
	last_slot = slot;
	if (!hang || ms->ms_op == MLXCX_OP_MANAGE_PAGES)
		model_complete_in(slot, 1);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	(void) slot; (void) ms; (void) out; (void) delivery;
}

static boolean_t
page_command(void)
{
	uint64_t pas[NREQ];
	int32_t nret = -1;

	return (mlxcx_cmd_return_pages(model_mlxp, NREQ, pas, &nret));
}

static boolean_t
other_command(void)
{
	return (mlxcx_cmd_enable_hca(model_mlxp));
}

/* Ordinary commands fill every slot they may use and stay stuck. */
static void
full_queue(void)
{
	uint_t i;

	model_cmd_low = (2 << 4) | 6;
	model_attach(B_TRUE);
	hang = B_TRUE;
	for (i = 0; i < 3; i++) {
		if (other_command())
			stub_fail("stuck command succeeded");
		if (last_slot == 3)
			stub_fail("ordinary command used the page slot");
	}
	if (!page_command())
		stub_fail("page command failed behind a full queue");
	if (last_slot != 3)
		stub_fail("page command used slot %d, not 3", last_slot);
	last_slot = -1;
	if (other_command())
		stub_fail("ordinary command succeeded with no free slot");
	if (last_slot != -1)
		stub_fail("ordinary command took slot %d", last_slot);
}

/* A page command uses its own slot even when others are free. */
static void
own_slot(void)
{
	model_attach(B_FALSE);
	if (!page_command() || last_slot != 31)
		stub_fail("page command used slot %d, not 31", last_slot);
	if (!other_command() || last_slot == 31)
		stub_fail("ordinary command used the page slot");
}

/* A queue with one slot has no room for the page slot. */
static void
one_slot(void)
{
	model_cmd_low = (0 << 4) | 6;
	model_mlxp = &model_mlx;
	if (mlxcx_cmd_queue_init(model_mlxp))
		stub_fail("queue with a single slot accepted");
}

static const char *const names[] = {
	"full-queue", "own-slot", "one-slot", NULL
};
static void (*const funcs[])(void) = {
	full_queue, own_slot, one_slot
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

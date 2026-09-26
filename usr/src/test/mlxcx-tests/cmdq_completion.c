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
 * A command completion event names slots with a bit vector the device
 * chooses. The driver must complete a command only when the slot is in the
 * queue, holds a command, has come back from hardware, and carries the
 * command's token.
 */

#include "cmdq_test.h"

#define	GOOD_NPAGES	77
#define	BAD_NPAGES	666

static uint_t cur_slot;
static void (*on_first)(uint_t);

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) ms;
	cur_slot = slot;
	if (seq == 0 && on_first != NULL)
		on_first(slot);
	else
		model_complete_in(slot, 1);
}

static void
write_npages(uint8_t *out, uint32_t npages)
{
	model_put_be32(out + offsetof(mlxcx_cmd_query_pages_out_t,
	    mlxo_query_pages_npages), npages);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	(void) slot; (void) delivery;
	if (ms->ms_op == MLXCX_OP_QUERY_PAGES)
		write_npages(out, GOOD_NPAGES);
}

static void
query_expect(int32_t want)
{
	int32_t npages = -1;

	if (!mlxcx_cmd_query_pages(model_mlxp, MLXCX_QUERY_PAGES_OPMOD_BOOT,
	    &npages))
		stub_fail("query pages failed");
	if (npages != want)
		stub_fail("query pages returned %d, device said %d", npages,
		    want);
}

/* A stray event for a slot with no command, then the real completion. */
static void
idle_first(uint_t slot)
{
	model_schedule(stub_now + 1, EV_EQE, 0, 1U << (slot + 3), NULL);
	model_complete_in(slot, 2);
}

static void
idle_slot(void)
{
	model_attach(B_TRUE);
	on_first = idle_first;
	query_expect(GOOD_NPAGES);
	query_expect(GOOD_NPAGES);
}

/* A slot number beyond the queue the device itself reported. */
static void
range_first(uint_t slot)
{
	model_schedule(stub_now + 1, EV_EQE, 0, 1U << 20, NULL);
	model_schedule(stub_now + 1, EV_EQE, 0, 1U << 31, NULL);
	model_complete_in(slot, 2);
}

static void
out_of_range(void)
{
	model_cmd_low = (3 << 4) | 6;
	model_attach(B_TRUE);
	on_first = range_first;
	query_expect(GOOD_NPAGES);
}

/* The event arrives while hardware still owns the entry. */
static void
owned_first(uint_t slot)
{
	model_schedule(stub_now + 1, EV_EQE, 0, 1U << slot, NULL);
	model_complete_in(slot, 5);
}

static void
still_owned(void)
{
	model_attach(B_TRUE);
	on_first = owned_first;
	query_expect(GOOD_NPAGES);
}

/* The entry comes back with some other command's token. */
static void
forge(void)
{
	mlxcx_cmd_ent_t *ent = (mlxcx_cmd_ent_t *)model_entry(cur_slot);

	ent->mce_token ^= 0xff;
	write_npages(ent->mce_output, BAD_NPAGES);
	ent->mce_status = 0;
	model_eqe(1U << cur_slot);
}

static void
unforge(void)
{
	mlxcx_cmd_ent_t *ent = (mlxcx_cmd_ent_t *)model_entry(cur_slot);

	ent->mce_token ^= 0xff;
	model_finish(cur_slot);
	model_eqe(1U << cur_slot);
}

static void
token_first(uint_t slot)
{
	(void) slot;
	model_schedule(stub_now + 1, EV_CALL, 0, 0, forge);
	model_schedule(stub_now + 5, EV_CALL, 0, 0, unforge);
}

static void
wrong_token(void)
{
	model_attach(B_TRUE);
	on_first = token_first;
	query_expect(GOOD_NPAGES);
}

/* A second event for a command that has already finished. */
static void
double_first(uint_t slot)
{
	model_complete_in(slot, 1);
	model_schedule(stub_now + 2, EV_EQE, 0, 1U << slot, NULL);
}

static void
double_event(void)
{
	model_attach(B_TRUE);
	on_first = double_first;
	query_expect(GOOD_NPAGES);
	while (stub_sleep_hook(-1))
		;
	query_expect(GOOD_NPAGES);
}

static const char *const names[] = {
	"idle-slot", "out-of-range", "still-owned", "wrong-token",
	"double-event", NULL
};
static void (*const funcs[])(void) = {
	idle_slot, out_of_range, still_owned, wrong_token, double_event
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

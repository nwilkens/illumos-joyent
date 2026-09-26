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
 * When a command times out, hardware still owns its entry and may still
 * write its output mailboxes. The driver must keep the slot, the token and
 * the mailboxes until hardware gives the entry back, and must not free them
 * or post another command in that slot before then.
 */

#include "cmdq_test.h"

#define	NREQ		8

static boolean_t hang;
static int last_slot = -1;

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) ms; (void) seq;
	last_slot = slot;
	if (!hang)
		model_complete_in(slot, 1);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	(void) slot; (void) delivery;
	if (ms->ms_op == MLXCX_OP_MANAGE_PAGES)
		model_put_be32(out + offsetof(mlxcx_cmd_manage_pages_out_t,
		    mlxo_manage_pages_npages), 0);
}

/* Issue a command that uses output mailboxes; return whether it worked. */
static boolean_t
command(void)
{
	uint64_t pas[NREQ];
	int32_t nret = -1;

	return (mlxcx_cmd_return_pages(model_mlxp, NREQ, pas, &nret));
}

static void
timeout_one(void)
{
	hang = B_TRUE;
	if (command())
		stub_fail("a command hardware never answered succeeded");
	hang = B_FALSE;
}

static void
late(uint_t slot)
{
	model_complete_in(slot, 1);
	while (stub_sleep_hook(-1))
		;
}

/* Hardware answers after the timeout, into the old mailboxes. */
static void
late_write(void)
{
	int64_t base;

	model_attach(B_FALSE);
	base = stub_dma_live;
	timeout_one();
	late(last_slot);
	if (!command())
		stub_fail("command after a late completion failed");
	if (stub_dma_live != base)
		stub_fail("%" PRId64 " mailboxes not freed after hardware "
		    "returned the slot", stub_dma_live - base);
	if (stub_ids_used(model_mlx.mlx_cmd.mcmd_tokens) != 0)
		stub_fail("token of a returned slot not freed");
}

/* The next command must not reuse a slot hardware still owns. */
static void
no_alias(void)
{
	uint_t first;

	model_attach(B_FALSE);
	timeout_one();
	first = last_slot;
	if (!command())
		stub_fail("second command failed");
	if (last_slot == (int)first)
		stub_fail("second command reused the timed out slot");
	late(first);
	if (!command() || last_slot != (int)first)
		stub_fail("returned slot %u was not reused", first);
}

/* Every slot is abandoned; a new command waits for one to come back. */
static void
give_back_first(void)
{
	model_finish(0);
}

static void
all_abandoned(void)
{
	model_cmd_low = (1 << 4) | 6;
	model_attach(B_FALSE);
	timeout_one();
	timeout_one();
	model_schedule(stub_now + 500, EV_CALL, 0, 0, give_back_first);
	if (!command())
		stub_fail("command after a slot came back failed");
	if (last_slot != 0)
		stub_fail("command used slot %d, not the returned slot 0",
		    last_slot);
}

/* A slot handed back with some other token stays abandoned. */
static void
forge_return(void)
{
	mlxcx_cmd_ent_t *ent = (mlxcx_cmd_ent_t *)model_entry(0);

	ent->mce_token ^= 0xff;
	ent->mce_status = 0;
}

static void
late_wrong_token(void)
{
	model_cmd_low = (1 << 4) | 6;
	model_attach(B_FALSE);
	timeout_one();
	model_schedule(stub_now + 1, EV_CALL, 0, 0, forge_return);
	while (stub_sleep_hook(-1))
		;
	if (!command() || last_slot != 1)
		stub_fail("command did not use the free slot 1");
	if (!command() || last_slot != 1)
		stub_fail("forged return freed the abandoned slot 0");
}

/* Detach while hardware still owns a command: nothing may be freed. */
static void
fini_leak(void)
{
	model_attach(B_FALSE);
	timeout_one();
	mlxcx_cmd_queue_fini(model_mlxp);
	late(last_slot);
}

static const char *const names[] = {
	"late-write", "no-alias", "all-abandoned", "late-wrong-token",
	"fini-leak", NULL
};
static void (*const funcs[])(void) = {
	late_write, no_alias, all_abandoned, late_wrong_token, fini_leak
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

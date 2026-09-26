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
 * Many callers can set up commands and then wait for a slot. None of them
 * may hold something that a MANAGE_PAGES command needs before it can take
 * the reserved page slot, and each post to a slot must carry a new token.
 */

#include "cmdq_test.h"

#define	NREQ		8
#define	NWAITERS	300

static uint8_t last_token[32];
static boolean_t seen[32];

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) seq;
	if (ms->ms_token == 0)
		stub_fail("command posted with token 0");
	if (seen[slot] && ms->ms_token == last_token[slot])
		stub_fail("slot %u posted twice with token %u", slot,
		    ms->ms_token);
	seen[slot] = B_TRUE;
	last_token[slot] = ms->ms_token;
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

/* Callers that have set up a command but not yet got a slot. */
static mlxcx_cmd_t waiters[NWAITERS];

static void
waiting_callers(void)
{
	model_attach(B_TRUE);
	for (uint_t i = 0; i < NWAITERS; i++)
		mlxcx_cmd_init(model_mlxp, &waiters[i]);
	if (!page_command())
		stub_fail("page command failed behind waiting callers");
	for (uint_t i = 0; i < NWAITERS; i++)
		mlxcx_cmd_fini(model_mlxp, &waiters[i]);
}

static void
fresh_tokens(void)
{
	model_attach(B_TRUE);
	for (uint_t i = 0; i < 600; i++) {
		if (!mlxcx_cmd_enable_hca(model_mlxp) || !page_command())
			stub_fail("command %u failed", i);
	}
}

/*
 * The page slot sees one command, then 254 ordinary ones elsewhere; a
 * single rolling counter would give the next page command the same token.
 */
static void
page_gap(void)
{
	model_attach(B_TRUE);
	for (uint_t round = 0; round < 3; round++) {
		if (!page_command())
			stub_fail("page command failed");
		for (uint_t i = 0; i < 254; i++) {
			if (!mlxcx_cmd_enable_hca(model_mlxp))
				stub_fail("command %u failed", i);
		}
	}
	if (!page_command())
		stub_fail("page command failed");
}

static const char *const names[] = {
	"waiting-callers", "fresh-tokens", "page-gap", NULL
};
static void (*const funcs[])(void) = {
	waiting_callers, fresh_tokens, page_gap
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

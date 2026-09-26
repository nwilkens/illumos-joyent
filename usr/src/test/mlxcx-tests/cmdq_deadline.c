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
 * In event mode a command must not wait forever: not for a firmware that
 * never answers, not for a completion event that never comes, and not for a
 * free slot.
 */

#include "cmdq_test.h"

#define	NREQ		8
#define	HOUR		(100 * 60 * 60)

static boolean_t hang;
static boolean_t quiet;
static int last_slot = -1;

static void
silent_finish(void)
{
	model_finish(last_slot);
}

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) ms; (void) seq;
	last_slot = slot;
	if (quiet)
		model_schedule(stub_now + 1, EV_CALL, 0, 0, silent_finish);
	else if (!hang)
		model_complete_in(slot, 1);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	(void) slot; (void) ms; (void) out; (void) delivery;
}

static boolean_t
command(void)
{
	uint64_t pas[NREQ];
	int32_t nret = -1;

	return (mlxcx_cmd_return_pages(model_mlxp, NREQ, pas, &nret));
}

/* Firmware never answers; the caller gets an error in bounded time. */
static void
event_hang(void)
{
	clock_t start;
	uint_t first;

	model_attach(B_TRUE);
	hang = B_TRUE;
	start = stub_now;
	if (command())
		stub_fail("a command hardware never answered succeeded");
	if (stub_now - start > HOUR)
		stub_fail("waited more than an hour");
	first = last_slot;
	hang = B_FALSE;

	/* A late completion event gives the slot back. */
	model_complete_in(first, 1);
	while (stub_sleep_hook(-1))
		;
	if (!command() || last_slot != (int)first)
		stub_fail("slot %u was not reused after it came back", first);
}

/* The command finishes but its completion event is lost. */
static void
lost_event(void)
{
	clock_t start;

	model_attach(B_TRUE);
	quiet = B_TRUE;
	start = stub_now;
	if (!command())
		stub_fail("a finished command failed");
	if (stub_now - start > HOUR)
		stub_fail("waited more than an hour");
}

/* Every slot stays with hardware; a new command must give up. */
static void
slot_wait(void)
{
	clock_t start;

	model_cmd_low = (1 << 4) | 6;
	model_attach(B_TRUE);
	hang = B_TRUE;
	(void) command();
	(void) command();
	start = stub_now;
	if (command())
		stub_fail("a command with no free slot succeeded");
	if (stub_now - start > HOUR)
		stub_fail("waited more than an hour for a slot");
}

static const char *const names[] = {
	"event-hang", "lost-event", "slot-wait", NULL
};
static void (*const funcs[])(void) = {
	event_hang, lost_event, slot_wait
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

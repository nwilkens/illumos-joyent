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
 * MANAGE_PAGES(RETURN) reports how many pages hardware gave back. The driver
 * must not trust that count past the number it asked for.
 */

#include "cmdq_test.h"

#define	NREQ		8
#define	PA(i)		(0x7000000000ULL + (uint64_t)(i) * 4096)

static uint32_t reply_npages;

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) ms; (void) seq;
	model_complete_in(slot, 1);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	size_t off = offsetof(mlxcx_cmd_manage_pages_out_t,
	    mlxo_manage_pages_pas);
	uint32_t i;

	(void) slot; (void) delivery;
	if (ms->ms_op != MLXCX_OP_MANAGE_PAGES)
		return;
	model_put_be32(out + offsetof(mlxcx_cmd_manage_pages_out_t,
	    mlxo_manage_pages_npages), reply_npages);
	for (i = 0; off + (i + 1) * 8 <= ms->ms_outlen; i++)
		model_put_be64(out + off + i * 8, PA(i));
}

static boolean_t
take(int32_t nreq, uint64_t *pas, int32_t *nret)
{
	model_attach(B_FALSE);
	return (mlxcx_cmd_return_pages(model_mlxp, nreq, pas, nret));
}

static void
too_many(void)
{
	uint64_t *pas = calloc(NREQ, sizeof (*pas));
	int32_t nret = -1;

	reply_npages = NREQ + 1;
	if (take(NREQ, pas, &nret))
		stub_fail("accepted %u pages for a request of %d",
		    reply_npages, NREQ);
	free(pas);
}

static void
negative(void)
{
	uint64_t *pas = calloc(NREQ, sizeof (*pas));
	int32_t nret = -1;

	reply_npages = 0x80000000U;
	if (take(NREQ, pas, &nret))
		stub_fail("accepted a count of 0x%x pages", reply_npages);
	free(pas);
}

static void
exact(void)
{
	uint64_t *pas = calloc(NREQ, sizeof (*pas));
	int32_t nret = -1;

	reply_npages = NREQ;
	if (!take(NREQ, pas, &nret) || nret != NREQ)
		stub_fail("a full return failed");
	for (int i = 0; i < NREQ; i++) {
		if (pas[i] != PA(i))
			stub_fail("PA %d wrong", i);
	}
	free(pas);
}

static void
huge_request(void)
{
	uint64_t *pas = calloc(MLXCX_MANAGE_PAGES_MAX_PAGES + 1, sizeof (*pas));
	int32_t nret = -1;

	reply_npages = 0;
	if (take(MLXCX_MANAGE_PAGES_MAX_PAGES + 1, pas, &nret))
		stub_fail("sent a request for more than the page limit");
	free(pas);
}

static const char *const names[] = {
	"too-many", "negative", "exact", "huge-request", NULL
};
static void (*const funcs[])(void) = {
	too_many, negative, exact, huge_request
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

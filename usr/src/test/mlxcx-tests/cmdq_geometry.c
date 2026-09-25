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
 * The device reports the command queue size and entry stride. Attach must
 * refuse any geometry that does not fit the one 4 KiB queue page.
 */

#include "cmdq_test.h"

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) ms; (void) seq;
	model_complete_in(slot, 1);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	(void) slot; (void) ms; (void) out; (void) delivery;
}

static void
geometry(uint_t size_l2, uint_t stride_l2, boolean_t fits)
{
	model_cmd_low = (size_l2 << 4) | stride_l2;
	model_mlxp = &model_mlx;
	if (mlxcx_cmd_queue_init(model_mlxp) != fits) {
		stub_fail("queue of 2^%u entries, stride 2^%u: init %s",
		    size_l2, stride_l2, fits ? "refused" : "accepted");
	}
	if (!fits)
		return;
	if (!mlxcx_cmd_enable_hca(model_mlxp))
		stub_fail("a command on an accepted queue failed");
	mlxcx_cmd_queue_fini(model_mlxp);
}

static void
small_stride(void)
{
	geometry(5, 5, B_FALSE);
}

static void
zero_stride(void)
{
	geometry(1, 0, B_FALSE);
}

static void
overflow(void)
{
	geometry(5, 8, B_FALSE);
}

static void
huge_stride(void)
{
	geometry(1, 15, B_FALSE);
}

static void
normal(void)
{
	geometry(5, 6, B_TRUE);
}

static void
tight(void)
{
	geometry(5, 7, B_TRUE);
	geometry(4, 8, B_TRUE);
}

static const char *const names[] = {
	"small-stride", "zero-stride", "overflow", "huge-stride", "normal",
	"tight", NULL
};
static void (*const funcs[])(void) = {
	small_stride, zero_stride, overflow, huge_stride, normal, tight
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

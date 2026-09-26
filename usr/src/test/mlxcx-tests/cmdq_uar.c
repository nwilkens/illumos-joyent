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
 * ALLOC_UAR returns a UAR page index that we turn into a BAR0 offset for
 * doorbell writes. The whole page must lie inside BAR0, must not be the init
 * segment at page 0, and must not wrap.
 */

#include "cmdq_test.h"

static uint32_t reply_uar;

static void
model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq)
{
	(void) ms; (void) seq;
	model_complete_in(slot, 1);
}

static void
model_output(uint_t slot, model_slot_t *ms, uint8_t *out, uint8_t *delivery)
{
	uint8_t *p = out + offsetof(mlxcx_cmd_alloc_uar_out_t,
	    mlxo_alloc_uar_uar);

	(void) slot; (void) delivery;
	if (ms->ms_op != MLXCX_OP_ALLOC_UAR)
		return;
	p[0] = reply_uar >> 16;
	p[1] = reply_uar >> 8;
	p[2] = reply_uar;
}

static boolean_t
alloc(uint32_t uar)
{
	model_attach(B_FALSE);
	if (!mlxcx_regs_map(model_mlxp))
		stub_fail("register map failed");
	reply_uar = uar;
	memset(&model_mlx.mlx_uar, 0, sizeof (model_mlx.mlx_uar));
	return (mlxcx_cmd_alloc_uar(model_mlxp, &model_mlx.mlx_uar));
}

static void
zero(void)
{
	if (alloc(0))
		stub_fail("UAR 0 accepted");
}

static void
past_bar(void)
{
	if (alloc(stub_regsize / 4096))
		stub_fail("UAR past the end of BAR0 accepted");
	if (model_mlx.mlx_uar.mlu_allocated)
		stub_fail("refused UAR marked allocated");
}

static void
last_page(void)
{
	uint32_t n = stub_regsize / 4096 - 1;
	mlxcx_uar_t *u = &model_mlx.mlx_uar;

	if (!alloc(n))
		stub_fail("last UAR page refused");
	if (u->mlu_num != n || u->mlu_base != n * 4096)
		stub_fail("UAR %u base 0x%x", u->mlu_num, u->mlu_base);
	if (u->mlu_bf[1].mbf_odd + 0x100 > n * 4096 + 4096)
		stub_fail("blue flame buffer outside the UAR page");
}

static void
wrap(void)
{
	stub_regsize = (off_t)1 << 40;
	if (alloc(0xffffff))
		stub_fail("UAR whose offset wraps 32 bits accepted");
}

static const char *const names[] = {
	"zero", "past-bar", "last-page", "wrap", NULL
};
static void (*const funcs[])(void) = {
	zero, past_bar, last_page, wrap
};

int
main(int argc, char **argv)
{
	return (test_main(argc, argv, names, funcs));
}

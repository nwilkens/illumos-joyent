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
 * Copyright 2026 Edgecast Cloud LLC.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint16_t __le16;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define	LE16_TO_CPU(x)	(x)
#define	CPU_TO_LE16(x)	(x)
#else
#define	LE16_TO_CPU(x)	__builtin_bswap16(x)
#define	CPU_TO_LE16(x)	__builtin_bswap16(x)
#endif
#define	ICE_NONDMA_TO_NONDMA	0
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))

#include "ice_sched_resource_types.h"

struct ice_hw {
	u8 num_tx_sched_layers, num_tx_sched_phys_layers;
	u8 flattened_layers, max_cgds;
	u16 max_children[ICE_AQC_TOPO_MAX_LEVEL_NUM];
	struct ice_aqc_layer_props *layer_info;
};

static struct ice_hw device;
static struct ice_aqc_query_txsched_res_resp response;
static unsigned allocations, live, queries, copies;
static unsigned fail_alloc;
static int query_status;

static void *
ice_malloc(struct ice_hw *hw, size_t len)
{
	void *p;

	assert(hw == &device);
	if (++allocations == fail_alloc)
		return (NULL);
	p = malloc(len);
	assert(p != NULL);
	live++;
	return (p);
}

static void
ice_free(struct ice_hw *hw, void *p)
{
	assert(hw == &device && p != NULL && live != 0);
	live--;
	free(p);
}

static void *
ice_memdup(struct ice_hw *hw, const void *src, size_t len, int kind)
{
	void *p;

	assert(kind == ICE_NONDMA_TO_NONDMA);
	assert(len <= sizeof (response.layer_props));
	copies++;
	p = ice_malloc(hw, len);
	if (p != NULL)
		(void) memcpy(p, src, len);
	return (p);
}

static int
ice_aq_query_sched_res(struct ice_hw *hw, u16 len,
    struct ice_aqc_query_txsched_res_resp *buf, void *details)
{
	assert(hw == &device && len == sizeof (*buf) && details == NULL);
	queries++;
	(void) memcpy(buf, &response, sizeof (*buf));
	return (query_status);
}

#include "ice_sched_resource_body.h"

static void
setup(u16 levels)
{
	unsigned i;

	assert(live == 0);
	(void) memset(&device, 0, sizeof (device));
	/* Invalid responses must not replace previously held scalar state. */
	device.num_tx_sched_layers = 9;
	device.num_tx_sched_phys_layers = 9;
	device.flattened_layers = 0xa5;
	device.max_cgds = 7;
	for (i = 0; i < ARRAY_SIZE(device.max_children); i++)
		device.max_children[i] = 42;
	(void) memset(&response, 0, sizeof (response));
	response.sched_props.logical_levels = CPU_TO_LE16(levels);
	response.sched_props.phys_levels = CPU_TO_LE16(9);
	response.sched_props.flattening_bitmap = 3;
	response.sched_props.max_pf_cgds = 4;
	for (i = 0; i < ARRAY_SIZE(response.layer_props) &&
	    i < levels; i++) {
		response.layer_props[i].logical_layer = i;
		response.layer_props[i].max_sibl_grp_sz =
		    CPU_TO_LE16(i + 1);
	}
	allocations = queries = copies = fail_alloc = 0;
	query_status = 0;
}

static void
expect_status(int wanted)
{
	int status = ice_sched_query_res_alloc(&device);

	if (status != wanted) {
		(void) fprintf(stderr, "scheduler query returned %d, "
		    "expected %d\n", status, wanted);
		exit(EXIT_FAILURE);
	}
}

static void
expect_rejected(void)
{
	struct ice_hw before;

	(void) memcpy(&before, &device, sizeof (before));
	expect_status(ICE_ERR_AQ_ERROR);
	assert(memcmp(&before, &device, sizeof (device)) == 0);
	assert(live == 0 && queries == 1 && copies == 0);
}

static void
invalid_levels(void)
{
	static const u16 levels[] = {
		1, 0, 2, 3, 4, 6, 7, 8, 10, 255, 256, 261, 265, UINT16_MAX
	};
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(levels); i++) {
		setup(levels[i]);
		expect_rejected();
	}
}

static void
zero_fanout(void)
{
	static const u16 levels[] = { 5, 9 };
	unsigned n, i;

	/* Child fanouts are sourced from layers 1 through levels - 1. */
	for (n = 0; n < ARRAY_SIZE(levels); n++) {
		for (i = 1; i < levels[n]; i++) {
			setup(levels[n]);
			response.layer_props[i].max_sibl_grp_sz = 0;
			expect_rejected();
		}
	}
}

static void
valid_responses(void)
{
	static const u16 levels[] = { 5, 9, 5, 9 };
	unsigned n, i;

	for (n = 0; n < ARRAY_SIZE(levels); n++) {
		setup(levels[n]);
		if (n >= 2) {
			for (i = 1; i < levels[n]; i++) {
				response.layer_props[i].max_sibl_grp_sz =
				    CPU_TO_LE16(1);
			}
		}
		expect_status(ICE_SUCCESS);
		assert(device.num_tx_sched_layers == levels[n]);
		assert(device.num_tx_sched_phys_layers == 9);
		assert(device.flattened_layers == 3 && device.max_cgds == 4);
		for (i = 0; i < levels[n] - 1U; i++)
			assert(device.max_children[i] == (n >= 2 ? 1 : i + 2));
		assert(device.max_children[levels[n] - 1] == 42);
		assert(device.layer_info != NULL);
		assert(memcmp(device.layer_info, response.layer_props,
		    levels[n] * sizeof (*device.layer_info)) == 0);
		assert(live == 1 && queries == 1 && copies == 1);
		ice_free(&device, device.layer_info);
		device.layer_info = NULL;
	}
}

static void
failures(void)
{
	setup(9);
	fail_alloc = 1;
	expect_status(ICE_ERR_NO_MEMORY);
	assert(live == 0 && queries == 0 && copies == 0);

	setup(9);
	query_status = ICE_ERR_AQ_TIMEOUT;
	expect_status(ICE_ERR_AQ_TIMEOUT);
	assert(live == 0 && queries == 1 && copies == 0);

	setup(9);
	fail_alloc = 2;
	expect_status(ICE_ERR_NO_MEMORY);
	assert(live == 0 && queries == 1 && copies == 1);
	assert(device.layer_info == NULL);
}

static void
cached(void)
{
	struct ice_hw before;

	setup(5);
	expect_status(ICE_SUCCESS);
	(void) memcpy(&before, &device, sizeof (before));
	/* An already accepted resource description suppresses another query. */
	(void) memset(&response, 0, sizeof (response));
	expect_status(ICE_SUCCESS);
	assert(memcmp(&before, &device, sizeof (device)) == 0);
	assert(live == 1 && queries == 1 && copies == 1);
	ice_free(&device, device.layer_info);
	device.layer_info = NULL;
}

int
main(int argc, char **argv)
{
	assert(argc == 2);
	if (strcmp(argv[1], "levels") == 0)
		invalid_levels();
	else if (strcmp(argv[1], "fanout") == 0)
		zero_fanout();
	else if (strcmp(argv[1], "valid") == 0)
		valid_responses();
	else if (strcmp(argv[1], "failures") == 0)
		failures();
	else if (strcmp(argv[1], "cached") == 0)
		cached();
	else
		return (EXIT_FAILURE);
	(void) printf("PASS: scheduler resources (%s)\n", argv[1]);
	return (EXIT_SUCCESS);
}

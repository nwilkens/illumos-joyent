/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define	B_FALSE 0
#define	B_TRUE 1
#define	ASSERT(n) assert(n)
#define	MUTEX_HELD(m) (*(m) != 0)
typedef int boolean_t;
typedef uint64_t u64;

#include "vsi_stats_types.h"

typedef struct ice ice_t;
struct ice_hw {
	ice_t *owner;
	boolean_t valid;
	uint16_t vsi_num;
};
struct ice {
	struct ice_hw ice_hw;
	struct ice_eth_stats ice_stat_vsi_cur, ice_stat_vsi_prev;
	boolean_t ice_stat_vsi_loaded;
	int ice_stat_lock;
	struct { uint16_t vi_handle; } ice_pf_vsi;
};

static unsigned lookups, reads, writes, repc_calls;
static uint32_t seen[10];

static boolean_t
ice_is_vsi_valid(struct ice_hw *hw, uint16_t handle)
{
	assert(handle == hw->owner->ice_pf_vsi.vi_handle);
	return (hw->valid);
}

static uint16_t
ice_get_hw_vsi_num(struct ice_hw *hw, uint16_t handle)
{
	assert(hw->valid && handle == hw->owner->ice_pf_vsi.vi_handle);
	lookups++;
	return (hw->vsi_num);
}

static void
ice_stat_update40(struct ice_hw *hw, uint32_t reg, boolean_t loaded,
    uint64_t *prev, uint64_t *cur)
{
	assert(MUTEX_HELD(&hw->owner->ice_stat_lock));
	assert(loaded == hw->owner->ice_stat_vsi_loaded);
	assert(reads < sizeof (seen) / sizeof (seen[0]));
	seen[reads++] = reg;
	(*prev)++;
	(*cur)++;
}

static void
ice_stat_update32(struct ice_hw *hw, uint32_t reg, boolean_t loaded,
    uint64_t *prev, uint64_t *cur)
{
	ice_stat_update40(hw, reg, loaded, prev, cur);
}

static void
ice_stat_update_repc(struct ice_hw *hw, uint16_t handle, boolean_t loaded,
    struct ice_eth_stats *cur)
{
	assert(MUTEX_HELD(&hw->owner->ice_stat_lock));
	assert(handle == hw->owner->ice_pf_vsi.vi_handle);
	assert(cur == &hw->owner->ice_stat_vsi_cur);
	/* The core reads after initialization and always clears by writing. */
	if (loaded)
		reads++;
	writes++;
	repc_calls++;
}

#include "vsi_stats_function.h"

static void
check(uint16_t number, boolean_t present, boolean_t loaded)
{
	ice_t ice = { 0 };
	struct ice_eth_stats saved_cur, saved_prev;
	const uint32_t expected[] = {
		GLV_GORCL(number), GLV_UPRCL(number), GLV_MPRCL(number),
		GLV_BPRCL(number), GLV_RDPC(number), GLV_GOTCL(number),
		GLV_UPTCL(number), GLV_MPTCL(number), GLV_BPTCL(number),
		GLV_TEPC(number)
	};

	ice.ice_hw.owner = &ice;
	ice.ice_hw.valid = present;
	ice.ice_hw.vsi_num = number;
	ice.ice_pf_vsi.vi_handle = 37;
	ice.ice_stat_vsi_loaded = loaded;
	ice.ice_stat_lock = 1;
	(void) memset(&ice.ice_stat_vsi_cur, 0x5a,
	    sizeof (ice.ice_stat_vsi_cur));
	(void) memset(&ice.ice_stat_vsi_prev, 0x6b,
	    sizeof (ice.ice_stat_vsi_prev));
	saved_cur = ice.ice_stat_vsi_cur;
	saved_prev = ice.ice_stat_vsi_prev;
	lookups = reads = writes = repc_calls = 0;

	ice_stats_update_vsi(&ice);

	assert(lookups == (present ? 1U : 0U));
	if (!present || number >= ICE_MAX_VSI) {
		assert(reads == 0 && writes == 0 && repc_calls == 0);
		assert(ice.ice_stat_vsi_loaded == loaded);
		assert(memcmp(&saved_cur, &ice.ice_stat_vsi_cur,
		    sizeof (saved_cur)) == 0);
		assert(memcmp(&saved_prev, &ice.ice_stat_vsi_prev,
		    sizeof (saved_prev)) == 0);
	} else {
		assert(reads == (loaded ? 11U : 10U));
		assert(writes == 1 && repc_calls == 1);
		assert(memcmp(seen, expected, sizeof (expected)) == 0);
		assert(ice.ice_stat_vsi_loaded == B_TRUE);
		assert(ice.ice_stat_vsi_cur.rx_bytes == saved_cur.rx_bytes + 1);
		assert(ice.ice_stat_vsi_prev.rx_bytes ==
		    saved_prev.rx_bytes + 1);
	}
	assert(MUTEX_HELD(&ice.ice_stat_lock));
}

int
main(int argc, char **argv)
{
	boolean_t present = B_TRUE;
	uint16_t number;

	assert(argc == 2);
	if (strcmp(argv[1], "zero") == 0) {
		number = 0;
	} else if (strcmp(argv[1], "last") == 0) {
		number = 767;
	} else if (strcmp(argv[1], "limit") == 0) {
		number = 768;
	} else if (strcmp(argv[1], "maximum") == 0) {
		number = UINT16_MAX;
	} else {
		assert(strcmp(argv[1], "missing") == 0);
		number = 0;
		present = B_FALSE;
	}
	check(number, present, B_FALSE);
	check(number, present, B_TRUE);
	(void) printf("VSI statistics bounds: PASS (%s, initial and loaded)\n",
	    argv[1]);
	return (0);
}

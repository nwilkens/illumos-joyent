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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef bzero

#define	CHECK(x)	assert(x)
#define	BIT(n)		(1U << (n))
#define	BIT_ULL(n)	(UINT64_C(1) << (n))
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))
#define	ASSERT3U(a, op, b)	CHECK((a) op(b))
#define	bzero(p, n)	((void) memset((p), 0, (n)))
#define	B_FALSE		false
#define	B_TRUE		true
#define	ICE_SUCCESS	0
#define	ICE_ERR_CFG	(-3)
#define	KM_SLEEP	1

typedef bool boolean_t;
typedef unsigned int uint_t;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#include "ice_control_types.h"

struct ice_port_info { uint8_t lport; };
struct ice_hw {
	struct ice_port_info *port_info;
	struct { struct { uint16_t rss_table_size; } common_cap; } func_caps;
};
typedef struct ice_vsi {
	uint16_t vi_handle;
	uint16_t vi_nrxq;
	boolean_t vi_rss_set;	/* supports pre-cleanup source selections */
} ice_vsi_t;
typedef struct ice {
	struct ice_hw ice_hw;
	ice_vsi_t ice_pf_vsi;
	boolean_t ice_safe_mode;
	unsigned int ice_lse_lock, ice_lse_flags;
} ice_t;

/* Legacy source writes this unused flag; no assertion depends on its value. */
#define	ICE_LSE_F_ENABLE	BIT(0)
#define	mutex_enter(p)	do { CHECK(*(p) == 0); *(p) = 1; } while (0)
#define	mutex_exit(p)	do { CHECK(*(p) == 1); *(p) = 0; } while (0)

static ice_t ice;
static struct ice_port_info port;
static unsigned int events, randoms, keys, luts, flows, errors, allocations;
static unsigned int frees;
static int event_status, key_status, lut_status, fail_flow;
static uint8_t *allocated_lut;
static size_t allocated_size;

static void
ice_error(ice_t *instance, const char *format, ...)
{
	CHECK(instance == &ice && format != NULL);
	errors++;
}

static int
ice_aq_set_event_mask(struct ice_hw *hw, uint8_t lport, uint16_t mask,
    void *details)
{
	CHECK(hw == &ice.ice_hw && lport == port.lport && details == NULL);
	CHECK(ice.ice_lse_lock == 0);
	/*
	 * Defined bits 1..12, with up/down, media and qualification delivered.
	 */
	CHECK(mask == 0x1ef8);
	events++;
	return (event_status);
}

static int
random_get_pseudo_bytes(uint8_t *buffer, size_t size)
{
	size_t i;

	CHECK(size == sizeof (struct ice_aqc_get_set_rss_keys));
	CHECK(keys == 0 && luts == 0 && flows == 0);
	for (i = 0; i < size; i++)
		buffer[i] = (uint8_t)(i + 1);
	randoms++;
	return (0);
}

static int
ice_aq_set_rss_key(struct ice_hw *hw, uint16_t handle,
    struct ice_aqc_get_set_rss_keys *key)
{
	const uint8_t *bytes = (const uint8_t *)key;
	size_t i;

	CHECK(hw == &ice.ice_hw && handle == ice.ice_pf_vsi.vi_handle);
	CHECK(randoms == 1 && luts == 0 && flows == 0);
	for (i = 0; i < sizeof (*key); i++)
		CHECK(bytes[i] == (uint8_t)(i + 1));
	keys++;
	return (key_status);
}

static void *
kmem_zalloc(size_t size, int flag)
{
	CHECK(flag == KM_SLEEP && allocated_lut == NULL);
	CHECK(size == ice.ice_hw.func_caps.common_cap.rss_table_size);
	CHECK(keys == 1 && key_status == 0);
	allocated_lut = calloc(1, size);
	CHECK(allocated_lut != NULL);
	allocated_size = size;
	allocations++;
	return (allocated_lut);
}

static void
kmem_free(void *buffer, size_t size)
{
	CHECK(buffer == allocated_lut && size == allocated_size);
	CHECK(luts == 1 && flows == 0);
	free(buffer);
	allocated_lut = NULL;
	frees++;
}

static int
ice_aq_set_rss_lut(struct ice_hw *hw,
    struct ice_aq_get_set_rss_lut_params *params)
{
	unsigned int i;

	CHECK(hw == &ice.ice_hw);
	CHECK(params->vsi_handle == ice.ice_pf_vsi.vi_handle);
	CHECK(params->lut_type == ICE_LUT_PF && params->global_lut_id == 0);
	CHECK(params->lut == allocated_lut &&
	    params->lut_size == allocated_size);
	CHECK(keys == 1 && luts == 0 && flows == 0);
	for (i = 0; i < params->lut_size; i++)
		CHECK(params->lut[i] == i % ice.ice_pf_vsi.vi_nrxq);
	luts++;
	return (lut_status);
}

static int
ice_add_rss_cfg(struct ice_hw *hw, uint16_t handle,
    const struct ice_rss_hash_cfg *cfg)
{
	static const uint64_t hashes[] = {
		ICE_HASH_TCP_IPV4, ICE_HASH_UDP_IPV4,
		ICE_HASH_TCP_IPV6, ICE_HASH_UDP_IPV6,
		ICE_FLOW_HASH_IPV4, ICE_FLOW_HASH_IPV6
	};
	static const uint32_t headers[] = {
		ICE_FLOW_SEG_HDR_IPV4 | ICE_FLOW_SEG_HDR_TCP,
		ICE_FLOW_SEG_HDR_IPV4 | ICE_FLOW_SEG_HDR_UDP,
		ICE_FLOW_SEG_HDR_IPV6 | ICE_FLOW_SEG_HDR_TCP,
		ICE_FLOW_SEG_HDR_IPV6 | ICE_FLOW_SEG_HDR_UDP,
		ICE_FLOW_SEG_HDR_IPV4, ICE_FLOW_SEG_HDR_IPV6
	};
	unsigned int index = flows++;

	CHECK(hw == &ice.ice_hw && handle == ice.ice_pf_vsi.vi_handle);
	CHECK(index < ARRAY_SIZE(hashes));
	CHECK(cfg->hash_flds == hashes[index] &&
	    cfg->addl_hdrs == headers[index]);
	CHECK(cfg->hdr_type == ICE_RSS_OUTER_HEADERS && !cfg->symm);
	CHECK(keys == 1 && luts == 1 && lut_status == 0);
	CHECK(allocated_lut == NULL && frees == 1);
	return ((int)index == fail_flow ? -29 : ICE_SUCCESS);
}

#include "ice_control_bodies.h"

static void
reset(void)
{
	CHECK(allocated_lut == NULL);
	memset(&ice, 0, sizeof (ice));
	port.lport = 7;
	ice.ice_hw.port_info = &port;
	ice.ice_hw.func_caps.common_cap.rss_table_size = 128;
	ice.ice_pf_vsi.vi_handle = 17;
	ice.ice_pf_vsi.vi_nrxq = 4;
	events = randoms = keys = luts = flows = errors = 0;
	allocations = frees = 0;
	event_status = key_status = lut_status = 0;
	fail_flow = -1;
}

static void
link_events(void)
{
	reset();
	ice.ice_hw.port_info = NULL;
	CHECK(ice_set_link_events(&ice) == B_FALSE);
	CHECK(events == 0 && errors == 0);

	reset();
	CHECK(ice_set_link_events(&ice) == B_TRUE);
	CHECK(events == 1 && errors == 0);

	reset();
	event_status = -23;
	CHECK(ice_set_link_events(&ice) == B_FALSE);
	CHECK(events == 1 && errors == 1);
}

static void
rss_failures(void)
{
	unsigned int i;
	const uint16_t invalid_sizes[] = { 0, ICE_LUT_PF_SIZE + 1 };

	reset();
	ice.ice_safe_mode = B_TRUE;
	ice.ice_hw.func_caps.common_cap.rss_table_size = 0;
	CHECK(ice_rss_setup(&ice) == ICE_SUCCESS);
	CHECK(randoms == 0 && keys == 0 && luts == 0 && flows == 0);
	CHECK(errors == 0 && allocations == 0);

	for (i = 0; i < ARRAY_SIZE(invalid_sizes); i++) {
		reset();
		ice.ice_hw.func_caps.common_cap.rss_table_size =
		    invalid_sizes[i];
		CHECK(ice_rss_setup(&ice) == ICE_ERR_CFG);
		CHECK(randoms == 0 && keys == 0 && luts == 0 && flows == 0);
		CHECK(errors == 1 && allocations == 0);
	}

	reset();
	key_status = -19;
	CHECK(ice_rss_setup(&ice) == key_status);
	CHECK(keys == 1 && luts == 0 && flows == 0 && errors == 1);
	CHECK(allocations == 0);

	reset();
	lut_status = -27;
	CHECK(ice_rss_setup(&ice) == lut_status);
	CHECK(keys == 1 && luts == 1 && flows == 0 && errors == 1);
	CHECK(allocations == 1 && frees == 1 && allocated_lut == NULL);

	for (i = 0; i < 6; i++) {
		reset();
		fail_flow = (int)i;
		CHECK(ice_rss_setup(&ice) == -29);
		CHECK(flows == i + 1 && errors == 1);
		CHECK(allocations == 1 && frees == 1 && allocated_lut == NULL);
	}
}

static void
rss_success(void)
{
	unsigned int i;
	static const struct { uint16_t size, queues; } cases[] = {
		{ 1, 1 }, { 128, 4 }, { ICE_LUT_PF_SIZE, 16 }
	};

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		reset();
		ice.ice_hw.func_caps.common_cap.rss_table_size = cases[i].size;
		ice.ice_pf_vsi.vi_nrxq = cases[i].queues;
		CHECK(ice_rss_setup(&ice) == ICE_SUCCESS);
		CHECK(randoms == 1 && keys == 1 && luts == 1 && flows == 6);
		CHECK(errors == 0 && allocations == 1 && frees == 1);
		CHECK(allocated_lut == NULL);
	}
}

int
main(void)
{
	link_events();
	rss_failures();
	rss_success();
	(void) puts("PASS: ICE link-event and RSS setup (17 scenarios)");
	return (0);
}

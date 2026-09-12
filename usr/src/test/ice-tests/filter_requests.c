/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/* Reuse the MAC callback fixtures and run their existing scenarios too. */
#define	main terminal_filters_main
#include "terminal_filters.c"
#undef main

#include <stdbool.h>

#define	ICE_ERR_PARAM -2
#define	ICE_ERR_CFG -3
#define	IS_ZERO_ETHER_ADDR(a) (memcmp((a), zero_addr, ETHERADDRL) == 0)

typedef unsigned int uint_t;
struct ice_vsi_ctx { int unused; };
static struct ice_vsi_ctx context;
static const uint8_t zero_addr[ETHERADDRL];
static unsigned int replay_prepares, replay_finishes, vsi_frees;

static ice_mac_filter_t *
list_remove_head(list_t *list)
{
	ice_mac_filter_t *entry = list->head;

	if (entry != NULL)
		list_remove(list, entry);
	return (entry);
}

static struct ice_vsi_ctx *
ice_get_vsi_ctx(struct ice_hw *hw, uint16_t handle)
{
	CHECK(handle == hw->owner->ice_pf_vsi.vi_handle);
	return (&context);
}

static int
ice_free_vsi(struct ice_hw *hw, uint16_t handle, struct ice_vsi_ctx *ctx,
    bool keep, void *details)
{
	CHECK(handle == hw->owner->ice_pf_vsi.vi_handle);
	CHECK(ctx == &context && !keep && details == NULL);
	vsi_frees++;
	return (0);
}

static int
ice_vsi_setup(ice_t *ice)
{
	CHECK(MUTEX_HELD(&ice->ice_rebuild_lock));
	ice->ice_pf_vsi.vi_added = B_TRUE;
	return (0);
}

static int
ice_replay_pre_init(struct ice_hw *hw, void *info)
{
	CHECK(MUTEX_HELD(&hw->owner->ice_rebuild_lock));
	CHECK(info == hw->switch_info);
	replay_prepares++;
	return (0);
}

static void
ice_rm_all_sw_replay_rule_info(struct ice_hw *hw)
{
	CHECK(MUTEX_HELD(&hw->owner->ice_rebuild_lock));
	replay_finishes++;
}

static int
ice_rss_setup(ice_t *ice)
{
	CHECK(MUTEX_HELD(&ice->ice_rebuild_lock));
	return (0);
}

#include "ice_vsi_filter_bodies.h"

static void
check_entry(const struct ice_fltr_list_entry *entry, const uint8_t *addr)
{
	CHECK(entry->status == 0);
	CHECK(entry->fltr_info.flag == ICE_FLTR_TX);
	CHECK(entry->fltr_info.lkup_type == ICE_SW_LKUP_MAC);
	CHECK(entry->fltr_info.fltr_act == ICE_FWD_TO_VSI);
	CHECK(entry->fltr_info.vsi_handle == 37);
	CHECK(entry->fltr_info.src_id == ICE_SRC_ID_VSI);
	CHECK(memcmp(entry->fltr_info.l_data.mac.mac_addr, addr,
	    ETHERADDRL) == 0);
}

static const struct ice_fltr_list_entry *
request_entry(unsigned int request, const uint8_t *addr)
{
	unsigned int i;

	CHECK(request < nrequests);
	for (i = 0; i < requests[request].count; i++) {
		const struct ice_fltr_list_entry *entry =
		    &requests[request].entries[i];

		if (memcmp(entry->fltr_info.l_data.mac.mac_addr,
		    addr, ETHERADDRL) == 0) {
			check_entry(entry, addr);
			return (entry);
		}
	}
	CHECK(!"missing MAC request");
	return (NULL);
}

static void
same_request(const struct ice_fltr_list_entry *a,
    const struct ice_fltr_list_entry *b)
{
	CHECK(memcmp(&a->fltr_info, &b->fltr_info,
	    sizeof (a->fltr_info)) == 0);
}

static void
lifecycle_requests(void)
{
	ice_t ice;
	struct ice_port_info port;
	struct ice_fltr_list_entry gld_uc, gld_mc, attach_bc;
	ice_mac_filter_t *first;

	init(&ice);
	ice.ice_pf_vsi.vi_handle = 37;
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == 0);
	CHECK(nrequests == 2 && requests[0].add && requests[1].add);
	gld_uc = *request_entry(0, unicast);
	gld_mc = *request_entry(1, multicast);
	CHECK(ice_group_remove_mac(&ice, unicast) == 0);
	CHECK(ice_m_multicst(&ice, B_FALSE, multicast) == 0);
	CHECK(nrequests == 4 && !requests[2].add && !requests[3].add);
	same_request(&gld_uc, request_entry(2, unicast));
	same_request(&gld_mc, request_entry(3, multicast));
	finish(&ice);

	init(&ice);
	ice.ice_pf_vsi.vi_handle = 37;
	(void) memcpy(port.mac.perm_addr, unicast, ETHERADDRL);
	ice.ice_hw.port_info = &port;
	require_rebuild_lock = B_FALSE;
	CHECK(ice_add_mac_filters(&ice) == 0);
	CHECK(nrequests == 1 && requests[0].count == 2 && requests[0].add);
	same_request(&gld_uc, request_entry(0, unicast));
	attach_bc = *request_entry(0, ice_bcast_addr);
	CHECK(allocations == 2);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == 0);
	CHECK(allocations == 3);
	first = ice.ice_pf_vsi.vi_macs.head;

	require_rebuild_lock = B_TRUE;
	mutex_enter(&ice.ice_rebuild_lock);
	CHECK(ice_vsi_rebuild(&ice) == 0);
	mutex_exit(&ice.ice_rebuild_lock);
	CHECK(nrequests == 3 && requests[2].add && requests[2].count == 3);
	CHECK(allocations == 3 && ice.ice_pf_vsi.vi_macs.head == first);
	CHECK(replay_prepares == 1 && replay_finishes == 1);
	same_request(&gld_uc, request_entry(2, unicast));
	same_request(&gld_mc, request_entry(2, multicast));
	same_request(&attach_bc, request_entry(2, ice_bcast_addr));

	require_rebuild_lock = B_FALSE;
	ice_vsi_teardown(&ice);
	CHECK(nrequests == 4 && !requests[3].add && requests[3].count == 3);
	same_request(&gld_uc, request_entry(3, unicast));
	same_request(&gld_mc, request_entry(3, multicast));
	same_request(&attach_bc, request_entry(3, ice_bcast_addr));
	CHECK(vsi_frees == 1 && !ice.ice_pf_vsi.vi_added);
	finish(&ice);
}

static void
attach_rollback(void)
{
	ice_t ice;
	struct ice_port_info port;

	init(&ice);
	ice.ice_pf_vsi.vi_handle = 37;
	(void) memcpy(port.mac.perm_addr, unicast, ETHERADDRL);
	ice.ice_hw.port_info = &port;
	ice.ice_hw.result = -1;
	require_rebuild_lock = B_FALSE;
	CHECK(ice_add_mac_filters(&ice) == -1);
	CHECK(nrequests == 2 && requests[0].add && !requests[1].add);
	CHECK(requests[0].count == 2 && requests[1].count == 2);
	same_request(request_entry(0, unicast), request_entry(1, unicast));
	same_request(request_entry(0, ice_bcast_addr),
	    request_entry(1, ice_bcast_addr));
	finish(&ice);
}

int
main(void)
{
	CHECK(terminal_filters_main() == EXIT_SUCCESS);
	lifecycle_requests();
	attach_rollback();
	(void) puts("MAC request construction: PASS (lifecycle and rollback)");
	return (EXIT_SUCCESS);
}

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
#include <stddef.h>

#define	ICE_ERR_PARAM -2
#define	ICE_ERR_CFG -3
#define	ICE_ERR_DOES_NOT_EXIST -4
#define	ICE_MAX_TRAFFIC_CLASS 8
#define	ICE_MAX_VSI 768
#define	ICE_PF_VSI_HANDLE 0
#define	MUTEX_DRIVER 0
#define	BIT(n) (1U << (n))
#define	IS_ZERO_ETHER_ADDR(a) (memcmp((a), zero_addr, ETHERADDRL) == 0)

typedef unsigned int uint_t;
struct ice_vsi_ctx { struct { unsigned int flags; } info; };
static struct ice_vsi_ctx context;
static const uint8_t zero_addr[ETHERADDRL];
static unsigned int replay_prepares, replay_finishes, vsi_frees;
static unsigned int list_destroys, lock_destroys;
static enum {
	SETUP_OK, SETUP_CTX_FILL, SETUP_ADD, SETUP_INVALID,
	SETUP_NUMBER, SETUP_CONTEXT, SETUP_SCHEDULER, SETUP_RSS
} setup_failure;

static void
mutex_init(kmutex_t *lock, void *name, int type, void *cookie)
{
	CHECK(name == NULL && type == MUTEX_DRIVER && cookie == NULL);
	*lock = 0;
}

static void
mutex_destroy(kmutex_t *lock)
{
	CHECK(!MUTEX_HELD(lock));
	lock_destroys++;
}

static void
list_create(list_t *list, size_t size, size_t offset)
{
	CHECK(size == sizeof (ice_mac_filter_t));
	CHECK(offset == offsetof(ice_mac_filter_t, imf_node));
	list->head = list->tail = NULL;
}

static void
list_destroy(list_t *list)
{
	CHECK(list->head == NULL && list->tail == NULL);
	list_destroys++;
}

static int
ice_vsi_ctx_fill(ice_t *ice, struct ice_vsi_ctx *ctx)
{
	CHECK(ice->ice_pf_vsi.vi_nrxq == ice->ice_nqueues);
	CHECK(ice->ice_pf_vsi.vi_ntxq == ice->ice_nqueues);
	bzero(ctx, sizeof (*ctx));
	ctx->info.flags = 17;
	return (setup_failure == SETUP_CTX_FILL ? -1 : ICE_SUCCESS);
}

static int
ice_add_vsi(struct ice_hw *hw, uint16_t handle, struct ice_vsi_ctx *ctx,
    void *details)
{
	CHECK(handle == ICE_PF_VSI_HANDLE && ctx->info.flags == 17);
	CHECK(!MUTEX_HELD(&hw->owner->ice_pf_vsi.vi_mac_lock));
	CHECK(details == NULL);
	return (setup_failure == SETUP_ADD ? -1 : ICE_SUCCESS);
}

static boolean_t
ice_is_vsi_valid(struct ice_hw *hw, uint16_t handle)
{
	CHECK(handle == hw->owner->ice_pf_vsi.vi_handle);
	return (setup_failure != SETUP_INVALID);
}

static uint16_t
ice_get_hw_vsi_num(struct ice_hw *hw, uint16_t handle)
{
	CHECK(handle == hw->owner->ice_pf_vsi.vi_handle);
	return (setup_failure == SETUP_NUMBER ? ICE_MAX_VSI : 11);
}

static int
ice_cfg_vsi_lan(struct ice_port_info *port, uint16_t handle, unsigned int tcs,
    uint16_t *max_lanqs)
{
	unsigned int i;

	CHECK(port != NULL && handle == ICE_PF_VSI_HANDLE && tcs == BIT(0));
	CHECK(context.info.flags == 17 && max_lanqs[0] == 1);
	for (i = 1; i < ICE_MAX_TRAFFIC_CLASS; i++)
		CHECK(max_lanqs[i] == 0);
	return (setup_failure == SETUP_SCHEDULER ? -1 : ICE_SUCCESS);
}

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
	return (setup_failure == SETUP_CONTEXT ? NULL : &context);
}

static int
ice_free_vsi(struct ice_hw *hw, uint16_t handle, struct ice_vsi_ctx *ctx,
    bool keep, void *details)
{
	CHECK(handle == hw->owner->ice_pf_vsi.vi_handle);
	CHECK(ctx == (setup_failure == SETUP_CONTEXT ? NULL : &context));
	CHECK(!keep && details == NULL);
	vsi_frees++;
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
	if (require_rebuild_lock)
		CHECK(MUTEX_HELD(&ice->ice_rebuild_lock));
	return (setup_failure == SETUP_RSS ? -1 : ICE_SUCCESS);
}

#include "ice_vsi_filter_bodies.h"

static void
check_entry(const struct ice_fltr_list_entry *entry, const uint8_t *addr,
    uint16_t handle)
{
	CHECK(entry->status == 0);
	CHECK(entry->fltr_info.flag == ICE_FLTR_TX);
	CHECK(entry->fltr_info.lkup_type == ICE_SW_LKUP_MAC);
	CHECK(entry->fltr_info.fltr_act == ICE_FWD_TO_VSI);
	CHECK(entry->fltr_info.vsi_handle == handle);
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
			check_entry(entry, addr, ICE_PF_VSI_HANDLE);
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
	struct ice_fltr_list_entry gld_uc, gld_mc, attach_bc, nonzero;
	ice_mac_filter_t *first;

	/* Retain coverage of the constructor's handle argument. */
	(void) memset(&nonzero, 0xa5, sizeof (nonzero));
	ice_fltr_entry_init(&nonzero, 37, unicast);
	check_entry(&nonzero, unicast, 37);
	CHECK(nonzero.list_entry.entry == NULL);

	init(&ice);
	ice.ice_pf_vsi.vi_handle = ICE_PF_VSI_HANDLE;
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
	ice.ice_pf_vsi.vi_handle = ICE_PF_VSI_HANDLE;
	(void) memcpy(port.mac.perm_addr, unicast, ETHERADDRL);
	ice.ice_hw.port_info = &port;
	ice.ice_nqueues = 1;
	require_rebuild_lock = B_FALSE;
	CHECK(ice_filters_setup(&ice) == 0);
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
	ice.ice_pf_vsi.vi_handle = ICE_PF_VSI_HANDLE;
	(void) memcpy(port.mac.perm_addr, unicast, ETHERADDRL);
	ice.ice_hw.port_info = &port;
	ice.ice_nqueues = 1;
	ice.ice_hw.result = -1;
	require_rebuild_lock = B_FALSE;
	CHECK(ice_filters_setup(&ice) == -1);
	CHECK(nrequests == 2 && requests[0].add && !requests[1].add);
	CHECK(requests[0].count == 2 && requests[1].count == 2);
	same_request(request_entry(0, unicast), request_entry(1, unicast));
	same_request(request_entry(0, ice_bcast_addr),
	    request_entry(1, ice_bcast_addr));
	finish(&ice);
}

static void
rebuild_failure(int failure)
{
	ice_t ice;
	struct ice_port_info port;
	ice_mac_filter_t *first;
	int status;

	init(&ice);
	ice.ice_hw.port_info = &port;
	ice.ice_nqueues = 1;
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == 0);
	first = ice.ice_pf_vsi.vi_macs.head;
	CHECK(allocations == 2 && nrequests == 2);
	setup_failure = failure;
	mutex_enter(&ice.ice_rebuild_lock);
	status = ice_vsi_rebuild(&ice);
	mutex_exit(&ice.ice_rebuild_lock);
	CHECK(status == (failure == SETUP_CONTEXT ? ICE_ERR_DOES_NOT_EXIST :
	    failure == SETUP_SCHEDULER ? -1 : ICE_ERR_PARAM));
	CHECK(ice.ice_pf_vsi.vi_macs.head == first && allocations == 2);
	CHECK(nrequests == 2 && vsi_frees == 0 && ice.ice_pf_vsi.vi_added);
	CHECK(replay_prepares == 0 && replay_finishes == 0);

	/* Publish terminal failure before MAC retires its clients. */
	ice.ice_state = ICE_STATE_RESET_FAILED | ICE_STATE_ERROR;
	ice.ice_hw.result = -1;
	CHECK(ice_group_remove_mac(&ice, unicast) == 0);
	CHECK(ice_m_multicst(&ice, B_FALSE, multicast) == 0);
	CHECK(allocations == 0 && nrequests == 2);
	require_rebuild_lock = B_FALSE;
	ice_vsi_teardown(&ice);
	CHECK(vsi_frees == 1 && !ice.ice_pf_vsi.vi_added);
	finish(&ice);
}

static void
attach_failures(void)
{
	ice_t ice;
	struct ice_port_info port;
	int failure;

	for (failure = SETUP_CTX_FILL; failure <= SETUP_RSS; failure++) {
		init(&ice);
		(void) memcpy(port.mac.perm_addr, unicast, ETHERADDRL);
		ice.ice_hw.port_info = &port;
		ice.ice_nqueues = 1;
		require_rebuild_lock = B_FALSE;
		setup_failure = failure;
		vsi_frees = list_destroys = lock_destroys = 0;
		CHECK(!ice_vsi_init(&ice));
		CHECK(!ice.ice_pf_vsi.vi_added);
		CHECK(vsi_frees == (failure >= SETUP_INVALID ? 1U : 0U));
		CHECK(list_destroys == 1 && lock_destroys == 1);
		if (failure == SETUP_RSS) {
			CHECK(nrequests == 2 && requests[0].add &&
			    !requests[1].add);
			CHECK(requests[0].count == 2 && requests[1].count == 2);
			same_request(request_entry(0, unicast),
			    request_entry(1, unicast));
			same_request(request_entry(0, ice_bcast_addr),
			    request_entry(1, ice_bcast_addr));
		} else {
			CHECK(nrequests == 0);
		}
		finish(&ice);
	}
}

static void
recovery_replay(void)
{
	const uint8_t rejected[ETHERADDRL] = { 2, 0, 0, 0, 0, 2 };
	ice_t ice;
	struct ice_port_info port;
	unsigned int before, sets, scenario;

	for (scenario = 0; scenario < 4; scenario++) {
		init(&ice);
		ice.ice_hw.port_info = &port;
		ice.ice_nqueues = 1;
		setup_failure = SETUP_OK;
		replay_prepares = replay_finishes = 0;
		CHECK(ice_group_add_mac(&ice, unicast) == 0);
		CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == 0);
		if (scenario == 0) {
			ice.ice_hw.result = -1;
			CHECK(ice_group_add_mac(&ice, rejected) == EIO);
			/* Retire another client while that reset is owed. */
			CHECK(ice_group_remove_mac(&ice, unicast) == 0);
		} else if (scenario == 1) {
			ice.ice_hw.result = -1;
			CHECK(ice_m_multicst(&ice, B_FALSE, multicast) == 0);
		} else if (scenario == 2) {
			ice.ice_hw.result = -1;
			CHECK(ice_m_promisc(&ice, B_TRUE) == EIO);
		} else {
			CHECK(ice_m_promisc(&ice, B_TRUE) == 0);
			ice.ice_hw.result = -1;
			CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
		}
		check_recovery(&ice);
		CHECK(!ice.ice_promisc_on);
		before = nrequests;
		sets = ice.ice_hw.promisc_sets;
		ice.ice_hw.result = ICE_SUCCESS;
		/* The worker claims its cause before reconstructing the VSI. */
		ice.ice_state = ICE_STATE_ERROR;
		mutex_enter(&ice.ice_rebuild_lock);
		CHECK(ice_vsi_rebuild(&ice) == ICE_SUCCESS);
		mutex_exit(&ice.ice_rebuild_lock);
		CHECK(replay_prepares == 1 && replay_finishes == 1);
		CHECK(nrequests == before + 1 && requests[before].add);
		CHECK(requests[before].count == (scenario < 2 ? 1U : 2U));
		if (scenario != 0)
			(void) request_entry(before, unicast);
		if (scenario != 1)
			(void) request_entry(before, multicast);
		CHECK(ice.ice_hw.promisc_sets == sets);
		CHECK(!ice.ice_promisc_on);
		/* A completed reset admits later ownership again. */
		ice.ice_state = 0;
		CHECK(ice_group_add_mac(&ice, rejected) == 0);
		mutex_enter(&ice.ice_rebuild_lock);
		ice_vsi_teardown(&ice);
		mutex_exit(&ice.ice_rebuild_lock);
		finish(&ice);
	}
}

int
main(int argc, char **argv)
{
	CHECK(argc == 2);
	if (strcmp(argv[1], "requests") == 0) {
		CHECK(terminal_filters_main() == EXIT_SUCCESS);
		lifecycle_requests();
		attach_rollback();
	} else if (strcmp(argv[1], "rebuild_invalid") == 0) {
		rebuild_failure(SETUP_INVALID);
	} else if (strcmp(argv[1], "rebuild_number") == 0) {
		rebuild_failure(SETUP_NUMBER);
	} else if (strcmp(argv[1], "rebuild_context") == 0) {
		rebuild_failure(SETUP_CONTEXT);
	} else if (strcmp(argv[1], "rebuild_scheduler") == 0) {
		rebuild_failure(SETUP_SCHEDULER);
	} else if (strcmp(argv[1], "attach_failures") == 0) {
		attach_failures();
	} else if (strcmp(argv[1], "recovery_replay") == 0) {
		recovery_replay();
	} else {
		CHECK(!"unknown scenario");
	}
	(void) printf("MAC filter lifecycle: PASS (%s)\n", argv[1]);
	return (EXIT_SUCCESS);
}

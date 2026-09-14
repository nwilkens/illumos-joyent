/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Host-side boundary stubs for the unchanged callbacks extracted by
 * terminal_filters.py.  The tests execute the driver's list ownership and
 * error handling; they do not model firmware, DMA, or concurrent kernel work.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Some hosts supply fortified macros for these non-C99 interfaces. */
#undef bzero
#undef bcopy
#undef bcmp

#define	ETHERADDRL	6
#define	B_FALSE		0
#define	B_TRUE		1
#define	KM_SLEEP	0
#define	ICE_SUCCESS	0
#define	DDI_SERVICE_LOST	1
#define	LINK_STATE_DOWN	0
#define	ASSERT(x)	assert(x)
#define	MUTEX_HELD(m)	(*(m) != 0)
#define	bzero(p, n)	((void) memset((p), 0, (n)))
#define	bcopy(s, d, n)	((void) memcpy((d), (s), (n)))
#define	bcmp(a, b, n)	memcmp((a), (b), (n))

typedef int boolean_t;
typedef int kmutex_t;
typedef struct ice_mac_filter ice_mac_filter_t;
struct ice_mac_filter {
	ice_mac_filter_t *prev;
	ice_mac_filter_t *next;
	uint8_t imf_addr[ETHERADDRL];
	int imf_node;
};
typedef struct list {
	ice_mac_filter_t *head;
	ice_mac_filter_t *tail;
} list_t;
typedef struct ice_vsi {
	uint16_t vi_handle;
	uint16_t vi_nrxq, vi_ntxq, vi_hw_num;
	kmutex_t vi_mac_lock;
	list_t vi_macs;
	boolean_t vi_added;
} ice_vsi_t;
typedef struct ice ice_t;
struct ice_port_info {
	struct { uint8_t perm_addr[ETHERADDRL]; } mac;
};
struct ice_hw {
	ice_t *owner;
	int result, clear_result, last_status;
	boolean_t override_clear;
	unsigned int promisc_sets, promisc_clears;
	unsigned int calls;
	struct ice_port_info *port_info;
	void *switch_info;
};
struct ice {
	struct ice_hw ice_hw;
	ice_vsi_t ice_pf_vsi;
	kmutex_t ice_rebuild_lock;
	uint32_t ice_state;
	boolean_t ice_promisc_on;
	unsigned int ice_nqueues;
	void *ice_dip;
	unsigned int recoveries, service_lost, link_down;
};

struct LIST_HEAD_TYPE {
	struct LIST_HEAD_TYPE *entry;
};
struct ice_fltr_list_entry {
	struct LIST_HEAD_TYPE list_entry;
	int status;
	struct {
		int flag, lkup_type, fltr_act, src_id;
		uint16_t vsi_handle;
		union {
			struct {
				uint8_t mac_addr[ETHERADDRL];
			} mac;
		} l_data;
	} fltr_info;
};
enum {
	ICE_FLTR_TX = 2, ICE_SW_LKUP_MAC = 1,
	ICE_FWD_TO_VSI = 0, ICE_SRC_ID_VSI = 1
};
#define	INIT_LIST_HEAD(h)	((h)->entry = NULL)
#define	LIST_ADD(e, h) do {		\
	(e)->entry = (h)->entry;		\
	(h)->entry = (e);		\
} while (0)

enum {
	ICE_PROMISC_UCAST_RX, ICE_PROMISC_UCAST_TX,
	ICE_PROMISC_MCAST_RX, ICE_PROMISC_MCAST_TX, ICE_PROMISC_MAX
};
#define	ice_declare_bitmap(n, bits)	uint32_t n
#define	ice_zero_bitmap(n, bits)		((n) = 0)
#define	ice_set_bit(bit, n)		((n) |= 1U << (bit))

static unsigned int allocations;
static boolean_t require_rebuild_lock = B_TRUE;
static struct {
	boolean_t add;
	unsigned int count;
	struct ice_fltr_list_entry entries[8];
} requests[8];
static unsigned int nrequests;
static ice_t *latch_ice;
static uint32_t latch_state;


static void
mutex_enter(kmutex_t *lock)
{
	assert(!MUTEX_HELD(lock));
	*lock = 1;
}

static void
mutex_exit(kmutex_t *lock)
{
	assert(MUTEX_HELD(lock));
	*lock = 0;
	if (latch_ice != NULL &&
	    lock == &latch_ice->ice_pf_vsi.vi_mac_lock) {
		latch_ice->ice_state |= latch_state;
		latch_ice = NULL;
	}
}

static ice_mac_filter_t *
list_head(list_t *list)
{
	return (list->head);
}

static ice_mac_filter_t *
list_next(list_t *list, ice_mac_filter_t *entry)
{
	(void) list;
	return (entry->next);
}

static void
list_insert_tail(list_t *list, ice_mac_filter_t *entry)
{
	entry->prev = list->tail;
	entry->next = NULL;
	if (list->tail != NULL)
		list->tail->next = entry;
	else
		list->head = entry;
	list->tail = entry;
}

static void
list_remove(list_t *list, ice_mac_filter_t *entry)
{
	if (entry->prev != NULL)
		entry->prev->next = entry->next;
	else
		list->head = entry->next;
	if (entry->next != NULL)
		entry->next->prev = entry->prev;
	else
		list->tail = entry->prev;
}

static void *
kmem_zalloc(size_t size, int flags)
{
	void *p = calloc(1, size);

	(void) flags;
	assert(p != NULL);
	allocations++;
	return (p);
}

static void
kmem_free(void *p, size_t size)
{
	(void) size;
	assert(allocations != 0);
	allocations--;
	free(p);
}

static void
ice_error(ice_t *ice, const char *format, ...)
{
	(void) ice;
	(void) format;
}

static int
ice_status_to_errno(ice_t *ice, int status)
{
	assert(status != ICE_SUCCESS);
	return (ice->ice_hw.last_status == -2 ? ENOSPC : EIO);
}

static int
aq_result(struct ice_hw *hw)
{
	if (require_rebuild_lock)
		assert(MUTEX_HELD(&hw->owner->ice_rebuild_lock));
	assert(!MUTEX_HELD(&hw->owner->ice_pf_vsi.vi_mac_lock));
	hw->calls++;
	hw->last_status = hw->result;
	return (hw->result);
}

static int
filter_request(struct ice_hw *hw, struct LIST_HEAD_TYPE *list, boolean_t add)
{
	struct LIST_HEAD_TYPE *node;
	unsigned int count = 0;

	assert(list->entry != NULL && nrequests < 8);
	for (node = list->entry; node != NULL; node = node->entry) {
		assert(count < 8);
		requests[nrequests].entries[count++] =
		    *(struct ice_fltr_list_entry *)(void *)node;
	}
	requests[nrequests].add = add;
	requests[nrequests++].count = count;
	return (aq_result(hw));
}

static int
ice_add_mac(struct ice_hw *hw, struct LIST_HEAD_TYPE *list)
{
	return (filter_request(hw, list, B_TRUE));
}

static int
ice_remove_mac(struct ice_hw *hw, struct LIST_HEAD_TYPE *list)
{
	return (filter_request(hw, list, B_FALSE));
}

static int
ice_set_vsi_promisc(struct ice_hw *hw, uint16_t vsi, uint32_t mask,
    uint16_t vlan)
{
	assert(vsi == 0 && mask == 0xf && vlan == 0);
	hw->promisc_sets++;
	return (aq_result(hw));
}

static int
ice_clear_vsi_promisc(struct ice_hw *hw, uint16_t vsi, uint32_t mask,
    uint16_t vlan)
{
	int status;

	assert(vsi == 0 && mask == 0xf && vlan == 0);
	hw->promisc_clears++;
	status = aq_result(hw);
	if (hw->override_clear)
		status = hw->clear_result;
	hw->last_status = status;
	return (status);
}

static void atomic_or_32(uint32_t *, uint32_t);
static void ddi_fm_service_impact(void *, int);
static void ice_link_report(ice_t *, int);
static void ice_reset_redispatch(ice_t *);

#include "ice_filter_callbacks.h"

static void
atomic_or_32(uint32_t *state, uint32_t bits)
{
	*state |= bits;
}

static void
ddi_fm_service_impact(void *dip, int impact)
{
	ice_t *ice = dip;

	assert(impact == DDI_SERVICE_LOST);
	assert(MUTEX_HELD(&ice->ice_rebuild_lock));
	ice->service_lost++;
}

static void
ice_link_report(ice_t *ice, int state)
{
	assert(state == LINK_STATE_DOWN);
	assert(MUTEX_HELD(&ice->ice_rebuild_lock));
	ice->link_down++;
}

static void
ice_reset_redispatch(ice_t *ice)
{
	assert(MUTEX_HELD(&ice->ice_rebuild_lock));
	assert(!MUTEX_HELD(&ice->ice_pf_vsi.vi_mac_lock));
	assert((ice->ice_state & (ICE_STATE_ERROR | ICE_STATE_PFR_REQ)) ==
	    (ICE_STATE_ERROR | ICE_STATE_PFR_REQ));
	ice->recoveries++;
}

#define	CHECK(expr) do {						\
	if (!(expr)) {							\
		(void) fprintf(stderr, "%s:%d: %s\n", __func__,	\
		    __LINE__, #expr);					\
		exit(EXIT_FAILURE);					\
	}								\
} while (0)

static const uint8_t unicast[ETHERADDRL] = { 2, 0, 0, 0, 0, 1 };
static const uint8_t multicast[ETHERADDRL] = { 1, 0, 0x5e, 0, 0, 1 };

static void
init(ice_t *ice)
{
	CHECK(allocations == 0);
	(void) memset(ice, 0, sizeof (*ice));
	ice->ice_hw.owner = ice;
	ice->ice_dip = ice;
	nrequests = 0;
	latch_ice = NULL;
	require_rebuild_lock = B_TRUE;
}

static void
finish(ice_t *ice)
{
	CHECK(allocations == 0);
	CHECK(ice->ice_pf_vsi.vi_macs.head == NULL);
	CHECK(ice->ice_pf_vsi.vi_macs.tail == NULL);
	CHECK(!MUTEX_HELD(&ice->ice_rebuild_lock));
	CHECK(!MUTEX_HELD(&ice->ice_pf_vsi.vi_mac_lock));
}

static void
check_recovery(ice_t *ice)
{
	CHECK((ice->ice_state & (ICE_STATE_ERROR | ICE_STATE_PFR_REQ)) ==
	    (ICE_STATE_ERROR | ICE_STATE_PFR_REQ));
	CHECK(ice->recoveries == 1 && ice->service_lost == 1);
	CHECK(ice->link_down == 1);
}

static void
normal_filters(void)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_group_remove_mac(&ice, unicast) == ENOENT);
	CHECK(ice.ice_hw.calls == 0);
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(allocations == 1 && ice.ice_hw.calls == 1);
	CHECK(ice_group_remove_mac(&ice, unicast) == 0);
	CHECK(ice.ice_hw.calls == 2 && ice.recoveries == 0);
	finish(&ice);
}

static void
failed_filter_add(boolean_t multi)
{
	ice_t ice;
	int ret;

	init(&ice);
	ice.ice_hw.result = -2;
	ret = multi ? ice_m_multicst(&ice, B_TRUE, multicast) :
	    ice_group_add_mac(&ice, unicast);
	CHECK(ret == ENOSPC);
	CHECK(allocations == 0 && ice.ice_hw.calls == 1);
	check_recovery(&ice);
	ice.ice_hw.result = ICE_SUCCESS;
	CHECK(ice_group_add_mac(&ice, unicast) == EIO);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == EIO);
	CHECK(ice.ice_hw.calls == 1 && ice.recoveries == 1);
	finish(&ice);
}

static void
failed_filter_remove(boolean_t multi)
{
	ice_t ice;
	int ret;

	init(&ice);
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == 0);
	/* An ordinary datapath error alone does not suppress the AQ attempt. */
	ice.ice_state = ICE_STATE_ERROR;
	ice.ice_hw.result = -1;
	ret = multi ? ice_m_multicst(&ice, B_FALSE, multicast) :
	    ice_group_remove_mac(&ice, unicast);
	CHECK(ret == 0);
	CHECK(allocations == 1 && ice.ice_hw.calls == 3);
	check_recovery(&ice);
	ret = multi ? ice_m_multicst(&ice, B_FALSE, multicast) :
	    ice_group_remove_mac(&ice, unicast);
	CHECK(ret == ENOENT);
	ret = multi ? ice_group_remove_mac(&ice, unicast) :
	    ice_m_multicst(&ice, B_FALSE, multicast);
	CHECK(ret == 0 && ice.ice_hw.calls == 3);
	CHECK(ice.recoveries == 1);
	finish(&ice);
}

static void
late_filter_request(uint32_t state, boolean_t multi, boolean_t add)
{
	ice_t ice;
	int ret;

	init(&ice);
	if (!add) {
		ret = multi ? ice_m_multicst(&ice, B_TRUE, multicast) :
		    ice_group_add_mac(&ice, unicast);
		CHECK(ret == 0);
	}
	latch_ice = &ice;
	latch_state = state;
	ret = multi ? ice_m_multicst(&ice, add, multicast) :
	    ice_filters_set_mac(&ice, unicast, add);
	CHECK(latch_ice == NULL && ice.ice_state == state);
	CHECK(ret == (add ? EIO : 0));
	CHECK(allocations == 0 && ice.ice_hw.calls == (add ? 0U : 1U));
	finish(&ice);
}

static void
terminal_filters(uint32_t state)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == 0);
	CHECK(allocations == 2 && ice.ice_hw.calls == 2);
	ice.ice_state = state;
	ice.ice_hw.result = -1;
	CHECK(ice_group_remove_mac(&ice, unicast) == 0);
	CHECK(allocations == 1 && ice.ice_hw.calls == 2);
	CHECK(memcmp(ice.ice_pf_vsi.vi_macs.head->imf_addr,
	    multicast, ETHERADDRL) == 0);
	CHECK(ice_group_remove_mac(&ice, unicast) == ENOENT);
	CHECK(ice_group_add_mac(&ice, unicast) == EIO);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == EIO);
	CHECK(ice_m_multicst(&ice, B_FALSE, multicast) == 0);
	CHECK(ice_m_multicst(&ice, B_TRUE, multicast) == EIO);
	CHECK(ice.ice_hw.calls == 2);
	CHECK(ice.recoveries == 0 && ice.service_lost == 0);
	CHECK(ice.link_down == 0);
	finish(&ice);
}

static void
normal_promisc(void)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(ice.ice_hw.calls == 0);
	CHECK(ice_m_promisc(&ice, B_TRUE) == 0);
	CHECK(ice_m_promisc(&ice, B_TRUE) == 0);
	CHECK(ice.ice_promisc_on && ice.ice_hw.calls == 1);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(!ice.ice_promisc_on && ice.ice_hw.calls == 2);
	CHECK(ice.recoveries == 0);
	finish(&ice);
}

static void
failed_promisc_disable(void)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_m_promisc(&ice, B_TRUE) == 0);
	ice.ice_state = ICE_STATE_ERROR;
	ice.ice_hw.result = -1;
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(!ice.ice_promisc_on && ice.ice_hw.calls == 2);
	check_recovery(&ice);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(ice_m_promisc(&ice, B_TRUE) == EIO);
	CHECK(!ice.ice_promisc_on && ice.ice_hw.calls == 2);
	finish(&ice);
}

static void
failed_promisc_enable(boolean_t rollback_fails)
{
	ice_t ice;

	init(&ice);
	ice.ice_hw.result = -2;
	ice.ice_hw.override_clear = B_TRUE;
	ice.ice_hw.clear_result = rollback_fails ? -1 : ICE_SUCCESS;
	CHECK(ice_m_promisc(&ice, B_TRUE) == ENOSPC);
	CHECK(ice.ice_hw.last_status == ice.ice_hw.clear_result);
	CHECK(!ice.ice_promisc_on && ice.ice_hw.calls == 2);
	CHECK(ice.ice_hw.promisc_sets == 1 && ice.ice_hw.promisc_clears == 1);
	/* A clear cannot disprove an unrecorded rule after an AQ error. */
	check_recovery(&ice);
	CHECK(ice_m_promisc(&ice, B_TRUE) == EIO);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(ice.ice_hw.calls == 2 && !ice.ice_promisc_on);
	finish(&ice);
}

static void
promisc_replay(void)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_m_promisc(&ice, B_TRUE) == 0);
	/* Replay must program even when the remembered policy is unchanged. */
	mutex_enter(&ice.ice_rebuild_lock);
	CHECK(ice_filters_replay_promisc(&ice) == 0);
	CHECK(ice.ice_hw.calls == 2 && ice.ice_promisc_on);
	/* A later request does not suppress the current worker's replay. */
	ice.ice_state = ICE_STATE_ERROR | ICE_STATE_PFR_REQ;
	CHECK(ice_filters_replay_promisc(&ice) == 0);
	mutex_exit(&ice.ice_rebuild_lock);
	CHECK(ice.ice_hw.calls == 3 && ice.ice_promisc_on);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(!ice.ice_promisc_on && ice.ice_hw.calls == 3);
	finish(&ice);
}

static void
terminal_promisc(uint32_t state)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_m_promisc(&ice, B_TRUE) == 0);
	ice.ice_state = state;
	ice.ice_hw.result = -1;
	CHECK(ice_m_promisc(&ice, B_TRUE) == EIO);
	CHECK(ice.ice_promisc_on);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(!ice.ice_promisc_on);
	CHECK(ice_m_promisc(&ice, B_FALSE) == 0);
	CHECK(ice_m_promisc(&ice, B_TRUE) == EIO);
	CHECK(!ice.ice_promisc_on && ice.ice_hw.calls == 1);
	CHECK(ice.recoveries == 0 && ice.service_lost == 0);
	CHECK(ice.link_down == 0);
	finish(&ice);
}

int
main(void)
{
	const uint32_t blocked[] = {
		ICE_STATE_RESET_FAILED,
		ICE_STATE_RESET_FAILED | ICE_STATE_ERROR,
		ICE_STATE_PFR_REQ,
		ICE_STATE_PFR_REQ | ICE_STATE_ERROR,
		ICE_STATE_RESET_PENDING,
		ICE_STATE_RESET_PENDING | ICE_STATE_ERROR
	};
	size_t i;

	normal_filters();
	failed_filter_add(B_FALSE);
	failed_filter_add(B_TRUE);
	failed_filter_remove(B_FALSE);
	failed_filter_remove(B_TRUE);
	normal_promisc();
	failed_promisc_disable();
	failed_promisc_enable(B_FALSE);
	failed_promisc_enable(B_TRUE);
	promisc_replay();
	late_filter_request(ICE_STATE_PFR_REQ, B_FALSE, B_TRUE);
	late_filter_request(ICE_STATE_PFR_REQ, B_TRUE, B_TRUE);
	late_filter_request(ICE_STATE_PFR_REQ, B_FALSE, B_FALSE);
	late_filter_request(ICE_STATE_PFR_REQ, B_TRUE, B_FALSE);
	late_filter_request(ICE_STATE_RESET_PENDING, B_FALSE, B_TRUE);
	late_filter_request(ICE_STATE_RESET_PENDING, B_TRUE, B_TRUE);
	late_filter_request(ICE_STATE_RESET_PENDING, B_FALSE, B_FALSE);
	late_filter_request(ICE_STATE_RESET_PENDING, B_TRUE, B_FALSE);
	for (i = 0; i < sizeof (blocked) / sizeof (blocked[0]); i++) {
		terminal_filters(blocked[i]);
		terminal_promisc(blocked[i]);
	}
	(void) puts("filter callbacks: PASS (30 scenarios)");
	return (EXIT_SUCCESS);
}

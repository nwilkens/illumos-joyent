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
};
typedef struct list {
	ice_mac_filter_t *head;
	ice_mac_filter_t *tail;
} list_t;
typedef struct ice_vsi {
	uint16_t vi_handle;
	kmutex_t vi_mac_lock;
	list_t vi_macs;
} ice_vsi_t;
typedef struct ice ice_t;
struct ice_hw {
	ice_t *owner;
	int result;
	unsigned int calls;
};
struct ice {
	struct ice_hw ice_hw;
	ice_vsi_t ice_pf_vsi;
	kmutex_t ice_rebuild_lock;
	uint32_t ice_state;
	boolean_t ice_promisc_on;
};

struct LIST_HEAD_TYPE {
	struct LIST_HEAD_TYPE *entry;
};
struct ice_fltr_list_entry {
	struct LIST_HEAD_TYPE list_entry;
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
	ICE_FLTR_TX, ICE_SW_LKUP_MAC, ICE_FWD_TO_VSI, ICE_SRC_ID_VSI
};
#define	INIT_LIST_HEAD(h)	((h)->entry = NULL)
#define	LIST_ADD(e, h)		((h)->entry = (e))

enum {
	ICE_PROMISC_UCAST_RX, ICE_PROMISC_UCAST_TX,
	ICE_PROMISC_MCAST_RX, ICE_PROMISC_MCAST_TX, ICE_PROMISC_MAX
};
#define	ice_declare_bitmap(n, bits)	uint32_t n
#define	ice_zero_bitmap(n, bits)		((n) = 0)
#define	ice_set_bit(bit, n)		((n) |= 1U << (bit))

static unsigned int allocations;

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
	(void) ice;
	assert(status != ICE_SUCCESS);
	return (EIO);
}

static int
aq_result(struct ice_hw *hw)
{
	assert(MUTEX_HELD(&hw->owner->ice_rebuild_lock));
	assert(!MUTEX_HELD(&hw->owner->ice_pf_vsi.vi_mac_lock));
	hw->calls++;
	return (hw->result);
}

static int
ice_add_mac(struct ice_hw *hw, struct LIST_HEAD_TYPE *list)
{
	assert(list->entry != NULL);
	return (aq_result(hw));
}

static int
ice_remove_mac(struct ice_hw *hw, struct LIST_HEAD_TYPE *list)
{
	assert(list->entry != NULL);
	return (aq_result(hw));
}

static int
ice_set_vsi_promisc(struct ice_hw *hw, uint16_t vsi, uint32_t mask,
    uint16_t vlan)
{
	assert(vsi == 0 && mask == 0xf && vlan == 0);
	return (aq_result(hw));
}

static int
ice_clear_vsi_promisc(struct ice_hw *hw, uint16_t vsi, uint32_t mask,
    uint16_t vlan)
{
	return (ice_set_vsi_promisc(hw, vsi, mask, vlan));
}

#include "ice_filter_callbacks.h"

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
normal_filters(void)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_group_remove_mac(&ice, unicast) == ENOENT);
	CHECK(ice.ice_hw.calls == 0);
	ice.ice_hw.result = -1;
	CHECK(ice_group_add_mac(&ice, unicast) == EIO);
	CHECK(allocations == 0 && ice.ice_hw.calls == 1);
	ice.ice_hw.result = ICE_SUCCESS;
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(ice_group_add_mac(&ice, unicast) == 0);
	CHECK(allocations == 1 && ice.ice_hw.calls == 2);
	ice.ice_state = ICE_STATE_ERROR;
	ice.ice_hw.result = -1;
	CHECK(ice_group_remove_mac(&ice, unicast) == EIO);
	CHECK(allocations == 1 && ice.ice_hw.calls == 3);
	CHECK(memcmp(ice.ice_pf_vsi.vi_macs.head->imf_addr,
	    unicast, ETHERADDRL) == 0);
	ice.ice_hw.result = ICE_SUCCESS;
	CHECK(ice_group_remove_mac(&ice, unicast) == 0);
	CHECK(ice.ice_hw.calls == 4);
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
	finish(&ice);
}

static void
normal_promisc(void)
{
	ice_t ice;

	init(&ice);
	CHECK(ice_m_promisc(&ice, B_TRUE) == 0);
	CHECK(ice.ice_promisc_on && ice.ice_hw.calls == 1);
	ice.ice_state = ICE_STATE_ERROR;
	ice.ice_hw.result = -1;
	CHECK(ice_m_promisc(&ice, B_FALSE) == EIO);
	CHECK(ice.ice_promisc_on && ice.ice_hw.calls == 2);
	ice.ice_hw.result = ICE_SUCCESS;
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
	finish(&ice);
}

int
main(void)
{
	normal_filters();
	normal_promisc();
	terminal_filters(ICE_STATE_RESET_FAILED);
	terminal_filters(ICE_STATE_RESET_FAILED | ICE_STATE_ERROR);
	terminal_promisc(ICE_STATE_RESET_FAILED);
	terminal_promisc(ICE_STATE_RESET_FAILED | ICE_STATE_ERROR);
	(void) puts("terminal filter callbacks: PASS (6 scenarios)");
	return (EXIT_SUCCESS);
}

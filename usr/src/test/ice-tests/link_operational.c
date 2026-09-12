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
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#undef bcopy
#define	bcopy(s, d, n)	((void) memcpy((d), (s), (n)))
#define	B_FALSE 0
#define	B_TRUE 1
#define	ASSERT(x) assert(x)
#define	MUTEX_HELD(m) (*(m) != 0)
#define	BIT(n) (1U << (n))
#define	_NOTE(x) (void) (x)
#define	ARGUNUSED(x) x

typedef int boolean_t;
typedef int kmutex_t;
typedef unsigned int uint_t;
typedef uint16_t u16;

#include "ice_link_types.h"

struct ice_link_status {
	uint8_t link_info;
	uint16_t link_speed;
	uint8_t an_info;
};
struct ice_port_info {
	struct { struct ice_link_status link_info; } phy;
};
typedef struct ice {
	struct { struct ice_port_info *port_info; } ice_hw;
	uint32_t ice_state;
	kmutex_t ice_rebuild_lock;
	kmutex_t ice_lse_lock;
	boolean_t ice_detaching;
	void *ice_mac_hdl;
	link_state_t ice_link_state;
	uint64_t ice_link_speed;
	link_duplex_t ice_link_duplex;
	link_flowctrl_t ice_link_fctl;
	uint32_t ice_loopback_mode;
	uint16_t ice_phy_speeds_adv, ice_phy_speeds_supp;
	link_fec_t ice_fec_neg;
	uint32_t ice_mtu;
} ice_t;

static ice_t device;
static struct ice_port_info port;
static link_state_t published;
static unsigned publications, starts;
static int start_result;
static boolean_t fault_during_start;

static void
mutex_enter(kmutex_t *lock)
{
	assert(!*lock);
	*lock = 1;
}

static void
mutex_exit(kmutex_t *lock)
{
	assert(*lock);
	*lock = 0;
}

static void
mac_link_update(void *handle, link_state_t state)
{
	assert(handle == &device && device.ice_lse_lock);
	publications++;
	published = state;
}

static void
atomic_and_32(uint32_t *ptr, uint32_t bits)
{
	*ptr &= bits;
}

void
atomic_or_32(uint32_t *ptr, uint32_t bits)
{
	*ptr |= bits;
}

static int
ice_start_datapath(ice_t *ice)
{
	assert(ice->ice_rebuild_lock);
	starts++;
	if (fault_during_start)
		ice->ice_state |= ICE_STATE_ERROR | ICE_STATE_PFR_REQ;
	return (start_result);
}

static uint_t
highbit(uint_t value)
{
	uint_t bit = 0;

	while (value != 0) {
		value >>= 1;
		bit++;
	}
	return (bit);
}

static uint64_t
ice_get_link_speed(u16 bit)
{
	assert(bit == 0);
	return (1000);
}

#include "ice_link_body.h"

static void
reset(void)
{
	(void) memset(&device, 0, sizeof (device));
	(void) memset(&port, 0, sizeof (port));
	device.ice_hw.port_info = &port;
	device.ice_mac_hdl = &device;
	device.ice_link_state = LINK_STATE_UNKNOWN;
	port.phy.link_info.link_info = ICE_AQ_LINK_UP;
	port.phy.link_info.link_speed = 1;
	published = LINK_STATE_UNKNOWN;
	publications = starts = 0;
	start_result = 0;
	fault_during_start = B_FALSE;
}

static void
refresh(void)
{
	mutex_enter(&device.ice_lse_lock);
	ice_link_prop_update(&device);
	mutex_exit(&device.ice_lse_lock);
}

static void
check_status(link_state_t expected)
{
	link_state_t state = LINK_STATE_UNKNOWN;

	assert(ice_m_getprop(&device, "status", MAC_PROP_STATUS,
	    sizeof (state), &state) == 0);
	assert(state == expected);
}

static void
check_failure_publication(void)
{
	const uint32_t failures[] = { ICE_STATE_ERROR, ICE_STATE_RESET_FAILED,
	    ICE_STATE_PFR_REQ, ICE_STATE_RESET_PENDING };
	size_t i;

	for (i = 0; i < sizeof (failures) / sizeof (failures[0]); i++) {
		reset();
		device.ice_state = failures[i];
		refresh();
		assert(device.ice_link_state == LINK_STATE_UP);
		assert(device.ice_link_speed == 1000);
		assert(published == LINK_STATE_DOWN);
		check_status(LINK_STATE_DOWN);
		/* Repeated physical UP must not reopen an unusable datapath. */
		refresh();
		ice_link_state_publish(&device);
		assert(published == LINK_STATE_DOWN);
		ice_link_report(&device, LINK_STATE_DOWN);
		assert(device.ice_link_state == LINK_STATE_UP);
	}

	/* A report before MAC registration only updates the carrier cache. */
	reset();
	device.ice_mac_hdl = NULL;
	device.ice_state = ICE_STATE_ERROR;
	refresh();
	assert(publications == 0 && device.ice_link_state == LINK_STATE_UP);
	device.ice_mac_hdl = &device;
	ice_link_state_publish(&device);
	assert(published == LINK_STATE_DOWN);
}

static void
check_start_recovery(void)
{
	reset();
	refresh();
	device.ice_state = ICE_STATE_ERROR;
	ice_link_report(&device, LINK_STATE_DOWN);
	start_result = EIO;
	assert(ice_m_start(&device) == EIO);
	assert((device.ice_state & ICE_STATE_ERROR) != 0);
	assert(published == LINK_STATE_DOWN);
	refresh();
	assert(published == LINK_STATE_DOWN);
	check_status(LINK_STATE_DOWN);

	start_result = 0;
	assert(ice_m_start(&device) == 0);
	assert((device.ice_state & ICE_STATE_ERROR) == 0);
	assert(published == LINK_STATE_UP);
	check_status(LINK_STATE_UP);

	/* The old error clear must not move past a newly failing start. */
	device.ice_state = ICE_STATE_ERROR;
	fault_during_start = B_TRUE;
	assert(ice_m_start(&device) == 0);
	assert((device.ice_state & ICE_STATE_ERROR) != 0);
	assert(published == LINK_STATE_DOWN);
	check_status(LINK_STATE_DOWN);
	assert(!device.ice_rebuild_lock && !device.ice_lse_lock);
}

static void
check_loopback(void)
{
	reset();
	device.ice_state = ICE_STATE_ERROR;
	ice_link_loopback_update(&device, ICE_LB_INTERNAL_MAC);
	assert(device.ice_link_state == LINK_STATE_UP);
	assert(published == LINK_STATE_DOWN);
	refresh();
	assert(published == LINK_STATE_DOWN);
	check_status(LINK_STATE_DOWN);
	ice_link_loopback_update(&device, ICE_LB_NONE);
	assert(device.ice_link_state == LINK_STATE_UNKNOWN);
	assert(published == LINK_STATE_DOWN);

	device.ice_state = 0;
	ice_link_state_publish(&device);
	assert(published == LINK_STATE_UNKNOWN);
	ice_link_loopback_update(&device, ICE_LB_INTERNAL_MAC);
	assert(published == LINK_STATE_UP);
	ice_link_loopback_update(&device, ICE_LB_NONE);
	port.phy.link_info.link_info = 0;
	refresh();
	assert(published == LINK_STATE_DOWN);
	check_status(LINK_STATE_DOWN);
}

int
main(void)
{
	check_failure_publication();
	check_start_recovery();
	check_loopback();
	(void) puts("PASS: ICE operational link and carrier separation");
	return (0);
}

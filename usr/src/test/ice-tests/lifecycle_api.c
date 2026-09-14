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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define	B_TRUE	true
#define	B_FALSE	false
#define	ICE_SUCCESS	0
#define	DDI_SERVICE_LOST	1
#define	ASSERT(x)	assert(x)
#define	MUTEX_HELD(p)	(*(p) != 0)
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))

typedef bool boolean_t;
typedef int kmutex_t;

#include "ice_lifecycle_types.h"

typedef struct ice {
	uint32_t ice_state;
	boolean_t ice_detaching;
	kmutex_t ice_rebuild_lock, ice_lse_lock;
	link_state_t ice_link_state;
	void *ice_mac_hdl, *ice_dip;
} ice_t;

static ice_t device;
static struct {
	int program_status;
	boolean_t rx_start_ok, disable_ok, rx_drained;
	boolean_t mapped, dissociated, dma_live, tx_closed, rx_closed;
	unsigned int tx_buffers, rx_buffers;
	unsigned int maps, programs, rx_starts, tx_starts, disables;
	unsigned int tx_quiesces, rx_quiesces, tx_reclaims, rx_reclaims;
	unsigned int publications, impacts, resets;
	uint32_t inject_during_start;
	link_state_t published;
} fixture;

static void
check_owner(ice_t *ice)
{
	assert(ice == &device && MUTEX_HELD(&ice->ice_rebuild_lock));
}

static void
mutex_enter(kmutex_t *lock)
{
	assert(!MUTEX_HELD(lock));
	if (lock == &device.ice_lse_lock)
		assert(MUTEX_HELD(&device.ice_rebuild_lock));
	else
		assert(lock == &device.ice_rebuild_lock);
	*lock = 1;
}

static void
mutex_exit(kmutex_t *lock)
{
	assert(MUTEX_HELD(lock));
	if (lock == &device.ice_rebuild_lock)
		assert(!MUTEX_HELD(&device.ice_lse_lock));
	*lock = 0;
}

static void
atomic_or_32(uint32_t *word, uint32_t bits)
{
	check_owner(&device);
	assert(word == &device.ice_state);
	*word |= bits;
}

static void
atomic_and_32(uint32_t *word, uint32_t bits)
{
	check_owner(&device);
	assert(word == &device.ice_state);
	*word &= bits;
}

static void
ice_queues_intr_map(ice_t *ice)
{
	check_owner(ice);
	fixture.mapped = B_TRUE;
	fixture.maps++;
}

static int
ice_queues_program(ice_t *ice)
{
	check_owner(ice);
	assert(fixture.mapped);
	assert(fixture.rx_starts == 0 && fixture.tx_starts == 0);
	fixture.programs++;
	if (fixture.program_status == ICE_SUCCESS)
		fixture.dma_live = B_TRUE;
	return (fixture.program_status);
}

static boolean_t
ice_rx_start(ice_t *ice)
{
	check_owner(ice);
	assert(fixture.programs == 1 && fixture.program_status == ICE_SUCCESS);
	assert(fixture.tx_starts == 0);
	fixture.rx_starts++;
	ice->ice_state |= fixture.inject_during_start;
	return (fixture.rx_start_ok);
}

static void
ice_tx_start(ice_t *ice)
{
	check_owner(ice);
	assert(fixture.rx_starts == 1 && fixture.rx_start_ok);
	fixture.tx_closed = B_FALSE;
	fixture.tx_starts++;
}

static void
ice_queues_intr_dissociate(ice_t *ice)
{
	check_owner(ice);
	assert((ice->ice_state & ICE_STATE_STARTED) == 0);
	fixture.dissociated = B_TRUE;
}

static boolean_t
ice_queues_disable(ice_t *ice)
{
	check_owner(ice);
	/* Start rollback follows RX failure; normal stop first dissociates. */
	assert(fixture.dissociated ||
	    (fixture.rx_starts == 1 && !fixture.rx_start_ok));
	fixture.disables++;
	if (fixture.disable_ok)
		fixture.dma_live = B_FALSE;
	return (fixture.disable_ok);
}

static void
ice_tx_quiesce(ice_t *ice)
{
	check_owner(ice);
	fixture.tx_closed = B_TRUE;
	fixture.tx_quiesces++;
}

static boolean_t
ice_rx_quiesce(ice_t *ice)
{
	check_owner(ice);
	fixture.rx_closed = B_TRUE;
	fixture.rx_quiesces++;
	return (fixture.rx_drained);
}

static void
ice_tx_reclaim(ice_t *ice)
{
	check_owner(ice);
	assert(!fixture.dma_live && fixture.tx_closed);
	fixture.tx_buffers = 0;
	fixture.tx_reclaims++;
}

static void
ice_rx_reclaim(ice_t *ice)
{
	check_owner(ice);
	assert(!fixture.dma_live && fixture.rx_closed);
	if (fixture.rx_drained)
		fixture.rx_buffers = 0;
	fixture.rx_reclaims++;
}

static void
ice_reset_redispatch(ice_t *ice)
{
	check_owner(ice);
	assert((ice->ice_state & ICE_STATE_STARTED) == 0);
	assert((ice->ice_state & ICE_STATE_ERROR) != 0);
	assert((ice->ice_state & ICE_STATE_PFR_REQ) != 0);
	assert(fixture.tx_closed && fixture.rx_closed);
	assert(fixture.tx_reclaims == 0 && fixture.rx_reclaims == 0);
	fixture.resets++;
}

static void
ddi_fm_service_impact(void *dip, int impact)
{
	check_owner(&device);
	assert(dip == &device && impact == DDI_SERVICE_LOST);
	fixture.impacts++;
}

static void
mac_link_update(void *handle, link_state_t state)
{
	check_owner(&device);
	assert(handle == &device && MUTEX_HELD(&device.ice_lse_lock));
	fixture.published = state;
	fixture.publications++;
}

#include "ice_lifecycle_bodies.h"

static void
check_unlocked(void)
{
	assert(!MUTEX_HELD(&device.ice_rebuild_lock));
	assert(!MUTEX_HELD(&device.ice_lse_lock));
}

static void
reset(void)
{
	check_unlocked();
	memset(&device, 0, sizeof (device));
	memset(&fixture, 0, sizeof (fixture));
	device.ice_state = ICE_STATE_ATTACHED;
	device.ice_link_state = LINK_STATE_UP;
	device.ice_mac_hdl = &device;
	device.ice_dip = &device;
	fixture.rx_start_ok = fixture.disable_ok = fixture.rx_drained = B_TRUE;
	fixture.tx_closed = fixture.rx_closed = B_TRUE;
	fixture.tx_buffers = 3;
	fixture.rx_buffers = 4;
	fixture.published = LINK_STATE_UNKNOWN;
}

static void
admission(void)
{
	const uint32_t blocked[] = {
		ICE_STATE_RESET_PENDING, ICE_STATE_PFR_REQ,
		ICE_STATE_RESET_FAILED,
		ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ
	};
	size_t i;

	for (i = 0; i <= ARRAY_SIZE(blocked); i++) {
		uint32_t before;

		reset();
		device.ice_state |= ICE_STATE_ERROR;
		if (i == ARRAY_SIZE(blocked))
			device.ice_detaching = B_TRUE;
		else
			device.ice_state |= blocked[i];
		before = device.ice_state;
		assert(ice_m_start(&device) == EIO);
		assert(device.ice_state == before);
		assert(fixture.maps == 0 && fixture.programs == 0);
		assert(fixture.publications == 0 && fixture.disables == 0);
		check_unlocked();
	}
}

static void
startup(void)
{
	unsigned int i;
	const uint32_t injected[] = { 0, ICE_STATE_ERROR,
		ICE_STATE_ERROR | ICE_STATE_PFR_REQ };

	for (i = 0; i < ARRAY_SIZE(injected); i++) {
		reset();
		device.ice_state |= ICE_STATE_ERROR;
		fixture.inject_during_start = injected[i];
		assert(ice_m_start(&device) == 0);
		assert(device.ice_state ==
		    (ICE_STATE_ATTACHED | ICE_STATE_STARTED | injected[i]));
		assert(fixture.maps == 1 && fixture.programs == 1);
		assert(fixture.rx_starts == 1 && fixture.tx_starts == 1);
		assert(fixture.disables == 0 && fixture.publications == 1);
		assert(fixture.published ==
		    (injected[i] != 0 ? LINK_STATE_DOWN : LINK_STATE_UP));
		assert(device.ice_link_state == LINK_STATE_UP);
		check_unlocked();
	}

	reset();
	fixture.program_status = -23;
	assert(ice_m_start(&device) == EIO);
	assert(device.ice_state == (ICE_STATE_ATTACHED | ICE_STATE_ERROR));
	assert(fixture.maps == 1 && fixture.programs == 1);
	assert(fixture.rx_starts == 0 && fixture.tx_starts == 0);
	assert(fixture.disables == 0 && fixture.publications == 1);
	assert(fixture.published == LINK_STATE_DOWN);
	check_unlocked();

	for (i = 0; i < 2; i++) {
		reset();
		fixture.rx_start_ok = B_FALSE;
		fixture.disable_ok = i != 0;
		assert(ice_m_start(&device) == EIO);
		assert(device.ice_state ==
		    (ICE_STATE_ATTACHED | ICE_STATE_ERROR));
		assert(fixture.rx_starts == 1 && fixture.tx_starts == 0);
		assert(fixture.disables == 1 && fixture.publications == 1);
		assert(fixture.published == LINK_STATE_DOWN);
		assert(fixture.tx_reclaims == 0 && fixture.rx_reclaims == 0);
		assert(fixture.tx_buffers == 3 && fixture.rx_buffers == 4);
		assert(fixture.dma_live == !fixture.disable_ok);
		check_unlocked();
	}
}

static void
stop_case(boolean_t disabled, boolean_t drained)
{
	reset();
	device.ice_state |= ICE_STATE_STARTED;
	fixture.dma_live = B_TRUE;
	fixture.tx_closed = fixture.rx_closed = B_FALSE;
	fixture.disable_ok = disabled;
	fixture.rx_drained = drained;
	ice_m_stop(&device);
	assert(fixture.dissociated && fixture.disables == 1);
	assert(fixture.tx_quiesces == 1 && fixture.rx_quiesces == 1);
	assert(fixture.tx_closed && fixture.rx_closed);
	assert((device.ice_state & ICE_STATE_STARTED) == 0);
	assert((device.ice_state & ICE_STATE_ATTACHED) != 0);
	if (disabled) {
		assert(!fixture.dma_live && fixture.tx_buffers == 0);
		assert(fixture.rx_buffers == (drained ? 0U : 4U));
		assert(fixture.tx_reclaims == 1);
		assert(fixture.rx_reclaims == 1);
		assert(fixture.resets == 0 && fixture.impacts == 0);
		assert(device.ice_state == ICE_STATE_ATTACHED);
	} else {
		assert(fixture.dma_live && fixture.tx_buffers == 3);
		assert(fixture.rx_buffers == 4);
		assert(fixture.tx_reclaims == 0);
		assert(fixture.rx_reclaims == 0);
		assert(fixture.resets == 1 && fixture.impacts == 1);
		assert(device.ice_state == (ICE_STATE_ATTACHED |
		    ICE_STATE_ERROR | ICE_STATE_PFR_REQ));
	}
	check_unlocked();
}

static void
restart_locked(void)
{
	reset();
	device.ice_state |= ICE_STATE_STARTED;
	mutex_enter(&device.ice_rebuild_lock);
	assert(ice_start_datapath(&device) == 0);
	assert(MUTEX_HELD(&device.ice_rebuild_lock));
	assert(device.ice_state == (ICE_STATE_ATTACHED | ICE_STATE_STARTED));
	assert(fixture.maps == 1 && fixture.programs == 1);
	assert(fixture.rx_starts == 1 && fixture.tx_starts == 1);
	assert(fixture.publications == 0);
	mutex_exit(&device.ice_rebuild_lock);
	check_unlocked();
}

int
main(void)
{
	admission();
	startup();
	stop_case(B_TRUE, B_TRUE);
	stop_case(B_TRUE, B_FALSE);
	stop_case(B_FALSE, B_TRUE);
	stop_case(B_FALSE, B_FALSE);
	restart_locked();
	(void) puts("PASS: ICE lifecycle start/stop boundary (16 scenarios)");
	return (0);
}

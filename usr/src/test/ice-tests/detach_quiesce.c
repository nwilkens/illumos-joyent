/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/* Boundary stubs; all detach decisions come from the production C body. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define	B_TRUE 1
#define	B_FALSE 0
#define	DDI_SUCCESS 0
#define	DDI_FAILURE -1
#define	DDI_DETACH 1
#define	ICE_SUCCESS 0
#define	ICE_RESET_PFR 1
#define	DDI_FM_OK 0
#define	DDI_FME_VERSION 1
#define	DDI_SERVICE_LOST 1
#define	ASSERT(x) assert(x)
#define	MUTEX_HELD(m) (*(m) != 0)

typedef int boolean_t;
typedef int kmutex_t;
typedef int ddi_detach_cmd_t;
typedef int dev_info_t;
typedef int ddi_acc_handle_t;
typedef struct { int fme_status; } ddi_fm_error_t;
struct ice_hw { bool reset_ongoing; };
typedef struct ice {
	struct ice_hw ice_hw;
	struct { int ios_reg_handle; } ice_osdep;
	dev_info_t *ice_dip;
	kmutex_t ice_rebuild_lock;
	uint32_t ice_state;
	uint32_t ice_acc_errors, ice_acc_clears;
	uint32_t ice_attach_progress;
	boolean_t ice_detaching;
} ice_t;

static ice_t device;
static void *ice_state_p;
static kmutex_t ice_glock;
static int ice_glist;
static struct {
	boolean_t drained, disabled, reset_ok, access_ok, unregister_ok;
	boolean_t quiet, tx_closed, rx_closed, freed;
	unsigned int disables, resets, reclaims, unregisters, redispatches;
	unsigned int starts, clears;
	boolean_t consume_fault, pending_fault, inject_after_ok_get;
} fixture;

static void
mutex_enter(kmutex_t *p)
{
	assert(!*p);
	*p = 1;
}

static void
mutex_exit(kmutex_t *p)
{
	assert(*p);
	*p = 0;
}

static void
atomic_or_32(uint32_t *p, uint32_t v)
{
	*p |= v;
}

static void
atomic_and_32(uint32_t *p, uint32_t v)
{
	*p &= v;
}

static int ice_m_start(void *);
static int ice_start_datapath(ice_t *p)
{
	assert(!p->ice_detaching);
	fixture.starts++;
	return (0);
}
static int
ddi_get_instance(dev_info_t *p)
{
	(void) p;
	return (0);
}

static void *ddi_get_soft_state(void *p, int i)
{ (void) p; (void) i; return (&device); }
static void
ddi_soft_state_free(void *p, int i)
{
	(void) p;
	(void) i;
	assert(fixture.freed);
}

static void
ice_error(ice_t *p, const char *s, ...)
{
	(void) p;
	(void) s;
}

static void
ddi_fm_service_impact(dev_info_t *p, int impact)
{
	(void) p;
	assert(impact == DDI_SERVICE_LOST);
}

int ice_check_acc_handle(ice_t *, ddi_acc_handle_t);

static void
membar_enter(void)
{
}

static void
membar_exit(void)
{
}

static void
atomic_inc_32(uint32_t *p)
{
	(*p)++;
}

static void
atomic_dec_32(uint32_t *p)
{
	assert(*p != 0);
	(*p)--;
}

static uint32_t
atomic_add_32_nv(uint32_t *p, uint32_t v)
{
	return (*p += v);
}

static void
ddi_fm_acc_err_get(int handle, ddi_fm_error_t *err, int version)
{
	(void) handle;
	(void) version;
	err->fme_status = fixture.access_ok && !fixture.pending_fault ? 0 : -1;
	if (fixture.inject_after_ok_get)
		fixture.pending_fault = B_TRUE;
}

static void
ddi_fm_acc_err_clear(int handle, int version)
{
	(void) handle;
	(void) version;
	assert(device.ice_acc_clears > 0 && device.ice_acc_errors > 0);
	fixture.pending_fault = B_FALSE;
	fixture.clears++;
}

static void
ice_tx_quiesce(ice_t *p)
{
	assert(MUTEX_HELD(&p->ice_rebuild_lock));
	fixture.tx_closed = B_TRUE;
}

static boolean_t ice_rx_quiesce(ice_t *p)
{
	assert(MUTEX_HELD(&p->ice_rebuild_lock));
	fixture.rx_closed = B_TRUE;
	return (fixture.drained);
}
static boolean_t
ice_rx_drain(ice_t *p)
{
	(void) p;
	return (fixture.drained);
}

static void
ice_queues_intr_dissociate(ice_t *p)
{
	assert(MUTEX_HELD(&p->ice_rebuild_lock));
}

static boolean_t ice_queues_disable(ice_t *p)
{
	assert(MUTEX_HELD(&p->ice_rebuild_lock));
	assert(fixture.tx_closed && fixture.rx_closed);
	fixture.disables++;
	fixture.quiet = fixture.disabled && fixture.access_ok;
	if (fixture.consume_fault) {
		fixture.pending_fault = B_TRUE;
		assert(ice_check_acc_handle(p, 0) != DDI_FM_OK);
		fixture.quiet = B_FALSE;
	}
	return (fixture.disabled);
}
static int ice_reset(struct ice_hw *hw, int type)
{
	assert(hw->reset_ongoing && type == ICE_RESET_PFR);
	assert(fixture.tx_closed && fixture.rx_closed);
	fixture.resets++;
	fixture.quiet = fixture.reset_ok && fixture.access_ok;
	return (fixture.reset_ok ? ICE_SUCCESS : -1);
}
static void ice_tx_reclaim(ice_t *p)
{
	assert(MUTEX_HELD(&p->ice_rebuild_lock));
	assert(fixture.quiet && fixture.tx_closed);
	fixture.reclaims++;
}
static int ice_mac_unregister(ice_t *p)
{
	assert(!MUTEX_HELD(&p->ice_rebuild_lock));
	assert(p->ice_detaching);
	assert(ice_m_start(p) == EIO && fixture.starts == 0);
	/* MAC must not be irreversibly freed before proven DMA isolation. */
	assert(fixture.quiet && fixture.tx_closed && fixture.rx_closed);
	fixture.unregisters++;
	return (fixture.unregister_ok ? 0 : -1);
}
void ice_reset_redispatch(ice_t *);

static void
ice_reset_dispatch(ice_t *p)
{
	assert(MUTEX_HELD(&p->ice_rebuild_lock));
	fixture.redispatches++;
}

static void
ice_loopback_fini(ice_t *p)
{
	assert(p->ice_detaching);
}

static void
list_remove(int *list, ice_t *p)
{
	(void) list;
	assert(p->ice_detaching);
}

static void ice_unconfigure(ice_t *p)
{
	assert(fixture.quiet && fixture.drained);
	assert(p->ice_detaching);
	fixture.freed = B_TRUE;
}

#include "ice_detach_body.h"

static void
init(void)
{
	(void) memset(&device, 0, sizeof (device));
	(void) memset(&fixture, 0, sizeof (fixture));
	device.ice_attach_progress = ICE_ATTACH_RINGS |
	    ICE_ATTACH_QUEUE_INTR | ICE_ATTACH_MAC;
	fixture.drained = fixture.disabled = fixture.reset_ok = B_TRUE;
	fixture.access_ok = fixture.unregister_ok = B_TRUE;
}

static void
failed(void)
{
	assert(!device.ice_detaching && !fixture.freed);
	assert(device.ice_attach_progress & ICE_ATTACH_MAC);
	assert(!MUTEX_HELD(&device.ice_rebuild_lock));
}

int
main(void)
{
	/* Stop failure followed by reset failure preserves all resources. */
	init();
	fixture.disabled = fixture.reset_ok = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.disables == 1 && fixture.resets == 1);
	assert(fixture.unregisters == 0 && fixture.reclaims == 0);
	assert(device.ice_state & ICE_STATE_PFR_REQ);
	assert(fixture.redispatches == 1);
	failed();

	/* An active interface must not be stopped by a failed detach. */
	init();
	device.ice_state = ICE_STATE_STARTED;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.disables == 0 && fixture.resets == 0);
	assert(!fixture.tx_closed && !fixture.rx_closed);
	failed();

	/* Queue stop suffices without resetting healthy hardware. */
	init();
	assert(ice_detach(NULL, DDI_DETACH) == DDI_SUCCESS);
	assert(fixture.disables == 1 && fixture.resets == 0);
	assert(fixture.unregisters == 1 && fixture.reclaims == 1);
	assert(fixture.freed);

	/* A successful fallback reset permits release. */
	init();
	fixture.disabled = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_SUCCESS);
	assert(fixture.resets == 1 && fixture.reclaims == 1);
	assert(fixture.freed);

	/* Open control clients can still refuse unregister after a reset. */
	init();
	fixture.disabled = fixture.unregister_ok = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(device.ice_state & ICE_STATE_PFR_REQ);
	assert(device.ice_state & ICE_STATE_ERROR);
	assert(device.ice_hw.reset_ongoing && fixture.redispatches == 1);
	failed();

	/* No commands or frees on a loan timeout. */
	init();
	fixture.drained = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.disables == 0 && fixture.resets == 0);
	assert(fixture.unregisters == 0 && fixture.reclaims == 0);
	failed();

	/* Polling success from a faulted register mapping proves nothing. */
	init();
	fixture.access_ok = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.unregisters == 0 && fixture.reclaims == 0);
	failed();

	/* Preserve an independently latched global reset on rollback. */
	init();
	device.ice_state = ICE_STATE_RESET_PENDING;
	fixture.unregister_ok = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(device.ice_state & ICE_STATE_RESET_PENDING);
	assert(fixture.redispatches == 1);
	failed();

	/* A consumed MMIO error still invalidates the polling result. */
	init();
	fixture.consume_fault = B_TRUE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.clears == 1 && !fixture.pending_fault);
	assert(fixture.unregisters == 0 && fixture.reclaims == 0);
	failed();

	/* An older observer may still clear the latch during new polling. */
	init();
	device.ice_acc_errors = device.ice_acc_clears = 1;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.disables == 0 && fixture.unregisters == 0);
	failed();

	/* An OK getter cannot erase a later error it never observed. */
	init();
	fixture.inject_after_ok_get = B_TRUE;
	assert(ice_check_acc_handle(&device, 0) == DDI_FM_OK);
	assert(fixture.pending_fault && fixture.clears == 0);

	/* Refused unregister after ordinary stop permits the next start. */
	init();
	fixture.unregister_ok = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	failed();
	assert(ice_m_start(&device) == 0 && fixture.starts == 1);

	/* Terminal failure retains resources and never schedules recovery. */
	init();
	device.ice_state = ICE_STATE_RESET_FAILED | ICE_STATE_ERROR;
	fixture.disabled = fixture.reset_ok = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.redispatches == 0 && fixture.reclaims == 0);
	failed();

	/* A reset result from faulted MMIO also cannot authorize reclaim. */
	init();
	fixture.disabled = fixture.access_ok = B_FALSE;
	assert(ice_detach(NULL, DDI_DETACH) == DDI_FAILURE);
	assert(fixture.resets == 1 && fixture.reclaims == 0);
	failed();

	(void) puts("detach quiescence: PASS (14 scenarios)");
	return (0);
}

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
#include <string.h>

#define	B_TRUE 1
#define	B_FALSE 0
#define	DDI_NOSLEEP 0
#define	DDI_SUCCESS 0
#define	ICE_SUCCESS 0
#define	ICE_RESET_PFR 1
#define	ICE_LB_NONE 0
#define	PFINT_OICR_ENA 0
#define	CE_NOTE 0
#define	LINK_STATE_DOWN 0
#define	ASSERT(x) assert(x)
#define	MUTEX_HELD(m) (*(m) != 0)
#define	OWED (ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ)

typedef int boolean_t;
typedef int kmutex_t;
enum ice_ddp_state { DDP_OK };
struct ice_hw {
	bool reset_ongoing;
	void *port_info;
	void *pkg_copy;
	unsigned pkg_size;
};
typedef struct ice {
	uint32_t ice_state;
	kmutex_t ice_lock;
	kmutex_t ice_rebuild_lock;
	boolean_t ice_reset_pending;
	boolean_t ice_attaching;
	boolean_t ice_detaching;
	boolean_t ice_safe_mode;
	boolean_t ice_stat_port_loaded;
	boolean_t ice_stat_vsi_loaded;
	enum ice_ddp_state ice_ddp_state;
	void *ice_reset_taskq;
	void *ice_dip;
	struct ice_hw ice_hw;
} ice_t;

#include "ice_reset_types.h"

void ice_reset_task(void *);
void ice_reset_dispatch(ice_t *);
static ice_t device;
static unsigned queued, prepared, pfrs, global_waits, starts, errors;
static int fail_dispatch, fail_reset, start_result, resume_ok;
static unsigned down_reports;
static uint32_t at_lock, after_barrier, at_rearm, at_complete;
static uint32_t at_consume;

static void
request(uint32_t bits)
{
	device.ice_state |= bits | ICE_STATE_ERROR;
	ice_reset_dispatch(&device);
}

static void
mutex_enter(kmutex_t *lock)
{
	assert(!*lock);
	if (lock == &device.ice_rebuild_lock && at_lock != 0) {
		uint32_t bits = at_lock;

		at_lock = 0;
		request(bits);
	}
	*lock = 1;
}

static void
mutex_exit(kmutex_t *lock)
{
	assert(*lock);
	*lock = 0;
}

static uint32_t
atomic_cas_32(uint32_t *ptr, uint32_t old, uint32_t new)
{
	uint32_t found;

	if (at_consume != 0 && (old & OWED) != 0 && (new & OWED) == 0) {
		*ptr |= at_consume;
		at_consume = 0;
	}
	if (at_complete != 0 && (old & ICE_STATE_ERROR) != 0 &&
	    (new & ICE_STATE_ERROR) == 0) {
		*ptr |= at_complete | ICE_STATE_ERROR;
		at_complete = 0;
	}
	found = *ptr;
	if (found == old)
		*ptr = new;
	return (found);
}

static void
atomic_and_32(uint32_t *ptr, uint32_t bits)
{
	*ptr &= bits;
}

static void
atomic_or_32(uint32_t *ptr, uint32_t bits)
{
	*ptr |= bits;
}

static int
ddi_taskq_dispatch(void *queue, void (*task)(void *), void *arg, int flags)
{
	(void) queue;
	(void) flags;
	assert(task == ice_reset_task && arg == &device);
	if (fail_dispatch)
		return (-1);
	queued++;
	return (DDI_SUCCESS);
}

static void
ice_error(ice_t *ice, const char *format, ...)
{
	(void) ice;
	(void) format;
	errors++;
}

static void
dev_err(void *dip, int level, const char *message)
{
	(void) dip;
	(void) level;
	(void) message;
}

static void
ice_prepare_for_reset(ice_t *ice)
{
	assert(MUTEX_HELD(&ice->ice_rebuild_lock));
	prepared++;
	ice->ice_hw.reset_ongoing = true;
}

static int
ice_reset(struct ice_hw *hw, int type)
{
	(void) hw;
	assert(type == ICE_RESET_PFR);
	pfrs++;
	return (fail_reset);
}

static int
ice_check_reset(struct ice_hw *hw)
{
	(void) hw;
	global_waits++;
	return (fail_reset);
}

/* BEGIN CSTYLED */
#define	HW_OK(name) static int name(struct ice_hw *hw) \
	{ (void) hw; return (0); }
HW_OK(ice_init_all_ctrlq)
HW_OK(ice_sched_query_res_alloc)
HW_OK(ice_clear_pf_cfg)
HW_OK(ice_get_caps)

#define	ICE_NOOP(name) static void name(ice_t *ice) { (void) ice; }
ICE_NOOP(ice_tx_reclaim)
ICE_NOOP(ice_rx_reclaim)
ICE_NOOP(ice_loopback_replay)
ICE_NOOP(ice_queues_intr_map)
ICE_NOOP(ice_link_status_update)
ICE_NOOP(ice_setup_link)
ICE_NOOP(ice_phy_caps_update)
ICE_NOOP(ice_link_state_publish)
ICE_NOOP(ice_intr_oicr_disable)

static void
ice_stats_reset(ice_t *ice)
{
	assert(MUTEX_HELD(&ice->ice_rebuild_lock));
	ice->ice_stat_port_loaded = B_FALSE;
	ice->ice_stat_vsi_loaded = B_FALSE;
}

static void ice_clear_pxe_mode(struct ice_hw *hw) { (void) hw; }
static int ice_validate_caps(ice_t *ice) { (void) ice; return (1); }
static int ice_sched_init_port(void *port) { (void) port; return (0); }
static int ice_set_link_events(ice_t *ice) { (void) ice; return (1); }
static void ice_flush(struct ice_hw *hw) { (void) hw; }
/* END CSTYLED */

static enum ice_ddp_state
ice_init_pkg(struct ice_hw *hw, void *data, unsigned size)
{
	(void) hw;
	(void) data;
	(void) size;
	return (DDP_OK);
}
static int
ice_is_init_pkg_successful(enum ice_ddp_state s)
{
	return (s == DDP_OK);
}
static void
wr32(struct ice_hw *hw, unsigned reg, unsigned val)
{
	(void) hw;
	(void) reg;
	(void) val;
}
static void
ice_shutdown_all_ctrlq(struct ice_hw *hw, bool unloading)
{
	(void) hw;
	(void) unloading;
}
static void ice_link_loopback_update(ice_t *ice, int mode)
{
	(void) ice;
	(void) mode;
}
static void ice_link_report(ice_t *ice, int state)
{
	(void) ice;
	assert(state == LINK_STATE_DOWN);
	down_reports++;
}
static void ice_reset_set_failed(ice_t *ice)
{
	ice->ice_state |= ICE_STATE_RESET_FAILED | ICE_STATE_ERROR;
}
static int
ice_vsi_rebuild(ice_t *ice)
{
	(void) ice;
	if (after_barrier != 0) {
		uint32_t bits = after_barrier;

		after_barrier = 0;
		request(bits);
	}
	return (ICE_SUCCESS);
}
static void
ice_intr_oicr_setup(ice_t *ice, int harvest)
{
	(void) ice;
	assert(!harvest);
	if (at_rearm != 0) {
		uint32_t bits = at_rearm;

		at_rearm = 0;
		request(bits);
	}
}
static int ice_start_datapath(ice_t *ice)
{
	(void) ice;
	starts++;
	return (start_result);
}
static int ice_rx_rings_resume(ice_t *ice) { (void) ice; return (resume_ok); }

#include "ice_reset_body.h"

static void
reset(void)
{
	(void) memset(&device, 0, sizeof (device));
	device.ice_safe_mode = B_TRUE;
	queued = prepared = pfrs = global_waits = starts = errors = 0;
	fail_dispatch = fail_reset = start_result = 0;
	resume_ok = 1;
	down_reports = 0;
	at_lock = after_barrier = at_rearm = at_complete = at_consume = 0;
}

static void
run_one(void)
{
	assert(queued != 0);
	queued--;
	ice_reset_task(&device);
	assert(!device.ice_lock && !device.ice_rebuild_lock);
}

static void
check_coalescing(void)
{
	reset();
	request(ICE_STATE_PFR_REQ);
	/* Redispatch the same still-owed request while the worker waits. */
	at_lock = ICE_STATE_PFR_REQ;
	run_one();
	assert(queued == 0 && prepared == 1 && pfrs == 1);
	assert(!device.ice_reset_pending && (device.ice_state & OWED) == 0);

	/* A stale callback must not prepare or reset the hardware. */
	ice_reset_dispatch(&device);
	run_one();
	assert(prepared == 1 && pfrs == 1 && queued == 0);
}

static void
check_new_requests(void)
{
	reset();
	device.ice_state = ICE_STATE_STARTED;
	request(ICE_STATE_PFR_REQ);
	after_barrier = ICE_STATE_PFR_REQ;
	run_one();
	assert(pfrs == 1 && queued == 1 && starts == 0);
	assert((device.ice_state & (OWED | ICE_STATE_ERROR)) ==
	    (ICE_STATE_PFR_REQ | ICE_STATE_ERROR));
	run_one();
	assert(pfrs == 2 && queued == 0 && starts == 1);

	reset();
	device.ice_state = ICE_STATE_STARTED;
	request(ICE_STATE_PFR_REQ);
	at_rearm = ICE_STATE_RESET_PENDING;
	run_one();
	assert(pfrs == 1 && queued == 1 && starts == 0);
	run_one();
	assert(global_waits == 1 && pfrs == 1 && starts == 1);

	/* A request racing completion's CAS cannot lose its fail-closed bit. */
	reset();
	device.ice_state = ICE_STATE_STARTED;
	request(ICE_STATE_PFR_REQ);
	at_complete = ICE_STATE_PFR_REQ;
	run_one();
	assert(queued == 1 && starts == 0);
	assert((device.ice_state & ICE_STATE_ERROR) != 0);
	run_one();
	assert(pfrs == 2 && starts == 1 && queued == 0);

	/* CAS retry consumes both reset types and preserves unrelated state. */
	reset();
	request(ICE_STATE_PFR_REQ);
	at_consume = ICE_STATE_RESET_PENDING | ICE_STATE_MDD_PENDING;
	run_one();
	assert(global_waits == 1 && pfrs == 0 && queued == 0);
	assert((device.ice_state & ICE_STATE_MDD_PENDING) != 0);
}

static void
check_gates(void)
{
	unsigned gate;

	for (gate = 0; gate < 2; gate++) {
		reset();
		device.ice_attaching = gate == 0;
		device.ice_detaching = gate == 1;
		request(ICE_STATE_PFR_REQ);
		run_one();
		assert(prepared == 0 && queued == 0);
		assert(!device.ice_reset_pending);
		assert((device.ice_state & ICE_STATE_PFR_REQ) != 0);
		device.ice_attaching = device.ice_detaching = B_FALSE;
		mutex_enter(&device.ice_rebuild_lock);
		ice_reset_redispatch(&device);
		mutex_exit(&device.ice_rebuild_lock);
		run_one();
		assert(pfrs == 1 && queued == 0);
	}

	reset();
	fail_dispatch = 1;
	request(ICE_STATE_PFR_REQ);
	assert(queued == 0 && !device.ice_reset_pending && errors == 1);
	assert((device.ice_state & ICE_STATE_PFR_REQ) != 0);
	fail_dispatch = 0;
	ice_reset_dispatch(&device);
	run_one();
	assert(pfrs == 1);

	reset();
	fail_reset = 1;
	request(ICE_STATE_RESET_PENDING);
	run_one();
	assert(global_waits == 1 && queued == 0);
	assert((device.ice_state & ICE_STATE_RESET_FAILED) != 0);
	assert((device.ice_state & OWED) == 0);
	ice_reset_dispatch(&device);
	run_one();
	assert(global_waits == 1 && queued == 0);
}

static void
check_restart_failure(void)
{
	unsigned phase;

	for (phase = 0; phase < 2; phase++) {
		reset();
		device.ice_state = ICE_STATE_STARTED;
		start_result = phase == 0 ? -1 : 0;
		resume_ok = phase == 0;
		request(ICE_STATE_PFR_REQ);
		run_one();
		assert(starts == 1 && down_reports == 1 && queued == 0);
		assert((device.ice_state & ICE_STATE_ERROR) != 0);
		assert((device.ice_state & ICE_STATE_RESET_FAILED) == 0);
	}
}

int
main(void)
{
	check_coalescing();
	check_new_requests();
	check_gates();
	check_restart_failure();
	(void) puts("PASS: ICE reset ownership and rebuild interleavings");
	return (0);
}

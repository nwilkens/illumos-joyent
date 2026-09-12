/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/* Real TX control flow; only the ring, DMA, MAC, and kernel boundaries vary. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define	B_TRUE true
#define	B_FALSE false
#define	DDI_FM_OK 0
#define	DDI_SERVICE_DEGRADED 1
#define	ASSERT(x) assert(x)
#define	ASSERT3U(a, op, b) assert(a op b)
#define	MUTEX_HELD(m) ((m)->held && pthread_equal((m)->owner, pthread_self()))
#define	RING_SIZE 4

typedef bool boolean_t;
typedef unsigned int uint_t;
typedef struct {
	pthread_mutex_t native;
	pthread_t owner;
	boolean_t held;
} kmutex_t;
typedef pthread_cond_t kcondvar_t;
typedef struct ice ice_t;
typedef struct { unsigned int id; } ice_tx_ctrl_block_t;
typedef struct ice_tx_ring {
	ice_t *itxr_ice;
	kmutex_t itxr_lock;
	kcondvar_t itxr_cv;
	uint16_t itxr_head, itxr_avail, itxr_size;
	uint16_t itxr_rs_cidx, itxr_rs_pidx;
	uint16_t itxr_rsq[RING_SIZE];
	ice_tx_ctrl_block_t *itxr_tcbs[RING_SIZE];
	struct {
		uint64_t buf_addr, cmd_type_offset_bsz;
	} itxr_descs[RING_SIZE];
	struct { int idb_dma_handle; } itxr_dma;
	void *itxr_mactxring;
	boolean_t itxr_blocked, itxr_quiesce;
	uint_t itxr_tx_active;
} ice_tx_ring_t;
struct ice {
	ice_tx_ring_t *ice_txr;
	uint_t ice_num_txr;
	void *ice_mac_hdl, *ice_dip;
	uint32_t ice_state;
};

static ice_t device;
static ice_tx_ring_t ring;
static ice_tx_ctrl_block_t block;
static struct {
	unsigned int notices, frees, probes;
	boolean_t hold_notice, notice_entered, release_notice;
	boolean_t quiesce_attempted, quiesce_done, wait_entered;
	pthread_t quiesce_thread;
	boolean_t quiesce_thread_valid;
} fixture;
static pthread_mutex_t schedule_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_cv = PTHREAD_COND_INITIALIZER;

static void
mutex_enter(kmutex_t *m)
{
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	if (fixture.quiesce_thread_valid &&
	    pthread_equal(fixture.quiesce_thread, pthread_self())) {
		fixture.quiesce_attempted = B_TRUE;
		assert(pthread_cond_broadcast(&schedule_cv) == 0);
	}
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
	assert(pthread_mutex_lock(&m->native) == 0);
	m->owner = pthread_self();
	m->held = B_TRUE;
}

static void
mutex_exit(kmutex_t *m)
{
	assert(MUTEX_HELD(m));
	m->held = B_FALSE;
	assert(pthread_mutex_unlock(&m->native) == 0);
}

static void
cv_wait(kcondvar_t *cv, kmutex_t *m)
{
	assert(MUTEX_HELD(m));
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	fixture.wait_entered = B_TRUE;
	assert(pthread_cond_broadcast(&schedule_cv) == 0);
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
	m->held = B_FALSE;
	assert(pthread_cond_wait(cv, &m->native) == 0);
	m->owner = pthread_self();
	m->held = B_TRUE;
}

static void
atomic_or_32(uint32_t *p, uint32_t value)
{
	*p |= value;
}

static uint16_t
ice_tx_ring_next(ice_tx_ring_t *itr, uint16_t slot)
{
	return ((slot + 1) % itr->itxr_size);
}

static boolean_t
ice_tx_desc_done(ice_tx_ring_t *itr, uint16_t slot)
{
	assert(MUTEX_HELD(&itr->itxr_lock));
	fixture.probes++;
	return (itr->itxr_descs[slot].cmd_type_offset_bsz != 0);
}

static int
ice_check_dma_handle(int handle)
{
	(void) handle;
	return (DDI_FM_OK);
}

static void
ddi_fm_service_impact(void *dip, int impact)
{
	(void) dip;
	(void) impact;
	assert(!"unexpected DMA error");
}

static void
ice_tcb_free(ice_tx_ring_t *itr, ice_tx_ctrl_block_t *tcb)
{
	assert(MUTEX_HELD(&itr->itxr_lock) && tcb == &block);
	fixture.frees++;
}

static void
mac_tx_ring_update(void *mac, void *macring)
{
	assert(mac == &device && macring == &ring);
	assert(MUTEX_HELD(&ring.itxr_lock));
	fixture.notices++;
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	fixture.notice_entered = B_TRUE;
	assert(pthread_cond_broadcast(&schedule_cv) == 0);
	while (fixture.hold_notice && !fixture.release_notice)
		assert(pthread_cond_wait(&schedule_cv, &schedule_lock) == 0);
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
}

#include "ice_tx_quiesce_body.h"

static void
init(boolean_t completed)
{
	(void) memset(&fixture, 0, sizeof (fixture));
	(void) memset(&device, 0, sizeof (device));
	(void) memset(&ring, 0, sizeof (ring));
	assert(pthread_mutex_init(&ring.itxr_lock.native, NULL) == 0);
	assert(pthread_cond_init(&ring.itxr_cv, NULL) == 0);
	device.ice_txr = &ring;
	device.ice_num_txr = 1;
	device.ice_mac_hdl = &device;
	ring.itxr_ice = &device;
	ring.itxr_mactxring = &ring;
	ring.itxr_size = ring.itxr_avail = RING_SIZE;
	ring.itxr_blocked = B_TRUE;
	if (completed) {
		ring.itxr_avail--;
		ring.itxr_rs_pidx = 1;
		ring.itxr_rsq[0] = 0;
		ring.itxr_tcbs[0] = &block;
		ring.itxr_descs[0].buf_addr = 42;
		ring.itxr_descs[0].cmd_type_offset_bsz = 1;
	}
}

static void
fini(void)
{
	assert(pthread_cond_destroy(&ring.itxr_cv) == 0);
	assert(pthread_mutex_destroy(&ring.itxr_lock.native) == 0);
}

static void
late_interrupt(boolean_t completed)
{
	init(completed);
	ice_tx_quiesce(&device);
	ice_tx_ring_intr(&ring);
	assert(ring.itxr_quiesce && !ring.itxr_blocked);
	assert(fixture.notices == 0 && fixture.frees == 0 &&
	    fixture.probes == 0);
	assert(ring.itxr_avail == RING_SIZE - (completed ? 1 : 0));
	assert(ring.itxr_tcbs[0] == (completed ? &block : NULL));
	if (completed)
		assert(ring.itxr_descs[0].buf_addr == 42);
	fini();
}

static void empty_late(void) { late_interrupt(B_FALSE); }
static void completed_late(void) { late_interrupt(B_TRUE); }

static void
healthy_recycle(void)
{
	init(B_FALSE);
	ice_tx_ring_intr(&ring);
	assert(fixture.notices == 1 && fixture.frees == 0);
	assert(!ring.itxr_blocked);
	fini();
	init(B_TRUE);
	ice_tx_ring_intr(&ring);
	assert(fixture.notices == 1 && fixture.frees == 1);
	assert(!ring.itxr_blocked && ring.itxr_avail == RING_SIZE);
	assert(ring.itxr_tcbs[0] == NULL);
	fini();
}

static void *
quiesce_worker(void *arg)
{
	(void) arg;
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	fixture.quiesce_thread = pthread_self();
	fixture.quiesce_thread_valid = B_TRUE;
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
	ice_tx_quiesce(&device);
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	fixture.quiesce_done = B_TRUE;
	assert(pthread_cond_broadcast(&schedule_cv) == 0);
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
	return (NULL);
}

static void
active_builder(void)
{
	pthread_t worker;

	init(B_FALSE);
	ring.itxr_blocked = B_FALSE;
	ring.itxr_tx_active = 1;
	assert(pthread_create(&worker, NULL, quiesce_worker, NULL) == 0);
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	while (!fixture.wait_entered)
		assert(pthread_cond_wait(&schedule_cv, &schedule_lock) == 0);
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
	/* An admitted builder can re-arm backpressure while quiesce waits. */
	mutex_enter(&ring.itxr_lock);
	assert(ring.itxr_quiesce);
	ring.itxr_blocked = B_TRUE;
	ring.itxr_tx_active = 0;
	assert(pthread_cond_signal(&ring.itxr_cv) == 0);
	mutex_exit(&ring.itxr_lock);
	assert(pthread_join(worker, NULL) == 0);
	assert(!ring.itxr_blocked && fixture.notices == 0 &&
	    fixture.frees == 0);
	fini();
}

static void *
interrupt_worker(void *arg)
{
	(void) arg;
	ice_tx_ring_intr(&ring);
	return (NULL);
}

static void
prior_notification(void)
{
	pthread_t interrupt, quiesce;

	init(B_FALSE);
	fixture.hold_notice = B_TRUE;
	assert(pthread_create(&interrupt, NULL, interrupt_worker, NULL) == 0);
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	while (!fixture.notice_entered)
		assert(pthread_cond_wait(&schedule_cv, &schedule_lock) == 0);
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
	assert(pthread_create(&quiesce, NULL, quiesce_worker, NULL) == 0);
	assert(pthread_mutex_lock(&schedule_lock) == 0);
	while (!fixture.quiesce_attempted)
		assert(pthread_cond_wait(&schedule_cv, &schedule_lock) == 0);
	assert(!fixture.quiesce_done);
	assert(pthread_mutex_trylock(&ring.itxr_lock.native) == EBUSY);
	fixture.release_notice = B_TRUE;
	assert(pthread_cond_broadcast(&schedule_cv) == 0);
	assert(pthread_mutex_unlock(&schedule_lock) == 0);
	assert(pthread_join(interrupt, NULL) == 0);
	assert(pthread_join(quiesce, NULL) == 0);
	assert(fixture.quiesce_done && ring.itxr_quiesce);
	ice_tx_ring_intr(&ring);
	assert(fixture.notices == 1 && fixture.frees == 0);
	fini();
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] = {
		{ "empty_late", empty_late },
		{ "completed_late", completed_late },
		{ "healthy_recycle", healthy_recycle },
		{ "active_builder", active_builder },
		{ "prior_notification", prior_notification }
	};
	unsigned int i, ran = 0;

	for (i = 0; i < sizeof (cases) / sizeof (cases[0]); i++) {
		if (argc == 2 && strcmp(argv[1], cases[i].name) != 0)
			continue;
		cases[i].run();
		(void) printf("PASS: %s\n", cases[i].name);
		ran++;
	}
	assert(ran != 0);
	return (0);
}

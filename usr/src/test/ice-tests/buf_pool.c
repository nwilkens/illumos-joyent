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

/*
 * Run the actual TX pool sizing, per-ring pool lifetime, and the deferred LSO
 * allocation with DDI, taskq and MAC boundaries substituted.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int uint_t;
typedef int boolean_t;
typedef char *caddr_t;
typedef int kmutex_t;
typedef void *ddi_acc_handle_t;
typedef void *ddi_dma_handle_t;
typedef struct { uint64_t dmac_laddress; } ddi_dma_cookie_t;
typedef int ddi_dma_attr_t;
typedef int ddi_device_acc_attr_t;
typedef int taskq_t;
typedef int taskq_ent_t;
typedef void *mac_handle_t;
typedef void *mac_ring_handle_t;
#define	B_TRUE 1
#define	B_FALSE 0
#define	KM_SLEEP 1
#define	TASKQ_PREPOPULATE 1
#define	minclsyspri 60
#define	MIN(a, b) ((a) < (b) ? (a) : (b))
#define	ICE_TX_SMALL_PKT 512
#define	ICE_TX_COPY_BUFSZ 12288
#define	ICE_TX_LSO_BUFSZ 12288
#define	ICE_MAX_FRAME_SIZE 9728
#define	ICE_TX_MAX_BUFSZ 16383
#define	ICE_MAX_QUEUES 127
#define	ICE_LSO_MAXLEN (64 * 1024)
#define	ASSERT3U(a, op, b) assert((a) op(b))
#define	VERIFY3U(a, op, b) assert((a) op(b))
#define	ASSERT3P(a, op, b) assert((a) op(b))
#define	ASSERT(x) assert(x)
#include "ice_pool_types.h"

typedef struct ice_tx_ring {
	struct ice *itxr_ice;
	uint32_t itxr_index;
	uint16_t itxr_size;
	kmutex_t itxr_lock;
	kmutex_t itxr_tcb_lock;
	boolean_t itxr_blocked;
	boolean_t itxr_quiesce;
	mac_ring_handle_t itxr_mactxring;
	ice_buf_pool_t itxr_copy_pool;
	ice_buf_pool_t itxr_small_pool;
	ice_buf_pool_t itxr_lso_pool;
	ice_tx_lso_state_t itxr_lso_state;
	taskq_ent_t itxr_lso_ent;
	struct {
		struct {
			struct {
				uint64_t ui64;
			} value;
		} ictxs_blocked, ictxs_lso_nores;
	} itxr_stats;
	/* Test bookkeeping. */
	boolean_t lso_handles;
	unsigned wakes;
} ice_tx_ring_t;

typedef struct ice {
	int ice_instance;
	uint_t ice_num_txr;
	ice_tx_ring_t *ice_txr;
	taskq_t *ice_tx_taskq;
	mac_handle_t ice_mac_hdl;
} ice_t;

static unsigned locks, allocations, dma_live, attempts, fail_at;
static unsigned handle_fail, taskqs, errors;
static taskq_t the_taskq;

static void
mutex_enter(kmutex_t *lock)
{
	assert(*lock == 0);
	*lock = 1;
	locks++;
}

static void
mutex_exit(kmutex_t *lock)
{
	assert(*lock == 1 && locks > 0);
	*lock = 0;
	locks--;
}

static void
membar_producer(void)
{
}

static void *
kmem_zalloc(size_t size, int flags)
{
	void *p;

	assert(flags == KM_SLEEP);
	/* Sleeping allocations cannot hold pool or ring locks. */
	assert(locks == 0);
	p = calloc(1, size);
	assert(p != NULL);
	allocations++;
	return (p);
}

static void
kmem_free(void *p, size_t size)
{
	(void) size;
	assert(p != NULL && allocations > 0 && locks == 0);
	allocations--;
	free(p);
}

static void
ice_pkt_dma_attr(ice_t *ice, ddi_dma_attr_t *attr)
{
	(void) ice;
	*attr = 1;
}

static void
ice_dma_acc_attr(ice_t *ice, ddi_device_acc_attr_t *attr)
{
	(void) ice;
	*attr = 1;
}

static void
ice_error(ice_t *ice, const char *fmt, ...)
{
	(void) ice;
	(void) fmt;
	errors++;
}

/* Packet memory is not touched here; each buffer is a counted token. */
static char token;

static boolean_t
ice_dma_alloc(ice_t *ice, ice_dma_buffer_t *buf, ddi_dma_attr_t *attr,
    ddi_device_acc_attr_t *acc, boolean_t zero, size_t size, boolean_t sleep)
{
	(void) ice;
	assert(*attr == 1 && *acc == 1 && zero && sleep && locks == 0);
	if (++attempts == fail_at)
		return (B_FALSE);
	buf->idb_va = &token;
	buf->idb_len = size;
	dma_live++;
	return (B_TRUE);
}

static void
ice_dma_free(ice_dma_buffer_t *buf)
{
	assert(locks == 0);
	if (buf->idb_va != NULL) {
		assert(dma_live > 0);
		dma_live--;
		buf->idb_va = NULL;
		buf->idb_len = 0;
	}
}

static taskq_t *
taskq_create_instance(const char *name, int instance, int nthreads, int pri,
    int minalloc, int maxalloc, uint_t flags)
{
	(void) name;
	(void) instance;
	(void) pri;
	(void) minalloc;
	(void) maxalloc;
	assert(nthreads == 1 && flags == TASKQ_PREPOPULATE && taskqs == 0);
	taskqs++;
	return (&the_taskq);
}

static ice_t *current;

/* The task finishes before any pool is freed. */
static void
taskq_destroy(taskq_t *tq)
{
	uint_t i;

	assert(tq == &the_taskq && taskqs == 1);
	for (i = 0; i < current->ice_num_txr; i++) {
		ice_tx_ring_t *itr = &current->ice_txr[i];

		assert(itr->itxr_lso_state != ICE_TX_LSO_PENDING);
		assert(itr->itxr_copy_pool.ibp_nfree ==
		    itr->itxr_copy_pool.ibp_nbufs);
	}
	taskqs--;
}

static boolean_t
ice_tcb_lso_handles_alloc(ice_t *ice, ice_tx_ring_t *itr)
{
	(void) ice;
	assert(locks == 0 && !itr->lso_handles);
	if (handle_fail)
		return (B_FALSE);
	itr->lso_handles = B_TRUE;
	return (B_TRUE);
}

static void
ice_tcb_lso_handles_free(ice_tx_ring_t *itr)
{
	assert(locks == 0);
	itr->lso_handles = B_FALSE;
}

static void
mac_tx_ring_update(mac_handle_t mh, mac_ring_handle_t rh)
{
	ice_tx_ring_t *itr = rh;

	assert(mh == current && itr->itxr_lock == 1);
	itr->wakes++;
}

static void
membar_consumer(void)
{
}

typedef void (task_func_t)(void *);
static task_func_t *queued_func;
static void *queued_arg;
static unsigned dispatches;

/* The entry is dispatched once, outside the ring lock. */
static void
taskq_dispatch_ent(taskq_t *tq, task_func_t func, void *arg, uint_t flags,
    taskq_ent_t *ent)
{
	ice_tx_ring_t *itr = arg;

	assert(tq == &the_taskq && flags == 0 && locks == 0);
	assert(ent == &itr->itxr_lso_ent && queued_func == NULL);
	assert(itr->itxr_lso_state == ICE_TX_LSO_PENDING);
	queued_func = func;
	queued_arg = arg;
	dispatches++;
}

void ice_buf_fini(ice_t *);
void ice_tx_lso_fini(ice_tx_ring_t *);
void ice_tx_lso_task(void *);
#include "ice_pool_code.h"

static ice_t *
make(uint_t nrings, uint16_t size)
{
	ice_t *ice = calloc(1, sizeof (*ice));
	uint_t i;

	assert(ice != NULL);
	ice->ice_num_txr = nrings;
	ice->ice_txr = calloc(nrings, sizeof (*ice->ice_txr));
	assert(ice->ice_txr != NULL);
	ice->ice_mac_hdl = ice;
	for (i = 0; i < nrings; i++) {
		ice->ice_txr[i].itxr_ice = ice;
		ice->ice_txr[i].itxr_index = i;
		ice->ice_txr[i].itxr_size = size;
		ice->ice_txr[i].itxr_mactxring = &ice->ice_txr[i];
	}
	current = ice;
	return (ice);
}

static void
destroy(ice_t *ice)
{
	free(ice->ice_txr);
	free(ice);
	current = NULL;
}

/*
 * Pool sizes depend on the ring count only.  The descriptor count does not
 * enter, the totals stay within the caps, and no LSO buffer exists yet.
 */
static void
check_sizing(void)
{
	static const uint_t rings[] = { 1, 2, 7, 16, 32, 64, 127 };
	static const uint16_t sizes[] = { 64, 1024, 4096 };
	uint_t r, z, i;

	for (r = 0; r < sizeof (rings) / sizeof (rings[0]); r++) {
		uint_t n = rings[r];
		uint_t ncopy = MIN(ICE_TX_COPY_BUFS_RING,
		    ICE_TX_COPY_BUFS_MAX / n);
		uint_t nsmall = MIN(ICE_TX_SMALL_BUFS_RING,
		    ICE_TX_SMALL_BUFS_MAX / n);
		uint_t nlso = ice_tx_pool_bufs(ICE_TX_LSO_BUFS_RING,
		    ICE_TX_LSO_BUFS_MAX, n);

		assert(ncopy * n <= ICE_TX_COPY_BUFS_MAX && ncopy >= 1);
		assert(nsmall * n <= ICE_TX_SMALL_BUFS_MAX && nsmall >= 1);
		assert(nlso * n <= ICE_TX_LSO_BUFS_MAX);
		/* The largest LSO packet copies seven payload buffers. */
		assert(nlso >= 7);

		for (z = 0; z < sizeof (sizes) / sizeof (sizes[0]); z++) {
			ice_t *ice = make(n, sizes[z]);

			attempts = 0;
			fail_at = 0;
			assert(ice_buf_init(ice));
			assert(dma_live == n * (ncopy + nsmall));
			for (i = 0; i < n; i++) {
				ice_tx_ring_t *itr = &ice->ice_txr[i];

				assert(itr->itxr_copy_pool.ibp_nbufs == ncopy);
				assert(itr->itxr_small_pool.ibp_nbufs ==
				    nsmall);
				assert(itr->itxr_lso_pool.ibp_nbufs == 0);
				assert(itr->itxr_lso_pool.ibp_bufs == NULL);
				assert(itr->itxr_lso_state ==
				    ICE_TX_LSO_NONE && !itr->lso_handles);
				assert(itr->itxr_copy_pool.ibp_lock ==
				    &itr->itxr_tcb_lock);
				assert(itr->itxr_small_pool.ibp_lock ==
				    &itr->itxr_tcb_lock);
				assert(itr->itxr_lso_pool.ibp_lock ==
				    &itr->itxr_tcb_lock);
			}
			ice_buf_fini(ice);
			assert(dma_live == 0 && allocations == 0);
			assert(taskqs == 0 && ice->ice_tx_taskq == NULL);
			destroy(ice);
		}
	}
}

/* Each ring's buffers go back to that ring's pools. */
static void
check_stacks(void)
{
	ice_t *ice = make(2, 1024);
	ice_tx_ring_t *a = &ice->ice_txr[0], *b = &ice->ice_txr[1];
	ice_dma_buffer_t *held[ICE_TX_COPY_BUFS_RING], *other, *small;
	uint_t n, i;

	attempts = fail_at = 0;
	assert(ice_buf_init(ice));
	n = a->itxr_copy_pool.ibp_nbufs;
	assert(n == ICE_TX_COPY_BUFS_RING);
	for (i = 0; i < n; i++) {
		held[i] = ice_buf_alloc(a);
		assert(held[i] != NULL);
		assert(held[i]->idb_len == ICE_TX_COPY_BUFSZ);
	}
	/* One ring's empty pool leaves the other ring's pool alone. */
	assert(ice_buf_alloc(a) == NULL);
	other = ice_buf_alloc(b);
	assert(other != NULL && other->idb_pool == &b->itxr_copy_pool);
	small = ice_small_buf_alloc(a);
	assert(small != NULL && small->idb_len == ICE_TX_SMALL_PKT);
	/* No LSO pool exists before the first LSO packet. */
	assert(ice_lso_buf_alloc(a) == NULL);
	ice_buf_free(NULL);
	ice_buf_free(other);
	assert(b->itxr_copy_pool.ibp_nfree == n);
	assert(a->itxr_copy_pool.ibp_nfree == 0);
	ice_buf_free(small);
	for (i = 0; i < n; i++) {
		ice_buf_free(held[i]);
		assert(a->itxr_copy_pool.ibp_nfree == i + 1);
	}
	assert(locks == 0);
	ice_buf_fini(ice);
	assert(dma_live == 0 && allocations == 0);
	destroy(ice);
}

/* Every partial construction is released. */
static void
check_failures(void)
{
	uint_t total = 2 * (ICE_TX_COPY_BUFS_RING + ICE_TX_SMALL_BUFS_RING);
	uint_t failure;

	for (failure = 1; failure <= total; failure++) {
		ice_t *ice = make(2, 1024);

		attempts = 0;
		fail_at = failure;
		errors = 0;
		assert(!ice_buf_init(ice));
		assert(errors == 1);
		assert(dma_live == 0 && allocations == 0 && taskqs == 0);
		/* Cleanup is repeatable. */
		ice_buf_fini(ice);
		destroy(ice);
	}
}

/*
 * The first LSO packet queues the task.  It allocates the pool and handles,
 * publishes READY and wakes MAC unless the ring was closed meanwhile; a
 * failure publishes FAILED and still wakes MAC so the packet is dropped.
 */
static void
check_lso_task(void)
{
	uint_t n, fail, quiesce;

	for (fail = 0; fail <= 3; fail++) {
		for (quiesce = 0; quiesce <= 1; quiesce++) {
			ice_t *ice = make(16, 1024);
			ice_tx_ring_t *itr = &ice->ice_txr[3];

			attempts = fail_at = 0;
			assert(ice_buf_init(ice));
			n = ice_tx_pool_bufs(ICE_TX_LSO_BUFS_RING,
			    ICE_TX_LSO_BUFS_MAX, 16);
			itr->itxr_lso_state = ICE_TX_LSO_PENDING;
			itr->itxr_blocked = B_TRUE;
			itr->itxr_quiesce = (boolean_t)quiesce;
			attempts = 0;
			fail_at = fail == 1 ? 1 : fail == 2 ? n : 0;
			handle_fail = fail == 3;
			errors = 0;
			ice_tx_lso_task(itr);
			assert(locks == 0 && itr->itxr_lock == 0);
			if (fail == 0) {
				assert(itr->itxr_lso_state ==
				    ICE_TX_LSO_READY);
				assert(itr->itxr_lso_pool.ibp_nbufs == n);
				assert(itr->itxr_lso_pool.ibp_nfree == n);
				assert(itr->lso_handles && errors == 0);
				{
					ice_dma_buffer_t *lso;

					lso = ice_lso_buf_alloc(itr);
					assert(lso->idb_len ==
					    ICE_TX_LSO_BUFSZ);
					ice_buf_free(lso);
				}
			} else {
				assert(itr->itxr_lso_state ==
				    ICE_TX_LSO_FAILED);
				assert(itr->itxr_lso_pool.ibp_bufs == NULL);
				assert(!itr->lso_handles && errors == 1);
			}
			assert(itr->wakes == (quiesce ? 0u : 1u));
			assert(itr->itxr_blocked == (boolean_t)quiesce);
			/* Only the ring that sent LSO has LSO buffers. */
			assert(ice->ice_txr[2].itxr_lso_pool.ibp_bufs ==
			    NULL);

			/* Stop releases the pool and resets the state. */
			ice_tx_lso_fini(itr);
			assert(itr->itxr_lso_state == ICE_TX_LSO_NONE);
			assert(itr->itxr_lso_pool.ibp_bufs == NULL);
			assert(!itr->lso_handles);

			/* Detach releases a ready pool as well. */
			if (fail == 0) {
				itr->itxr_lso_state = ICE_TX_LSO_PENDING;
				attempts = fail_at = 0;
				ice_tx_lso_task(itr);
				assert(itr->itxr_lso_state ==
				    ICE_TX_LSO_READY);
			}
			handle_fail = 0;
			ice_buf_fini(ice);
			assert(dma_live == 0 && allocations == 0);
			assert(!itr->lso_handles && taskqs == 0);
			destroy(ice);
		}
	}
}

/*
 * The TX path's view: LSO packets block the ring until the task has run once,
 * then pass; a failed allocation drops them.
 */
static void
check_lso_admission(void)
{
	ice_t *ice = make(4, 1024);
	ice_tx_ring_t *itr = &ice->ice_txr[1];

	attempts = fail_at = 0;
	dispatches = 0;
	assert(ice_buf_init(ice));
	assert(ice_tx_lso_resources(itr) == ICE_TX_BUILD_NORES);
	assert(dispatches == 1 && itr->itxr_blocked);
	assert(itr->itxr_lso_pool.ibp_bufs == NULL);
	/* Later packets wait on the same queued task. */
	assert(ice_tx_lso_resources(itr) == ICE_TX_BUILD_NORES);
	assert(dispatches == 1);
	assert(itr->itxr_stats.ictxs_blocked.value.ui64 == 2);
	assert(itr->itxr_stats.ictxs_lso_nores.value.ui64 == 2);

	queued_func(queued_arg);
	queued_func = NULL;
	assert(itr->wakes == 1 && !itr->itxr_blocked);
	assert(ice_tx_lso_resources(itr) == ICE_TX_BUILD_OK);
	assert(itr->itxr_lso_pool.ibp_nbufs ==
	    ice_tx_pool_bufs(ICE_TX_LSO_BUFS_RING, ICE_TX_LSO_BUFS_MAX, 4));
	assert(dispatches == 1 && locks == 0);
	assert(ice->ice_txr[0].itxr_lso_pool.ibp_bufs == NULL);

	/* After a stop the next LSO packet allocates again. */
	ice_tx_lso_fini(itr);
	handle_fail = 1;
	assert(ice_tx_lso_resources(itr) == ICE_TX_BUILD_NORES);
	queued_func(queued_arg);
	queued_func = NULL;
	assert(dispatches == 2 && itr->wakes == 2);
	assert(ice_tx_lso_resources(itr) == ICE_TX_BUILD_DROP);
	assert(dispatches == 2 && !itr->itxr_blocked);
	handle_fail = 0;

	ice_buf_fini(ice);
	assert(dma_live == 0 && allocations == 0);
	destroy(ice);
}

int
main(void)
{
	check_sizing();
	check_stacks();
	check_failures();
	check_lso_task();
	check_lso_admission();
	(void) printf("PASS: TX pools sized per ring within caps; LSO pool "
	    "allocated on first use\n");
	return (0);
}

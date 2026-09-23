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
 * Run the actual TX pool sizing, per-ring pool lifetime, and the LSO pool
 * allocation at MAC start with the DDI boundaries substituted.
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
typedef struct {
	int marker;
	uint64_t dma_attr_align;
} ddi_dma_attr_t;
typedef int ddi_device_acc_attr_t;
#define	B_TRUE 1
#define	B_FALSE 0
#define	KM_SLEEP 1
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
#define	MUTEX_HELD(p) (*(p) != 0)
#include "ice_pool_types.h"

typedef struct ice_tx_ring {
	struct ice *itxr_ice;
	uint32_t itxr_index;
	uint16_t itxr_size;
	kmutex_t itxr_tcb_lock;
	ice_buf_pool_t itxr_copy_pool;
	ice_buf_pool_t itxr_small_pool;
	ice_buf_pool_t itxr_lso_pool;
	/* Test bookkeeping. */
	boolean_t lso_handles;
} ice_tx_ring_t;

typedef struct ice {
	int ice_instance;
	uint_t ice_num_txr;
	ice_tx_ring_t *ice_txr;
	boolean_t ice_tx_lso_enable;
	kmutex_t ice_rebuild_lock;
} ice_t;

static unsigned locks, allocations, dma_live, attempts, fail_at;
static unsigned handle_fail, errors;

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
	attr->marker = 1;
	attr->dma_attr_align = 0x1000;
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
	assert(attr->marker == 1 && *acc == 1 && zero && sleep && locks == 0);
	/* A sub-page buffer must not be page aligned; the others are. */
	assert(attr->dma_attr_align ==
	    (size <= ICE_TX_SMALL_PKT ? ICE_TX_SMALL_ALIGN : 0x1000));
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

static ice_t *current;

/* handle_fail names the ring, plus one, whose handles cannot be had. */
static boolean_t
ice_tcb_lso_handles_alloc(ice_t *ice, ice_tx_ring_t *itr)
{
	assert(ice == current && MUTEX_HELD(&ice->ice_rebuild_lock));
	assert(locks == 0 && !itr->lso_handles);
	if (handle_fail == itr->itxr_index + 1)
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

void ice_buf_fini(ice_t *);
boolean_t ice_tx_lso_alloc(ice_t *);
void ice_tx_lso_free(ice_t *);
ice_dma_buffer_t *ice_buf_take(ice_buf_pool_t *);
void ice_buf_put(ice_dma_buffer_t *);
#include "ice_pool_code.h"

/* The TX path takes and returns buffers under the pool (TCB) lock. */
static ice_dma_buffer_t *
take(ice_buf_pool_t *pool)
{
	ice_dma_buffer_t *buf;

	mutex_enter(pool->ibp_lock);
	buf = ice_buf_take(pool);
	mutex_exit(pool->ibp_lock);
	return (buf);
}

static void
put(ice_dma_buffer_t *buf)
{
	mutex_enter(buf->idb_pool->ibp_lock);
	ice_buf_put(buf);
	mutex_exit(buf->idb_pool->ibp_lock);
}

static ice_t *
make(uint_t nrings, uint16_t size)
{
	ice_t *ice = calloc(1, sizeof (*ice));
	uint_t i;

	assert(ice != NULL);
	ice->ice_num_txr = nrings;
	ice->ice_txr = calloc(nrings, sizeof (*ice->ice_txr));
	assert(ice->ice_txr != NULL);
	ice->ice_tx_lso_enable = B_TRUE;
	for (i = 0; i < nrings; i++) {
		ice->ice_txr[i].itxr_ice = ice;
		ice->ice_txr[i].itxr_index = i;
		ice->ice_txr[i].itxr_size = size;
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
				assert(!itr->lso_handles);
				assert(itr->itxr_copy_pool.ibp_lock ==
				    &itr->itxr_tcb_lock);
				assert(itr->itxr_small_pool.ibp_lock ==
				    &itr->itxr_tcb_lock);
				assert(itr->itxr_lso_pool.ibp_lock ==
				    &itr->itxr_tcb_lock);
			}
			ice_buf_fini(ice);
			assert(dma_live == 0 && allocations == 0);
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
		held[i] = take(&a->itxr_copy_pool);
		assert(held[i] != NULL);
		assert(held[i]->idb_len == ICE_TX_COPY_BUFSZ);
	}
	/* One ring's empty pool leaves the other ring's pool alone. */
	assert(take(&a->itxr_copy_pool) == NULL);
	other = take(&b->itxr_copy_pool);
	assert(other != NULL && other->idb_pool == &b->itxr_copy_pool);
	small = take(&a->itxr_small_pool);
	assert(small != NULL && small->idb_len == ICE_TX_SMALL_PKT);
	/* No LSO pool exists before MAC start. */
	assert(take(&a->itxr_lso_pool) == NULL);
	put(other);
	assert(b->itxr_copy_pool.ibp_nfree == n);
	assert(a->itxr_copy_pool.ibp_nfree == 0);
	put(small);
	for (i = 0; i < n; i++) {
		put(held[i]);
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
		assert(dma_live == 0 && allocations == 0);
		/* Cleanup is repeatable. */
		ice_buf_fini(ice);
		destroy(ice);
	}
}

/*
 * MAC start gives every ring its LSO pool and handles under the lifecycle
 * lock, a rebuild finds them present, and any failure leaves no ring with
 * part of one.  MAC stop frees them all.
 */
static void
check_lso_alloc(void)
{
	static const uint_t rings[] = { 1, 4, 16, 127 };
	uint_t r, i, n, nrings, base, failure, bad;

	for (r = 0; r < sizeof (rings) / sizeof (rings[0]); r++) {
		ice_t *ice;

		nrings = rings[r];
		ice = make(nrings, 1024);
		n = ice_tx_pool_bufs(ICE_TX_LSO_BUFS_RING,
		    ICE_TX_LSO_BUFS_MAX, nrings);
		attempts = fail_at = handle_fail = errors = 0;
		assert(ice_buf_init(ice));
		base = dma_live;
		ice->ice_rebuild_lock = 1;

		/* LSO off: nothing to allocate. */
		ice->ice_tx_lso_enable = B_FALSE;
		attempts = 0;
		assert(ice_tx_lso_alloc(ice) && attempts == 0);
		assert(dma_live == base);
		ice->ice_tx_lso_enable = B_TRUE;

		assert(ice_tx_lso_alloc(ice));
		assert(dma_live == base + nrings * n && errors == 0);
		for (i = 0; i < nrings; i++) {
			ice_tx_ring_t *itr = &ice->ice_txr[i];
			ice_dma_buffer_t *lso;

			assert(itr->itxr_lso_pool.ibp_nbufs == n);
			assert(itr->itxr_lso_pool.ibp_nfree == n);
			assert(itr->lso_handles);
			lso = take(&itr->itxr_lso_pool);
			assert(lso != NULL && lso->idb_len == ICE_TX_LSO_BUFSZ);
			put(lso);
		}

		/* A rebuild restarts with the pools it kept. */
		attempts = 0;
		assert(ice_tx_lso_alloc(ice) && attempts == 0);
		assert(dma_live == base + nrings * n);

		ice_tx_lso_free(ice);
		assert(dma_live == base);
		for (i = 0; i < nrings; i++) {
			assert(ice->ice_txr[i].itxr_lso_pool.ibp_bufs == NULL);
			assert(!ice->ice_txr[i].lso_handles);
			assert(take(&ice->ice_txr[i].itxr_lso_pool) == NULL);
		}
		ice_tx_lso_free(ice);

		/* Every buffer allocation failure unwinds every ring. */
		for (failure = 1; failure <= nrings * n;
		    failure += (nrings > 4 ? n - 1 : 1)) {
			attempts = 0;
			fail_at = failure;
			errors = 0;
			assert(!ice_tx_lso_alloc(ice));
			assert(errors == 1 && dma_live == base);
			for (i = 0; i < nrings; i++) {
				assert(ice->ice_txr[i].itxr_lso_pool.ibp_bufs ==
				    NULL);
				assert(!ice->ice_txr[i].lso_handles);
			}
		}
		fail_at = 0;

		/* So does a handle failure on any ring. */
		for (bad = 1; bad <= nrings; bad += (nrings > 4 ? 7 : 1)) {
			handle_fail = bad;
			errors = 0;
			assert(!ice_tx_lso_alloc(ice));
			assert(errors == 1 && dma_live == base);
			for (i = 0; i < nrings; i++) {
				assert(ice->ice_txr[i].itxr_lso_pool.ibp_bufs ==
				    NULL);
				assert(!ice->ice_txr[i].lso_handles);
			}
		}
		handle_fail = 0;

		/* Detach releases pools a stop left in place. */
		assert(ice_tx_lso_alloc(ice));
		ice->ice_rebuild_lock = 0;
		ice_buf_fini(ice);
		assert(dma_live == 0 && allocations == 0 && locks == 0);
		for (i = 0; i < nrings; i++)
			assert(!ice->ice_txr[i].lso_handles);
		destroy(ice);
	}
}

int
main(void)
{
	check_sizing();
	check_stacks();
	check_failures();
	check_lso_alloc();
	(void) printf("PASS: TX pools sized per ring within caps; LSO pools "
	    "allocated at MAC start\n");
	return (0);
}

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
#define	B_TRUE 1
#define	B_FALSE 0
#define	KM_SLEEP 1
#define	ICE_TX_SMALL_PKT 512
#define	ICE_TX_COPY_BUFSZ 12288
#define	ICE_TX_LSO_BUFSZ 12288
#define	ICE_MAX_FRAME_SIZE 9728
#define	ICE_TX_MAX_BUFSZ 16383
#define	ASSERT3U(a, op, b) assert((a) op(b))
#define	VERIFY3U(a, op, b) assert((a) op(b))
#define	ASSERT3P(a, op, b) assert((a) op(b))
#define	ASSERT(x) assert(x)
#include "ice_pool_types.h"

static unsigned locks, allocations, dma_live, attempts, fail_at;
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
	assert(locks == 0); /* Sleeping allocations cannot hold pool locks. */
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
ice_error(ice_t *ice, const char *message)
{
	(void) ice;
	(void) message;
}
static boolean_t
ice_dma_alloc(ice_t *ice, ice_dma_buffer_t *buf, ddi_dma_attr_t *attr,
    ddi_device_acc_attr_t *acc, boolean_t zero, size_t size, boolean_t sleep)
{
	(void) ice;
	assert(*attr == 1 && *acc == 1 && zero && !sleep && locks == 0);
	if (++attempts == fail_at)
		return (B_FALSE);
	buf->idb_va = calloc(1, size);
	assert(buf->idb_va != NULL);
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
		free(buf->idb_va);
		buf->idb_va = NULL;
		buf->idb_len = 0;
	}
}
void ice_buf_fini(ice_t *);
#include "ice_pool_code.h"

static void
exercise_stacks(ice_t *ice)
{
	ice_dma_buffer_t *small[5], *normal[5], *lso[5];
	unsigned i;

	for (i = 0; i < 5; i++) {
		small[i] = ice_small_buf_alloc(ice);
		normal[i] = ice_buf_alloc(ice);
		lso[i] = ice_lso_buf_alloc(ice);
		assert(small[i] != NULL && normal[i] != NULL);
		assert(small[i]->idb_len == ICE_TX_SMALL_PKT);
		assert(normal[i]->idb_len == ICE_TX_COPY_BUFSZ);
		assert((lso[i] != NULL) == ice->ice_tx_lso_enable);
		if (lso[i] != NULL) {
			assert(lso[i]->idb_len == ICE_TX_LSO_BUFSZ);
			assert(lso[i] != normal[i]);
		}
	}
	assert(ice_small_buf_alloc(ice) == NULL);
	assert(ice_buf_alloc(ice) == NULL);
	assert(ice_lso_buf_alloc(ice) == NULL);
	ice_buf_free(NULL);
	for (i = 0; i < 5; i++) {
		/* Equal-sized normal/LSO buffers have distinct owners. */
		ice_buf_free(normal[i]);
		assert(ice_small_buf_alloc(ice) == NULL);
		assert(ice_lso_buf_alloc(ice) == NULL);
		assert(ice_buf_alloc(ice) == normal[i]);
	}
	for (i = 0; i < 5; i++) {
		ice_buf_free(small[i]);
		ice_buf_free(normal[i]);
		ice_buf_free(lso[i]);
	}
	assert(locks == 0);
}

int
main(void)
{
	ice_t ice = { 0 };
	unsigned lso, failure, cases = 0;

	ice.ice_num_txr = 2;
	ice.ice_txr = calloc(2, sizeof (*ice.ice_txr));
	assert(ice.ice_txr != NULL);
	ice.ice_txr[0].itxr_size = 2;
	ice.ice_txr[1].itxr_size = 3;
	for (lso = 0; lso <= 1; lso++) {
		unsigned total = 5 * (lso ? 3 : 2);
		ice.ice_tx_lso_enable = lso;
		for (failure = 0; failure <= total; failure++) {
			boolean_t ok;
			attempts = 0;
			fail_at = failure;
			ok = ice_buf_init(&ice);
			assert(ok == (failure == 0));
			if (ok) {
				assert(dma_live == total);
				assert(ice.ice_copy_pool.ibp_lock ==
				    &ice.ice_buf_lock);
				assert(ice.ice_lso_pool.ibp_lock ==
				    &ice.ice_buf_lock);
				assert(ice.ice_small_pool.ibp_lock ==
				    &ice.ice_small_buf_lock);
				exercise_stacks(&ice);
				ice_buf_fini(&ice);
			}
			assert(allocations == 0 && dma_live == 0 && locks == 0);
			/* Partial and complete cleanup is repeatable. */
			ice_buf_fini(&ice);
			cases++;
		}
	}
	free(ice.ice_txr);
	(void) printf("PASS: %u TX pool allocation/lifetime cases\n", cases);
	return (0);
}

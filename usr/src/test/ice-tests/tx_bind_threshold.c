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
#include <string.h>

#undef bzero
#undef bcopy
#define	bzero(p, n)	((void) memset((p), 0, (n)))
#define	bcopy(s, d, n)	((void) memcpy((d), (s), (n)))
#define	MBLKL(mp)	((size_t)((mp)->b_wptr - (mp)->b_rptr))

typedef unsigned int uint_t;
typedef char *caddr_t;
typedef void *ddi_acc_handle_t;
typedef void *ddi_dma_handle_t;
typedef struct ddi_dma_cookie {
	uint64_t dmac_laddress;
	size_t dmac_size;
} ddi_dma_cookie_t;
typedef struct kstat_named {
	union { uint64_t ui64; } value;
} kstat_named_t;
typedef struct mblk {
	unsigned char *b_rptr;
	unsigned char *b_wptr;
	struct mblk *b_cont;
	uint_t cookies;
} mblk_t;

#include "ice_tx_types.h"

typedef struct ice {
	ice_dma_buffer_t small;
	ice_dma_buffer_t general;
	int small_available;
	int general_available;
} ice_t;
typedef struct ice_tx_ring {
	ice_t *itxr_ice;
	ice_txq_stat_t itxr_stats;
} ice_tx_ring_t;

/* Substitute pool and DMA allocation; copy/build behavior is actual C. */
static ice_tx_ctrl_block_t blocks[16];
static int used[16];
static uint_t nbinds, nfrees;
static int fail_bind;

static ice_tx_ctrl_block_t *
ice_tcb_alloc(ice_tx_ring_t *itr)
{
	size_t i;

	(void) itr;
	for (i = 0; i < 16; i++) {
		if (!used[i]) {
			used[i] = 1;
			(void) memset(&blocks[i], 0, sizeof (blocks[i]));
			return (&blocks[i]);
		}
	}
	return (NULL);
}

static void
ice_tcb_free(ice_tx_ring_t *itr, ice_tx_ctrl_block_t *tcb)
{
	size_t slot = tcb - blocks;

	(void) itr;
	assert(slot < 16 && used[slot]);
	used[slot] = 0;
	nfrees++;
}

static ice_dma_buffer_t *
ice_small_buf_alloc(ice_t *ice)
{
	return (ice->small_available ? &ice->small : NULL);
}

static ice_dma_buffer_t *
ice_buf_alloc(ice_t *ice)
{
	return (ice->general_available ? &ice->general : NULL);
}

static ice_tx_ctrl_block_t *
ice_tx_bind_fragment(ice_tx_ring_t *itr, mblk_t *mp, uint_t *ncookies)
{
	ice_tx_ctrl_block_t *tcb;

	nbinds++;
	if (fail_bind)
		return (NULL);
	tcb = ice_tcb_alloc(itr);
	assert(tcb != NULL);
	tcb->itcb_type = ITCB_BIND;
	tcb->itcb_len = MBLKL(mp);
	*ncookies = mp->cookies;
	return (tcb);
}

#include "ice_tx_build.h"

static ice_t ice;
static ice_tx_ring_t ring;
static unsigned char small[512], general[1024], payload[1024];

static mblk_t
packet(size_t len)
{
	mblk_t mp = { payload, payload + len, NULL, 1 };

	assert(len <= sizeof (payload));
	return (mp);
}

static void
reset(void)
{
	(void) memset(used, 0, sizeof (used));
	(void) memset(&ring, 0, sizeof (ring));
	(void) memset(small, 0xcc, sizeof (small));
	(void) memset(general, 0xcc, sizeof (general));
	(void) memset(payload, 0x5a, sizeof (payload));
	ice.small.idb_va = (caddr_t)small;
	ice.small.idb_len = sizeof (small);
	ice.general.idb_va = (caddr_t)general;
	ice.general.idb_len = sizeof (general);
	ice.small_available = ice.general_available = 1;
	ring.itxr_ice = &ice;
	nbinds = nfrees = 0;
	fail_bind = 0;
}

static void
check_no_blocks(void)
{
	size_t i;

	for (i = 0; i < 16; i++)
		assert(!used[i]);
}

static void
check_small(void)
{
	const size_t lengths[] = { 16, 17, 128, 512, 513 };
	ice_tx_ctrl_block_t *tcbs[ICE_TX_MAX_COOKIE + 1];
	uint_t ntcb, ndesc;
	mblk_t mp;
	size_t i;

	for (i = 0; i < sizeof (lengths) / sizeof (lengths[0]); i++) {
		reset();
		mp = packet(lengths[i]);
		assert(ice_tx_build_tcbs(&ring, &mp, lengths[i], tcbs,
		    &ntcb, &ndesc) == ICE_TX_BUILD_OK);
		assert(ntcb == 1 && ndesc == 1);
		if (lengths[i] <= 512) {
			assert(nbinds == 0);
			assert(tcbs[0]->itcb_type == ITCB_SMALL_COPY);
			assert(memcmp(small, payload, lengths[i]) == 0);
		} else {
			assert(nbinds == 1);
			assert(tcbs[0]->itcb_type == ITCB_BIND);
		}
		if (lengths[i] == 16) {
			assert(tcbs[0]->itcb_len == 17);
			assert(small[16] == 0 && small[17] == 0xcc);
		} else {
			assert(tcbs[0]->itcb_len == lengths[i]);
		}
		ice_tcb_free(&ring, tcbs[0]);
		check_no_blocks();
	}

	/* Failed runt copies must retry; a bind would omit padding. */
	reset();
	ice.small_available = ice.general_available = 0;
	mp = packet(16);
	assert(ice_tx_build_tcbs(&ring, &mp, 16, tcbs, &ntcb, &ndesc) ==
	    ICE_TX_BUILD_NORES);
	assert(nbinds == 0);
	check_no_blocks();

	/* At the minimum length a bind can replace an unavailable copy. */
	mp = packet(17);
	assert(ice_tx_build_tcbs(&ring, &mp, 17, tcbs, &ntcb, &ndesc) ==
	    ICE_TX_BUILD_OK);
	assert(nbinds == 1 && ntcb == 1 && ndesc == 1);
	ice_tcb_free(&ring, tcbs[0]);
	check_no_blocks();

	/* A permanent copy failure must not be retried by binding. */
	reset();
	ice.small.idb_len = 127;
	mp = packet(128);
	assert(ice_tx_build_tcbs(&ring, &mp, 128, tcbs, &ntcb, &ndesc) ==
	    ICE_TX_BUILD_DROP);
	assert(nbinds == 0);
	check_no_blocks();
}

static void
check_fallback(void)
{
	ice_tx_ctrl_block_t *tcbs[ICE_TX_MAX_COOKIE + 1];
	uint_t ntcb, ndesc;
	mblk_t first, second;

	/* The second fragment exceeds the budget; retire both partial binds. */
	reset();
	first = packet(400);
	second = packet(400);
	first.cookies = 5;
	second.cookies = 4;
	first.b_cont = &second;
	assert(ice_tx_build_tcbs(&ring, &first, 800, tcbs, &ntcb, &ndesc) ==
	    ICE_TX_BUILD_OK);
	assert(nbinds == 2 && nfrees == 2);
	assert(ntcb == 1 && ndesc == 1);
	assert(tcbs[0]->itcb_type == ITCB_COPY && tcbs[0]->itcb_len == 800);
	assert(memcmp(general, payload, 800) == 0);
	assert(ring.itxr_stats.ictxs_no_pkt_cache.value.ui64 == 1);
	ice_tcb_free(&ring, tcbs[0]);
	check_no_blocks();

	/* A bind failure has the same fallback and preserves copy errors. */
	reset();
	fail_bind = 1;
	first = packet(513);
	assert(ice_tx_build_tcbs(&ring, &first, 513, tcbs, &ntcb, &ndesc) ==
	    ICE_TX_BUILD_OK);
	assert(nbinds == 1 && ntcb == 1 && ndesc == 1);
	assert(tcbs[0]->itcb_type == ITCB_COPY);
	ice_tcb_free(&ring, tcbs[0]);
	check_no_blocks();

	ice.general_available = 0;
	assert(ice_tx_build_tcbs(&ring, &first, 513, tcbs, &ntcb, &ndesc) ==
	    ICE_TX_BUILD_NORES);
	check_no_blocks();
	ice.general_available = 1;
	ice.general.idb_len = 512;
	assert(ice_tx_build_tcbs(&ring, &first, 513, tcbs, &ntcb, &ndesc) ==
	    ICE_TX_BUILD_DROP);
	check_no_blocks();
}

int
main(void)
{
	check_small();
	check_fallback();
	(void) puts("PASS: ICE TX copy, runt padding, and bind fallback");
	return (0);
}

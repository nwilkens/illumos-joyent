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
 * Run the actual ice_tx_swlso() against a controlled mac_hw_emul(): an LSO
 * packet that arrived without checksum offload must be segmented with full
 * software checksums, its segments must ask the hardware for no offload, and
 * a packet that cannot be segmented is counted as a drop.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int uint_t;
#define	ETHERTYPE_IP	0x0800
#define	ETHERTYPE_IPV6	0x86dd

#include "tx_swlso_defs.h"

typedef struct mblk {
	struct mblk *b_next;
	uint32_t flags;
	uint32_t mss;
	int freed;
} mblk_t;

typedef struct {
	uint16_t meoi_l3proto;
	uint32_t meoi_len, meoi_l2hlen, meoi_l3hlen, meoi_l4hlen;
} mac_ether_offload_info_t;

typedef struct {
	union {
		uint64_t ui64;
	} value;
} kstat_named_t;

typedef struct {
	int itxr_lock;
	struct {
		kstat_named_t ictxs_lso_nohck, ictxs_drops, ictxs_lso_drops;
	} itxr_stats;
} ice_tx_ring_t;

static uint16_t l3proto;
static uint32_t payload;
static uint32_t emul_flags, emul_mode;
static unsigned emul_calls, nsegs, locked;
static mblk_t segs[3];

static void
mutex_enter(int *lock)
{
	assert(*lock == 0);
	*lock = 1;
	locked++;
}

static void
mutex_exit(int *lock)
{
	assert(*lock == 1);
	*lock = 0;
}

static uint32_t
proto_hlen(void)
{
	return (l3proto == ETHERTYPE_IP ? 20 : 40);
}

static void
mac_ether_offload_info(mblk_t *mp, mac_ether_offload_info_t *meo)
{
	(void) mp;
	meo->meoi_l3proto = l3proto;
	meo->meoi_l2hlen = 14;
	meo->meoi_l3hlen = proto_hlen();
	meo->meoi_l4hlen = 20;
	meo->meoi_len = 14 + proto_hlen() + 20 + payload;
}

static void
mac_lso_get(mblk_t *mp, uint32_t *mss, uint32_t *flags)
{
	*flags = mp->flags & HW_LSO;
	*mss = mp->mss;
}

/* Like the real one, this stores the flags whole and leaves the MSS alone. */
static void
mac_hcksum_set(mblk_t *mp, uint32_t start, uint32_t stuff, uint32_t end,
    uint32_t value, uint32_t flags)
{
	assert(start == 0 && stuff == 0 && end == 0 && value == 0);
	mp->flags = flags;
}

/*
 * Segment into nsegs mblks carrying the emulated-checksum flags, or drop.
 * Like mac_sw_lso(), LSO emulation drops a packet that makes one segment.
 */
static void
mac_hw_emul(mblk_t **mpp, mblk_t **tailp, uint_t *countp, uint32_t emul)
{
	mblk_t *mp = *mpp;
	unsigned i;

	assert(countp == NULL);
	emul_calls++;
	emul_flags = mp->flags;
	emul_mode = emul;
	assert(mp->mss == 1448);
	mp->freed = 1;
	if ((emul & MAC_LSO_EMUL) != 0 && (mp->flags & HW_LSO) != 0 &&
	    (payload + mp->mss - 1) / mp->mss < 2)
		nsegs = 0;
	if (nsegs == 0) {
		*mpp = NULL;
		return;
	}
	for (i = 0; i < nsegs; i++) {
		segs[i].flags = HCK_FULLCKSUM_OK | HCK_IPV4_HDRCKSUM_OK;
		segs[i].b_next = i + 1 < nsegs ? &segs[i + 1] : NULL;
	}
	*mpp = &segs[0];
	*tailp = &segs[nsegs - 1];
}

#include "tx_swlso_body.h"

static void
run(uint16_t proto, unsigned n, uint32_t bytes)
{
	ice_tx_ring_t ring;
	mblk_t lso = { 0 }, next = { 0 }, *out, *mp;
	unsigned i;

	memset(&ring, 0, sizeof (ring));
	memset(segs, 0, sizeof (segs));
	l3proto = proto;
	nsegs = n;
	payload = bytes;
	emul_calls = locked = 0;
	lso.flags = HW_LSO;
	lso.mss = 1448;

	out = ice_tx_swlso(&ring, &lso, &next);
	assert(emul_calls == 1);
	/*
	 * Full checksums, the IPv4 header's too, and the LSO request kept
	 * only for a packet that makes more than one segment.
	 */
	if (bytes > 1448) {
		assert(emul_flags == (HCK_FULLCKSUM | HW_LSO |
		    (proto == ETHERTYPE_IP ? HCK_IPV4_HDRCKSUM : 0)));
		assert(emul_mode == (MAC_LSO_EMUL | MAC_HWCKSUM_EMULS));
	} else {
		assert(emul_flags == (HCK_FULLCKSUM |
		    (proto == ETHERTYPE_IP ? HCK_IPV4_HDRCKSUM : 0)));
		assert(emul_mode == MAC_HWCKSUM_EMULS);
	}
	assert(ring.itxr_stats.ictxs_lso_nohck.value.ui64 == 1);
	assert(ring.itxr_lock == 0 && locked == 1);

	if (n == 0) {
		assert(out == &next);
		assert(ring.itxr_stats.ictxs_drops.value.ui64 == 1);
		assert(ring.itxr_stats.ictxs_lso_drops.value.ui64 == 1);
		return;
	}
	assert(ring.itxr_stats.ictxs_drops.value.ui64 == 0);
	for (mp = out, i = 0; i < n; i++, mp = mp->b_next) {
		assert(mp == &segs[i]);
		/* The software checksums are final. */
		assert(mp->flags == 0);
	}
	assert(mp == &next);
}

int
main(void)
{
	run(ETHERTYPE_IP, 3, 3 * 1448);
	run(ETHERTYPE_IPV6, 3, 2 * 1448 + 1);
	/* One segment's worth is checksummed without segmentation. */
	run(ETHERTYPE_IP, 1, 1448);
	run(ETHERTYPE_IPV6, 1, 100);
	run(ETHERTYPE_IPV6, 0, 3 * 1448);
	(void) puts("PASS: LSO without checksum offload is segmented in "
	    "software");
	return (0);
}

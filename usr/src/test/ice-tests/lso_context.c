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
#include <netinet/in.h>

typedef int boolean_t;
#define	B_FALSE	0
#define	B_TRUE	1
#define	ETHERTYPE_IP	0x0800
#define	ETHERTYPE_IPV6	0x86dd
#undef bzero
#define	bzero(ptr, len)	((void) memset(ptr, 0, len))
/* CSTYLED */
#define	ASSERT3U(a, op, b)	assert((a) op (b))

#include "ice_tx_types.h"

/* Only the MAC metadata boundary is substituted; validation is actual C. */
typedef struct mblk {
	mac_ether_offload_info_t info;
	size_t len;
	uint32_t checksum_flags;
	uint32_t lso_flags;
	uint32_t mss;
} mblk_t;

/* The old implementation needs this owner to decide whether to downgrade. */
typedef struct ice {
	size_t ice_mtu;
} ice_t;

static void
mac_hcksum_get(const mblk_t *mp, uint32_t *start, uint32_t *stuff,
    uint32_t *end, uint32_t *value, uint32_t *flags)
{
	assert(start == NULL && stuff == NULL && end == NULL && value == NULL);
	*flags = mp->checksum_flags;
}

static void
mac_lso_get(mblk_t *mp, uint32_t *mss, uint32_t *flags)
{
	*mss = mp->mss;
	*flags = mp->lso_flags;
}

static void
mac_ether_offload_info(mblk_t *mp, mac_ether_offload_info_t *info)
{
	*info = mp->info;
}

static size_t
msgdsize(mblk_t *mp)
{
	return (mp->len);
}

#include "ice_tx_context.h"

static ice_tx_build_t
context(mblk_t *mp, ice_tx_ctx_t *ctx)
{
	ice_t ice = { 1500 };

	(void) ice;
	return (CONTEXT(mp, ctx));
}

static mblk_t
packet(boolean_t ipv6, size_t len, uint32_t mss)
{
	mblk_t mp = { 0 };

	mp.info.meoi_flags = MEOI_L2INFO_SET | MEOI_L3INFO_SET |
	    MEOI_L4INFO_SET;
	mp.info.meoi_len = mp.len = len;
	mp.info.meoi_l2hlen = 14;
	mp.info.meoi_l3proto = ipv6 ? ETHERTYPE_IPV6 : ETHERTYPE_IP;
	mp.info.meoi_l3hlen = ipv6 ? 40 : 20;
	mp.info.meoi_l4proto = IPPROTO_TCP;
	mp.info.meoi_l4hlen = 20;
	mp.checksum_flags = HCK_PARTIALCKSUM;
	if (!ipv6)
		mp.checksum_flags |= HCK_IPV4_HDRCKSUM;
	mp.lso_flags = HW_LSO;
	mp.mss = mss;
	return (mp);
}

static void
check_protocol(boolean_t ipv6)
{
	/* Include the old MTU downgrade boundary and a jumbo-sized request. */
	const size_t lengths[] = { 128, 1500, 1501, 1514, 9000 };
	const uint32_t rejected[] = { 0, 1, 63, 9669 };
	const uint32_t accepted[] = { 64, 9668 };
	ice_tx_ctx_t ctx;
	mblk_t mp;
	size_t i, j;

	for (i = 0; i < sizeof (lengths) / sizeof (lengths[0]); i++) {
		for (j = 0; j < sizeof (rejected) / sizeof (rejected[0]); j++) {
			mp = packet(ipv6, lengths[i], rejected[j]);
			assert(context(&mp, &ctx) == ICE_TX_BUILD_DROP);
			/* Preserve the caller's LSO drop accounting. */
			assert(ctx.itc_use_ctx);
		}
		for (j = 0; j < sizeof (accepted) / sizeof (accepted[0]); j++) {
			mp = packet(ipv6, lengths[i], accepted[j]);
			assert(context(&mp, &ctx) == ICE_TX_BUILD_OK);
			assert(ctx.itc_use_ctx);
			assert(ctx.itc_mss == accepted[j]);
			assert(ctx.itc_tsolen == lengths[i] - (ipv6 ? 74 : 54));
			assert(ctx.itc_data_cmd == (ipv6 ? 0x120 : 0x160));
			assert(ctx.itc_data_off == (ipv6 ? 0x14507 : 0x14287));
		}
	}

	/* Ordinary checksum offload ignores MSS, including zero metadata. */
	mp = packet(ipv6, 128, 0);
	mp.lso_flags = 0;
	assert(context(&mp, &ctx) == ICE_TX_BUILD_OK);
	assert(!ctx.itc_use_ctx);
	assert(ctx.itc_data_cmd == (ipv6 ? 0x120 : 0x160));
	assert(ctx.itc_data_off == (ipv6 ? 0x14507 : 0x14287));

	/* Invalid requests must not turn into ordinary transmission. */
	mp = packet(ipv6, 128, 63);
	mp.checksum_flags = 0;
	assert(context(&mp, &ctx) == ICE_TX_BUILD_DROP);
	mp = packet(ipv6, ipv6 ? 74 : 54, 63);
	assert(context(&mp, &ctx) == ICE_TX_BUILD_DROP);
	mp = packet(ipv6, 128, 63);
	mp.len++;
	assert(context(&mp, &ctx) == ICE_TX_BUILD_DROP);
}

int
main(void)
{
	check_protocol(B_FALSE);
	check_protocol(B_TRUE);
	(void) puts("PASS: ICE LSO MSS rejection and IPv4/IPv6 contexts");
	return (0);
}

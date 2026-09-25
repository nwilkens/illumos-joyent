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

/* Run the receive checksum verdict and its per-ring counters. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef int boolean_t;
typedef int kmutex_t;
typedef struct {
	union {
		uint64_t ui64;
	} value;
} kstat_named_t;
typedef struct {
	int unused;
} mblk_t;

#define	BIT(n)			(1u << (n))
#define	ASSERT(x)		assert(x)
#define	MUTEX_HELD(m)		(*(m) != 0)
#define	HCK_IPV4_HDRCKSUM_OK	0x01
#define	HCK_FULLCKSUM_OK	0x04

#include "ice_rx_types.h"

typedef struct ice {
	boolean_t ice_safe_mode;
} ice_t;

typedef struct ice_rx_ring {
	ice_t *irxr_ice;
	kmutex_t irxr_lock;
	ice_rxq_stat_t irxr_stats;
} ice_rx_ring_t;

static struct ice_rx_ptype_decoded decoded;
static int unknown_ptype;
static uint32_t reported;
static unsigned sets;

static struct ice_rx_ptype_decoded
ice_decode_rx_desc_ptype(uint16_t ptype)
{
	(void) ptype;
	if (unknown_ptype)
		decoded.known = 0;
	return (decoded);
}

static void
mac_hcksum_set(mblk_t *mp, uint32_t start, uint32_t stuff, uint32_t end,
    uint32_t value, uint32_t flags)
{
	(void) mp;
	(void) start;
	(void) stuff;
	(void) end;
	(void) value;
	reported = flags;
	sets++;
}

#include "ice_rx_hcksum.h"

#define	IPE	BIT(ICE_RX_FLEX_DESC_STATUS0_XSUM_IPE_S)
#define	EIPE	BIT(ICE_RX_FLEX_DESC_STATUS0_XSUM_EIPE_S)
#define	L4E	BIT(ICE_RX_FLEX_DESC_STATUS0_XSUM_L4E_S)
#define	EUDPE	BIT(ICE_RX_FLEX_DESC_STATUS0_XSUM_EUDPE_S)
#define	EXADD	BIT(ICE_RX_FLEX_DESC_STATUS0_IPV6EXADD_S)
#define	L3L4P	BIT(ICE_RX_FLEX_DESC_STATUS0_L3L4P_S)

static ice_t ice;
static ice_rx_ring_t ring;

static uint32_t
run(unsigned ver, unsigned prot, unsigned tunnel, uint16_t status)
{
	mblk_t mp;

	(void) memset(&decoded, 0, sizeof (decoded));
	decoded.known = 1;
	decoded.outer_ip = ver != 0 ? ICE_RX_PTYPE_OUTER_IP :
	    ICE_RX_PTYPE_OUTER_L2;
	decoded.outer_ip_ver = ver;
	decoded.tunnel_type = tunnel;
	decoded.inner_prot = prot;
	reported = 0;
	ring.irxr_lock = 1;
	ice_rx_hcksum(&ring, &mp, status, 0);
	return (reported);
}

#define	COUNT(f)	(ring.irxr_stats.icrxs_hck_##f.value.ui64)

int
main(void)
{
	const unsigned v4 = ICE_RX_PTYPE_OUTER_IPV4;
	const unsigned v6 = ICE_RX_PTYPE_OUTER_IPV6;
	const unsigned tcp = ICE_RX_PTYPE_INNER_PROT_TCP;
	const unsigned udp = ICE_RX_PTYPE_INNER_PROT_UDP;
	const unsigned icmp = ICE_RX_PTYPE_INNER_PROT_ICMP;

	ring.irxr_ice = &ice;

	/* Clean IPv4/TCP verifies both layers. */
	assert(run(v4, tcp, 0, L3L4P) ==
	    (HCK_IPV4_HDRCKSUM_OK | HCK_FULLCKSUM_OK));
	assert(COUNT(v4hdr_ok) == 1 && COUNT(l4_ok) == 1);

	/* An IP header error withholds only the header verdict. */
	assert(run(v4, udp, 0, L3L4P | IPE) == HCK_FULLCKSUM_OK);
	assert(COUNT(v4hdr_err) == 1 && COUNT(v4hdr_ok) == 1);
	assert(run(v4, udp, 0, L3L4P | EIPE) == HCK_FULLCKSUM_OK);
	assert(COUNT(outer_err) == 1);

	/* L4 errors and IPv6 extension headers withhold the L4 verdict. */
	assert(run(v6, tcp, 0, L3L4P | L4E) == 0);
	assert(run(v6, udp, 0, L3L4P | EUDPE) == 0);
	assert(COUNT(l4_err) == 2);
	assert(run(v6, tcp, 0, L3L4P | EXADD) == 0);
	assert(COUNT(v6exthdr) == 1);
	assert(run(v6, tcp, 0, L3L4P) == HCK_FULLCKSUM_OK);
	assert(COUNT(l4_ok) == 4);

	/* IP without a summed L4 protocol verifies only the IPv4 header. */
	assert(run(v4, icmp, 0, L3L4P) == HCK_IPV4_HDRCKSUM_OK);
	assert(run(v6, icmp, 0, L3L4P) == 0);
	assert(COUNT(nol4) == 2);

	/* Unprocessed, unknown, tunneled and non-IP frames get no verdict. */
	sets = 0;
	assert(run(v4, tcp, 0, 0) == 0);
	assert(COUNT(unprocessed) == 1);
	assert(run(v4, tcp, ICE_RX_PTYPE_TUNNEL_IP_GRENAT, L3L4P) == 0);
	assert(run(0, 0, 0, L3L4P) == 0);
	unknown_ptype = 1;
	assert(run(v4, tcp, 0, L3L4P) == 0);
	unknown_ptype = 0;
	assert(COUNT(unknown) == 3 && sets == 0);

	/* Safe mode reports and counts nothing. */
	(void) memset(&ring.irxr_stats, 0, sizeof (ring.irxr_stats));
	ice.ice_safe_mode = 1;
	assert(run(v4, tcp, 0, L3L4P) == 0);
	assert(COUNT(v4hdr_ok) == 0 && COUNT(unknown) == 0 && sets == 0);

	(void) puts("PASS: receive checksum verdicts and counters");
	return (0);
}

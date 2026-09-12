/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 * A copy of the CDDL is available at http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Execute the production MAC L3 parser and mblk cursor against IPv6
 * extension-header chains.  A guest can hand viona a frame whose extension
 * headers sum past the 16-bit L3 length; the parser must report failure, not
 * success with wrapped or unset outputs.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mblk {
	unsigned char *b_rptr, *b_wptr;
	struct mblk *b_cont;
} mblk_t;
#define	MBLKL(m)	((size_t)((m)->b_wptr - (m)->b_rptr))
#define	ASSERT(x)	assert(x)
/* CSTYLED */
#define	ASSERT3U(a, op, b)	assert((a) op (b))
#define	MIN(a, b)	((a) < (b) ? (a) : (b))

#define	ETHERTYPE_IP	0x0800
#define	ETHERTYPE_IPV6	0x86dd
#define	IPPROTO_HOPOPTS		0
#define	IPPROTO_TCP		6
#define	IPPROTO_ROUTING		43
#define	IPPROTO_FRAGMENT	44
#define	IPPROTO_ESP		50
#define	IPPROTO_AH		51
#define	IPPROTO_DSTOPTS		60
#define	IPPROTO_MH		135
#define	IPPROTO_HIP		139
#define	IPPROTO_SHIM6		140
#define	IPH_MF		0x2000
#define	IPH_OFFSET	0x1fff
/* illumos ip6.h defines these in network order for a little-endian host. */
#define	IP6F_OFF_MASK	0xf8ff
#define	IP6F_MORE_FRAG	0x0100
#undef htons
#define	htons(x)	((uint16_t)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))

typedef struct {
	uint8_t ipha_version_and_hdr_length;
	uint8_t ipha_type_of_service;
	uint16_t ipha_length;
	uint16_t ipha_ident;
	uint16_t ipha_fragment_offset_and_flags;
	uint8_t ipha_ttl;
	uint8_t ipha_protocol;
	uint16_t ipha_hdr_checksum;
	uint32_t ipha_src, ipha_dst;
} ipha_t;
typedef struct {
	uint32_t ip6_flow;
	uint16_t ip6_plen;
	uint8_t ip6_nxt;
	uint8_t ip6_hlim;
	uint8_t ip6_src[16], ip6_dst[16];
} ip6_t;

typedef enum {
	MEOI_L3_FRAG_MORE = 1 << 5,
	MEOI_L3_FRAG_OFFSET = 1 << 6,
} mac_ether_offload_flags_t;

#include "mac_ipv6_eh_body.h"

/* Build an IPv6 header followed by n destination-options headers. */
static unsigned char *
build(unsigned n_eh, uint8_t eh_len_val, uint8_t last, size_t *lenp)
{
	const size_t eh_sz = ((size_t)eh_len_val + 1) * 8;
	const size_t len = sizeof (ip6_t) + n_eh * eh_sz;
	unsigned char *pkt = calloc(1, len);
	size_t off = sizeof (ip6_t);
	unsigned i;

	assert(pkt != NULL);
	pkt[0] = 0x60;
	pkt[offsetof(ip6_t, ip6_nxt)] = n_eh > 0 ? IPPROTO_DSTOPTS : last;
	for (i = 0; i < n_eh; i++) {
		pkt[off] = (i + 1 < n_eh) ? IPPROTO_DSTOPTS : last;
		pkt[off + 1] = eh_len_val;
		off += eh_sz;
	}
	*lenp = len;
	return (pkt);
}

/* Split a buffer into an mblk chain at the given piece size. */
static mblk_t *
chain(unsigned char *pkt, size_t len, size_t piece)
{
	mblk_t *head = NULL, **tail = &head;

	while (len > 0) {
		const size_t n = MIN(len, piece);
		mblk_t *mp = calloc(1, sizeof (*mp));

		assert(mp != NULL);
		mp->b_rptr = pkt;
		mp->b_wptr = pkt + n;
		*tail = mp;
		tail = &mp->b_cont;
		pkt += n;
		len -= n;
	}
	return (head);
}

static bool
parse(unsigned n_eh, uint8_t eh_len_val, size_t piece, uint8_t *proto,
    mac_ether_offload_flags_t *frag, uint16_t *l3)
{
	size_t len;
	unsigned char *pkt = build(n_eh, eh_len_val, IPPROTO_TCP, &len);
	mblk_t *mp = chain(pkt, len, piece);
	mac_mblk_cursor_t cursor;
	bool ok;

	/* Poison the outputs: a failed parse must not be trusted. */
	*proto = 0xbb;
	*frag = (mac_ether_offload_flags_t)0xcccccccc;
	*l3 = 0xaaaa;
	mac_mmc_init(&cursor, mp);
	ok = mac_mmc_parse_l3(&cursor, ETHERTYPE_IPV6, proto, frag, l3);
	while (mp != NULL) {
		mblk_t *next = mp->b_cont;
		free(mp);
		mp = next;
	}
	free(pkt);
	return (ok);
}

int
main(void)
{
	uint8_t proto;
	mac_ether_offload_flags_t frag;
	uint16_t l3;

	/* No extension headers. */
	assert(parse(0, 0, 4096, &proto, &frag, &l3));
	assert(proto == IPPROTO_TCP && frag == 0 && l3 == 40);

	/* One 88-byte destination-options header, split across mblks. */
	assert(parse(1, 10, 7, &proto, &frag, &l3));
	assert(proto == IPPROTO_TCP && frag == 0 && l3 == 128);

	/* The largest chain that still fits the 16-bit length: 31 x 2048. */
	assert(parse(31, 255, 1500, &proto, &frag, &l3));
	assert(proto == IPPROTO_TCP && l3 == 40 + 31 * 2048);

	/*
	 * 32 x 2048 exceeds UINT16_MAX.  The former parser returned -1 from a
	 * bool function, which is true, and left every output unset.
	 */
	assert(!parse(32, 255, 1500, &proto, &frag, &l3));
	assert(proto == 0xbb && l3 == 0xaaaa);

	/* A fragment header sets the fragment flags. */
	{
		unsigned char pkt[sizeof (ip6_t) + 8] = { 0x60 };
		mblk_t mp = { pkt, pkt + sizeof (pkt), NULL };
		mac_mblk_cursor_t cursor;

		pkt[offsetof(ip6_t, ip6_nxt)] = IPPROTO_FRAGMENT;
		pkt[40] = IPPROTO_TCP;
		pkt[42] = 0x05;	/* offset 0, more fragments */
		pkt[43] = 0xa9;
		mac_mmc_init(&cursor, &mp);
		assert(mac_mmc_parse_l3(&cursor, ETHERTYPE_IPV6, &proto, &frag,
		    &l3));
		assert(proto == IPPROTO_TCP && l3 == 48);
		assert(frag == (MEOI_L3_FRAG_MORE | MEOI_L3_FRAG_OFFSET));
	}

	(void) printf("PASS: mac ipv6 extension-header parse bounds\n");
	return (0);
}

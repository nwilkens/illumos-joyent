// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2020, Mellanox Technologies inc. All rights reserved.
 * Copyright (c) 2004, 2011 Intel Corporation.  All rights reserved.
 * Copyright (c) 2004 Topspin Corporation.  All rights reserved.
 * Copyright (c) 2004 Voltaire Corporation.  All rights reserved.
 * Copyright (c) 2019, Mellanox Technologies inc.  All rights reserved.
 * Copyright (c) 2005 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2002-2005, Network Appliance, Inc. All rights reserved.
 * Copyright (c) 1999-2019, Mellanox Technologies, Inc. All rights reserved.
 * Copyright (c) 2005-2006 Intel Corporation.  All rights reserved.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * IB CM messages: offsets are those of the IBTA tables in Linux
 * ibta_vol1_c12.h, measured from the start of the MAD.  A bit field's bit
 * offset counts from the most significant bit of its byte.
 *
 * The parser takes the 256 bytes of a MAD from any peer.  It checks what
 * the RoCE CM depends on and leaves the rest (ECE vendor IDs, packet rate,
 * LIDs, the alternate path) unused, as Linux does; a request with an
 * alternate path is served on the primary one.
 */

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#endif

#include "rdk_ibcm_msg.h"

#define	CM(off)		(IBCM_MAD_HDR_LEN + (off))

static uint16_t
get16(const uint8_t *b, size_t off)
{
	return ((uint16_t)(b[off] << 8 | b[off + 1]));
}

static uint32_t
get24(const uint8_t *b, size_t off)
{
	return ((uint32_t)b[off] << 16 | (uint32_t)b[off + 1] << 8 |
	    b[off + 2]);
}

static uint32_t
get32(const uint8_t *b, size_t off)
{
	return ((uint32_t)b[off] << 24 | get24(b, off + 1));
}

static uint64_t
get64(const uint8_t *b, size_t off)
{
	return ((uint64_t)get32(b, off) << 32 | get32(b, off + 4));
}

/* width bits at bit offset bit (from the most significant) of byte off */
static uint8_t
getbits(const uint8_t *b, size_t off, unsigned int bit, unsigned int width)
{
	return ((uint8_t)((b[off] >> (8 - bit - width)) & ((1U << width) - 1)));
}

static void
put16(uint8_t *b, size_t off, uint16_t v)
{
	b[off] = (uint8_t)(v >> 8);
	b[off + 1] = (uint8_t)v;
}

static void
put24(uint8_t *b, size_t off, uint32_t v)
{
	b[off] = (uint8_t)(v >> 16);
	b[off + 1] = (uint8_t)(v >> 8);
	b[off + 2] = (uint8_t)v;
}

static void
put32(uint8_t *b, size_t off, uint32_t v)
{
	b[off] = (uint8_t)(v >> 24);
	put24(b, off + 1, v);
}

static void
put64(uint8_t *b, size_t off, uint64_t v)
{
	put32(b, off, (uint32_t)(v >> 32));
	put32(b, off + 4, (uint32_t)v);
}

static void
putbits(uint8_t *b, size_t off, unsigned int bit, unsigned int width,
    uint32_t v)
{
	const unsigned int shift = 8 - bit - width;
	const uint8_t mask = (uint8_t)(((1U << width) - 1) << shift);

	b[off] = (uint8_t)((b[off] & ~mask) | ((v << shift) & mask));
}

static boolean_t
rdk_ibcm_unicast4(uint32_t a)
{
	/* a is in host order here */
	return (a != 0 && (a >> 24) != 0 && (a >> 24) != 127 &&
	    (a >> 28) != 0xe && (a >> 28) != 0xf);
}

boolean_t
rdk_ibcm_gid_ip4(const uint8_t *gid, uint32_t *ip)
{
	static const uint8_t prefix[12] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
	};

	if (memcmp(gid, prefix, sizeof (prefix)) != 0)
		return (B_FALSE);
	bcopy(&gid[12], ip, sizeof (*ip));
	return (B_TRUE);
}

void
rdk_ibcm_ip4_gid(uint32_t ip, uint8_t *gid)
{
	bzero(gid, 16);
	gid[10] = 0xff;
	gid[11] = 0xff;
	bcopy(&ip, &gid[12], sizeof (ip));
}

/*
 * The IPv4 header a RoCEv2 device writes in the last 20 of the 40 bytes in
 * front of a UD receive (Linux ib_get_gids_from_rdma_hdr()).  The checksum
 * test is the one Linux uses to tell IPv4 from IPv6 there.
 */
int
rdk_ibcm_parse_ip4(const uint8_t *grh, rdk_ibcm_ip4_t *ip)
{
	const uint8_t *h = grh + IBCM_GRH_LEN - 20;
	uint32_t sum = 0, src, dst;
	unsigned int i;

	if (h[0] != 0x45 || get16(h, 2) != IBCM_IP4_MAD_LEN ||
	    (get16(h, 6) & 0xbfff) != 0 || h[9] != 17)
		return (EINVAL);
	for (i = 0; i < 20; i += 2)
		sum += get16(h, i);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	if (sum != 0xffff)
		return (EINVAL);
	src = get32(h, 12);
	dst = get32(h, 16);
	if (!rdk_ibcm_unicast4(src) || !rdk_ibcm_unicast4(dst))
		return (EINVAL);
	bcopy(&h[12], &ip->ip_src, sizeof (ip->ip_src));
	bcopy(&h[16], &ip->ip_dst, sizeof (ip->ip_dst));
	ip->ip_ttl = h[8];
	ip->ip_tos = h[1];
	return (0);
}

uint16_t
rdk_ibcm_pdata_len(uint16_t attr)
{
	switch (attr) {
	case IBCM_ATTR_REQ:
		return (IBCM_REQ_PDATA);
	case IBCM_ATTR_MRA:
		return (IBCM_MRA_PDATA);
	case IBCM_ATTR_REJ:
		return (IBCM_REJ_PDATA);
	case IBCM_ATTR_REP:
		return (IBCM_REP_PDATA);
	case IBCM_ATTR_RTU:
		return (IBCM_RTU_PDATA);
	case IBCM_ATTR_DREQ:
		return (IBCM_DREQ_PDATA);
	case IBCM_ATTR_DREP:
		return (IBCM_DREP_PDATA);
	default:
		return (0);
	}
}

static uint16_t
rdk_ibcm_pdata_off(uint16_t attr)
{
	switch (attr) {
	case IBCM_ATTR_REQ:
		return (CM(140));
	case IBCM_ATTR_MRA:
		return (CM(10));
	case IBCM_ATTR_REJ:
		return (CM(84));
	case IBCM_ATTR_REP:
		return (CM(36));
	case IBCM_ATTR_DREQ:
		return (CM(12));
	default:
		return (CM(8));
	}
}

static boolean_t
rdk_ibcm_qpn_ok(uint32_t qpn)
{
	return (qpn >= 2 && qpn <= IBCM_QPN_MAX);
}

/* Table 106. */
static int
rdk_ibcm_parse_req(const uint8_t *b, rdk_ibcm_msg_t *m)
{
	m->m_vendor_id = get24(b, CM(5));
	m->m_service_id = get64(b, CM(8));
	m->m_ca_guid = get64(b, CM(16));
	m->m_qpn = get24(b, CM(32));
	m->m_resp_res = b[CM(35)];
	m->m_init_depth = b[CM(39)];
	m->m_remote_resp_to = getbits(b, CM(43), 0, 5);
	m->m_transport = getbits(b, CM(43), 5, 2);
	m->m_flow_ctl = getbits(b, CM(43), 7, 1);
	m->m_psn = get24(b, CM(44));
	m->m_local_resp_to = getbits(b, CM(47), 0, 5);
	m->m_retry = getbits(b, CM(47), 5, 3);
	m->m_pkey = get16(b, CM(48));
	m->m_mtu = getbits(b, CM(50), 0, 4);
	m->m_rnr_retry = getbits(b, CM(50), 5, 3);
	m->m_max_retries = getbits(b, CM(51), 0, 4);
	m->m_srq = getbits(b, CM(51), 4, 1);
	m->m_llid = get16(b, CM(52));
	m->m_rlid = get16(b, CM(54));
	bcopy(&b[CM(56)], m->m_lgid, 16);
	bcopy(&b[CM(72)], m->m_rgid, 16);
	m->m_flow_label = get32(b, CM(88)) >> 12;
	m->m_rate = getbits(b, CM(91), 2, 6);
	m->m_tclass = b[CM(92)];
	m->m_hop_limit = b[CM(93)];
	m->m_sl = getbits(b, CM(94), 0, 4);
	m->m_subnet_local = getbits(b, CM(94), 4, 1);
	m->m_ack_timeout = getbits(b, CM(95), 0, 5);

	if (m->m_local_id == 0 || m->m_transport != IBCM_TRANSPORT_RC ||
	    !rdk_ibcm_qpn_ok(m->m_qpn) || m->m_pkey != IBCM_PKEY_DEFAULT ||
	    m->m_mtu < 1 || m->m_mtu > 5 || getbits(b, CM(51), 5, 3) != 0)
		return (EINVAL);
	return (0);
}

/* Table 110. */
static int
rdk_ibcm_parse_rep(const uint8_t *b, rdk_ibcm_msg_t *m)
{
	m->m_qpn = get24(b, CM(12));
	m->m_vendor_id = (uint32_t)b[CM(15)] << 16 | (uint32_t)b[CM(19)] << 8 |
	    b[CM(23)];
	m->m_psn = get24(b, CM(20));
	m->m_resp_res = b[CM(24)];
	m->m_init_depth = b[CM(25)];
	m->m_target_ack_delay = getbits(b, CM(26), 0, 5);
	m->m_failover = getbits(b, CM(26), 5, 2);
	m->m_flow_ctl = getbits(b, CM(26), 7, 1);
	m->m_rnr_retry = getbits(b, CM(27), 0, 3);
	m->m_srq = getbits(b, CM(27), 3, 1);
	m->m_ca_guid = get64(b, CM(28));
	if (m->m_local_id == 0 || m->m_remote_id == 0 ||
	    !rdk_ibcm_qpn_ok(m->m_qpn))
		return (EINVAL);
	return (0);
}

int
rdk_ibcm_parse(const uint8_t *b, rdk_ibcm_msg_t *m)
{
	uint16_t off;
	int ret = 0;

	bzero(m, sizeof (*m));
	if (b[0] != IBCM_BASE_VERSION || b[1] != IBCM_MGMT_CLASS_CM ||
	    b[3] != IBCM_METHOD_SEND)
		return (ENOTSUP);
	if (b[2] != IBCM_CLASS_VERSION || get16(b, 4) != 0)
		return (EINVAL);
	m->m_tid = get64(b, 8);
	m->m_attr = get16(b, 16);
	m->m_attr_mod = get32(b, 20);
	m->m_local_id = get32(b, CM(0));
	/* A REQ has the vendor ID where the others have the remote ID. */
	if (m->m_attr != IBCM_ATTR_REQ)
		m->m_remote_id = get32(b, CM(4));

	switch (m->m_attr) {
	case IBCM_ATTR_REQ:
		ret = rdk_ibcm_parse_req(b, m);
		break;
	case IBCM_ATTR_REP:
		ret = rdk_ibcm_parse_rep(b, m);
		break;
	case IBCM_ATTR_MRA:
		m->m_msg = getbits(b, CM(8), 0, 2);
		m->m_service_timeout = getbits(b, CM(9), 0, 5);
		if (m->m_local_id == 0 || m->m_remote_id == 0 || m->m_msg > 2)
			ret = EINVAL;
		break;
	case IBCM_ATTR_REJ:
		m->m_msg = getbits(b, CM(8), 0, 2);
		m->m_ari_len = getbits(b, CM(9), 0, 7);
		m->m_reason = get16(b, CM(10));
		if (m->m_remote_id == 0 || m->m_msg > 2 ||
		    m->m_ari_len > IBCM_REJ_ARI_MAX) {
			ret = EINVAL;
			break;
		}
		bcopy(&b[CM(12)], m->m_ari, m->m_ari_len);
		break;
	case IBCM_ATTR_RTU:
	case IBCM_ATTR_DREP:
		if (m->m_local_id == 0 || m->m_remote_id == 0)
			ret = EINVAL;
		break;
	case IBCM_ATTR_DREQ:
		m->m_qpn = get24(b, CM(8));
		if (m->m_local_id == 0 || m->m_remote_id == 0 ||
		    !rdk_ibcm_qpn_ok(m->m_qpn))
			ret = EINVAL;
		break;
	default:
		return (ENOTSUP);
	}
	if (ret != 0)
		return (ret);
	off = rdk_ibcm_pdata_off(m->m_attr);
	m->m_pdata = &b[off];
	m->m_pdata_len = rdk_ibcm_pdata_len(m->m_attr);
	return (0);
}

static void
rdk_ibcm_build_req(uint8_t *b, const rdk_ibcm_msg_t *m)
{
	put24(b, CM(5), m->m_vendor_id);
	put64(b, CM(8), m->m_service_id);
	put64(b, CM(16), m->m_ca_guid);
	put24(b, CM(32), m->m_qpn);
	b[CM(35)] = m->m_resp_res;
	b[CM(39)] = m->m_init_depth;
	putbits(b, CM(43), 0, 5, m->m_remote_resp_to);
	putbits(b, CM(43), 5, 2, m->m_transport);
	putbits(b, CM(43), 7, 1, m->m_flow_ctl);
	put24(b, CM(44), m->m_psn);
	putbits(b, CM(47), 0, 5, m->m_local_resp_to);
	putbits(b, CM(47), 5, 3, m->m_retry);
	put16(b, CM(48), m->m_pkey);
	putbits(b, CM(50), 0, 4, m->m_mtu);
	putbits(b, CM(50), 5, 3, m->m_rnr_retry);
	putbits(b, CM(51), 0, 4, m->m_max_retries);
	putbits(b, CM(51), 4, 1, m->m_srq);
	put16(b, CM(52), m->m_llid);
	put16(b, CM(54), m->m_rlid);
	bcopy(m->m_lgid, &b[CM(56)], 16);
	bcopy(m->m_rgid, &b[CM(72)], 16);
	put32(b, CM(88), (m->m_flow_label & 0xfffff) << 12);
	putbits(b, CM(91), 2, 6, m->m_rate);
	b[CM(92)] = m->m_tclass;
	b[CM(93)] = m->m_hop_limit;
	putbits(b, CM(94), 0, 4, m->m_sl);
	putbits(b, CM(94), 4, 1, m->m_subnet_local);
	putbits(b, CM(95), 0, 5, m->m_ack_timeout);
}

static void
rdk_ibcm_build_rep(uint8_t *b, const rdk_ibcm_msg_t *m)
{
	put24(b, CM(12), m->m_qpn);
	b[CM(15)] = (uint8_t)(m->m_vendor_id >> 16);
	b[CM(19)] = (uint8_t)(m->m_vendor_id >> 8);
	put24(b, CM(20), m->m_psn);
	b[CM(23)] = (uint8_t)m->m_vendor_id;
	b[CM(24)] = m->m_resp_res;
	b[CM(25)] = m->m_init_depth;
	putbits(b, CM(26), 0, 5, m->m_target_ack_delay);
	putbits(b, CM(26), 5, 2, m->m_failover);
	putbits(b, CM(26), 7, 1, m->m_flow_ctl);
	putbits(b, CM(27), 0, 3, m->m_rnr_retry);
	putbits(b, CM(27), 3, 1, m->m_srq);
	put64(b, CM(28), m->m_ca_guid);
}

/* Build the whole MAD; fields the message does not have are zero. */
void
rdk_ibcm_build(uint8_t *b, const rdk_ibcm_msg_t *m)
{
	uint16_t len;

	bzero(b, IBCM_MAD_LEN);
	b[0] = IBCM_BASE_VERSION;
	b[1] = IBCM_MGMT_CLASS_CM;
	b[2] = IBCM_CLASS_VERSION;
	b[3] = IBCM_METHOD_SEND;
	put64(b, 8, m->m_tid);
	put16(b, 16, m->m_attr);
	put32(b, 20, m->m_attr_mod);
	put32(b, CM(0), m->m_local_id);
	if (m->m_attr != IBCM_ATTR_REQ)
		put32(b, CM(4), m->m_remote_id);

	switch (m->m_attr) {
	case IBCM_ATTR_REQ:
		rdk_ibcm_build_req(b, m);
		break;
	case IBCM_ATTR_REP:
		rdk_ibcm_build_rep(b, m);
		break;
	case IBCM_ATTR_MRA:
		putbits(b, CM(8), 0, 2, m->m_msg);
		putbits(b, CM(9), 0, 5, m->m_service_timeout);
		break;
	case IBCM_ATTR_REJ:
		putbits(b, CM(8), 0, 2, m->m_msg);
		len = m->m_ari_len > IBCM_REJ_ARI_MAX ? IBCM_REJ_ARI_MAX :
		    m->m_ari_len;
		putbits(b, CM(9), 0, 7, len);
		put16(b, CM(10), m->m_reason);
		bcopy(m->m_ari, &b[CM(12)], len);
		break;
	case IBCM_ATTR_DREQ:
		put24(b, CM(8), m->m_qpn);
		break;
	default:
		break;
	}
	len = rdk_ibcm_pdata_len(m->m_attr);
	if (m->m_pdata != NULL && m->m_pdata_len != 0) {
		bcopy(m->m_pdata, &b[rdk_ibcm_pdata_off(m->m_attr)],
		    m->m_pdata_len < len ? m->m_pdata_len : len);
	}
}

/*
 * struct cma_hdr: version, IP version in the high nibble, the requester's
 * port, then the source and destination addresses as 16 bytes each with an
 * IPv4 address in the last four.
 */
int
rdk_cma_hdr_parse(const uint8_t *p, uint16_t len, rdk_cma_hdr_t *h)
{
	uint32_t src, dst;

	if (len < IBCM_CMA_HDR_LEN || p[0] != IBCM_CMA_VERSION ||
	    (p[1] >> 4) != 4)
		return (EINVAL);
	src = get32(p, 16);
	dst = get32(p, 32);
	if (get16(p, 2) == 0 || !rdk_ibcm_unicast4(src) ||
	    !rdk_ibcm_unicast4(dst))
		return (EINVAL);
	bcopy(&p[2], &h->ch_sport, sizeof (h->ch_sport));
	bcopy(&p[16], &h->ch_src, sizeof (h->ch_src));
	bcopy(&p[32], &h->ch_dst, sizeof (h->ch_dst));
	return (0);
}

void
rdk_cma_hdr_build(uint8_t *p, const rdk_cma_hdr_t *h)
{
	bzero(p, IBCM_CMA_HDR_LEN);
	p[0] = IBCM_CMA_VERSION;
	p[1] = 4 << 4;
	bcopy(&h->ch_sport, &p[2], sizeof (h->ch_sport));
	bcopy(&h->ch_src, &p[16], sizeof (h->ch_src));
	bcopy(&h->ch_dst, &p[32], sizeof (h->ch_dst));
}

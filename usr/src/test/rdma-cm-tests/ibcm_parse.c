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
 * The IB CM message parser against hand-placed bytes, round trips of every
 * message, the IPv4 and RDMA CM headers, and a seeded fuzz run: whatever
 * the parser accepts must hold the invariants the CM depends on.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdk_ibcm_msg.h"

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		exit(1);						\
	}								\
} while (0)

static uint64_t seed = 0x2545f4914f6cdd1dULL;

static uint32_t
rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return ((uint32_t)(seed >> 11));
}

static void
hdr(uint8_t *b, uint16_t attr)
{
	memset(b, 0, IBCM_MAD_LEN);
	b[0] = 1;
	b[1] = 7;
	b[2] = 2;
	b[3] = 3;
	b[16] = attr >> 8;
	b[17] = attr & 0xff;
}

static void
put(uint8_t *b, int off, uint64_t v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		b[off + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

/* A REQ laid out byte by byte from IBTA vol 1 table 106 (offset + 24). */
static void
req_bytes(uint8_t *b)
{
	int i;

	hdr(b, IBCM_ATTR_REQ);
	put(b, 8, 0x1122334455667788ULL, 8);	/* TID */
	put(b, 24, 0xa1b2c3d4, 4);		/* local comm ID */
	put(b, 29, 0x123456, 3);		/* vendor ID */
	put(b, 32, 0x0000000001064e20ULL, 8);	/* service ID, port 20000 */
	put(b, 40, 0x0102030405060708ULL, 8);	/* CA GUID */
	put(b, 56, 0xabcdef, 3);		/* QPN */
	b[59] = 0x10;				/* responder resources */
	b[63] = 0x08;				/* initiator depth */
	b[67] = (20 << 3) | (0 << 1) | 1;	/* resp timeout, RC, flow */
	put(b, 68, 0x654321, 3);		/* starting PSN */
	b[71] = (19 << 3) | 6;			/* local resp timeout, retry */
	put(b, 72, 0xffff, 2);			/* P_Key */
	b[74] = (3 << 4) | 5;			/* MTU 1024, RNR retry 5 */
	b[75] = (15 << 4) | (1 << 3);		/* max retries, SRQ */
	put(b, 76, 0xffff, 2);
	put(b, 78, 0xffff, 2);
	b[90] = b[91] = 0xff;			/* local GID ::ffff:10.0.0.1 */
	b[92] = 10;
	b[95] = 1;
	b[106] = b[107] = 0xff;			/* remote GID ::ffff:10.0.0.2 */
	b[108] = 10;
	b[111] = 2;
	put(b, 112, (uint32_t)0xabcde << 12, 4);	/* flow label */
	b[115] |= 0x2a;				/* packet rate */
	b[116] = 0x60;				/* traffic class */
	b[117] = 64;				/* hop limit */
	b[118] = (3 << 4) | (1 << 3);		/* SL 3, subnet local */
	b[119] = 17 << 3;			/* local ACK timeout */
	for (i = 0; i < IBCM_REQ_PDATA; i++)
		b[164 + i] = (uint8_t)(i + 1);
}

static void
t_req_layout(void)
{
	uint8_t b[IBCM_MAD_LEN], o[IBCM_MAD_LEN];
	rdk_ibcm_msg_t m;
	uint32_t ip;

	req_bytes(b);
	CHECK(rdk_ibcm_parse(b, &m) == 0);
	CHECK(m.m_attr == IBCM_ATTR_REQ && m.m_tid == 0x1122334455667788ULL);
	CHECK(m.m_local_id == 0xa1b2c3d4 && m.m_remote_id == 0);
	CHECK(m.m_vendor_id == 0x123456);
	CHECK(m.m_service_id == IBCM_SID_TCP(20000));
	CHECK(m.m_ca_guid == 0x0102030405060708ULL);
	CHECK(m.m_qpn == 0xabcdef && m.m_resp_res == 0x10 &&
	    m.m_init_depth == 8);
	CHECK(m.m_remote_resp_to == 20 && m.m_transport == 0 &&
	    m.m_flow_ctl == 1);
	CHECK(m.m_psn == 0x654321 && m.m_local_resp_to == 19 &&
	    m.m_retry == 6);
	CHECK(m.m_pkey == 0xffff && m.m_mtu == 3 && m.m_rnr_retry == 5);
	CHECK(m.m_max_retries == 15 && m.m_srq == 1);
	CHECK(m.m_llid == 0xffff && m.m_rlid == 0xffff);
	CHECK(rdk_ibcm_gid_ip4(m.m_lgid, &ip) && ip == htonl(0x0a000001));
	CHECK(rdk_ibcm_gid_ip4(m.m_rgid, &ip) && ip == htonl(0x0a000002));
	CHECK(m.m_flow_label == 0xabcde && m.m_rate == 0x2a);
	CHECK(m.m_tclass == 0x60 && m.m_hop_limit == 64 && m.m_sl == 3 &&
	    m.m_subnet_local == 1 && m.m_ack_timeout == 17);
	CHECK(m.m_pdata == b + 164 && m.m_pdata_len == IBCM_REQ_PDATA);

	/* The builder writes the same bytes. */
	rdk_ibcm_build(o, &m);
	CHECK(memcmp(b, o, IBCM_MAD_LEN) == 0);
}

/* REP, table 110, and a REJ, table 108. */
static void
t_rep_rej_layout(void)
{
	uint8_t b[IBCM_MAD_LEN], o[IBCM_MAD_LEN];
	rdk_ibcm_msg_t m;
	int i;

	hdr(b, IBCM_ATTR_REP);
	put(b, 24, 0x01010101, 4);
	put(b, 28, 0x02020202, 4);
	put(b, 36, 0x123456, 3);		/* local QPN */
	b[39] = 0xaa;				/* vendor ID high */
	b[43] = 0xbb;				/* vendor ID middle */
	put(b, 44, 0xfedcba, 3);		/* starting PSN */
	b[47] = 0xcc;				/* vendor ID low */
	b[48] = 4;				/* responder resources */
	b[49] = 2;				/* initiator depth */
	b[50] = (21 << 3) | (1 << 1) | 1;	/* ack delay, failover, flow */
	b[51] = (7 << 5) | (1 << 4);		/* RNR retry, SRQ */
	put(b, 52, 0x1122334455667788ULL, 8);	/* CA GUID */
	for (i = 0; i < IBCM_REP_PDATA; i++)
		b[60 + i] = (uint8_t)(0xf0 ^ i);
	CHECK(rdk_ibcm_parse(b, &m) == 0);
	CHECK(m.m_local_id == 0x01010101 && m.m_remote_id == 0x02020202);
	CHECK(m.m_qpn == 0x123456 && m.m_psn == 0xfedcba);
	CHECK(m.m_vendor_id == 0xaabbcc);
	CHECK(m.m_resp_res == 4 && m.m_init_depth == 2);
	CHECK(m.m_target_ack_delay == 21 && m.m_failover == 1 &&
	    m.m_flow_ctl == 1 && m.m_rnr_retry == 7 && m.m_srq == 1);
	CHECK(m.m_ca_guid == 0x1122334455667788ULL);
	CHECK(m.m_pdata == b + 60 && m.m_pdata_len == IBCM_REP_PDATA);
	rdk_ibcm_build(o, &m);
	CHECK(memcmp(b, o, IBCM_MAD_LEN) == 0);

	hdr(b, IBCM_ATTR_REJ);
	put(b, 24, 0, 4);
	put(b, 28, 0x0badf00d, 4);
	b[32] = 1 << 6;				/* message rejected: REP */
	b[33] = 8 << 1;				/* ARI length */
	put(b, 34, 28, 2);			/* reason */
	for (i = 0; i < 8; i++)
		b[36 + i] = (uint8_t)i;
	b[108] = 'x';
	CHECK(rdk_ibcm_parse(b, &m) == 0);
	CHECK(m.m_msg == 1 && m.m_ari_len == 8 && m.m_reason == 28);
	CHECK(m.m_ari[7] == 7 && m.m_pdata == b + 108 && m.m_pdata[0] == 'x');
	rdk_ibcm_build(o, &m);
	CHECK(memcmp(b, o, IBCM_MAD_LEN) == 0);
}

static void
rand_fill(rdk_ibcm_msg_t *m, uint16_t attr, uint8_t *pd)
{
	unsigned int i;

	memset(m, 0, sizeof (*m));
	m->m_attr = attr;
	m->m_tid = (uint64_t)rnd() << 32 | rnd();
	m->m_attr_mod = rnd();
	m->m_local_id = rnd() | 1;
	m->m_remote_id = attr == IBCM_ATTR_REQ ? 0 : (rnd() | 1);
	for (i = 0; i < IBCM_MAD_LEN; i++)
		pd[i] = (uint8_t)rnd();
	m->m_pdata = pd;
	m->m_pdata_len = rdk_ibcm_pdata_len(attr);
	switch (attr) {
	case IBCM_ATTR_REQ:
		m->m_vendor_id = rnd() & 0xffffff;
		m->m_service_id = (uint64_t)rnd() << 32 | rnd();
		m->m_ca_guid = (uint64_t)rnd() << 32 | rnd();
		m->m_qpn = 2 + rnd() % (IBCM_QPN_MAX - 1);
		m->m_resp_res = rnd();
		m->m_init_depth = rnd();
		m->m_remote_resp_to = rnd() & 31;
		m->m_flow_ctl = rnd() & 1;
		m->m_psn = rnd() & 0xffffff;
		m->m_local_resp_to = rnd() & 31;
		m->m_retry = rnd() & 7;
		m->m_pkey = IBCM_PKEY_DEFAULT;
		m->m_mtu = 1 + rnd() % 5;
		m->m_rnr_retry = rnd() & 7;
		m->m_max_retries = rnd() & 15;
		m->m_srq = rnd() & 1;
		m->m_llid = rnd();
		m->m_rlid = rnd();
		for (i = 0; i < 16; i++) {
			m->m_lgid[i] = rnd();
			m->m_rgid[i] = rnd();
		}
		m->m_flow_label = rnd() & 0xfffff;
		m->m_rate = rnd() & 63;
		m->m_tclass = rnd();
		m->m_hop_limit = rnd();
		m->m_sl = rnd() & 15;
		m->m_subnet_local = rnd() & 1;
		m->m_ack_timeout = rnd() & 31;
		break;
	case IBCM_ATTR_REP:
		m->m_qpn = 2 + rnd() % (IBCM_QPN_MAX - 1);
		m->m_vendor_id = rnd() & 0xffffff;
		m->m_psn = rnd() & 0xffffff;
		m->m_resp_res = rnd();
		m->m_init_depth = rnd();
		m->m_target_ack_delay = rnd() & 31;
		m->m_failover = rnd() & 3;
		m->m_flow_ctl = rnd() & 1;
		m->m_rnr_retry = rnd() & 7;
		m->m_srq = rnd() & 1;
		m->m_ca_guid = (uint64_t)rnd() << 32 | rnd();
		break;
	case IBCM_ATTR_REJ:
		m->m_msg = rnd() % 3;
		m->m_reason = rnd();
		m->m_ari_len = rnd() % (IBCM_REJ_ARI_MAX + 1);
		for (i = 0; i < m->m_ari_len; i++)
			m->m_ari[i] = rnd();
		break;
	case IBCM_ATTR_MRA:
		m->m_msg = rnd() % 3;
		m->m_service_timeout = rnd() & 31;
		break;
	case IBCM_ATTR_DREQ:
		m->m_qpn = 2 + rnd() % (IBCM_QPN_MAX - 1);
		break;
	default:
		break;
	}
}

static const uint16_t attrs[] = {
	IBCM_ATTR_REQ, IBCM_ATTR_MRA, IBCM_ATTR_REJ, IBCM_ATTR_REP,
	IBCM_ATTR_RTU, IBCM_ATTR_DREQ, IBCM_ATTR_DREP
};

static void
t_roundtrip(int n)
{
	uint8_t b[IBCM_MAD_LEN], b2[IBCM_MAD_LEN], pd[IBCM_MAD_LEN];
	rdk_ibcm_msg_t m, p;
	int i;

	for (i = 0; i < n; i++) {
		rand_fill(&m, attrs[rnd() % 7], pd);
		rdk_ibcm_build(b, &m);
		CHECK(rdk_ibcm_parse(b, &p) == 0);
		CHECK(p.m_attr == m.m_attr && p.m_tid == m.m_tid &&
		    p.m_attr_mod == m.m_attr_mod &&
		    p.m_local_id == m.m_local_id &&
		    p.m_remote_id == m.m_remote_id);
		CHECK(memcmp(p.m_pdata, pd, p.m_pdata_len) == 0);
		p.m_pdata = pd;
		rdk_ibcm_build(b2, &p);
		CHECK(memcmp(b, b2, IBCM_MAD_LEN) == 0);
		CHECK(p.m_qpn == m.m_qpn && p.m_psn == m.m_psn &&
		    p.m_ari_len == m.m_ari_len && p.m_msg == m.m_msg &&
		    p.m_mtu == m.m_mtu && p.m_flow_label == m.m_flow_label &&
		    p.m_service_id == m.m_service_id &&
		    p.m_ack_timeout == m.m_ack_timeout &&
		    p.m_target_ack_delay == m.m_target_ack_delay);
	}
}

/* The invariants the CM relies on for anything the parser accepts. */
static void
invariants(const uint8_t *b, const rdk_ibcm_msg_t *m)
{
	CHECK(m->m_pdata >= b && m->m_pdata_len != 0 &&
	    m->m_pdata + m->m_pdata_len <= b + IBCM_MAD_LEN);
	CHECK(m->m_pdata_len == rdk_ibcm_pdata_len(m->m_attr));
	switch (m->m_attr) {
	case IBCM_ATTR_REQ:
		CHECK(m->m_local_id != 0 && m->m_transport == 0);
		CHECK(m->m_qpn >= 2 && m->m_qpn <= IBCM_QPN_MAX);
		CHECK(m->m_pkey == IBCM_PKEY_DEFAULT);
		CHECK(m->m_mtu >= 1 && m->m_mtu <= 5);
		break;
	case IBCM_ATTR_REP:
		CHECK(m->m_local_id != 0 && m->m_remote_id != 0);
		CHECK(m->m_qpn >= 2 && m->m_qpn <= IBCM_QPN_MAX);
		break;
	case IBCM_ATTR_REJ:
		CHECK((m->m_remote_id != 0 || rdk_ibcm_rej_by_guid(m, NULL)) &&
		    m->m_msg <= 2);
		CHECK(m->m_ari_len <= IBCM_REJ_ARI_MAX);
		break;
	case IBCM_ATTR_MRA:
		CHECK(m->m_local_id != 0 && m->m_remote_id != 0 &&
		    m->m_msg <= 2);
		break;
	case IBCM_ATTR_DREQ:
		CHECK(m->m_qpn >= 2 && m->m_qpn <= IBCM_QPN_MAX);
		/* FALLTHROUGH */
	case IBCM_ATTR_RTU:
	case IBCM_ATTR_DREP:
		CHECK(m->m_local_id != 0 && m->m_remote_id != 0);
		break;
	default:
		CHECK(0);
	}
}

static void
t_fuzz(int n)
{
	uint8_t b[IBCM_MAD_LEN], pd[IBCM_MAD_LEN];
	rdk_ibcm_msg_t m, p;
	int i, j, k, flips, ok = 0;

	for (i = 0; i < n; i++) {
		if (rnd() % 8 == 0) {
			for (j = 0; j < IBCM_MAD_LEN; j++)
				b[j] = rnd();
			if (rnd() % 2 == 0) {
				hdr(b, attrs[rnd() % 7]);
				for (j = 24; j < IBCM_MAD_LEN; j++)
					b[j] = rnd();
			}
		} else {
			rand_fill(&m, attrs[rnd() % 7], pd);
			rdk_ibcm_build(b, &m);
			flips = 1 + rnd() % 6;
			for (j = 0; j < flips; j++) {
				k = rnd() % IBCM_MAD_LEN;
				if (rnd() % 2)
					b[k] ^= 1 << (rnd() % 8);
				else
					b[k % 120] = rnd();
			}
		}
		if (rdk_ibcm_parse(b, &p) == 0) {
			invariants(b, &p);
			ok++;
		}
	}
	(void) printf("fuzz: %d MADs, %d accepted, invariants held\n", n, ok);
}

/* A RoCEv2 IPv4 header with one byte set before the checksum. */
static void
ip4(uint8_t *g, int off, uint8_t val)
{
	uint8_t *h = g + 20;
	uint32_t sum = 0;
	int i;

	memset(g, 0, IBCM_GRH_LEN);
	h[0] = 0x45;
	put(h, 2, IBCM_IP4_MAD_LEN, 2);
	put(h, 6, 0x4000, 2);
	h[8] = 64;
	h[9] = 17;
	put(h, 12, 0xc0000201, 4);
	put(h, 16, 0xc6336401, 4);
	if (off >= 0)
		g[off] = val;
	for (i = 0; i < 20; i += 2)
		sum += (uint32_t)(h[i] << 8 | h[i + 1]);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	put(h, 10, ~sum & 0xffff, 2);
}

static void
t_ip4(void)
{
	static const struct {
		int	off;
		uint8_t	val;
	} bad[] = {
		{ 20, 0x46 }, { 20, 0x65 }, { 22, 0x00 }, { 23, 0x00 },
		{ 26, 0x60 }, { 27, 0x01 }, { 29, 6 }, { 32, 224 },
		{ 32, 255 }, { 32, 0 }, { 36, 127 }, { 36, 240 }
	};
	rdk_ibcm_ip4_t ip;
	uint8_t g[IBCM_GRH_LEN];
	unsigned int i;

	ip4(g, -1, 0);
	CHECK(rdk_ibcm_parse_ip4(g, &ip) == 0);
	CHECK(ip.ip_src == htonl(0xc0000201) && ip.ip_dst == htonl(0xc6336401));
	CHECK(ip.ip_ttl == 64);
	ip4(g, 21, 0x68);
	CHECK(rdk_ibcm_parse_ip4(g, &ip) == 0 && ip.ip_tos == 0x68);
	ip4(g, 26, 0x00);
	CHECK(rdk_ibcm_parse_ip4(g, &ip) == 0);
	for (i = 0; i < sizeof (bad) / sizeof (bad[0]); i++) {
		ip4(g, bad[i].off, bad[i].val);
		CHECK(rdk_ibcm_parse_ip4(g, &ip) != 0);
	}
	ip4(g, -1, 0);
	g[30] ^= 0xff;
	CHECK(rdk_ibcm_parse_ip4(g, &ip) != 0);
}

static void
t_cma(void)
{
	uint8_t p[IBCM_REQ_PDATA];
	rdk_cma_hdr_t h, g;

	h.ch_src = htonl(0x0a000001);
	h.ch_dst = htonl(0x0a000002);
	h.ch_sport = htons(4791);
	rdk_cma_hdr_build(p, &h);
	CHECK(p[0] == 0 && p[1] == 0x40 && p[2] == 0x12 && p[3] == 0xb7);
	CHECK(p[16] == 10 && p[19] == 1 && p[32] == 10 && p[35] == 2);
	CHECK(rdk_cma_hdr_parse(p, sizeof (p), &g) == 0);
	CHECK(g.ch_src == h.ch_src && g.ch_dst == h.ch_dst &&
	    g.ch_sport == h.ch_sport);
	CHECK(rdk_cma_hdr_parse(p, IBCM_CMA_HDR_LEN - 1, &g) != 0);
	p[0] = 1;
	CHECK(rdk_cma_hdr_parse(p, sizeof (p), &g) != 0);
	p[0] = 0;
	p[1] = 0x60;
	CHECK(rdk_cma_hdr_parse(p, sizeof (p), &g) != 0);
	p[1] = 0x40;
	p[2] = p[3] = 0;
	CHECK(rdk_cma_hdr_parse(p, sizeof (p), &g) != 0);
	rdk_cma_hdr_build(p, &h);
	p[16] = 224;
	CHECK(rdk_cma_hdr_parse(p, sizeof (p), &g) != 0);
	rdk_cma_hdr_build(p, &h);
	p[32] = 0;
	p[35] = 0;
	CHECK(rdk_cma_hdr_parse(p, sizeof (p), &g) != 0);
}

/* A malformed header, attribute or field is refused. */
static void
t_refusals(void)
{
	uint8_t b[IBCM_MAD_LEN];
	rdk_ibcm_msg_t m;
	uint64_t guid;

	req_bytes(b);
	b[0] = 2;
	CHECK(rdk_ibcm_parse(b, &m) == ENOTSUP);
	req_bytes(b);
	b[1] = 4;
	CHECK(rdk_ibcm_parse(b, &m) == ENOTSUP);
	req_bytes(b);
	b[2] = 1;
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	b[3] = 0x81;
	CHECK(rdk_ibcm_parse(b, &m) == ENOTSUP);
	req_bytes(b);
	b[5] = 1;
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	b[17] = 0x17;				/* SIDR REQ */
	CHECK(rdk_ibcm_parse(b, &m) == ENOTSUP);
	req_bytes(b);
	b[67] = (20 << 3) | (1 << 1);		/* UC */
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	put(b, 56, 1, 3);			/* QP1 */
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	put(b, 56, 0xffffff, 3);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	put(b, 72, 0x7fff, 2);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	b[74] = (6 << 4);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	b[74] = 0;
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	b[75] |= 1;				/* extended transport: XRC */
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	req_bytes(b);
	put(b, 24, 0, 4);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);

	hdr(b, IBCM_ATTR_REJ);
	put(b, 28, 1, 4);
	b[33] = 73 << 1;
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	b[33] = 72 << 1;
	CHECK(rdk_ibcm_parse(b, &m) == 0 && m.m_ari_len == 72);
	b[32] = 3 << 6;
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);

	/* A timed-out REJ from Linux: no remote ID, the CA GUID in the ARI. */
	hdr(b, IBCM_ATTR_REJ);
	put(b, 24, 0x51, 4);
	b[33] = 8 << 1;
	put(b, 34, IBCM_REJ_TIMEOUT, 2);
	put(b, 36, 0x0123456789abcdefULL, 8);
	CHECK(rdk_ibcm_parse(b, &m) == 0 && m.m_remote_id == 0);
	CHECK(rdk_ibcm_rej_by_guid(&m, &guid) &&
	    guid == 0x0123456789abcdefULL && m.m_local_id == 0x51);
	b[33] = 7 << 1;
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	b[33] = 8 << 1;
	put(b, 34, 28, 2);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	put(b, 34, IBCM_REJ_TIMEOUT, 2);
	put(b, 24, 0, 4);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	/* With a remote ID it is still found by the GUID, as Linux does. */
	put(b, 24, 0x51, 4);
	put(b, 28, 0x77, 4);
	CHECK(rdk_ibcm_parse(b, &m) == 0 && rdk_ibcm_rej_by_guid(&m, NULL));
	put(b, 34, 28, 2);
	CHECK(rdk_ibcm_parse(b, &m) == 0 && !rdk_ibcm_rej_by_guid(&m, NULL));

	hdr(b, IBCM_ATTR_MRA);
	put(b, 24, 1, 4);
	put(b, 28, 1, 4);
	b[32] = 3 << 6;
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	hdr(b, IBCM_ATTR_DREQ);
	put(b, 24, 1, 4);
	put(b, 28, 1, 4);
	put(b, 32, 0, 3);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
	hdr(b, IBCM_ATTR_RTU);
	put(b, 24, 1, 4);
	CHECK(rdk_ibcm_parse(b, &m) == EINVAL);
}

int
main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100000;

	t_req_layout();
	t_rep_rej_layout();
	t_refusals();
	t_ip4();
	t_cma();
	t_roundtrip(n / 10);
	t_fuzz(n);
	(void) printf("layouts, refusals, IPv4 and CMA headers, %d round "
	    "trips checked\n", n / 10);
	return (0);
}

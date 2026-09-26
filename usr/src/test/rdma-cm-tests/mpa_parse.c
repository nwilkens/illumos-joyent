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
 * The iwcxgbe MPA start frame parser against good, malformed and random
 * input from the peer, delivered whole, a byte at a time and in random
 * pieces.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iwc_mpa.h"

#ifndef MIN
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#endif

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		exit(1);						\
	}								\
} while (0)

static uint64_t seed = 0x9e3779b97f4a7c15ULL;

static uint32_t
rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return ((uint32_t)(seed >> 11));
}

/* Feed buf in pieces; the result of the last call that ended the frame. */
static int
feed(iwc_mpa_rx_t *rx, const uint8_t *buf, size_t len, int mode,
    boolean_t reply, iwc_mpa_msg_t *msg)
{
	size_t off = 0, n;
	int ret = EAGAIN;

	(void) memset(rx, 0, sizeof (*rx));
	if (len == 0)
		return (iwc_mpa_rx(rx, buf, 0, reply, msg));
	while (off < len) {
		if (mode == 0)
			n = len - off;
		else if (mode == 1)
			n = 1;
		else
			n = 1 + rnd() % (len - off);
		ret = iwc_mpa_rx(rx, buf + off, n, reply, msg);
		off += n;
		if (ret != EAGAIN)
			break;
	}
	return (ret);
}

/* A parsed frame never points outside what was received. */
static void
sane(const iwc_mpa_rx_t *rx, const iwc_mpa_msg_t *m)
{
	CHECK(m->mm_pdata >= rx->mr_buf + IWC_MPA_HDR_LEN);
	CHECK(m->mm_pdata + m->mm_pdata_len <= rx->mr_buf + rx->mr_len);
	CHECK(m->mm_pdata_len <= IWC_MPA_MAX_PDATA);
	CHECK(rx->mr_len <= sizeof (rx->mr_buf));
	CHECK(m->mm_rev == 1 || m->mm_rev == 2);
	CHECK(!m->mm_v2 || m->mm_rev == 2);
	CHECK((m->mm_ird & ~IWC_MPA_V2_MASK) == 0);
	CHECK((m->mm_ord & ~IWC_MPA_V2_MASK) == 0);
}

static void
roundtrip(void)
{
	static const uint8_t p2p[] = { IWC_P2P_WRITE, IWC_P2P_READ,
	    IWC_P2P_DISABLED };
	uint8_t frame[IWC_MPA_MAX_FRAME + 8], pdata[IWC_MPA_MAX_PDATA];
	iwc_mpa_rx_t rx;
	iwc_mpa_msg_t m;
	uint32_t n = 0;
	size_t len;
	int v2, rep, mode, t;
	uint16_t plen, max;

	for (plen = 0; plen < sizeof (pdata); plen++)
		pdata[plen] = (uint8_t)(plen * 7 + 3);
	for (rep = 0; rep < 2; rep++)
	for (v2 = 0; v2 < 2; v2++)
	for (t = 0; t < 3; t++) {
		max = v2 ? IWC_MPA_MAX_ULP_PDATA : IWC_MPA_MAX_PDATA;
		for (plen = 0; plen <= max + 1; plen++) {
			len = iwc_mpa_build(frame, sizeof (frame), rep,
			    IWC_MPA_CRC, v2 ? 2 : 1, v2, 0x1234 + plen,
			    0x0567 + t, p2p[t], pdata, plen);
			if (plen > max) {
				CHECK(len == 0);
				continue;
			}
			CHECK(len == IWC_MPA_HDR_LEN + plen +
			    (v2 ? IWC_MPA_V2_LEN : 0));
			for (mode = 0; mode < 3; mode++) {
				CHECK(feed(&rx, frame, len, mode, rep, &m) ==
				    0);
				sane(&rx, &m);
				CHECK((int)m.mm_v2 == v2);
				CHECK(m.mm_pdata_len == plen);
				CHECK(memcmp(m.mm_pdata, pdata, plen) == 0);
				CHECK((m.mm_flags & IWC_MPA_CRC) != 0);
				if (v2) {
					CHECK(m.mm_ird ==
					    ((0x1234 + plen) & 0x3fff));
					CHECK(m.mm_ord == 0x0567 + t);
					CHECK(m.mm_p2p == (p2p[t] !=
					    IWC_P2P_DISABLED));
					CHECK(m.mm_rtr == p2p[t]);
				} else {
					CHECK(m.mm_rtr == IWC_P2P_DISABLED);
				}
				/* The other kind of frame is refused. */
				CHECK(feed(&rx, frame, len, mode, !rep, &m) ==
				    EPROTO);
				n++;
			}
		}
	}
	/* A frame too big for the buffer, and a request for v2 on rev 1. */
	CHECK(iwc_mpa_build(frame, IWC_MPA_HDR_LEN + 3, B_FALSE, 0, 1,
	    B_FALSE, 0, 0, IWC_P2P_DISABLED, pdata, 4) == 0);
	CHECK(iwc_mpa_build(frame, sizeof (frame), B_FALSE, 0, 1, B_TRUE, 0,
	    0, IWC_P2P_DISABLED, NULL, 0) == 0);
	CHECK(iwc_mpa_build(frame, sizeof (frame), B_FALSE, 0, 2, B_FALSE, 0,
	    0, IWC_P2P_DISABLED, NULL, 1) == 0);
	(void) printf("round trip: %u frames\n", n);
}

static void
malformed(void)
{
	uint8_t good[IWC_MPA_MAX_FRAME], bad[IWC_MPA_MAX_FRAME + 16];
	uint8_t big[2 * IWC_MPA_MAX_FRAME];
	iwc_mpa_rx_t rx;
	iwc_mpa_msg_t m;
	size_t len, i;
	int mode, ret;
	uint16_t plen;

	len = iwc_mpa_build(good, sizeof (good), B_FALSE, 0, 2, B_TRUE, 1, 1,
	    IWC_P2P_READ, "abcd", 4);
	CHECK(len == IWC_MPA_HDR_LEN + 8);

	for (mode = 0; mode < 3; mode++) {
		/* Every flipped key byte. */
		for (i = 0; i < IWC_MPA_KEY_LEN; i++) {
			(void) memcpy(bad, good, len);
			bad[i] ^= 0x20;
			CHECK(feed(&rx, bad, len, mode, B_FALSE, &m) == EPROTO);
		}
		/* Revisions other than 1 and 2. */
		for (i = 0; i < 256; i++) {
			if (i == 1 || i == 2)
				continue;
			(void) memcpy(bad, good, len);
			bad[17] = (uint8_t)i;
			CHECK(feed(&rx, bad, len, mode, B_FALSE, &m) == EPROTO);
		}
		/* Private data longer than the limit, up to 0xffff. */
		for (plen = IWC_MPA_MAX_PDATA + 1; plen != 0 &&
		    plen < 0xffff; plen = (uint16_t)(plen * 2 + 1)) {
			(void) memcpy(bad, good, IWC_MPA_HDR_LEN);
			bad[18] = (uint8_t)(plen >> 8);
			bad[19] = (uint8_t)plen;
			CHECK(feed(&rx, bad, IWC_MPA_HDR_LEN, mode, B_FALSE,
			    &m) == EPROTO);
		}
		(void) memcpy(bad, good, IWC_MPA_HDR_LEN);
		bad[18] = bad[19] = 0xff;
		CHECK(feed(&rx, bad, IWC_MPA_HDR_LEN, mode, B_FALSE, &m) ==
		    EPROTO);
		/* An enhanced frame without its four parameter bytes. */
		for (plen = 0; plen < IWC_MPA_V2_LEN; plen++) {
			(void) memcpy(bad, good, IWC_MPA_HDR_LEN + plen);
			bad[18] = 0;
			bad[19] = (uint8_t)plen;
			CHECK(feed(&rx, bad, IWC_MPA_HDR_LEN + plen, mode,
			    B_FALSE, &m) == EPROTO);
		}
		/* Revision 1 with the enhanced bit is not v2. */
		(void) memcpy(bad, good, len);
		bad[17] = 1;
		CHECK(feed(&rx, bad, len, mode, B_FALSE, &m) == 0);
		sane(&rx, &m);
		CHECK(!m.mm_v2 && m.mm_pdata_len == 8);
		/* Bytes past the frame, as a peer that did not wait. */
		(void) memcpy(bad, good, len);
		bad[len] = 0;
		ret = feed(&rx, bad, len + 1, mode, B_FALSE, &m);
		CHECK(ret == EPROTO || (mode != 0 && ret == 0));
		/* A short frame waits for the rest. */
		CHECK(feed(&rx, good, len - 1, mode, B_FALSE, &m) == EAGAIN);
		CHECK(feed(&rx, good, IWC_MPA_HDR_LEN - 1, mode, B_FALSE,
		    &m) == EAGAIN);
	}

	/* More than a frame can hold, in one piece or after a partial one. */
	(void) memset(big, 0, sizeof (big));
	(void) memset(&rx, 0, sizeof (rx));
	CHECK(iwc_mpa_rx(&rx, big, sizeof (rx.mr_buf) + 1, B_FALSE, &m) ==
	    EMSGSIZE);
	CHECK(rx.mr_len == 0);
	(void) memset(&rx, 0, sizeof (rx));
	CHECK(iwc_mpa_rx(&rx, good, 10, B_FALSE, &m) == EAGAIN);
	CHECK(iwc_mpa_rx(&rx, big, sizeof (rx.mr_buf) - 9, B_FALSE, &m) ==
	    EMSGSIZE);
	CHECK(rx.mr_len == 10);
	/* A corrupt length already in the buffer is refused. */
	rx.mr_len = sizeof (rx.mr_buf) + 1;
	CHECK(iwc_mpa_rx(&rx, big, 0, B_FALSE, &m) == EMSGSIZE);
	(void) printf("malformed: every case refused\n");
}

/* Random and mutated frames: no result outside the contract. */
static void
fuzz(uint32_t iters)
{
	uint8_t good[IWC_MPA_MAX_FRAME], buf[IWC_MPA_MAX_FRAME + 64];
	uint8_t pdata[IWC_MPA_MAX_PDATA];
	iwc_mpa_rx_t rx;
	iwc_mpa_msg_t m;
	uint32_t i, counts[4] = { 0 };
	size_t glen, len, k;
	int ret;

	for (k = 0; k < sizeof (pdata); k++)
		pdata[k] = (uint8_t)rnd();
	for (i = 0; i < iters; i++) {
		glen = iwc_mpa_build(good, sizeof (good), rnd() & 1,
		    (uint8_t)(rnd() & 0xc0), (uint8_t)(1 + (rnd() & 1)), B_FALSE,
		    0, 0, IWC_P2P_DISABLED, pdata, rnd() % 200);
		if (glen == 0)
			glen = iwc_mpa_build(good, sizeof (good), rnd() & 1,
			    0, 2, B_TRUE, (uint16_t)rnd(), (uint16_t)rnd(),
			    (uint8_t)(rnd() % 3), pdata, rnd() % 200);
		CHECK(glen != 0);
		switch (rnd() % 4) {
		case 0:
			len = rnd() % sizeof (buf);
			for (k = 0; k < len; k++)
				buf[k] = (uint8_t)rnd();
			if (len >= IWC_MPA_KEY_LEN && (rnd() & 1))
				(void) memcpy(buf, good, IWC_MPA_KEY_LEN);
			break;
		case 1:
			len = glen;
			(void) memcpy(buf, good, len);
			for (k = 1 + rnd() % 4; k > 0; k--)
				buf[16 + rnd() % (len - 16)] ^=
				    (uint8_t)(1 << (rnd() % 8));
			break;
		case 2:
			len = glen;
			(void) memcpy(buf, good, len);
			buf[18] = (uint8_t)rnd();
			buf[19] = (uint8_t)rnd();
			break;
		default:
			len = MIN(glen + rnd() % 64, sizeof (buf));
			(void) memcpy(buf, good, glen);
			for (k = glen; k < len; k++)
				buf[k] = (uint8_t)rnd();
			break;
		}
		ret = feed(&rx, buf, len, (int)(rnd() % 3), rnd() & 1, &m);
		switch (ret) {
		case 0:
			sane(&rx, &m);
			counts[0]++;
			break;
		case EAGAIN:
			CHECK(rx.mr_len == len);
			counts[1]++;
			break;
		case EPROTO:
			counts[2]++;
			break;
		case EMSGSIZE:
			counts[3]++;
			break;
		default:
			CHECK(!"unexpected result");
		}
	}
	(void) printf("fuzz: %u inputs: %u parsed, %u short, %u refused, %u "
	    "oversize\n", iters, counts[0], counts[1], counts[2], counts[3]);
	CHECK(counts[0] != 0 && counts[2] != 0);
}

int
main(int argc, char **argv)
{
	uint32_t iters = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) :
	    200000;

	roundtrip();
	malformed();
	fuzz(iters);
	return (0);
}

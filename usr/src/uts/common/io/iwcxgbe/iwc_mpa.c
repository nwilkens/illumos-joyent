/*
 * Copyright (c) 2009-2010 Chelsio, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * MPA start frames.  The receive checks are those of process_mpa_request()
 * and process_mpa_reply() in Linux cxgb4/cm.c, plus one Linux lacks: an
 * enhanced frame must carry the four bytes of its v2 parameters.
 */

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/byteorder.h>
#else
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <arpa/inet.h>
#define	BE_16(x)	ntohs(x)
#endif

#include "iwc_mpa.h"

static const char iwc_mpa_key_req[] = "MPA ID Req Frame";
static const char iwc_mpa_key_rep[] = "MPA ID Rep Frame";

static uint16_t
iwc_mpa_get16(const uint8_t *p)
{
	return ((uint16_t)((p[0] << 8) | p[1]));
}

static void
iwc_mpa_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

int
iwc_mpa_rx(iwc_mpa_rx_t *rx, const uint8_t *data, size_t len,
    boolean_t reply, iwc_mpa_msg_t *msg)
{
	const uint8_t *key = reply ? (const uint8_t *)iwc_mpa_key_rep :
	    (const uint8_t *)iwc_mpa_key_req;
	const uint8_t *b = rx->mr_buf;
	uint16_t plen, ird, ord;

	if (rx->mr_len > sizeof (rx->mr_buf) ||
	    len > sizeof (rx->mr_buf) - rx->mr_len)
		return (EMSGSIZE);
	if (len != 0)
		bcopy(data, &rx->mr_buf[rx->mr_len], len);
	rx->mr_len += (uint16_t)len;
	if (rx->mr_len < IWC_MPA_HDR_LEN)
		return (EAGAIN);

	if (bcmp(b, key, IWC_MPA_KEY_LEN) != 0)
		return (EPROTO);
	if (b[17] < 1 || b[17] > 2)
		return (EPROTO);
	plen = iwc_mpa_get16(&b[18]);
	if (plen > IWC_MPA_MAX_PDATA)
		return (EPROTO);
	if (rx->mr_len > IWC_MPA_HDR_LEN + plen)
		return (EPROTO);
	if (rx->mr_len < IWC_MPA_HDR_LEN + plen)
		return (EAGAIN);

	bzero(msg, sizeof (*msg));
	msg->mm_flags = b[16];
	msg->mm_rev = b[17];
	msg->mm_pdata = &b[IWC_MPA_HDR_LEN];
	msg->mm_pdata_len = plen;
	msg->mm_rtr = IWC_P2P_DISABLED;
	if (msg->mm_rev == 2 && (msg->mm_flags & IWC_MPA_ENHANCED) != 0) {
		if (plen < IWC_MPA_V2_LEN)
			return (EPROTO);
		ird = iwc_mpa_get16(&b[IWC_MPA_HDR_LEN]);
		ord = iwc_mpa_get16(&b[IWC_MPA_HDR_LEN + 2]);
		msg->mm_v2 = B_TRUE;
		msg->mm_ird = ird & IWC_MPA_V2_MASK;
		msg->mm_ord = ord & IWC_MPA_V2_MASK;
		msg->mm_p2p = (ird & IWC_MPA_V2_P2P) != 0;
		if (msg->mm_p2p) {
			if ((ord & IWC_MPA_V2_WRITE_RTR) != 0)
				msg->mm_rtr = IWC_P2P_WRITE;
			else if ((ord & IWC_MPA_V2_READ_RTR) != 0)
				msg->mm_rtr = IWC_P2P_READ;
		}
		msg->mm_pdata += IWC_MPA_V2_LEN;
		msg->mm_pdata_len -= IWC_MPA_V2_LEN;
	}
	return (0);
}

size_t
iwc_mpa_build(uint8_t *buf, size_t bufsz, boolean_t reply, uint8_t flags,
    uint8_t rev, boolean_t v2, uint16_t ird, uint16_t ord, uint8_t p2p_type,
    const void *pdata, uint16_t plen)
{
	const size_t v2len = v2 ? IWC_MPA_V2_LEN : 0;
	const size_t total = IWC_MPA_HDR_LEN + v2len + plen;
	uint8_t *p;

	if (v2len + plen > IWC_MPA_MAX_PDATA || total > bufsz ||
	    (plen != 0 && pdata == NULL) || (v2 && rev != 2))
		return (0);
	bzero(buf, total);
	bcopy(reply ? iwc_mpa_key_rep : iwc_mpa_key_req, buf,
	    IWC_MPA_KEY_LEN);
	buf[16] = flags | (v2 ? IWC_MPA_ENHANCED : 0);
	buf[17] = rev;
	iwc_mpa_put16(&buf[18], (uint16_t)(v2len + plen));
	p = &buf[IWC_MPA_HDR_LEN];
	if (v2) {
		ird &= IWC_MPA_V2_MASK;
		ord &= IWC_MPA_V2_MASK;
		if (p2p_type != IWC_P2P_DISABLED) {
			ird |= IWC_MPA_V2_P2P;
			if (p2p_type == IWC_P2P_WRITE)
				ord |= IWC_MPA_V2_WRITE_RTR;
			else if (p2p_type == IWC_P2P_READ)
				ord |= IWC_MPA_V2_READ_RTR;
		}
		iwc_mpa_put16(p, ird);
		iwc_mpa_put16(p + 2, ord);
		p += IWC_MPA_V2_LEN;
	}
	if (plen != 0)
		bcopy(pdata, p, plen);
	return (total);
}

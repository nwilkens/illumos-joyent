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

#ifndef _IWC_MPA_H
#define	_IWC_MPA_H

/*
 * MPA start frames (RFC 5044, with the RFC 6581 enhanced connection
 * parameters): the request and reply the provider exchanges over the
 * offloaded TCP connection before it moves to RDMA mode.  The frame layout
 * and the negotiation rules follow Linux drivers/infiniband/hw/cxgb4/cm.c
 * under the OpenIB license.  The parser takes bytes from the remote peer.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	IWC_MPA_KEY_LEN		16
#define	IWC_MPA_HDR_LEN		20
#define	IWC_MPA_V2_LEN		4
/* The private data field, the v2 parameters included (Linux's limit). */
#define	IWC_MPA_MAX_PDATA	256
#define	IWC_MPA_MAX_FRAME	(IWC_MPA_HDR_LEN + IWC_MPA_MAX_PDATA)
/* What a consumer may send with v2. */
#define	IWC_MPA_MAX_ULP_PDATA	(IWC_MPA_MAX_PDATA - IWC_MPA_V2_LEN)

#define	IWC_MPA_ENHANCED	0x10
#define	IWC_MPA_REJECT		0x20
#define	IWC_MPA_CRC		0x40
#define	IWC_MPA_MARKERS		0x80

#define	IWC_MPA_V2_P2P		0x8000	/* in IRD */
#define	IWC_MPA_V2_WRITE_RTR	0x8000	/* in ORD */
#define	IWC_MPA_V2_READ_RTR	0x4000	/* in ORD */
#define	IWC_MPA_V2_MASK		0x3fff

/* The ready-to-receive message types, as FW_RI_INIT_P2PTYPE_* uses them. */
#define	IWC_P2P_WRITE		0x0
#define	IWC_P2P_READ		0x1
#define	IWC_P2P_DISABLED	0xf

typedef struct iwc_mpa_attr {
	boolean_t	ma_initiator;
	boolean_t	ma_crc;
	boolean_t	ma_tx_markers;
	boolean_t	ma_enhanced;
	uint8_t		ma_version;
	uint8_t		ma_p2p_type;
} iwc_mpa_attr_t;

/* Bytes of a start frame received so far. */
typedef struct iwc_mpa_rx {
	uint8_t		mr_buf[IWC_MPA_MAX_FRAME];
	uint16_t	mr_len;
} iwc_mpa_rx_t;

/* A complete start frame; mm_pdata points into the receive buffer. */
typedef struct iwc_mpa_msg {
	uint8_t		mm_rev;
	uint8_t		mm_flags;
	boolean_t	mm_v2;
	uint16_t	mm_ird;
	uint16_t	mm_ord;
	boolean_t	mm_p2p;
	uint8_t		mm_rtr;		/* IWC_P2P_* the peer asks for */
	const uint8_t	*mm_pdata;
	uint16_t	mm_pdata_len;
} iwc_mpa_msg_t;

/*
 * Add received bytes.  0 when the frame is complete, EAGAIN when more is
 * needed, EPROTO for a frame that is not a valid start frame of the
 * expected kind, and EMSGSIZE when the peer sent more than one frame
 * holds.
 */
extern int iwc_mpa_rx(iwc_mpa_rx_t *, const uint8_t *, size_t, boolean_t,
    iwc_mpa_msg_t *);

/*
 * Build a start frame into buf.  Returns its length, or 0 when the private
 * data does not fit.
 */
extern size_t iwc_mpa_build(uint8_t *, size_t, boolean_t, uint8_t, uint8_t,
    boolean_t, uint16_t, uint16_t, uint8_t, const void *, uint16_t);

#ifdef __cplusplus
}
#endif

#endif /* _IWC_MPA_H */

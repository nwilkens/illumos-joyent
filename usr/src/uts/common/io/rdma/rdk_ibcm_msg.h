/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
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

#ifndef _RDK_IBCM_MSG_H
#define	_RDK_IBCM_MSG_H

/*
 * IB CM messages in RoCEv2 MADs, and the RDMA CM's IP header in a REQ's
 * private data.  The field layout is IBTA volume 1 chapter 12 as Linux
 * include/rdma/ibta_vol1_c12.h gives it, the attribute IDs follow
 * drivers/infiniband/core/cm_msgs.h and the header and service IDs follow
 * drivers/infiniband/core/cma.c; see README.illumos.  Every byte parsed here
 * comes from the network.  Builds in the kernel and on the host for tests.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	IBCM_MAD_LEN		256
#define	IBCM_MAD_HDR_LEN	24
#define	IBCM_GRH_LEN		40
/* IPv4 + UDP + BTH + DETH + MAD + ICRC, the IPv4 total length of a MAD. */
#define	IBCM_IP4_MAD_LEN	308

#define	IBCM_BASE_VERSION	1
#define	IBCM_MGMT_CLASS_CM	0x07
#define	IBCM_CLASS_VERSION	2
#define	IBCM_METHOD_SEND	0x03
#define	IBCM_QP1_QKEY		0x80010000U
#define	IBCM_PKEY_DEFAULT	0xffff

#define	IBCM_ATTR_REQ		0x0010
#define	IBCM_ATTR_MRA		0x0011
#define	IBCM_ATTR_REJ		0x0012
#define	IBCM_ATTR_REP		0x0013
#define	IBCM_ATTR_RTU		0x0014
#define	IBCM_ATTR_DREQ		0x0015
#define	IBCM_ATTR_DREP		0x0016

/* Private data sizes; the wire carries no length. */
#define	IBCM_REQ_PDATA		92
#define	IBCM_MRA_PDATA		222
#define	IBCM_REJ_PDATA		148
#define	IBCM_REP_PDATA		196
#define	IBCM_RTU_PDATA		224
#define	IBCM_DREQ_PDATA		220
#define	IBCM_DREP_PDATA		224
#define	IBCM_REJ_ARI_MAX	72

/* The RDMA CM header at the front of a REQ's private data. */
#define	IBCM_CMA_HDR_LEN	36
#define	IBCM_CMA_VERSION	0
#define	IBCM_CMA_REQ_PDATA	(IBCM_REQ_PDATA - IBCM_CMA_HDR_LEN)
#define	IBCM_PS_TCP		0x0106
#define	IBCM_SID_TCP(port)	((uint64_t)IBCM_PS_TCP << 16 | (port))

/* A timed-out REJ names the connection by the sender's CA GUID in its ARI. */
#define	IBCM_REJ_TIMEOUT	4

#define	IBCM_TRANSPORT_RC	0
#define	IBCM_QPN_MAX		0xfffffe
#define	IBCM_LID_PERMISSIVE	0xffff

/* One CM message, in host order; the private data points into the MAD. */
typedef struct rdk_ibcm_msg {
	uint16_t	m_attr;
	uint32_t	m_attr_mod;
	uint64_t	m_tid;
	uint32_t	m_local_id;
	uint32_t	m_remote_id;

	/* REQ */
	uint64_t	m_service_id;
	uint64_t	m_ca_guid;		/* REQ, REP */
	uint32_t	m_qpn;			/* REQ, REP; DREQ remote */
	uint32_t	m_psn;			/* REQ, REP */
	uint8_t		m_resp_res;		/* REQ, REP */
	uint8_t		m_init_depth;		/* REQ, REP */
	uint8_t		m_remote_resp_to;
	uint8_t		m_local_resp_to;
	uint8_t		m_transport;
	uint8_t		m_flow_ctl;		/* REQ, REP */
	uint8_t		m_retry;
	uint8_t		m_rnr_retry;		/* REQ, REP */
	uint8_t		m_max_retries;
	uint8_t		m_mtu;
	uint8_t		m_srq;			/* REQ, REP */
	uint16_t	m_pkey;
	uint16_t	m_llid;
	uint16_t	m_rlid;
	uint8_t		m_lgid[16];		/* the sender's */
	uint8_t		m_rgid[16];		/* the receiver's */
	uint32_t	m_flow_label;
	uint8_t		m_rate;
	uint8_t		m_tclass;
	uint8_t		m_hop_limit;
	uint8_t		m_sl;
	uint8_t		m_subnet_local;
	uint8_t		m_ack_timeout;
	uint32_t	m_vendor_id;		/* REQ, REP */

	/* REP */
	uint8_t		m_target_ack_delay;
	uint8_t		m_failover;

	/* REJ and MRA */
	uint8_t		m_msg;			/* message rejected or MRAed */
	uint16_t	m_reason;
	uint8_t		m_ari_len;
	uint8_t		m_ari[IBCM_REJ_ARI_MAX];
	uint8_t		m_service_timeout;

	const uint8_t	*m_pdata;
	uint16_t	m_pdata_len;
} rdk_ibcm_msg_t;

typedef struct rdk_cma_hdr {
	uint32_t	ch_src;		/* network order */
	uint32_t	ch_dst;
	uint16_t	ch_sport;	/* network order */
} rdk_cma_hdr_t;

/* The IPv4 header of a RoCEv2 MAD, in the 40 bytes before it. */
typedef struct rdk_ibcm_ip4 {
	uint32_t	ip_src;		/* network order */
	uint32_t	ip_dst;
	uint8_t		ip_ttl;
	uint8_t		ip_tos;
} rdk_ibcm_ip4_t;

extern int rdk_ibcm_parse_ip4(const uint8_t *, rdk_ibcm_ip4_t *);
extern int rdk_ibcm_parse(const uint8_t *, rdk_ibcm_msg_t *);
extern void rdk_ibcm_build(uint8_t *, const rdk_ibcm_msg_t *);
extern uint16_t rdk_ibcm_pdata_len(uint16_t);
extern int rdk_cma_hdr_parse(const uint8_t *, uint16_t, rdk_cma_hdr_t *);
extern void rdk_cma_hdr_build(uint8_t *, const rdk_cma_hdr_t *);
extern boolean_t rdk_ibcm_gid_ip4(const uint8_t *, uint32_t *);
extern boolean_t rdk_ibcm_rej_by_guid(const rdk_ibcm_msg_t *, uint64_t *);
extern void rdk_ibcm_ip4_gid(uint32_t, uint8_t *);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_IBCM_MSG_H */

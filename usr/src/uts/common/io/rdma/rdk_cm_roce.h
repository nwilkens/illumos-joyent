/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2005 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2002-2005, Network Appliance, Inc. All rights reserved.
 * Copyright (c) 1999-2019, Mellanox Technologies, Inc. All rights reserved.
 * Copyright (c) 2005-2006 Intel Corporation.  All rights reserved.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#ifndef _RDK_CM_ROCE_H
#define	_RDK_CM_ROCE_H

#include <sys/avl.h>

#include "rdk_cm_impl.h"
#include "rdk_gsi.h"
#include "rdk_ibcm_fsm.h"
#include "rdk_ibcm_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux cma.c: CMA_CM_RESPONSE_TIMEOUT, CMA_MAX_CM_RETRIES and the RoCE PLT */
#define	RDK_IBCM_RESP_TIMEOUT	20
#define	RDK_IBCM_MAX_RETRIES	15
#define	RDK_IBCM_PLT		16
#define	RDK_IBCM_MRA_TIMEOUT	20

/*
 * One IB CM connection.  rdk_ibcm_lock covers the tables; ic_lock covers
 * the rest, and is never held across a verb, a MAD send, a CM event or a
 * table lookup.  ic_busy serializes whole steps (the state change and the
 * actions it gives), so that the actions of one connection run in order.
 */
typedef struct rdk_ibconn {
	avl_node_t		ic_lnode;	/* by local ID */
	avl_node_t		ic_rnode;	/* by remote CA GUID and ID */
	avl_node_t		ic_qnode;	/* by remote CA GUID and QPN */
	list_node_t		ic_node;	/* rdk_ibcm_conns */
	boolean_t		ic_in_l;
	boolean_t		ic_in_r;
	boolean_t		ic_in_q;
	volatile uint32_t	ic_refs;

	kmutex_t		ic_lock;
	kcondvar_t		ic_cv;
	boolean_t		ic_busy;
	rdk_ibcm_fsm_t		ic_fsm;

	rdk_cm_dev_t		*ic_cd;
	rdk_gsi_t		*ic_gsi;	/* held */
	uint32_t		ic_port;
	struct rdk_cm_id	*ic_id;		/* held */
	struct rdk_qp		*ic_qp;		/* see rdk_cm_qp_lease() */
	struct rdk_cm_id	*ic_listener;	/* held until the child is */
	void			*ic_admit;

	uint32_t		ic_lid;
	uint32_t		ic_rid;
	uint64_t		ic_rguid;
	uint64_t		ic_tid;		/* the REQ's, then a DREQ's */
	uint64_t		ic_dreq_tid;	/* the one we sent */
	uint32_t		ic_lqpn;
	uint32_t		ic_rqpn;
	uint32_t		ic_spsn;	/* our starting PSN */
	uint32_t		ic_rpsn;	/* the peer's */
	ipaddr_t		ic_lip;
	ipaddr_t		ic_rip;
	uint16_t		ic_lport;	/* network order */
	uint16_t		ic_rport;
	uint64_t		ic_service_id;
	const struct rdk_gid_attr *ic_sgid;	/* held */
	rdk_gsi_path_t		ic_path;
	uint32_t		ic_flow;
	uint8_t			ic_mtu;		/* enum rdk_mtu */
	uint8_t			ic_ack_timeout;
	uint8_t			ic_retry;
	uint8_t			ic_rnr_retry;	/* what the peer asks of us */
	uint8_t			ic_ask_rnr;	/* what we ask of the peer */
	uint8_t			ic_resp_res;	/* inbound RDMA reads */
	uint8_t			ic_init_depth;	/* outbound RDMA reads */
	uint8_t			ic_peer_resp_res;
	uint8_t			ic_peer_init_depth;

	uint8_t			ic_msg[IBCM_MAD_LEN];	/* the one resent */
	boolean_t		ic_msg_valid;
	uint8_t			ic_pdata[IBCM_REP_PDATA];
	uint16_t		ic_pdata_len;

	timeout_id_t		ic_timer;
	hrtime_t		ic_deadline;	/* 0: no timer is due */
	boolean_t		ic_resolving;	/* the timer is resolution's */
	boolean_t		ic_awaiting;	/* resolution runs */
	rdk_cm_arp_t		*ic_arp;
	taskq_ent_t		ic_tqent;
	boolean_t		ic_tq_queued;
	boolean_t		ic_qp_gone;
} rdk_ibconn_t;

/* An input to a connection, and the message it came with. */
typedef struct rdk_ibconn_in {
	rdk_ibcm_in_t		ci_in;
	const rdk_ibcm_msg_t	*ci_msg;
	struct rdk_cm_id	*ci_attach;	/* CONNECT, ACCEPT: the ID */
} rdk_ibconn_in_t;

extern kmutex_t rdk_ibcm_lock;
extern uint64_t rdk_ibcm_hi_tid;
extern uint_t rdk_cm_roce_resolve_ms;
extern volatile uint_t rdk_cm_roce_resolving;

/* rdk_cm_roce_conn.c */
extern rdk_ibconn_t *rdk_ibconn_alloc(rdk_cm_dev_t *, uint32_t, boolean_t);
extern void rdk_ibconn_hold(rdk_ibconn_t *);
extern void rdk_ibconn_rele(rdk_ibconn_t *);
extern int rdk_ibconn_insert(rdk_ibconn_t *);
extern int rdk_ibconn_insert_remote(rdk_ibconn_t *, rdk_ibconn_t **);
extern void rdk_ibconn_drop_remote(rdk_ibconn_t *);
extern void rdk_ibconn_unlink(rdk_ibconn_t *);
extern rdk_ibconn_t *rdk_ibconn_find(uint32_t);
extern rdk_ibconn_t *rdk_ibconn_find_remote(uint64_t, uint32_t);
extern rdk_ibconn_t *rdk_ibconn_find_qpn(uint64_t, uint32_t);
extern int rdk_cm_qp_attach(rdk_ibconn_t *, struct rdk_qp *);
extern struct rdk_qp *rdk_cm_qp_lease(rdk_ibconn_t *);
extern void rdk_cm_qp_unlease(struct rdk_qp *);
extern void rdk_cm_qp_detach(rdk_ibconn_t *);
extern timeout_id_t rdk_ibconn_timer_set(rdk_ibconn_t *, uint32_t);
extern void rdk_ibconn_timer_cancel(rdk_ibconn_t *, timeout_id_t);
extern void rdk_ibconn_arm(rdk_ibconn_t *, uint32_t);
extern uint32_t rdk_ibcm_clamp_ms(uint32_t);
extern rdk_gsi_t *rdk_cm_roce_gsi(rdk_cm_dev_t *, uint32_t);
extern uint8_t rdk_cm_roce_mtu(struct rdk_device *, uint32_t, uint32_t);

/* rdk_cm_roce.c */
extern boolean_t rdk_ibconn_step(rdk_ibconn_t *, const rdk_ibconn_in_t *);
extern void rdk_ibconn_input(rdk_ibconn_t *, rdk_ibcm_input_t);
extern void rdk_ibcm_msg_init(const rdk_ibconn_t *, rdk_ibcm_msg_t *,
    uint16_t);

/* rdk_cm_roce_rx.c */
extern void rdk_cm_roce_req(rdk_gsi_t *, const rdk_gsi_rx_t *,
    const rdk_ibcm_msg_t *);
extern void rdk_cm_roce_reply(rdk_gsi_t *, const rdk_gsi_rx_t *,
    const rdk_ibcm_msg_t *, uint16_t, uint16_t, uint8_t);
extern void rdk_cm_roce_resolve_done(void *, uint32_t, int, const uint8_t *);
extern void rdk_cm_roce_resolve_timeout(rdk_ibconn_t *);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_CM_ROCE_H */

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

#ifndef _RDK_GSI_H
#define	_RDK_GSI_H

#include <sys/kstat.h>

#include "rdk.h"
#include "rdk_ibcm_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	RDK_GSI_NRECV		256
#define	RDK_GSI_NSEND		128
#define	RDK_GSI_RSLOT		512	/* the GRH area and a MAD, aligned */
#define	RDK_GSI_AH_MAX		64

struct rdk_gsi;

/* Where a MAD goes: the local GID (held by the caller) and the peer. */
typedef struct rdk_gsi_path {
	const struct rdk_gid_attr *gp_sgid;
	rdk_gid_t		gp_dgid;
	uint8_t			gp_dmac[ETHERADDRL];
	uint8_t			gp_hop;
	uint8_t			gp_tclass;
} rdk_gsi_path_t;

/* A MAD received on QP1, copied out of the receive ring. */
typedef struct rdk_gsi_rx {
	taskq_ent_t		rx_tqent;
	struct rdk_gsi		*rx_gsi;
	rdk_ibcm_ip4_t		rx_ip;
	boolean_t		rx_has_smac;
	uint8_t			rx_smac[ETHERADDRL];	/* the frame's source */
	uint8_t			rx_mad[IBCM_MAD_LEN];
} rdk_gsi_rx_t;

typedef struct rdk_gsi_ah {
	list_node_t		ga_node;	/* least recently used first */
	struct rdk_ah		*ga_ah;
	uint16_t		ga_sgid_index;
	rdk_gid_t		ga_sgid;
	rdk_gid_t		ga_dgid;
	uint8_t			ga_dmac[ETHERADDRL];
	uint8_t			ga_hop;
	uint8_t			ga_tclass;
	uint32_t		ga_refs;	/* sends in flight */
	boolean_t		ga_stale;	/* its GID is withdrawn */
	boolean_t		ga_cached;
} rdk_gsi_ah_t;

typedef struct rdk_gsi_send {
	struct rdk_cqe		gs_cqe;
	list_node_t		gs_node;
	struct rdk_gsi		*gs_gsi;
	uint32_t		gs_idx;
	rdk_gsi_ah_t		*gs_ah;
} rdk_gsi_send_t;

typedef struct rdk_gsi_recv {
	struct rdk_cqe		gr_cqe;
	struct rdk_gsi		*gr_gsi;
	uint32_t		gr_idx;
} rdk_gsi_recv_t;

typedef struct rdk_gsi_stats {
	kstat_named_t	gst_rx;
	kstat_named_t	gst_rx_bad;
	kstat_named_t	gst_rx_rate;
	kstat_named_t	gst_rx_queue;
	kstat_named_t	gst_rx_nomem;
	kstat_named_t	gst_rx_error;
	kstat_named_t	gst_tx;
	kstat_named_t	gst_tx_error;
	kstat_named_t	gst_tx_nobufs;
	kstat_named_t	gst_reply_rate;
	kstat_named_t	gst_ah_hit;
	kstat_named_t	gst_ah_create;
	kstat_named_t	gst_ah_evict;
} rdk_gsi_stats_t;

/*
 * rg_lock covers the free send slots, the AH cache, the counts and the
 * rate limiters; it is never held across a verb that may block.
 */
typedef struct rdk_gsi {
	kmutex_t		rg_lock;
	kcondvar_t		rg_cv;
	struct rdk_device	*rg_dev;
	uint32_t		rg_port;
	struct rdk_pd		*rg_pd;
	struct rdk_cq		*rg_scq;
	struct rdk_cq		*rg_rcq;
	struct rdk_qp		*rg_qp;
	rdk_dma_buf_t		rg_rbuf;
	rdk_dma_buf_t		rg_sbuf;
	rdk_gsi_recv_t		rg_recv[RDK_GSI_NRECV];
	rdk_gsi_send_t		rg_send[RDK_GSI_NSEND];
	list_t			rg_sfree;
	list_t			rg_ahs;
	uint32_t		rg_nah;
	uint32_t		rg_ah_max;
	uint64_t		rg_withdraw_gen;	/* GID withdrawals */
	boolean_t		rg_dying;
	uint32_t		rg_refs;
	uint32_t		rg_rx_out;	/* MADs being handled */
	uint32_t		rg_tx_out;	/* sends posted */
	uint32_t		rg_rx_posted;
	hrtime_t		rg_rx_last;
	uint64_t		rg_rx_tokens;	/* in 1/NANOSEC units */
	hrtime_t		rg_reply_last;
	uint64_t		rg_reply_tokens;
	taskq_ent_t		rg_reap_ent;
	boolean_t		rg_reap_queued;
	kstat_t			*rg_ksp;
	rdk_gsi_stats_t		rg_stats;
} rdk_gsi_t;

extern uint_t rdk_gsi_rx_rate;
extern uint_t rdk_gsi_reply_rate;

extern int rdk_gsi_create(struct rdk_device *, uint32_t, rdk_gsi_t **);
extern void rdk_gsi_destroy(rdk_gsi_t *);
extern void rdk_gsi_hold(rdk_gsi_t *);
extern void rdk_gsi_rele(rdk_gsi_t *);
extern int rdk_gsi_send(rdk_gsi_t *, const rdk_gsi_path_t *, const uint8_t *);
extern boolean_t rdk_gsi_reply_ok(rdk_gsi_t *);
extern void rdk_gsi_gid_withdrawn(rdk_gsi_t *, uint16_t);
extern int rdk_gsi_init(void);
extern void rdk_gsi_fini(void);

/* rdk_cm_roce.c handles each MAD, in a CM taskq thread. */
extern void rdk_cm_roce_recv(rdk_gsi_rx_t *);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_GSI_H */

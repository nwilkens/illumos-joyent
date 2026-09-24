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

#ifndef _ICE_RDMA_IMPL_H
#define	_ICE_RDMA_IMPL_H

/*
 * State of the RDMA peer interface shared by ice_rdma.c and ice_rdma_ops.c.
 */

#include "ice.h"
#include "ice_rdma.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Qsets the child can hold at once. */
#define	ICE_RDMA_QSET_TABLE	64

/* Bounds on the child's DMA memory. */
#define	ICE_RDMA_DMA_MAX_ALIGN	(2 * 1024 * 1024)
#define	ICE_RDMA_DMA_MAX_LEN	(4 * 1024 * 1024)
#define	ICE_RDMA_DMA_LIMIT	(2ULL * 1024 * 1024 * 1024)

/* BAR0 window of the RDMA doorbell and push pages. */
#define	ICE_RDMA_DEVMAP_BASE	0x7f0000
#define	ICE_RDMA_DEVMAP_END	0xa00000

typedef struct ice_rdma_buf {
	ice_rdma_dma_t		irb_pub;
	ice_dma_buffer_t	irb_dma;
	list_node_t		irb_node;
} ice_rdma_buf_t;

typedef struct ice_rdma_qrec {
	boolean_t	iqr_used;
	uint16_t	iqr_handle;
	uint8_t		iqr_tc;
	uint32_t	iqr_teid;
} ice_rdma_qrec_t;

typedef struct ice_rdma_kstats {
	kstat_named_t	irk_state;
	kstat_named_t	irk_vectors;
	kstat_named_t	irk_generation;
	kstat_named_t	irk_qsets;
	kstat_named_t	irk_dma_bufs;
	kstat_named_t	irk_dma_bytes;
	kstat_named_t	irk_quar_bufs;
	kstat_named_t	irk_quar_bytes;
	kstat_named_t	irk_quar_freed;
	kstat_named_t	irk_events;
	kstat_named_t	irk_reset_requests;
	kstat_named_t	irk_offline_fail;
	kstat_named_t	irk_crit_errors;
} ice_rdma_kstats_t;

/* Coalesced events the worker still owes the client (ir_ev_pending). */
#define	ICE_RDMA_EVP_LINK	0x1
#define	ICE_RDMA_EVP_MTU	0x2
#define	ICE_RDMA_EVP_CRIT	0x4

struct ice_rdma_peer {
	ice_rdma_peer_hdr_t	irp_hdr;
	ice_t			*irp_ice;
};

struct ice_rdma {
	struct ice_rdma_peer	ir_peer;
	uint_t			ir_vectors;

	kmutex_t		ir_cfg_lock;
	dev_info_t		*ir_cdip;	/* ir_cfg_lock */
	boolean_t		ir_online_owed;	/* ir_cfg_lock */

	kmutex_t		ir_lock;
	kcondvar_t		ir_cv;
	boolean_t		ir_stopping;
	boolean_t		ir_resetting;
	uint32_t		ir_gen;
	const ice_rdma_client_t	*ir_client;
	void			*ir_client_arg;
	uint32_t		ir_client_gen;
	uint_t			ir_cb_busy;

	ddi_taskq_t		*ir_evtq;
	uint32_t		ir_ev_pending;
	boolean_t		ir_ev_queued;
	uint32_t		ir_ev_oicr;
	link_state_t		ir_link;
	uint64_t		ir_speed;

	list_t			ir_bufs;
	list_t			ir_quarantine;
	uint64_t		ir_dma_bytes;
	uint64_t		ir_quar_bytes;
	uint_t			ir_nbufs;
	uint_t			ir_nquar;

	/* ice_rebuild_lock */
	ice_rdma_qrec_t		ir_qsets[ICE_RDMA_QSET_TABLE];
	uint_t			ir_nqsets;
	boolean_t		ir_pe_fltr;

	kstat_t			*ir_kstat;
	ice_rdma_kstats_t	ir_kstats;
	uint64_t		ir_quar_freed;
	uint64_t		ir_events;
	uint64_t		ir_reset_requests;
	uint64_t		ir_offline_fail;
	uint64_t		ir_crit_errors;
};

/* A reset is owed, running, or failed: no admin queue work for the peer. */
#define	ICE_RDMA_DOWN	\
	(ICE_STATE_RESET_FAILED | ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ)

extern const ice_rdma_ops_t ice_rdma_ops;

static inline ice_t *
ice_rdma_peer_ice(ice_rdma_peer_t *peer)
{
	return (peer->irp_ice);
}

extern boolean_t ice_rdma_client_ok(ice_rdma_t *);
extern void ice_rdma_buf_free(ice_rdma_buf_t *);

#ifdef __cplusplus
}
#endif

#endif /* _ICE_RDMA_IMPL_H */

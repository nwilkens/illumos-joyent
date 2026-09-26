/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2015 - 2021 Intel Corporation */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#ifndef _IRDMA_VERBS_H
#define	_IRDMA_VERBS_H

/*
 * The irdma objects behind the rdmak verbs.  The layouts follow the Linux
 * irdma verbs.h; see README.illumos for what was taken from where.
 */

#include "irdma_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	IRDMA_FLUSH_DELAY_MS		20
#define	IRDMA_PKEY_TBL_SZ		1
#define	IRDMA_DEFAULT_PKEY		0xFFFF
#define	IRDMA_ROCE_CWND_DEFAULT		0x400
#define	IRDMA_ROCE_ACKCREDS_DEFAULT	0x1E
#define	IRDMA_MAX_PAGES_PER_FMR		262144
#define	IRDMA_MIN_PAGES_PER_FMR		1
#define	IRDMA_GID_TBL_LEN		RDK_GID_TABLE_LEN
/* The CQ ring must fit one DMA buffer from ice, twice over on GEN2. */
#define	IRDMA_MAX_KCQE			65535
/* Reserved QP, CQ and PD ids: 0, 1 (GSI) and 2. */
#define	IRDMA_FIRST_ID			3
#define	IRDMA_GSI_QPN			1
/* ice hands out DMA buffers of at most this size. */
#define	IRDMA_DMA_BUF_MAX		(4 * 1024 * 1024)

/* RC opcodes of a write with immediate, as the receive CQE reports them. */
#define	IRDMA_RC_WRITE_LAST_IMM		0x09
#define	IRDMA_RC_WRITE_ONLY_IMM		0x0b
#define	IRDMA_ROCE_UDP_DPORT		4791
/* The kernel ABI version the core code expects of a PD and QP. */
#define	IRDMA_ABI_VER			5
/* Minor codes of a QP flush completion. */
#define	IRDMA_CQP_COMPL_RQ_WQE_FLUSHED	2
#define	IRDMA_CQP_COMPL_SQ_WQE_FLUSHED	3

#define	IRDMA_ARP_ADD		1
#define	IRDMA_ARP_DELETE	2
#define	IRDMA_ARP_RESOLVE	3

/* irdma_flush_wqes() */
#define	IRDMA_FLUSH_SQ		0x1
#define	IRDMA_FLUSH_RQ		0x2
#define	IRDMA_REFLUSH		0x4
#define	IRDMA_FLUSH_WAIT	0x8

typedef enum irdma_arp_state {
	IRDMA_ARP_FREE = 0,
	IRDMA_ARP_PENDING,
	IRDMA_ARP_LIVE,
	IRDMA_ARP_DYING
} irdma_arp_state_t;

/* Under irdma_arp_lock. */
typedef struct irdma_arp_entry {
	uint32_t		iae_ip[4];
	uint8_t			iae_mac[ETHERADDRL];
	irdma_arp_state_t	iae_state;
	uint32_t		iae_refs;
} irdma_arp_entry_t;

typedef struct irdma_pd {
	struct rdk_pd		ipd_rdk;	/* first */
	struct irdma_sc_pd	ipd_sc;
} irdma_pd_t;

/* A completion made by the driver for work the device did not flush. */
typedef struct irdma_cmpl_gen {
	list_node_t			icg_node;
	struct irdma_cq_poll_info	icg_cpi;
} irdma_cmpl_gen_t;

/*
 * icq_lock covers polling, arming and the generated completions.  The
 * reference count, under the CEQ's ic_lock, keeps the CQ while its
 * completion handler runs or it waits on the CEQ's ic_resched.
 */
typedef struct irdma_cq {
	struct rdk_cq		icq_rdk;	/* first */
	struct irdma_sc_cq	icq_sc;
	irdma_t			*icq_irdma;
	irdma_ceq_t		*icq_ceq;
	uint32_t		icq_num;
	kmutex_t		icq_lock;
	boolean_t		icq_armed;
	enum irdma_cmpl_notify	icq_last_notify;
	struct irdma_dma_mem	icq_mem;
	struct irdma_dma_mem	icq_shadow;
	list_t			icq_gen;
	uint32_t		icq_refs;
	boolean_t		icq_live;	/* created; under ic_lock */
	boolean_t		icq_dying;
	boolean_t		icq_resched;	/* on ic_resched */
	list_node_t		icq_rnode;
	kcondvar_t		icq_cv;
	uint64_t		icq_bad_cqes;
	struct irdma_cq_poll_info icq_cur;
	uint16_t		icq_hold_us;	/* modify_cq; ic_lock */
	ulong_t			*icq_qpmap;	/* QPs on it; icq_lock */
	size_t			icq_qpmap_size;
} irdma_cq_t;

/* What the driver keeps for each posted receive. */
typedef struct irdma_rq_slot {
	uint64_t	irs_wr_id;
	uint32_t	irs_len;
} irdma_rq_slot_t;

/*
 * iqp_lock covers posting and the state fields; iqp_mod_lock serializes
 * state changes, which issue control commands and so cannot hold iqp_lock.
 * The reference count, under irdma_qptable_lock, keeps the QP while the
 * event and flush work use it.
 */
typedef struct irdma_qp {
	struct rdk_qp		iqp_rdk;	/* first */
	struct irdma_sc_qp	iqp_sc;
	irdma_t			*iqp_irdma;
	irdma_pd_t		*iqp_pd;
	irdma_cq_t		*iqp_scq;
	irdma_cq_t		*iqp_rcq;
	kmutex_t		iqp_lock;
	kmutex_t		iqp_mod_lock;
	kcondvar_t		iqp_cv;		/* iqp_work, under iqp_lock */
	kcondvar_t		iqp_ref_cv;	/* iqp_refs */
	struct irdma_qp_host_ctx_info iqp_ctx;
	struct irdma_roce_offload_info iqp_roce;
	struct irdma_udp_offload_info iqp_udp;
	enum rdk_qp_state	iqp_state;
	uint8_t			iqp_hw_state;	/* IRDMA_QP_STATE_* */
	uint8_t			iqp_hw_ae_state; /* from the last AE */
	uint16_t		iqp_last_ae;
	boolean_t		iqp_flush_issued;
	boolean_t		iqp_sig_all;
	boolean_t		iqp_destroying;
	int			iqp_access;	/* RDK_ACCESS_* */
	boolean_t		iqp_ird_zero;
	uint32_t		iqp_arp_idx;	/* held; iqp_mod_lock */
	uint32_t		iqp_max_send_wr;
	uint32_t		iqp_max_recv_wr;
	struct irdma_dma_mem	iqp_q2ctx;
	struct irdma_dma_mem	iqp_ring;
	struct irdma_sq_uk_wr_trk_info *iqp_sq_wrid;
	u64			*iqp_rq_wrid;
	irdma_rq_slot_t		*iqp_rq_slots;
	uint32_t		iqp_sq_depth;
	uint32_t		iqp_rq_depth;
	uint32_t		iqp_refs;
	boolean_t		iqp_in_table;
	/* Deferred work, under iqp_lock. */
	timeout_id_t		iqp_flush_tid;
	boolean_t		iqp_flush_queued;
	boolean_t		iqp_err_queued;
	uint32_t		iqp_work;	/* queued or running tasks */
} irdma_qp_t;

typedef struct irdma_mr {
	struct rdk_mr		imr_rdk;	/* first */
	irdma_t			*imr_irdma;
	uint32_t		imr_stag;
	uint32_t		imr_page_cnt;
	uint32_t		imr_npages;
	struct irdma_pble_alloc	imr_pble;
	boolean_t		imr_pble_live;
	boolean_t		imr_hwreg;
} irdma_mr_t;

typedef struct irdma_ah {
	struct rdk_ah		iah_rdk;	/* first */
	struct irdma_sc_ah	iah_sc;
	boolean_t		iah_created;
} irdma_ah_t;

/*
 * Whether posts may go to the device.  Before a PF reset ice takes this
 * function offline or sends RESET_PREP, which taints it, so the post path
 * need not ask ice and take its lock.
 */
static inline boolean_t
irdma_post_ok(const irdma_t *irdma)
{
	return ((irdma->irdma_flags & (IRDMA_F_TAINTED | IRDMA_F_CQP_DEAD)) ==
	    0);
}

#define	IRDMA_DEV(d)	((irdma_t *)(void *)((char *)(d) - \
	offsetof(irdma_t, irdma_rdk)))
#define	IRDMA_PD(p)	((irdma_pd_t *)(void *)(p))
#define	IRDMA_CQ(c)	((irdma_cq_t *)(void *)(c))
#define	IRDMA_QP(q)	((irdma_qp_t *)(void *)(q))
#define	IRDMA_MR(m)	((irdma_mr_t *)(void *)(m))
#define	IRDMA_AH(a)	((irdma_ah_t *)(void *)(a))

/*
 * irdma_verbs.c
 */
extern int irdma_verbs_init(irdma_t *);
extern void irdma_verbs_fini(irdma_t *);
extern int irdma_verbs_register(irdma_t *);
extern void irdma_verbs_unregister(irdma_t *);
extern void irdma_verbs_event(irdma_t *, enum rdk_event_type);
extern int irdma_alloc_rsrc(irdma_t *, ulong_t *, uint32_t, uint32_t *,
    uint32_t *);
extern void irdma_free_rsrc(irdma_t *, ulong_t *, uint32_t);
extern int irdma_add_arp(irdma_t *, const uint32_t *, boolean_t,
    const uint8_t *);
extern void irdma_arp_rele(irdma_t *, uint32_t);
extern void irdma_verbs_uncertain(irdma_t *, const char *);
extern boolean_t irdma_hw_ok(irdma_t *);
extern boolean_t irdma_healthy(irdma_t *);
extern irdma_cqp_req_t *irdma_vreq(irdma_t *, uint8_t);

/*
 * irdma_cq.c
 */
extern int irdma_create_cq(struct rdk_cq *, const struct rdk_cq_init_attr *);
extern void irdma_destroy_cq(struct rdk_cq *);
extern int irdma_poll_cq(struct rdk_cq *, int, struct rdk_wc *);
extern int irdma_req_notify_cq(struct rdk_cq *, enum rdk_cq_notify_flags);
extern irdma_cq_t *irdma_cq_ceq_hold(irdma_ceq_t *, struct irdma_sc_cq *);
extern void irdma_cq_ceq_dispatch(irdma_cq_t *, boolean_t);
extern void irdma_cq_resched(struct rdk_cq *);
extern int irdma_modify_cq(struct rdk_cq *, uint16_t, uint16_t);
extern void irdma_cq_error(irdma_t *, uint32_t);
extern boolean_t irdma_cq_empty(irdma_cq_t *);
extern void irdma_cq_add_qp(irdma_cq_t *, uint32_t);
extern void irdma_cq_purge_qp(irdma_cq_t *, irdma_qp_t *);
extern void irdma_comp_handler(irdma_cq_t *);

/*
 * irdma_qp.c
 */
extern int irdma_create_qp(struct rdk_qp *, struct rdk_qp_init_attr *);
extern int irdma_modify_qp(struct rdk_qp *, struct rdk_qp_attr *, int);
extern int irdma_query_qp(struct rdk_qp *, struct rdk_qp_attr *, int,
    struct rdk_qp_init_attr *);
extern void irdma_destroy_qp(struct rdk_qp *);
extern irdma_qp_t *irdma_qp_get(irdma_t *, uint32_t);
extern void irdma_qp_rele(irdma_qp_t *);
extern void irdma_qp_to_error(irdma_qp_t *);
extern void irdma_qp_event(irdma_qp_t *, enum irdma_qp_event_type);
extern void irdma_flush_wqes(irdma_qp_t *, uint32_t);
extern void irdma_flush_later(irdma_qp_t *);
extern boolean_t irdma_generate_flush_completions(irdma_qp_t *);

/*
 * irdma_post.c
 */
extern int irdma_post_send(struct rdk_qp *, const struct rdk_send_wr *,
    const struct rdk_send_wr **);
extern int irdma_post_recv(struct rdk_qp *, const struct rdk_recv_wr *,
    const struct rdk_recv_wr **);
extern uint16_t irdma_get_mr_access(int);

/*
 * irdma_mr.c
 */
extern int irdma_alloc_mr(struct rdk_pd *, enum rdk_mr_type, uint32_t,
    struct rdk_mr **);
extern int irdma_map_mr_sg(struct rdk_mr *, const ddi_dma_cookie_t *, uint_t,
    uint64_t *);
extern int irdma_dereg_mr(struct rdk_mr *);

/*
 * irdma_aeq.c
 */
extern void irdma_aeq_process(irdma_t *);

#ifdef __cplusplus
}
#endif

#endif /* _IRDMA_VERBS_H */

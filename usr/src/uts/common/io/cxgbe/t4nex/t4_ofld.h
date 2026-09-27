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

#ifndef __CXGBE_T4_OFLD_H
#define	__CXGBE_T4_OFLD_H

/*
 * t4nex offload core ("TOE-lite") state, shared by the t4_ofld*.c, t4_tid.c,
 * t4_l2t.c and t4_clip.c files.  It exists only when the rdma-enable property
 * is set and the firmware grants TOE and RDMA capabilities.
 *
 * Lock order: of_cfg_lock, then the CLIP lock, then of_lock, td_lock, l2_lock
 * and of_wlock, then the queue locks.  The first two are taken only in thread
 * context; the rest are interrupt priority mutexes.  None is held across a
 * call into the client.
 */

#include <sys/types.h>
#include <sys/ksynch.h>
#include <sys/list.h>
#include <sys/avl.h>
#include <sys/kstat.h>
#include <sys/disp.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "t4_rdma.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	T4_RDMA_NODE_NAME	"iwcxgbe"

#define	T4_OFLD_RXQ_QSIZE	1024
#define	T4_OFLD_FL_QSIZE	512
#define	T4_OFLD_CIQ_QSIZE	1024
#define	T4_OFLD_CTRLQ_QSIZE	256
#define	T4_OFLD_TXQ_QSIZE	1024
/* Offload vectors: the connection queue and 1 to T4_RDMA_MAX_CIQ CIQs. */
#define	T4_OFLD_DEF_CIQ		8

/* Firmware values past these bounds disable offload. */
#define	T4_OFLD_M_TID		0xffffffU
#define	T4_OFLD_MAX_NTIDS	(1U << 20)
#define	T4_OFLD_MAX_NSTIDS	(1U << 14)
#define	T4_OFLD_MAX_NATIDS	(M_TID_TID + 1)
#define	T4_OFLD_MAX_L2T		(M_L2T_IDX + 1)
#define	T4_OFLD_MAX_WR_CRED	(1U << 16)
#define	T4_OFLD_MAX_ORDIRD	(1U << 16)
#define	T4_OFLD_MAX_IRD_ADAPTER	(1U << 24)

#define	T4_OFLD_MAX_CLIP	128

/* SYNs the client may hold unanswered; more are refused. */
#define	T4_OFLD_MAX_EMBRYOS	1024
#define	T4_OFLD_MAX_PAYLOAD	65535

/* TPT and PBL writes: 32 byte units, 96 bytes inline per work request. */
#define	T4_TPT_UNIT		32
#define	T4_TPT_INLINE_MAX	96
#define	T4_TPT_MAX_LEN		4096

#define	T4_OFLD_NWAITERS	16
#define	T4_OFLD_WR_TIMEOUT_US	(2 * MICROSEC)

/* The largest immediate payload of one FW_OFLD_TX_DATA_WR (IMMDLEN). */
#define	T4_OFLD_TX_IMM_MAX	255

/* Child DMA memory bounds. */
#define	T4_OFLD_DMA_MAX_ALIGN	(2 * 1024 * 1024)
#define	T4_OFLD_DMA_MAX_LEN	T4_RDMA_DMA_MAX_LEN
#define	T4_OFLD_DMA_LIMIT	(1ULL * 1024 * 1024 * 1024)

typedef enum t4_tid_kind {
	T4_TID_ATID,
	T4_TID_STID,
	T4_TID_HW
} t4_tid_kind_t;

typedef enum t4_tid_state {
	TTS_FREE = 0,
	TTS_OWNED,	/* the client holds it */
	TTS_ORPHAN	/* t4nex is releasing it for a client that left */
} t4_tid_state_t;

#define	TEF_V6		0x01	/* IPv6: an stid pair, or an IPv6 hwtid */
#define	TEF_EMBRYO	0x02	/* hwtid from PASS_ACCEPT_REQ, not accepted */
#define	TEF_FLOWC	0x04	/* hwtid: FLOWC sent */
#define	TEF_ABORT	0x08	/* hwtid: ABORT_REQ sent */
#define	TEF_LISTEN	0x10	/* stid: the server may be live */
#define	TEF_UNLISTEN	0x20	/* stid: CLOSE_LISTSRV_REQ sent */
#define	TEF_OPEN	0x40	/* atid, stid: open request sent */
#define	TEF_STID_BUSY	(TEF_LISTEN | TEF_UNLISTEN | TEF_OPEN)
#define	TEF_RELEASING	0x80	/* hwtid: TID_RELEASE on its way */
#define	TEF_RELPEND	0x100	/* hwtid: TID_RELEASE not yet sent */

#define	T4_TID_NIL	UINT32_MAX

typedef struct t4_tid_ent {
	uint8_t		te_state;
	uint16_t	te_flags;
	uint8_t		te_port;
	uint16_t	te_rxq;		/* absolute ID its CPLs arrive on */
	uint16_t	te_refs;
	uint32_t	te_owner;	/* client generation */
	uint32_t	te_seq;		/* hwtid: bumped at each claim */
	uint32_t	te_next;	/* atid free list */
	uint32_t	te_ri;		/* hwtid: the QP bound to it */
	void		*te_ctx;
} t4_tid_ent_t;

typedef struct t4_tid_tab {
	t4_tid_ent_t	*tt_ent;
	uint32_t	tt_n;
	uint32_t	tt_base;	/* hardware ID of tt_ent[0] */
	uint32_t	tt_head;
	uint32_t	tt_tail;
	uint32_t	tt_rotor;
	uint32_t	tt_inuse;
} t4_tid_tab_t;

typedef struct t4_tids {
	kmutex_t	td_lock;
	kcondvar_t	td_cv;
	uint32_t	td_embryos;
	t4_tid_tab_t	td_atid;
	t4_tid_tab_t	td_stid;
	t4_tid_tab_t	td_hw;
} t4_tids_t;

typedef enum t4_l2t_state {
	TLS_FREE = 0,
	TLS_WRITING,
	TLS_VALID,
	TLS_FAILED
} t4_l2t_state_t;

typedef struct t4_l2t_ent {
	uint32_t	le_refs;
	uint8_t		le_state;
	uint8_t		le_port;
	uint16_t	le_vlan;
	uint8_t		le_dmac[ETHERADDRL];
} t4_l2t_ent_t;

typedef struct t4_l2t {
	kmutex_t	l2_lock;
	kcondvar_t	l2_cv;
	uint32_t	l2_start;
	uint32_t	l2_size;
	uint32_t	l2_rotor;
	t4_l2t_ent_t	*l2_ent;
} t4_l2t_t;

typedef struct t4_clip_ent {
	in6_addr_t	ce_addr;
	uint32_t	ce_refs;
} t4_clip_ent_t;

typedef struct t4_clip {
	kmutex_t	cl_lock;
	uint_t		cl_n;
	t4_clip_ent_t	cl_ent[T4_OFLD_MAX_CLIP];
} t4_clip_t;

typedef enum t4_waiter_state {
	TWS_FREE = 0,
	TWS_BUSY,
	TWS_DONE,
	TWS_ABANDONED
} t4_waiter_state_t;

typedef struct t4_ofld_waiter {
	uint8_t		ow_state;
	uint32_t	ow_gen;
	int		ow_status;
} t4_ofld_waiter_t;

/* A work request cookie with this bit set belongs to t4nex. */
#define	T4_OFLD_COOKIE_PARENT	(1ULL << 63)

typedef struct t4_ofld_buf {
	t4_rdma_dma_t		ob_pub;
	ddi_dma_handle_t	ob_dhdl;
	ddi_acc_handle_t	ob_ahdl;
	list_node_t		ob_node;
	boolean_t		ob_bound;	/* queue memory t4nex owns */
	boolean_t		ob_quar;	/* on of_quar */
} t4_ofld_buf_t;

/* A CQ or QP of the child (t4_ofld_ri.c); keyed by CQ or SQ ID. */
typedef struct t4_ri_obj {
	avl_node_t	ro_node;
	uint32_t	ro_id;
	uint32_t	ro_rqid;
	uint32_t	ro_cq[2];	/* QP: send and receive CQ */
	uint32_t	ro_gen;
	uint32_t	ro_tid;		/* QP: the connection after INIT */
	uint32_t	ro_refs;	/* CQ: QPs using it */
	uint16_t	ro_flags;
	t4_ofld_buf_t	*ro_mem[2];
} t4_ri_obj_t;

struct t4_ofld;

typedef struct t4_ofld_port {
	struct t4_ofld		*op_ofld;
	struct port_info	*op_pi;
	uint8_t			op_idx;
	t4_sge_eq_t		op_ctrlq;
	t4_sge_eq_t		op_txq;
	link_state_t		op_link;
	uint64_t		op_speed;
	uint32_t		op_mtu;
} t4_ofld_port_t;

/* Pending event bits (of_ev_pending). */
#define	T4_OFLD_EVP_LINK(p)	(0x1U << (p))
#define	T4_OFLD_EVP_MTU(p)	(0x10U << (p))
#define	T4_OFLD_EVP_FATAL	0x100U

typedef struct t4_ofld_stats {
	uint64_t	os_cpl_rx;
	uint64_t	os_cpl_unknown;
	uint64_t	os_cpl_short;
	uint64_t	os_cpl_badid;
	uint64_t	os_cpl_stale;
	uint64_t	os_cpl_wrongq;
	uint64_t	os_cpl_mismatch;
	uint64_t	os_cpl_nomem;
	uint64_t	os_cq_notify;
	uint64_t	os_cq_badid;
	uint64_t	os_fl_badlen;
	uint64_t	os_orphan_release;
	uint64_t	os_orphan_abort;
	uint64_t	os_orphan_retry;
	uint64_t	os_wr_sent;
	uint64_t	os_wr_full;
	uint64_t	os_wr_badcookie;
	uint64_t	os_l2t_write;
	uint64_t	os_l2t_fail;
	uint64_t	os_tpt_write;
	uint64_t	os_events;
	uint64_t	os_eq_bad_cidx;
	uint64_t	os_syn_refused;
} t4_ofld_stats_t;

typedef struct t4_ofld_kstats {
	kstat_named_t	ok_state;
	kstat_named_t	ok_generation;
	kstat_named_t	ok_ntids;
	kstat_named_t	ok_natids;
	kstat_named_t	ok_nstids;
	kstat_named_t	ok_tids_inuse;
	kstat_named_t	ok_atids_inuse;
	kstat_named_t	ok_stids_inuse;
	kstat_named_t	ok_l2t_size;
	kstat_named_t	ok_stag_size;
	kstat_named_t	ok_pbl_size;
	kstat_named_t	ok_cpl_rx;
	kstat_named_t	ok_cpl_unknown;
	kstat_named_t	ok_cpl_short;
	kstat_named_t	ok_cpl_badid;
	kstat_named_t	ok_cpl_stale;
	kstat_named_t	ok_cpl_wrongq;
	kstat_named_t	ok_cpl_mismatch;
	kstat_named_t	ok_cpl_nomem;
	kstat_named_t	ok_cq_notify;
	kstat_named_t	ok_cq_badid;
	kstat_named_t	ok_fl_badlen;
	kstat_named_t	ok_orphan_release;
	kstat_named_t	ok_orphan_abort;
	kstat_named_t	ok_orphan_retry;
	kstat_named_t	ok_wr_sent;
	kstat_named_t	ok_wr_full;
	kstat_named_t	ok_wr_badcookie;
	kstat_named_t	ok_l2t_write;
	kstat_named_t	ok_l2t_fail;
	kstat_named_t	ok_tpt_write;
	kstat_named_t	ok_events;
	kstat_named_t	ok_eq_bad_cidx;
	kstat_named_t	ok_syn_refused;
	kstat_named_t	ok_dma_bytes;
	kstat_named_t	ok_quar_bytes;
	/* The chip's IPv4 TCP MIB: every offloaded connection. */
	kstat_named_t	ok_tcp_out_rsts;
	kstat_named_t	ok_tcp_in_segs;
	kstat_named_t	ok_tcp_out_segs;
	kstat_named_t	ok_tcp_retrans_segs;
} t4_ofld_kstats_t;

struct t4_rdma_peer {
	t4_rdma_peer_hdr_t	trp_hdr;
	struct t4_ofld		*trp_ofld;
};

typedef struct t4_ofld {
	struct adapter		*of_sc;
	struct t4_rdma_peer	of_peer;

	uint16_t		of_toecaps;
	uint16_t		of_rdmacaps;
	t4_rdma_vres_t		of_vres;
	uint32_t		of_ntids;
	uint32_t		of_tid_base;
	uint32_t		of_natids;
	uint32_t		of_nstids;
	uint32_t		of_stid_base;
	uint32_t		of_l2t_start;
	uint32_t		of_l2t_size;

	uint_t			of_nports;
	t4_ofld_port_t		of_port[MAX_NPORTS];
	struct sge_rxq		of_rxq;		/* connection CPLs */
	t4_sge_iq_t		of_ciq[T4_RDMA_MAX_CIQ]; /* CQ notifications */
	uint_t			of_nciq;
	uint_t			of_rxq_vec;
	uint_t			of_ciq_vec;	/* of of_ciq[0] */
	volatile boolean_t	of_ready;	/* locks initialized */
	boolean_t		of_queues_up;

	kmutex_t		of_cfg_lock;
	dev_info_t		*of_cdip;	/* of_cfg_lock */

	kmutex_t		of_lock;
	kcondvar_t		of_cv;
	boolean_t		of_stopping;
	boolean_t		of_fatal;
	uint32_t		of_gen;
	const t4_rdma_client_t	*of_client;
	void			*of_client_arg;
	uint32_t		of_client_gen;
	uint_t			of_cb_busy;
	uint_t			of_op_busy;	/* child operations running */
	boolean_t		of_closing;	/* old IDs not yet swept */
	kthread_t		*of_ev_thread;	/* delivering an event */
	boolean_t		of_client_test;
	ddi_taskq_t		*of_tq;
	uint32_t		of_ev_pending;
	boolean_t		of_ev_queued;

	t4_tids_t		of_tids;
	timeout_id_t		of_retry_tid;	/* td_lock */
	boolean_t		of_retry_stop;	/* td_lock */
	t4_l2t_t		of_l2t;
	t4_clip_t		of_clip;

	kmutex_t		of_wlock;
	kcondvar_t		of_wcv;
	uint32_t		of_wgen;
	t4_ofld_waiter_t	of_waiter[T4_OFLD_NWAITERS];

	kmutex_t		of_dma_lock;
	list_t			of_bufs;
	list_t			of_quar;
	uint64_t		of_dma_bytes;
	uint64_t		of_quar_bytes;

	kmutex_t		of_ri_lock;
	kcondvar_t		of_ri_cv;
	avl_tree_t		of_ri_objs;
	uint32_t		of_ri_nids;
	ulong_t			*of_ri_used;	/* IDs a queue may hold */

	kstat_t			*of_ksp;
	t4_ofld_kstats_t	of_kstats;
	t4_ofld_stats_t		of_stats;

	void			*of_test;	/* t4_ofld_test.c */
} t4_ofld_t;

#define	T4_OFLD_STAT(of, f)	atomic_inc_64(&(of)->of_stats.f)

/* t4_ofld.c */
extern boolean_t t4_ofld_requested(struct adapter *);
extern void t4_ofld_caps(struct adapter *, struct fw_caps_config_cmd *);
extern int t4_ofld_init(struct adapter *);
extern uint_t t4_ofld_vectors(struct adapter *, uint_t);
extern void t4_ofld_start(struct adapter *);
extern boolean_t t4_ofld_detach(struct adapter *);
extern void t4_ofld_fini(struct adapter *);
extern void t4_ofld_link_notify(struct adapter *, int);
extern void t4_ofld_mtu_notify(struct port_info *);
extern void t4_ofld_fatal(struct adapter *);
extern boolean_t t4_ofld_client_ok(t4_ofld_t *);
extern int t4_ofld_client_open(t4_ofld_t *, const t4_rdma_client_t *, void *,
    boolean_t);
extern void t4_ofld_client_close(t4_ofld_t *);
extern boolean_t t4_ofld_is_child(struct adapter *, dev_info_t *);
extern boolean_t t4_ofld_named(struct adapter *, const char *);
extern void t4_ofld_info(t4_ofld_t *, t4_rdma_info_t *);

/* t4_ofld_sge.c */
extern int t4_ofld_queues_init(t4_ofld_t *);
extern void t4_ofld_queues_fini(t4_ofld_t *);
extern int t4_ofld_intr_handlers(t4_ofld_t *, int *);
extern int t4_ofld_wr_send(t4_ofld_t *, t4_sge_eq_t *, const void *, size_t);
extern uint_t t4_intr_ofld(caddr_t, caddr_t);

/* t4_ofld_cpl.c */
extern void t4_ofld_cpl_dispatch(t4_ofld_t *, t4_rdma_queue_t, uint8_t,
    mblk_t *);
extern void t4_ofld_cq_notify(t4_ofld_t *, uint_t, uint32_t *, uint_t);

/* t4_ofld_orphan.c */
extern void t4_ofld_orphan_sweep(t4_ofld_t *, uint32_t);
extern void t4_ofld_retry_arm_locked(t4_ofld_t *);
extern void t4_ofld_retry_stop(t4_ofld_t *);
extern void t4_ofld_orphan_unlisten_locked(t4_ofld_t *, t4_tid_ent_t *,
    uint32_t);
extern void t4_ofld_orphan_abort_locked(t4_ofld_t *, t4_tid_ent_t *,
    uint32_t);
extern void t4_ofld_orphan_release_locked(t4_ofld_t *, uint8_t, uint32_t);
extern int t4_ofld_waiter_get(t4_ofld_t *, uint64_t *);
extern int t4_ofld_waiter_wait(t4_ofld_t *, uint64_t);
extern void t4_ofld_waiter_put(t4_ofld_t *, uint64_t);
extern void t4_ofld_init_tp_wr(void *, size_t, uint32_t);
extern int t4_ofld_send_tid_release(t4_ofld_t *, uint8_t, uint32_t);
extern int t4_ofld_send_abort(t4_ofld_t *, uint8_t, uint32_t, boolean_t);
extern int t4_ofld_send_abort_rpl(t4_ofld_t *, uint8_t, uint32_t, boolean_t);
extern int t4_ofld_send_flowc(t4_ofld_t *, uint8_t, uint32_t,
    const t4_rdma_flowc_t *);
extern int t4_ofld_send_unlisten(t4_ofld_t *, uint8_t, uint32_t, boolean_t);

/* t4_tid.c */
extern int t4_tids_init(t4_ofld_t *);
extern void t4_tids_fini(t4_ofld_t *);
extern t4_tid_tab_t *t4_tid_tab(t4_ofld_t *, t4_tid_kind_t);
extern t4_tid_ent_t *t4_tid_ent(t4_ofld_t *, t4_tid_kind_t, uint32_t);
extern t4_tid_ent_t *t4_hwtid_next(t4_ofld_t *, uint32_t *);
extern int t4_atid_alloc(t4_ofld_t *, uint32_t, void *, uint32_t *);
extern int t4_stid_alloc(t4_ofld_t *, uint32_t, sa_family_t, void *,
    uint32_t *);
extern void t4_tid_free_locked(t4_ofld_t *, t4_tid_kind_t, uint32_t);
extern int t4_tid_hold(t4_ofld_t *, t4_tid_kind_t, uint32_t, uint32_t,
    uint16_t, void **);
extern void t4_tid_rele(t4_ofld_t *, t4_tid_kind_t, uint32_t);
extern void t4_tid_wait_idle(t4_ofld_t *, t4_tid_ent_t *);
extern int t4_hwtid_claim(t4_ofld_t *, uint32_t, t4_tid_state_t, uint32_t,
    uint8_t, uint16_t, uint16_t, void *);
extern t4_tid_ent_t *t4_tid_owned(t4_ofld_t *, t4_tid_kind_t, uint32_t,
    uint32_t);

/* t4_l2t.c */
extern int t4_l2t_init(t4_ofld_t *);
extern void t4_l2t_fini(t4_ofld_t *);
extern int t4_l2t_get(t4_ofld_t *, uint8_t, uint16_t, const uint8_t *,
    uint32_t *);
extern void t4_l2t_put(t4_ofld_t *, uint32_t);
extern boolean_t t4_l2t_held(t4_ofld_t *, uint32_t, uint8_t, uint16_t *);
extern void t4_l2t_reset(t4_ofld_t *);
extern void t4_l2t_write_rpl(t4_ofld_t *, const struct cpl_l2t_write_rpl *);

/* t4_clip.c */
extern void t4_clip_init(t4_ofld_t *);
extern void t4_clip_fini(t4_ofld_t *);
extern int t4_clip_get(t4_ofld_t *, const in6_addr_t *);
extern void t4_clip_put(t4_ofld_t *, const in6_addr_t *);
extern boolean_t t4_clip_held(t4_ofld_t *, const in6_addr_t *);
extern void t4_clip_reset(t4_ofld_t *);

/* t4_rdma_peer.c */
extern const t4_rdma_ops_t t4_rdma_ops;

/* t4_ofld_ops.c */
extern int t4_ofld_gen(t4_ofld_t *, uint32_t *);
extern int t4_ofld_listen(t4_ofld_t *, const t4_rdma_listen_t *);
extern int t4_ofld_unlisten(t4_ofld_t *, uint32_t);
extern int t4_ofld_act_open(t4_ofld_t *, const t4_rdma_act_open_t *);
extern int t4_ofld_accept(t4_ofld_t *, const t4_rdma_accept_t *);
extern int t4_ofld_flowc(t4_ofld_t *, uint32_t, const t4_rdma_flowc_t *);
extern int t4_ofld_close_con(t4_ofld_t *, uint32_t);
extern int t4_ofld_abort(t4_ofld_t *, uint32_t, boolean_t);
extern int t4_ofld_abort_rpl(t4_ofld_t *, uint32_t, boolean_t);
extern int t4_ofld_tid_release(t4_ofld_t *, uint32_t);
extern int t4_ofld_tid_bind(t4_ofld_t *, uint32_t, void *);
extern int t4_ofld_tx_data(t4_ofld_t *, uint32_t, const void *, size_t);
extern int t4_ofld_rx_credits(t4_ofld_t *, uint32_t, uint32_t);
extern void t4_ofld_stid_free(t4_ofld_t *, uint32_t);
extern void t4_ofld_atid_free(t4_ofld_t *, uint32_t);
extern int t4_ofld_set_tcb_field(t4_ofld_t *, uint32_t, uint16_t, uint64_t,
    uint64_t);
extern int t4_ofld_tpt_write(t4_ofld_t *, uint32_t, const void *, size_t);

/* t4_ofld_dma.c */
extern int t4_ofld_dma_alloc(t4_ofld_t *, size_t, size_t, t4_rdma_dma_t **);
extern void t4_ofld_dma_free(t4_ofld_t *, t4_rdma_dma_t *, boolean_t);
extern void t4_ofld_dma_fini(t4_ofld_t *, boolean_t);
extern void t4_ofld_dma_close(t4_ofld_t *);
extern t4_ofld_buf_t *t4_ofld_dma_bind(t4_ofld_t *, t4_rdma_dma_t *, size_t);
extern void t4_ofld_dma_unbind(t4_ofld_t *, t4_ofld_buf_t *);
extern void t4_ofld_dma_release(t4_ofld_t *, t4_ofld_buf_t *, boolean_t);

/* t4_ofld_ri.c */
extern void t4_ofld_ri_setup(t4_ofld_t *);
extern void t4_ofld_ri_teardown(t4_ofld_t *);
extern void t4_ofld_ri_close(t4_ofld_t *, uint32_t);
extern void t4_ofld_ri_gone(t4_ofld_t *, uint32_t);
extern void t4_ofld_ri_untid(t4_ofld_t *, const t4_ri_obj_t *);
extern int t4_ofld_cq_create(t4_ofld_t *, const t4_rdma_cq_res_t *,
    t4_rdma_db_t *);
extern int t4_ofld_cq_destroy(t4_ofld_t *, uint32_t);
extern int t4_ofld_qp_create(t4_ofld_t *, const t4_rdma_qp_res_t *,
    t4_rdma_db_t *, t4_rdma_db_t *);
extern int t4_ofld_qp_destroy(t4_ofld_t *, uint32_t);
extern int t4_ofld_ri_init(t4_ofld_t *, uint32_t, const t4_rdma_ri_init_t *);
extern int t4_ofld_ri_fini(t4_ofld_t *, uint32_t, uint32_t);
extern int t4_ofld_ri_terminate(t4_ofld_t *, uint32_t, uint32_t, uint8_t,
    uint8_t);

/* t4_ofld_test.c */
extern int t4_ofld_test_ioctl(struct adapter *, void *, int);
extern void t4_ofld_test_fini(t4_ofld_t *);

#ifdef __cplusplus
}
#endif

#endif /* __CXGBE_T4_OFLD_H */

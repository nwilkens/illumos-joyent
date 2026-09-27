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

#ifndef _IWC_H
#define	_IWC_H

/*
 * iwcxgbe: the iWARP provider for Chelsio T5 and T6 adapters, a child of
 * t4nex(4D) that reaches the adapter only through t4_rdma.h.  Each adapter
 * port is one rdmak device.
 *
 * Contexts: t4nex calls iwc_cpl() and iwc_cq_notify() in interrupt context;
 * they only queue work.  The CM taskq runs every connection state change,
 * one CPL at a time.  Each completion vector (a t4nex CIQ with its own
 * interrupt) has a thread that runs the completion handlers of its CQs.
 *
 * Lock order: ep_lock, then the CQ locks (receive CQ first), then qp_lock,
 * then iwc_res_lock.  iwc_obj_lock, then iv_lock; these and iwc_cm_qlock
 * are interrupt priority.  No lock is held across a call into rdmak or
 * t4nex that can block.
 */

#include <sys/types.h>
#include <sys/ksynch.h>
#include <sys/list.h>
#include <sys/taskq.h>
#include <sys/taskq_impl.h>
#include <sys/kstat.h>
#include <sys/vmem.h>
#include <sys/stream.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "rdk.h"
#include "t4nex/t4_rdma.h"
#include "iwc_t4.h"
#include "iwc_mpa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	IWC_NAME		"iwcxgbe"

/* rdmak sees these limits; the hardware allows more. */
#define	IWC_MAX_QP_WR		4096
#define	IWC_MAX_CQE		16384
#define	IWC_MAX_PD		T4_MAX_NUM_PD
#define	IWC_MIN_PBL_SHIFT	8
#define	IWC_MIN_RQT_SHIFT	10

/* TCP for iWARP connections. */
#define	IWC_RCV_WIN		(256 * 1024)
#define	IWC_SND_WIN		(128 * 1024)
#define	IWC_CONG		CONG_ALG_TAHOE
#define	IWC_MPA_TIMEOUT_MS	10000
#define	IWC_CLOSE_TIMEOUT_MS	10000
#define	IWC_MAX_ORDIRD		32

typedef struct iwc iwc_t;
typedef struct iwc_ep iwc_ep_t;

typedef struct iwc_dev {
	struct rdk_device	d_rdk;
	iwc_t			*d_iwc;
	uint8_t			d_port;
	boolean_t		d_iw_attached;
	boolean_t		d_registered;
} iwc_dev_t;

typedef struct iwc_pd {
	struct rdk_pd	pd_rdk;
	uint32_t	pd_pdid;
} iwc_pd_t;

typedef struct iwc_cq {
	struct rdk_cq	cq_rdk;
	iwc_t		*cq_iwc;
	kmutex_t	cq_lock;
	t4_cq_t		cq_hw;
	t4_rdma_dma_t	*cq_mem;
	size_t		cq_memlen;
	boolean_t	cq_live;	/* the firmware has the queue */
	uint32_t	cq_vec;
	/* The vector's iv_lock */
	uint32_t	cq_refs;
	boolean_t	cq_pending;
	struct iwc_cq	*cq_next;
} iwc_cq_t;

/*
 * A completion vector: its CIQ's interrupt queues CQs here and the thread
 * calls their handlers, oldest first.
 */
typedef struct iwc_vec {
	struct iwc	*iv_iwc;
	uint_t		iv_idx;
	kmutex_t	iv_lock;	/* interrupt priority */
	kcondvar_t	iv_cv;
	iwc_cq_t	*iv_head;
	iwc_cq_t	*iv_tail;
	boolean_t	iv_exit;
	kt_did_t	iv_did;
	uint64_t	iv_intrs;	/* notifications taken */
	uint64_t	iv_runs;	/* handler calls */
	uint64_t	iv_busy_ns;
	/* Atomic: the arms of its CQs and the doorbells of their QPs. */
	uint64_t	iv_arms;
	uint64_t	iv_sq_db;
} iwc_vec_t;

typedef enum iwc_qp_state {
	IWC_QPS_IDLE = 0,
	IWC_QPS_RTS,
	IWC_QPS_CLOSING,
	IWC_QPS_ERROR
} iwc_qp_state_t;

typedef struct iwc_qp {
	struct rdk_qp	qp_rdk;
	iwc_t		*qp_iwc;
	kmutex_t	qp_lock;
	kcondvar_t	qp_cv;
	t4_wq_t		qp_wq;
	t4_rdma_dma_t	*qp_sqmem;
	t4_rdma_dma_t	*qp_rqmem;
	boolean_t	qp_live;	/* the firmware has the queues */
	iwc_qp_state_t	qp_state;
	iwc_ep_t	*qp_ep;		/* held */
	uint32_t	qp_pdid;
	boolean_t	qp_sig_all;
	uint32_t	qp_ird;
	uint32_t	qp_ord;
	uint32_t	qp_sq_max_sge;
	uint32_t	qp_rq_max_sge;
	/*
	 * With DSGL registration, T4_MAX_FR_DSGL bytes per SQ slot after the
	 * ring in the SQ memory; a slot's page list lives as long as its
	 * work request.
	 */
	size_t		qp_pbl_off;
	/* iwc_obj_lock */
	uint32_t	qp_refs;
} iwc_qp_t;

typedef struct iwc_mr {
	struct rdk_mr	mr_rdk;
	iwc_t		*mr_iwc;
	uint32_t	mr_pdid;
	uint32_t	mr_stag;
	uint32_t	mr_pbl_addr;	/* adapter memory, bytes */
	uint32_t	mr_max;		/* pages the PBL holds */
	uint32_t	mr_npages;
	uint64_t	*mr_pages;
} iwc_mr_t;

typedef enum iwc_ep_state {
	IWC_EP_IDLE = 0,
	IWC_EP_LISTEN,
	IWC_EP_CONNECTING,	/* ACT_OPEN_REQ sent */
	IWC_EP_ACCEPTING,	/* PASS_ACCEPT_RPL sent */
	IWC_EP_MPA_REQ_WAIT,
	IWC_EP_MPA_REQ_SENT,
	IWC_EP_MPA_REQ_RCVD,
	IWC_EP_FPDU,
	IWC_EP_CLOSING,
	IWC_EP_ABORTING,
	IWC_EP_DEAD
} iwc_ep_state_t;

/* ep_flags */
#define	EPF_ESTABLISHED		0x0001	/* the TCP connection is up */
#define	EPF_CLOSE_SENT		0x0002
#define	EPF_PEER_CLOSED		0x0004
#define	EPF_CLOSE_ACKED		0x0008
#define	EPF_ABORT_SENT		0x0010
#define	EPF_FINAL_SENT		0x0020	/* the framework got its last event */
#define	EPF_RELEASED		0x0040	/* TID and L2T entry given back */
#define	EPF_CM_REF		0x0080	/* ep_cmid is ours to report to */
#define	EPF_REQ_SENT		0x0100	/* CONNECT_REQUEST given up */
#define	EPF_DISC_SENT		0x0200	/* DISCONNECT given up */
#define	EPF_LISTENING		0x0400	/* the server is live */
#define	EPF_ATID		0x0800	/* ep_atid is held */
#define	EPF_STID		0x1000	/* ep_stid is held */
#define	EPF_TID			0x2000	/* ep_tid is bound */
#define	EPF_UP			0x4000	/* reached RDMA mode */

struct iwc_ep {
	list_node_t		ep_node;	/* iwc_ep_lock */
	iwc_t			*ep_iwc;
	iwc_dev_t		*ep_dev;
	kmutex_t		ep_lock;
	kcondvar_t		ep_cv;
	volatile uint32_t	ep_refs;
	iwc_ep_state_t		ep_state;
	uint32_t		ep_flags;
	struct rdk_iw_cm_id	*ep_cmid;
	iwc_ep_t		*ep_parent;	/* held */
	iwc_qp_t		*ep_qp;
	void			*ep_admit;
	uint32_t		ep_atid;
	uint32_t		ep_stid;
	uint32_t		ep_tid;
	uint32_t		ep_l2t;
	uint8_t			ep_port;
	struct sockaddr_in	ep_laddr;
	struct sockaddr_in	ep_raddr;
	uint32_t		ep_snd_seq;
	uint32_t		ep_rcv_seq;
	uint16_t		ep_emss;
	uint8_t			ep_snd_wscale;
	uint8_t			ep_mtu_idx;
	hrtime_t		ep_deadline;	/* 0: no timer */
	int			ep_status;	/* the reason for a close */
	iwc_ep_t		*ep_lost_next;	/* iwc_cm_qlock */
	boolean_t		ep_lost;	/* iwc_cm_qlock */
	/* MPA */
	iwc_mpa_rx_t		ep_mpa;
	iwc_mpa_attr_t		ep_attr;
	uint32_t		ep_ird;
	uint32_t		ep_ord;
	uint8_t			ep_pdata[IWC_MPA_MAX_PDATA];
	uint16_t		ep_pdata_len;
	/* listener */
	uint32_t		ep_embryos;
	boolean_t		ep_open_done;
	int			ep_open_status;
};

struct iwc {
	dev_info_t		*iwc_dip;
	t4_rdma_peer_t		*iwc_peer;
	const t4_rdma_ops_t	*iwc_ops;
	t4_rdma_info_t		iwc_info;
	boolean_t		iwc_open;
	volatile boolean_t	iwc_fatal;
	volatile boolean_t	iwc_tainted;	/* iwc_taint() */
	uint32_t		iwc_ndev;
	iwc_dev_t		iwc_dev[T4_RDMA_MAX_PORTS];

	kmutex_t		iwc_res_lock;
	uint32_t		iwc_qid_start;
	uint32_t		iwc_qid_n;
	ulong_t			*iwc_qid_map;
	uint32_t		iwc_qid_rotor;
	ulong_t			*iwc_pdid_map;
	uint32_t		iwc_nstag;
	ulong_t			*iwc_stag_map;
	uint32_t		iwc_stag_rotor;
	uint8_t			iwc_stag_key;
	vmem_t			*iwc_pbl_arena;
	vmem_t			*iwc_rqt_arena;

	kmutex_t		iwc_obj_lock;
	kcondvar_t		iwc_obj_cv;
	iwc_cq_t		**iwc_cqs;
	iwc_qp_t		**iwc_qps;

	uint_t			iwc_nvec;
	iwc_vec_t		iwc_vecs[T4_RDMA_MAX_CIQ];

	kmutex_t		iwc_cm_qlock;
	struct iwc_cmq		*iwc_cm_qhead;
	struct iwc_cmq		*iwc_cm_qtail;
	uint_t			iwc_cm_qlen;
	/* Endpoints whose CPL was dropped; each is held and gets aborted. */
	iwc_ep_t		*iwc_cm_lost;
	boolean_t		iwc_cm_queued;
	boolean_t		iwc_cm_closing;
	taskq_ent_t		iwc_cm_ent;
	taskq_t			*iwc_cm_tq;
	timeout_id_t		iwc_tick;
	boolean_t		iwc_tick_stop;
	boolean_t		iwc_tick_pending;

	kmutex_t		iwc_ep_lock;
	kcondvar_t		iwc_ep_cv;
	list_t			iwc_eps;

	kstat_t			*iwc_ksp;
	struct iwc_stats {
		uint64_t	is_cpl_drop;
		uint64_t	is_cpl_lost;
		uint64_t	is_cqe_bad;
		uint64_t	is_mpa_bad;
		uint64_t	is_syn_refused;
		uint64_t	is_conn_est;
		uint64_t	is_conn_abort;
		uint64_t	is_async_err;
		uint64_t	is_quar;
		uint64_t	is_term_sent;
		uint64_t	is_term_rcvd;
	} iwc_stats;
};

#define	IWC_STAT(i, f)	atomic_inc_64(&(i)->iwc_stats.f)

static inline iwc_dev_t *
iwc_dev(struct rdk_device *d)
{
	return ((iwc_dev_t *)d);
}

static inline iwc_t *
iwc_of(struct rdk_device *d)
{
	return (iwc_dev(d)->d_iwc);
}

/* iwc.c */
extern void iwc_warn(iwc_t *, const char *, ...);
extern void iwc_taint(iwc_t *);
extern int iwc_qid_alloc(iwc_t *, uint32_t *);
extern void iwc_qid_free(iwc_t *, uint32_t);
extern iwc_qp_t *iwc_qp_get(iwc_t *, uint32_t);
extern void iwc_qp_put(iwc_t *, iwc_qp_t *);
extern void iwc_db_write(iwc_t *, caddr_t, uint32_t, uint32_t);
extern const struct rdk_device_ops iwc_rdk_ops;

/* iwc_mem.c */
extern int iwc_alloc_pd(struct rdk_pd *);
extern void iwc_dealloc_pd(struct rdk_pd *);
extern int iwc_alloc_mr(struct rdk_pd *, enum rdk_mr_type, uint32_t,
    struct rdk_mr **);
extern int iwc_map_mr_sg(struct rdk_mr *, const ddi_dma_cookie_t *, uint_t,
    uint64_t *);
extern int iwc_dereg_mr(struct rdk_mr *);
extern int iwc_dma_alloc(struct rdk_device *, size_t, rdk_dma_buf_t *);
extern void iwc_dma_free(struct rdk_device *, rdk_dma_buf_t *);
extern int iwc_mem_init(iwc_t *);
extern void iwc_mem_fini(iwc_t *);
extern int iwc_tpt_perms(int);
extern uint32_t iwc_rqt_alloc(iwc_t *, uint32_t);
extern void iwc_rqt_free(iwc_t *, uint32_t, uint32_t);

/* iwc_cq.c */
extern int iwc_create_cq(struct rdk_cq *, const struct rdk_cq_init_attr *);
extern void iwc_destroy_cq(struct rdk_cq *);
extern int iwc_poll_cq(struct rdk_cq *, int, struct rdk_wc *);
extern int iwc_req_notify_cq(struct rdk_cq *, enum rdk_cq_notify_flags);
extern void iwc_cq_notify(void *, uint_t, const uint32_t *, uint_t);
extern void iwc_cq_resched(struct rdk_cq *);
extern void iwc_vecs_init(iwc_t *, uint_t);
extern void iwc_vecs_fini(iwc_t *);
extern void iwc_flush_qp(iwc_qp_t *);
extern void iwc_cq_insert_drain(iwc_cq_t *, iwc_qp_t *, uint64_t,
    uint8_t, boolean_t);
extern void iwc_cq_wake(iwc_cq_t *);

/* The pages a fast registration MR may map. */
#define	IWC_FR_DEPTH(iwc)	((iwc)->iwc_info.tri_vres.trv_memwrite_dsgl ? \
	T4_MAX_FR_DSGL_DEPTH : T4_MAX_FR_IMMD_DEPTH)

/* iwc_qp.c */
extern int iwc_create_qp(struct rdk_qp *, struct rdk_qp_init_attr *);
extern int iwc_modify_qp(struct rdk_qp *, struct rdk_qp_attr *, int);
extern int iwc_query_qp(struct rdk_qp *, struct rdk_qp_attr *, int,
    struct rdk_qp_init_attr *);
extern void iwc_destroy_qp(struct rdk_qp *);
extern int iwc_post_send(struct rdk_qp *, const struct rdk_send_wr *,
    const struct rdk_send_wr **);
extern int iwc_post_recv(struct rdk_qp *, const struct rdk_recv_wr *,
    const struct rdk_recv_wr **);
extern int iwc_qp_rts(iwc_qp_t *, iwc_ep_t *);
extern int iwc_qp_close(iwc_qp_t *, iwc_ep_t *);
extern void iwc_qp_error(iwc_qp_t *, iwc_ep_t *);
extern void iwc_qp_async(iwc_t *, const t4_cqe_t *);
extern void iwc_term_codes(const t4_cqe_t *, uint8_t *, uint8_t *);
extern size_t iwc_sq_bytes(iwc_t *, uint32_t, size_t *);
extern uint32_t iwc_max_qp_wr(iwc_t *);

/* iwc_ep.c */
extern iwc_ep_t *iwc_ep_alloc(iwc_t *, iwc_dev_t *);
extern void iwc_ep_hold(iwc_ep_t *);
extern void iwc_ep_rele(iwc_ep_t *);
extern void iwc_ep_deadline(iwc_ep_t *, uint32_t);
extern int iwc_cpl_errno(uint_t);
extern uint32_t iwc_path_mtu(iwc_ep_t *, uint32_t);
extern void iwc_tcp_opts(iwc_ep_t *, uint32_t, t4_rdma_tcp_opts_t *);
extern void iwc_set_emss(iwc_ep_t *, uint16_t);
extern int iwc_flowc(iwc_ep_t *);
extern int iwc_send_mpa(iwc_ep_t *, boolean_t, uint8_t, const void *,
    uint16_t);
extern void iwc_ep_event(iwc_ep_t *, enum rdk_iw_event_type, int,
    const void *, uint16_t);
extern void iwc_ep_release(iwc_ep_t *, int);
extern void iwc_ep_abort_locked(iwc_ep_t *, int);
extern void iwc_ep_abort(iwc_ep_t *, int);
extern void iwc_ep_close(iwc_ep_t *);
extern void iwc_ep_terminate(iwc_ep_t *, uint8_t, uint8_t);

/* iwc_cm.c */
extern void iwc_cpl(void *, t4_rdma_cpl_t *);
extern void iwc_cm_task(void *);
extern void iwc_cm_tick(void *);
extern void iwc_cm_fini(iwc_t *);

/* iwc_cm_ops.c */
extern const struct rdk_iw_cm_ops iwc_iw_ops;

#ifdef __cplusplus
}
#endif

#endif /* _IWC_H */

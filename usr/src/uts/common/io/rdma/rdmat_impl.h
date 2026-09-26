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

#ifndef _RDMAT_IMPL_H
#define	_RDMAT_IMPL_H

#include <sys/types.h>
#include <sys/ksynch.h>
#include <sys/list.h>

#include "rdk.h"
#include "rdmat_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	RDMAT_NCHUNKS	(RDMAT_MAX_BUF / RDMAT_CHUNK)
#define	RDMAT_QKEY	0x11111111
#define	RDMAT_LAT_SAMPLES	(1U << 20)

/* One send work request of a chain, with its gather element. */
typedef struct rdmat_swr {
	union {
		struct rdk_send_wr	sw_wr;
		struct rdk_rdma_wr	sw_rdma;
		struct rdk_ud_wr	sw_ud;
	};
	struct rdk_sge		sw_sge;
} rdmat_swr_t;

typedef struct rdmat_rwr {
	struct rdk_recv_wr	rw_wr;
	struct rdk_sge		rw_sge;
} rdmat_rwr_t;

/* A device rdmak offered us, with the sessions on it. */
typedef struct rdmat_dev {
	list_node_t		td_node;
	struct rdk_device	*td_dev;
	list_t			td_sessions;
} rdmat_dev_t;

struct rdmat_sess;

/*
 * A QP with its CQs, buffer and MRs.  The completion counters are under
 * tq_lock; the done functions run in rdmak's taskq or, for a DIRECT
 * session, in the ioctl thread.
 */
typedef struct rdmat_qp {
	struct rdmat_sess	*tq_sess;
	uint32_t		tq_idx;
	struct rdk_qp		*tq_qp;
	struct rdk_cq		*tq_scq;
	struct rdk_cq		*tq_rcq;
	rdk_dma_buf_t		tq_chunks[RDMAT_NCHUNKS];
	uint_t			tq_nchunks;
	uint64_t		tq_len;
	ddi_dma_cookie_t	*tq_cookies;
	struct rdk_mr		*tq_lmr;	/* local write */
	struct rdk_mr		*tq_rmr;	/* what the peer may reach */
	boolean_t		tq_lmr_bound;
	boolean_t		tq_rmr_bound;
	uint32_t		tq_rkey_next;	/* rkey of the next REG */
	struct rdk_ah		*tq_ah;
	uint32_t		tq_rqpn;
	uint32_t		tq_rqkey;
	uint32_t		tq_psn;
	boolean_t		tq_connected;

	kmutex_t		tq_lock;
	kcondvar_t		tq_cv;
	struct rdk_cqe		tq_send_cqe;
	struct rdk_cqe		tq_recv_cqe;
	struct rdk_cqe		tq_reg_cqe;
	uint64_t		tq_send_done;
	uint64_t		tq_recv_done;
	uint64_t		tq_reg_done;
	uint64_t		tq_bytes;
	uint32_t		tq_errors;
	uint32_t		tq_err_status;
	uint32_t		tq_err_opcode;
	uint32_t		tq_err_vendor;
	uint32_t		tq_expect_len;
	uint32_t		tq_last_len;
	uint32_t		tq_len_mismatch;
	uint32_t		tq_wc_flags;
	uint32_t		tq_inv_rkey;
	hrtime_t		tq_last_ns;
	uint64_t		tq_events;
	uint32_t		tq_last_event;

	/* The ioctl thread's, for the run in progress. */
	boolean_t		tq_busy;	/* RDMAT_F_BUSY */
	hrtime_t		tq_spin_ns;	/* RDMAT_F_ADAPT */
	boolean_t		tq_spoll;	/* busy polling tq_scq */
	boolean_t		tq_rpoll;
	uint64_t		tq_posted;
	uint64_t		tq_post_calls;
	rdmat_swr_t		tq_swr[RDMAT_MAX_BATCH];
	rdmat_rwr_t		tq_rwr[RDMAT_MAX_BATCH];
} rdmat_qp_t;

/*
 * A session is one open of /dev/rdmat.  ts_lock covers ts_busy, ts_dying
 * and ts_dead; an ioctl runs with ts_busy set and without the lock.
 */
typedef struct rdmat_sess {
	minor_t			ts_minor;
	list_node_t		ts_node;	/* rdmat_lock */
	rdmat_dev_t		*ts_tdev;	/* rdmat_lock */
	uint32_t		ts_holds;	/* rdmat_lock */
	kmutex_t		ts_lock;
	kcondvar_t		ts_cv;
	boolean_t		ts_busy;
	volatile boolean_t	ts_dying;
	boolean_t		ts_dead;
	boolean_t		ts_setup;
	struct rdk_device	*ts_dev;
	struct rdk_pd		*ts_pd;
	uint32_t		ts_qpt;
	uint32_t		ts_poll;
	uint32_t		ts_depth;
	uint32_t		ts_inline;
	uint32_t		ts_comp_vector;
	uint16_t		ts_mod_count;
	uint16_t		ts_mod_us;
	uint16_t		ts_gid_index;
	boolean_t		ts_gid_added;
	uint32_t		ts_nqp;
	rdmat_qp_t		ts_qp[RDMAT_MAX_QPS];
} rdmat_sess_t;

/* rdmat_run.c */
extern int rdmat_setup(rdmat_sess_t *, rdmat_setup_t *);
extern int rdmat_connect(rdmat_sess_t *, rdmat_connect_t *);
extern int rdmat_run(rdmat_sess_t *, rdmat_run_t *);
extern int rdmat_buf(rdmat_sess_t *, rdmat_buf_t *);
extern int rdmat_query(rdmat_sess_t *, rdmat_query_t *);
extern void rdmat_teardown(rdmat_sess_t *, boolean_t);
extern int rdmat_wait(rdmat_qp_t *, uint64_t *, uint64_t, hrtime_t);
extern void rdmat_spin_end(rdmat_qp_t *);

/* rdmat_bench.c: the data path runs */
extern int rdmat_post_recvs(rdmat_sess_t *, rdmat_qp_t *, rdmat_run_t *,
    uint32_t, uint64_t *);
extern int rdmat_stream(rdmat_sess_t *, rdmat_qp_t *, rdmat_run_t *,
    hrtime_t);
extern int rdmat_recv_stream(rdmat_sess_t *, rdmat_qp_t *, rdmat_run_t *,
    hrtime_t);
extern int rdmat_pingpong(rdmat_sess_t *, rdmat_qp_t *, rdmat_run_t *,
    hrtime_t);
extern int rdmat_write_pingpong(rdmat_sess_t *, rdmat_qp_t *, rdmat_run_t *,
    hrtime_t);
extern int rdmat_one_lat(rdmat_sess_t *, rdmat_qp_t *, rdmat_run_t *,
    hrtime_t);

#ifdef __cplusplus
}
#endif

#endif /* _RDMAT_IMPL_H */

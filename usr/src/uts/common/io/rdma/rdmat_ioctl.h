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

#ifndef _RDMAT_IOCTL_H
#define	_RDMAT_IOCTL_H

/*
 * The private ioctls of rdmat, the rdmak test client, on /dev/rdmat.  Only
 * the global zone with {PRIV_SYS_CONFIG} may open it.  Each open is a
 * session with its own PD, CQs, QPs, buffers and MRs on one RDMA device;
 * closing it destroys them, work in flight or not.
 *
 * RDMAT_IOC_DEVICES	List the devices (rdmat_devices_t).
 * RDMAT_IOC_SETUP	Create the session's objects (rdmat_setup_t).
 * RDMAT_IOC_CONNECT	Connect one QP to a peer (rdmat_connect_t).
 * RDMAT_IOC_RUN	Run one operation to completion (rdmat_run_t).
 * RDMAT_IOC_BUF	Fill, check or clear part of a QP's buffer
 *			(rdmat_buf_t); checking compares every byte.
 * RDMAT_IOC_QUERY	Read a QP's state and counters (rdmat_query_t).
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	RDMAT_IOC		(('R' << 24) | ('D' << 16) | ('T' << 8))
#define	RDMAT_IOC_DEVICES	(RDMAT_IOC | 0x01)
#define	RDMAT_IOC_SETUP		(RDMAT_IOC | 0x02)
#define	RDMAT_IOC_CONNECT	(RDMAT_IOC | 0x03)
#define	RDMAT_IOC_RUN		(RDMAT_IOC | 0x04)
#define	RDMAT_IOC_BUF		(RDMAT_IOC | 0x05)
#define	RDMAT_IOC_QUERY		(RDMAT_IOC | 0x06)

#define	RDMAT_NAME_MAX		32
#define	RDMAT_MAX_DEVS		8
#define	RDMAT_MAX_QPS		2
#define	RDMAT_MAX_BUF		(64ULL * 1024 * 1024)
#define	RDMAT_CHUNK		(1024 * 1024)
#define	RDMAT_MAX_DEPTH		256
#define	RDMAT_MAX_BATCH		32
#define	RDMAT_MAX_COUNT		(1U << 28)
#define	RDMAT_MAX_TIMEOUT_MS	120000
/* The GRH a UD receive gets in front of the payload. */
#define	RDMAT_GRH_LEN		40

typedef struct rdmat_devinfo {
	char		rdi_name[RDMAT_NAME_MAX];
	uint32_t	rdi_port_state;
	uint32_t	rdi_active_mtu;		/* bytes */
	uint32_t	rdi_phys_mtu;
	uint8_t		rdi_mac[6];
	uint16_t	rdi_pad;
	uint64_t	rdi_speed;
	uint32_t	rdi_max_qp;
	uint32_t	rdi_max_qp_wr;
	uint32_t	rdi_max_sge;
	uint32_t	rdi_max_mr_pages;
	uint32_t	rdi_max_inline;
	uint32_t	rdi_pad2;
} rdmat_devinfo_t;

typedef struct rdmat_devices {
	uint32_t	rdd_count;
	uint32_t	rdd_pad;
	rdmat_devinfo_t	rdd_devs[RDMAT_MAX_DEVS];
} rdmat_devices_t;

typedef enum rdmat_qpt {
	RDMAT_QPT_RC = 1,
	RDMAT_QPT_UD
} rdmat_qpt_t;

typedef enum rdmat_poll {
	RDMAT_POLL_DIRECT = 1,	/* the ioctl thread polls */
	RDMAT_POLL_TASKQ	/* rdmak's taskq polls; the thread sleeps */
} rdmat_poll_t;

typedef struct rdmat_qpinfo {
	uint32_t	rqi_qpn;
	uint32_t	rqi_psn;
	uint32_t	rqi_rkey;	/* of the buffer, once connected */
	uint32_t	rqi_qkey;
	uint64_t	rqi_addr;	/* the buffer's address for the rkey */
	uint64_t	rqi_len;
} rdmat_qpinfo_t;

typedef struct rdmat_setup {
	/* In */
	char		rs_dev[RDMAT_NAME_MAX];
	uint32_t	rs_ipv4;	/* network order */
	uint32_t	rs_qpt;		/* rdmat_qpt_t */
	uint32_t	rs_nqp;
	uint32_t	rs_poll;	/* rdmat_poll_t */
	uint64_t	rs_buf_len;	/* per QP, multiple of RDMAT_CHUNK */
	uint32_t	rs_depth;	/* send and receive queue depth */
	uint32_t	rs_inline;	/* inline bytes the QPs take */
	/* Out */
	uint8_t		rs_gid[16];
	uint8_t		rs_mac[6];
	uint16_t	rs_gid_index;
	rdmat_qpinfo_t	rs_qp[RDMAT_MAX_QPS];
} rdmat_setup_t;

typedef struct rdmat_connect {
	uint32_t	rc_qp;		/* local QP index */
	uint32_t	rc_rqpn;
	uint32_t	rc_rpsn;
	uint32_t	rc_path_mtu;	/* bytes: 256 to 4096 */
	uint8_t		rc_dgid[16];
	uint8_t		rc_dmac[6];
	uint8_t		rc_retry;
	uint8_t		rc_rnr_retry;
	uint32_t	rc_rqkey;	/* UD */
	uint32_t	rc_qp_access;	/* RC: 0 for all, or RDMAT_QPACC_* */
} rdmat_connect_t;

/*
 * rc_qp_access: RDMAT_QPACC_SET with the RDMAT_ACC_* rights the QP grants,
 * and RDMAT_QPACC_NO_IRD for no inbound read resources.
 */
#define	RDMAT_QPACC_SET		0x100
#define	RDMAT_QPACC_NO_IRD	0x200

typedef enum rdmat_op {
	RDMAT_OP_SEND = 1,	/* count sends of size from offset */
	RDMAT_OP_SEND_INV,	/* sends that invalidate rkey */
	RDMAT_OP_POST_RECV,	/* post count receives of size, return */
	RDMAT_OP_WAIT_RECV,	/* wait for count receive completions */
	RDMAT_OP_WRITE,		/* count writes to raddr/rkey */
	RDMAT_OP_READ,		/* count reads from raddr/rkey */
	RDMAT_OP_PING,		/* send, wait for the reply; count times */
	RDMAT_OP_PONG,		/* wait, send back; count times */
	RDMAT_OP_REG,		/* rebind the buffer MR: access, new key */
	RDMAT_OP_LOCAL_INV,	/* invalidate the buffer MR */
	RDMAT_OP_RECV_STREAM,	/* receive count, keeping depth posted */
	RDMAT_OP_WRITE_PING,	/* write, wait for the peer's write; count */
	RDMAT_OP_WRITE_PONG	/* wait for the peer's write, write back */
} rdmat_op_t;

/*
 * rr_flags.  RDMAT_F_UNSIGNALED with rr_signal 0 signals every rr_depth'th
 * request and the last.  RDMAT_F_BUSY waits by polling without sleeping
 * (RDMAT_POLL_DIRECT only).  RDMAT_F_LAT times each READ or WRITE alone.
 */
#define	RDMAT_F_UNSIGNALED	0x01
#define	RDMAT_F_DMA_LKEY	0x02	/* use the local DMA lkey, not the MR */
#define	RDMAT_F_INLINE		0x04
#define	RDMAT_F_BUSY		0x08
#define	RDMAT_F_LAT		0x10

typedef struct rdmat_run {
	/* In */
	uint32_t	rr_qp;
	uint32_t	rr_op;		/* rdmat_op_t */
	uint32_t	rr_size;
	uint32_t	rr_count;
	uint32_t	rr_depth;
	uint32_t	rr_flags;
	uint64_t	rr_offset;	/* in the local buffer */
	uint64_t	rr_raddr;	/* remote address, or wrap length */
	uint64_t	rr_rlen;	/* remote window that offsets wrap in */
	uint32_t	rr_rkey;	/* remote key, or key to invalidate */
	uint32_t	rr_access;	/* REG: RDMAT_ACC_* */
	uint32_t	rr_timeout_ms;
	uint32_t	rr_pad;
	/* Out */
	uint64_t	rr_done;	/* successful completions */
	uint64_t	rr_bytes;
	uint64_t	rr_ns;		/* first post to last completion */
	uint32_t	rr_errors;
	uint32_t	rr_status;	/* first error, an rdk_wc_status */
	uint32_t	rr_err_opcode;
	uint32_t	rr_vendor_err;
	uint32_t	rr_last_len;	/* byte_len of the last receive */
	uint32_t	rr_len_mismatch;
	uint32_t	rr_wc_flags;	/* of the last receive */
	uint32_t	rr_inv_rkey;	/* rkey the last receive invalidated */
	uint32_t	rr_new_rkey;	/* REG */
	uint32_t	rr_qp_state;	/* after the run */
	uint64_t	rr_lat_min;	/* PING, ns per round trip */
	uint64_t	rr_lat_avg;
	uint64_t	rr_lat_max;
	uint64_t	rr_lat_p50;
	uint64_t	rr_lat_p99;
	/* In */
	uint32_t	rr_batch;	/* requests per post call; 0 is 1 */
	uint32_t	rr_signal;	/* signal every rr_signal'th request */
	uint32_t	rr_run_ms;	/* stop posting after this long */
	uint32_t	rr_pad2;
	/* Out */
	uint64_t	rr_lat_p999;
	uint64_t	rr_posted;	/* requests posted */
	uint64_t	rr_post_calls;
} rdmat_run_t;

#define	RDMAT_ACC_REMOTE_WRITE	0x1
#define	RDMAT_ACC_REMOTE_READ	0x2
#define	RDMAT_ACC_LOCAL_WRITE	0x4	/* rc_qp_access only */

typedef enum rdmat_bufop {
	RDMAT_BUF_FILL = 1,
	RDMAT_BUF_VERIFY,
	RDMAT_BUF_ZERO
} rdmat_bufop_t;

typedef struct rdmat_buf {
	uint32_t	rb_qp;
	uint32_t	rb_op;		/* rdmat_bufop_t */
	uint64_t	rb_offset;
	uint64_t	rb_len;
	uint64_t	rb_seed;
	uint64_t	rb_pattern_base; /* pattern offset of rb_offset */
	int64_t		rb_mismatch;	/* out: first bad offset, or -1 */
	uint64_t	rb_ns;		/* out */
} rdmat_buf_t;

typedef struct rdmat_query {
	uint32_t	rq_qp;
	uint32_t	rq_state;	/* rdk_qp_state */
	uint64_t	rq_events;	/* asynchronous QP events seen */
	uint32_t	rq_last_event;
	uint32_t	rq_pad;
} rdmat_query_t;

#ifdef __cplusplus
}
#endif

#endif /* _RDMAT_IOCTL_H */

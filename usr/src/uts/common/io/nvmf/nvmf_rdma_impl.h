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

#ifndef _NVMF_RDMA_IMPL_H
#define	_NVMF_RDMA_IMPL_H

/*
 * NVMe over Fabrics RDMA transport, controller side.  See nvmf_rdma.c.
 */

#include <sys/nvme/nvmf_rdma.h>

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/ksynch.h>
#include <sys/list.h>
#include <sys/taskq.h>
#include <sys/taskq_impl.h>
#include <sys/vmem.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <netinet/in.h>
#include "nvmf_transport_internal.h"
#include "../rdma/rdk.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The CM private data of the NVMe RDMA transport binding, all little
 * endian: a REQ of 32 bytes (RECFMT, QID, HRQSIZE, HSQSIZE, CNTLID), a REP
 * of 32 bytes (RECFMT, CRQSIZE) and a REJ of 4 bytes (RECFMT, STS).
 */
#define	NVMF_RDMA_REQ_LEN	32
#define	NVMF_RDMA_REP_LEN	32
#define	NVMF_RDMA_REJ_LEN	4

typedef enum nvmf_rdma_rej {
	NVMF_RDMA_OK = 0,
	NVMF_RDMA_REJ_INVALID_LEN = 1,
	NVMF_RDMA_REJ_INVALID_RECFMT = 2,
	NVMF_RDMA_REJ_INVALID_QID = 3,
	NVMF_RDMA_REJ_INVALID_HSQSIZE = 4,
	NVMF_RDMA_REJ_INVALID_HRQSIZE = 5,
	NVMF_RDMA_REJ_NO_RESOURCES = 6,
	NVMF_RDMA_REJ_INVALID_IRD = 7,
	NVMF_RDMA_REJ_INVALID_ORD = 8,
	NVMF_RDMA_REJ_INVALID_CNTLID = 9
} nvmf_rdma_rej_t;

typedef struct nvmf_rdma_req {
	uint16_t	nrq_qid;
	uint16_t	nrq_hrqsize;
	uint16_t	nrq_hsqsize;	/* 0's based, as Connect SQSIZE */
	uint16_t	nrq_cntlid;
} nvmf_rdma_req_t;

/* What a listener accepts; entry counts are 1's based. */
typedef struct nvmf_rdma_limits {
	uint16_t	nrl_max_qid;
	uint32_t	nrl_admin_entries;
	uint32_t	nrl_io_entries;
} nvmf_rdma_limits_t;

/* The device attributes queue sizing needs. */
typedef struct nvmf_rdma_devlim {
	uint32_t	ndl_max_qp_wr;
	uint32_t	ndl_max_cqe;
	uint32_t	ndl_rw_wrs;	/* send queue entries of one transfer */
} nvmf_rdma_devlim_t;

/*
 * A queue of depth entries: depth RECVs, twice as many command contexts
 * (a context outlives its response until the SEND completes), xfers
 * transfers in flight at once, and one extra entry in each work queue for
 * the drain.
 */
typedef struct nvmf_rdma_sizes {
	uint32_t	nrs_depth;
	uint32_t	nrs_cmds;
	uint32_t	nrs_xfers;
	uint32_t	nrs_rq;
	uint32_t	nrs_sq;
	uint32_t	nrs_cq;
	uint32_t	nrs_slot;	/* bytes of one RECV buffer */
	uint64_t	nrs_bytes;	/* charged against the memory caps */
} nvmf_rdma_sizes_t;

#define	NVMF_RDMA_ADMIN_ENTRIES	32
#define	NVMF_RDMA_MAX_ENTRIES	1024
#define	NVMF_RDMA_MAX_ICD	16384
#define	NVMF_RDMA_SQE_LEN	64
#define	NVMF_RDMA_CQE_LEN	16
#define	NR_CIDHASH		64

extern nvmf_rdma_rej_t nvmf_rdma_req_parse(const void *, size_t, uint32_t,
    const nvmf_rdma_limits_t *, nvmf_rdma_req_t *);
extern void nvmf_rdma_rep_build(uint8_t *, uint16_t);
extern void nvmf_rdma_rej_build(uint8_t *, nvmf_rdma_rej_t);
extern nvmf_rdma_rej_t nvmf_rdma_size_queue(uint32_t, uint32_t,
    const nvmf_rdma_devlim_t *, nvmf_rdma_sizes_t *);
extern boolean_t nvmf_rdma_range_ok(uint32_t, uint32_t, uint32_t);
extern uint32_t nvmf_rdma_cid_hash(uint16_t);

#ifdef _KERNEL

struct nr_queue;
struct nr_cmd;
struct nr_listener;

/* The first member of each CM context, so the handler can tell them apart. */
typedef enum nr_kind {
	NR_KIND_LISTENER = 0x4c53,
	NR_KIND_QUEUE = 0x5155
} nr_kind_t;

/*
 * A device's registered buffer pool: physically contiguous chunks from
 * rdk_dma_buf_alloc(), carved with vmem.  Chunks sit in the arena with a
 * gap between them so that no buffer spans two.
 */
typedef struct nr_pool {
	kmutex_t	np_lock;
	kcondvar_t	np_cv;
	vmem_t		*np_arena;
	rdk_dma_buf_t	*np_chunks;
	uint_t		np_nchunks;
	uint_t		np_maxchunks;
	size_t		np_free;
	uint_t		np_bufs;	/* outstanding, not leaked */
	uint_t		np_leaked;
	boolean_t	np_growing;
	boolean_t	np_dying;
	taskq_ent_t	np_grow_ent;
} nr_pool_t;

typedef struct nr_buf {
	struct nr_dev		*nb_dev;
	uintptr_t		nb_key;		/* the vmem address */
	caddr_t			nb_va;
	size_t			nb_len;
	ddi_dma_cookie_t	nb_ck;
} nr_buf_t;

typedef struct nr_dev {
	list_node_t		nd_node;
	struct rdk_device	*nd_dev;
	struct rdk_pd		*nd_pd;
	boolean_t		nd_iwarp;
	kmutex_t		nd_lock;
	kcondvar_t		nd_cv;
	list_t			nd_qlist;
	uint_t			nd_building;	/* queues not yet listed */
	uint_t			nd_listeners;
	boolean_t		nd_removing;
	nr_pool_t		nd_pool;
} nr_dev_t;

typedef struct nr_peer {
	list_node_t	np_node;
	ipaddr_t	np_addr;
	uint_t		np_queues;
	uint_t		np_unconnected;
} nr_peer_t;

typedef struct nr_listener {
	nr_kind_t		nl_kind;
	list_node_t		nl_node;
	uint32_t		nl_id;
	rdk_cm_id_t		*nl_cmid;
	nr_dev_t		*nl_dev;
	struct sockaddr_in	nl_addr;
	nvmf_rdma_limits_t	nl_lim;
	uint32_t		nl_icd;
	kmutex_t		nl_lock;
	uint_t			nl_refs;	/* the port and its queues */
	list_t			nl_peers;
	list_t			nl_queues;
} nr_listener_t;

/* A RECV buffer. */
typedef struct nr_recv {
	struct rdk_cqe		rv_cqe;
	struct nr_queue		*rv_q;
	list_node_t		rv_node;	/* the backlog */
	caddr_t			rv_va;
	uint64_t		rv_pa;
	uint32_t		rv_len;
	uint32_t		rv_byte_len;
	boolean_t		rv_posted;
} nr_recv_t;

typedef enum nr_xstate {
	NR_X_FREE = 0,
	NR_X_WAIT,	/* on the queue's wait list for an xfer */
	NR_X_POSTED,	/* its RDMA READ or WRITE is on the send queue */
	NR_X_SENDING,	/* data done; the response SEND completes it */
	NR_X_DONE	/* its callback is about to run */
} nr_xstate_t;

/* A resource for one RDMA transfer: its rdk_rw context and READ-sink MRs. */
typedef struct nr_xfer {
	struct rdk_cqe		xf_cqe;
	list_node_t		xf_node;
	struct nr_queue		*xf_q;
	rdk_rw_ctx_t		*xf_rw;
	struct rdk_mr		**xf_mrs;
	uint_t			xf_mrs_len;
	uint_t			xf_nmrs;
	struct nr_xreq		*xf_req;
	boolean_t		xf_busy;
} nr_xfer_t;

/* One data transfer that nvmft asked for. */
typedef struct nr_xreq {
	list_node_t		xr_node;
	struct nr_cmd		*xr_cmd;
	nr_xstate_t		xr_state;
	boolean_t		xr_read;	/* host to controller */
	boolean_t		xr_final;	/* carries the response */
	uint32_t		xr_off;
	uint32_t		xr_len;
	nvmf_memdesc_t		xr_mem;
	nr_buf_t		*xr_bounce;
	ddi_dma_cookie_t	xr_ck;
	nr_xfer_t		*xr_xfer;
	nvmf_io_complete_t	*xr_io_cb;
	nvmf_send_complete_t	*xr_send_cb;
	void			*xr_cb_arg;
	uint_t			xr_status;	/* send status or errno */
	boolean_t		xr_sent;	/* its SEND completed first */
	boolean_t		xr_send_ok;
	nvme_cqe_t		xr_cqe;
} nr_xreq_t;

#define	NR_CMD_XREQS	4

typedef enum nr_cstate {
	NR_C_FREE = 0,
	NR_C_ACTIVE,	/* received; no response yet */
	NR_C_DONE	/* response posted, or given up */
} nr_cstate_t;

/*
 * A command context.  nvmft owns the capsule until it frees it; the context
 * returns to the free list once that has happened and no work request that
 * names it remains.
 */
typedef struct nr_cmd {
	struct nvmf_capsule	nc_nc;
	nr_kind_t		nc_kind;
	struct nr_queue		*nc_q;
	list_node_t		nc_node;	/* free list or CID hash */
	list_node_t		nc_park;
	nr_cstate_t		nc_state;
	boolean_t		nc_capsule;	/* nvmft holds it */
	boolean_t		nc_hashed;
	uint_t			nc_wrs;		/* SEND and transfers posted */
	nr_recv_t		*nc_recv;	/* held for in-capsule data */
	uint32_t		nc_icd;
	uint8_t			nc_sgl_sc;
	boolean_t		nc_sgl_done;
	nvmf_sgl_t		nc_sgl;
	uint16_t		nc_cid;
	nvme_cqe_t		*nc_cqe;	/* DMA slot of the response */
	uint64_t		nc_cqe_pa;
	struct rdk_cqe		nc_send_cqe;
	struct rdk_send_wr	nc_swr;
	struct rdk_sge		nc_ssge;
	boolean_t		nc_send_posted;
	nr_xreq_t		*nc_final;
	nr_xreq_t		nc_xreq[NR_CMD_XREQS];
} nr_cmd_t;

/* A response capsule; nvmft frees it once transmit_capsule returns. */
typedef struct nr_rsp {
	struct nvmf_capsule	rs_nc;
	nr_kind_t		rs_kind;
} nr_rsp_t;

#define	NR_KIND_CMD	0x434d
#define	NR_KIND_RSP	0x5253

typedef enum nr_qstate {
	NR_Q_ACCEPTING = 0,	/* capsules park until ESTABLISHED */
	NR_Q_LIVE,
	NR_Q_DYING,
	NR_Q_DEAD		/* hardware objects destroyed */
} nr_qstate_t;

typedef struct nr_queue {
	struct nvmf_qpair	nq_nq;
	nr_kind_t		nq_kind;
	list_node_t		nq_lnode;	/* the listener's queues */
	list_node_t		nq_dnode;	/* the device's queues */
	kmutex_t		nq_lock;
	kcondvar_t		nq_cv;
	nr_qstate_t		nq_state;
	uint_t			nq_refs;	/* nvmft, capsules, setup */
	int			nq_error;
	boolean_t		nq_adopted;
	boolean_t		nq_reported;
	boolean_t		nq_connected;
	boolean_t		nq_have_connect;
	uint16_t		nq_connect_cid;
	uint16_t		nq_qid;
	uint16_t		nq_cntlid;
	ipaddr_t		nq_peer;
	nr_peer_t		*nq_peer_ent;
	uint64_t		nq_charge;
	boolean_t		nq_counted;	/* no longer unconnected */
	nr_listener_t		*nq_listener;
	nr_dev_t		*nq_dev;
	rdk_cm_id_t		*nq_cmid;
	boolean_t		nq_established;
	struct rdk_cq		*nq_cq;
	struct rdk_qp		*nq_qp;
	rdk_teardown_t		*nq_td;
	timeout_id_t		nq_deadline;
	nvmf_rdma_sizes_t	nq_sz;
	uint32_t		nq_icd;		/* of this queue's RECVs */
	uint32_t		nq_io_icd;	/* of the listener's I/O queues */
	uint64_t		nq_max_xfer;
	uint32_t		nq_xfer_len;	/* largest transfer */
	boolean_t		nq_send_inv;	/* SEND_WITH_INV works */
	boolean_t		nq_inline;	/* a CQE may go inline */

	rdk_dma_buf_t		*nq_ring;	/* RECV buffers, by chunk */
	uint_t			nq_nring;
	rdk_dma_buf_t		nq_cqebuf;
	nr_recv_t		*nq_recvs;

	nr_cmd_t		*nq_cmds;
	list_t			nq_free_cmds;
	list_t			nq_cid[NR_CIDHASH];
	list_t			nq_parked;	/* until ESTABLISHED */
	list_t			nq_backlog;	/* RECVs awaiting a context */
	boolean_t		nq_backlog_queued;
	taskq_ent_t		nq_backlog_ent;

	nr_xfer_t		*nq_xfers;
	list_t			nq_free_xfers;
	list_t			nq_wait;	/* nr_xreq_t awaiting an xfer */
	uint64_t		nq_rw_waits;
} nr_queue_t;

#define	NR_Q(nq)	((nr_queue_t *)(void *)(nq))

/* nvmf_rdma.c */
extern struct nvmf_transport_ops nvmf_rdma_ops;
extern taskq_t *nvmf_rdma_taskq;
extern uint32_t nvmf_rdma_max_xfer;
extern nr_queue_t *nr_queue_create(nr_dev_t *, const nvmf_rdma_sizes_t *,
    uint16_t, uint32_t, uint32_t, int *);
extern int nr_queue_post_ring(nr_queue_t *);
extern void nr_queue_destroy_unadopted(nr_queue_t *);
extern void nr_queue_fail(nr_queue_t *, int);
extern void nr_queue_fail_locked(nr_queue_t *, int);
extern void nr_queue_established(nr_queue_t *);
extern void nr_queue_rele(nr_queue_t *);
extern void nr_queue_deadline(void *);
extern int nr_post_locked(nr_queue_t *, struct rdk_send_wr *);
extern void nr_cmd_rele_locked(nr_cmd_t *);
extern int nr_respond_locked(nr_cmd_t *, const nvme_cqe_t *,
    struct rdk_send_wr **, boolean_t);
extern void nr_respond_posted_locked(nr_cmd_t *);
extern void nr_respond_unposted_locked(nr_cmd_t *);

/* nvmf_rdma_xfer.c */
extern int nr_xfer_init(nr_queue_t *);
extern void nr_xfer_fini(nr_queue_t *);
extern int nr_receive_controller_data(struct nvmf_capsule *, uint32_t,
    struct nvmf_io_request *);
extern int nr_send_controller_data_io(struct nvmf_capsule *, uint32_t,
    const struct nvmf_send_request *, const nvme_cqe_t *);
extern void nr_xreq_send_done(nr_cmd_t *, boolean_t);
extern void nr_xfer_fail_all(nr_queue_t *);
extern uint_t nr_rw_attr(nr_dev_t *, struct rdk_rw_attr *);

/* nvmf_rdma_pool.c */
extern uint32_t nvmf_rdma_dbuf_max;
extern int nr_pool_init(nr_dev_t *);
extern int nr_pool_prime(nr_dev_t *);
extern void nr_pool_fini(nr_dev_t *);
extern nr_buf_t *nr_buf_alloc(nr_dev_t *, size_t, size_t);
extern void nr_buf_free(nr_buf_t *);
extern int nr_alloc_data_buf(struct nvmf_qpair *, size_t, size_t,
    nvmf_databuf_t *);
extern void nr_free_data_buf(nvmf_databuf_t *);

/* nvmf_rdma_cm.c */
extern int nr_cm_init(void);
extern void nr_cm_fini(void);
extern boolean_t nr_cm_busy(void);
extern int nr_listen(cred_t *, const struct sockaddr_in *,
    const struct sockaddr_in *, uint_t, const nvmf_rdma_limits_t *,
    uint32_t, uint32_t *);
extern int nr_unlisten(uint32_t);
extern void nr_queue_detach(nr_queue_t *);
extern void nr_queue_connected(nr_queue_t *);

#endif /* _KERNEL */

#ifdef __cplusplus
}
#endif

#endif /* _NVMF_RDMA_IMPL_H */

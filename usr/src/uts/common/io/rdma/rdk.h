/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2004 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2004 Infinicon Corporation.  All rights reserved.
 * Copyright (c) 2004, 2020 Intel Corporation.  All rights reserved.
 * Copyright (c) 2004 Topspin Corporation.  All rights reserved.
 * Copyright (c) 2004 Voltaire Corporation.  All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2005, 2006, 2007 Cisco Systems.  All rights reserved.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#ifndef _RDK_H
#define	_RDK_H

/*
 * rdmak: the kernel RDMA verbs framework.  The types and the calling model
 * follow the Linux kernel verbs (include/rdma/ib_verbs.h, used under the
 * OpenIB license; see README.illumos), with every exported name carrying the
 * rdk_ or RDK_ prefix so that nothing collides with the older ib_ and rdma_
 * symbols of sol_ofs.  Errors are returned as positive errno values.
 *
 * A provider driver fills in a struct rdk_device and registers it.  Kernel
 * consumers register a struct rdk_client and get add and remove callbacks
 * for each device.  When a device is unregistered, each client's remove
 * callback must destroy every object the client made on that device before
 * it returns.
 *
 * Objects are created and destroyed in thread context.  rdk_post_send(),
 * rdk_post_recv(), rdk_poll_cq() and rdk_req_notify_cq() may be called from
 * any context that can take an adaptive mutex, but not at high interrupt
 * level.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ethernet.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/taskq.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	RDK_ABI_VERSION		1
#define	RDK_NAME_MAX		32
#define	RDK_GID_TABLE_LEN	16

struct rdk_device;
struct rdk_pd;
struct rdk_cq;
struct rdk_qp;
struct rdk_mr;
struct rdk_ah;

typedef union rdk_gid {
	uint8_t		raw[16];
	struct {
		uint64_t	subnet_prefix;	/* big endian */
		uint64_t	interface_id;	/* big endian */
	} global;
} rdk_gid_t;

enum rdk_gid_type {
	RDK_GID_TYPE_ROCE_UDP_ENCAP = 2
};

enum rdk_network_type {
	RDK_NETWORK_IPV4 = 1,
	RDK_NETWORK_IPV6
};

/*
 * An entry of a port's GID table.  The framework owns it; a reference from
 * rdk_get_gid_attr() keeps it from being deleted.
 */
struct rdk_gid_attr {
	struct rdk_device	*device;
	rdk_gid_t		gid;
	enum rdk_gid_type	gid_type;
	enum rdk_network_type	network_type;
	uint16_t		index;
	uint32_t		port_num;
	uint16_t		vlan_id;	/* 0xffff: untagged */
	uint8_t			mac[ETHERADDRL];
};

#define	RDK_VLAN_NONE		0xffff

enum rdk_mtu {
	RDK_MTU_256 = 1,
	RDK_MTU_512 = 2,
	RDK_MTU_1024 = 3,
	RDK_MTU_2048 = 4,
	RDK_MTU_4096 = 5
};

static inline int
rdk_mtu_enum_to_int(enum rdk_mtu mtu)
{
	switch (mtu) {
	case RDK_MTU_256:
		return (256);
	case RDK_MTU_512:
		return (512);
	case RDK_MTU_1024:
		return (1024);
	case RDK_MTU_2048:
		return (2048);
	case RDK_MTU_4096:
		return (4096);
	default:
		return (-1);
	}
}

static inline enum rdk_mtu
rdk_mtu_int_to_enum(int mtu)
{
	if (mtu >= 4096)
		return (RDK_MTU_4096);
	else if (mtu >= 2048)
		return (RDK_MTU_2048);
	else if (mtu >= 1024)
		return (RDK_MTU_1024);
	else if (mtu >= 512)
		return (RDK_MTU_512);
	return (RDK_MTU_256);
}

enum rdk_port_state {
	RDK_PORT_NOP = 0,
	RDK_PORT_DOWN = 1,
	RDK_PORT_INIT = 2,
	RDK_PORT_ARMED = 3,
	RDK_PORT_ACTIVE = 4
};

struct rdk_port_attr {
	enum rdk_port_state	state;
	enum rdk_mtu		max_mtu;
	enum rdk_mtu		active_mtu;
	uint32_t		phys_mtu;
	int			gid_tbl_len;
	uint32_t		max_msg_sz;
	uint16_t		pkey_tbl_len;
	uint64_t		speed;		/* bits per second */
	uint8_t			mac[ETHERADDRL];
};

/* device_cap_flags */
#define	RDK_DEVICE_RC_RNR_NAK_GEN	(1ULL << 0)
#define	RDK_DEVICE_MEM_MGT_EXTENSIONS	(1ULL << 21)

/* kernel_cap_flags */
#define	RDK_KCAP_LOCAL_DMA_LKEY		(1ULL << 0)

struct rdk_device_attr {
	uint64_t	fw_ver;
	uint64_t	sys_image_guid;
	uint64_t	max_mr_size;
	uint64_t	page_size_cap;
	uint32_t	vendor_id;
	uint32_t	vendor_part_id;
	uint32_t	hw_ver;
	int		max_qp;
	int		max_qp_wr;
	uint64_t	device_cap_flags;
	uint64_t	kernel_cap_flags;
	int		max_send_sge;
	int		max_recv_sge;
	int		max_sge_rd;
	int		max_cq;
	int		max_cqe;
	int		max_mr;
	int		max_pd;
	int		max_qp_rd_atom;
	int		max_qp_init_rd_atom;
	int		max_ah;
	uint32_t	max_fast_reg_page_list_len;
	uint32_t	max_inline_data;
	uint16_t	max_pkeys;
	uint32_t	local_dma_lkey;
};

enum rdk_event_type {
	RDK_EVENT_CQ_ERR,
	RDK_EVENT_QP_FATAL,
	RDK_EVENT_QP_REQ_ERR,
	RDK_EVENT_QP_ACCESS_ERR,
	RDK_EVENT_COMM_EST,
	RDK_EVENT_SQ_DRAINED,
	RDK_EVENT_PATH_MIG,
	RDK_EVENT_PATH_MIG_ERR,
	RDK_EVENT_DEVICE_FATAL,
	RDK_EVENT_PORT_ACTIVE,
	RDK_EVENT_PORT_ERR,
	RDK_EVENT_LID_CHANGE,
	RDK_EVENT_PKEY_CHANGE,
	RDK_EVENT_SM_CHANGE,
	RDK_EVENT_SRQ_ERR,
	RDK_EVENT_SRQ_LIMIT_REACHED,
	RDK_EVENT_QP_LAST_WQE_REACHED,
	RDK_EVENT_CLIENT_REREGISTER,
	RDK_EVENT_GID_CHANGE
};

struct rdk_event {
	struct rdk_device	*device;
	union {
		struct rdk_cq	*cq;
		struct rdk_qp	*qp;
		uint32_t	port_num;
	} element;
	enum rdk_event_type	event;
};

struct rdk_event_handler {
	struct rdk_device	*device;
	void			(*handler)(struct rdk_event_handler *,
	    struct rdk_event *);
	list_node_t		reh_node;	/* framework */
};

/*
 * Address handles and paths.
 */
#define	RDK_AH_GRH		1

enum rdk_ah_attr_type {
	RDK_AH_ATTR_TYPE_UNDEFINED,
	RDK_AH_ATTR_TYPE_ROCE = 2
};

struct rdk_global_route {
	const struct rdk_gid_attr *sgid_attr;	/* set by the framework */
	rdk_gid_t		dgid;
	uint32_t		flow_label;
	uint8_t			sgid_index;
	uint8_t			hop_limit;
	uint8_t			traffic_class;
};

struct rdk_ah_attr {
	struct rdk_global_route	grh;
	uint8_t			sl;
	uint8_t			static_rate;
	uint32_t		port_num;
	uint8_t			ah_flags;
	enum rdk_ah_attr_type	type;
	struct {
		uint8_t		dmac[ETHERADDRL];
	} roce;
};

/*
 * Work completions.
 */
enum rdk_wc_status {
	RDK_WC_SUCCESS,
	RDK_WC_LOC_LEN_ERR,
	RDK_WC_LOC_QP_OP_ERR,
	RDK_WC_LOC_EEC_OP_ERR,
	RDK_WC_LOC_PROT_ERR,
	RDK_WC_WR_FLUSH_ERR,
	RDK_WC_MW_BIND_ERR,
	RDK_WC_BAD_RESP_ERR,
	RDK_WC_LOC_ACCESS_ERR,
	RDK_WC_REM_INV_REQ_ERR,
	RDK_WC_REM_ACCESS_ERR,
	RDK_WC_REM_OP_ERR,
	RDK_WC_RETRY_EXC_ERR,
	RDK_WC_RNR_RETRY_EXC_ERR,
	RDK_WC_LOC_RDD_VIOL_ERR,
	RDK_WC_REM_INV_RD_REQ_ERR,
	RDK_WC_REM_ABORT_ERR,
	RDK_WC_INV_EECN_ERR,
	RDK_WC_INV_EEC_STATE_ERR,
	RDK_WC_FATAL_ERR,
	RDK_WC_RESP_TIMEOUT_ERR,
	RDK_WC_GENERAL_ERR
};

enum rdk_wc_opcode {
	RDK_WC_SEND = 0,
	RDK_WC_RDMA_WRITE = 1,
	RDK_WC_RDMA_READ = 2,
	RDK_WC_COMP_SWAP = 3,
	RDK_WC_FETCH_ADD = 4,
	RDK_WC_BIND_MW = 5,
	RDK_WC_LOCAL_INV = 6,
	RDK_WC_LSO = 7,
	RDK_WC_REG_MR = 10,
	/* Receive completions have this bit set. */
	RDK_WC_RECV = 1 << 7,
	RDK_WC_RECV_RDMA_WITH_IMM
};

/* wc_flags */
#define	RDK_WC_GRH			(1 << 0)
#define	RDK_WC_WITH_IMM			(1 << 1)
#define	RDK_WC_WITH_INVALIDATE		(1 << 2)
#define	RDK_WC_WITH_SMAC		(1 << 4)
#define	RDK_WC_WITH_VLAN		(1 << 5)
#define	RDK_WC_WITH_NETWORK_HDR_TYPE	(1 << 6)

struct rdk_wc;

/*
 * A consumer that allocates its CQ with rdk_alloc_cq() puts a struct
 * rdk_cqe in each work request instead of a wr_id; done() runs for the
 * completion.  done() must not destroy the QP or the CQ: completions
 * polled with it may still name them.
 */
struct rdk_cqe {
	void	(*done)(struct rdk_cq *, struct rdk_wc *);
};

struct rdk_wc {
	union {
		uint64_t	wr_id;
		struct rdk_cqe	*wr_cqe;
	};
	enum rdk_wc_status	status;
	enum rdk_wc_opcode	opcode;
	uint32_t		vendor_err;
	uint32_t		byte_len;
	struct rdk_qp		*qp;
	union {
		uint32_t	imm_data;	/* big endian */
		uint32_t	invalidate_rkey;
	} ex;
	uint32_t		src_qp;
	int			wc_flags;
	uint16_t		pkey_index;
	uint8_t			sl;
	uint32_t		port_num;
	uint8_t			smac[ETHERADDRL];
	uint16_t		vlan_id;
	uint8_t			network_hdr_type;
};

enum rdk_cq_notify_flags {
	RDK_CQ_SOLICITED = 1 << 0,
	RDK_CQ_NEXT_COMP = 1 << 1,
	RDK_CQ_SOLICITED_MASK = RDK_CQ_SOLICITED | RDK_CQ_NEXT_COMP,
	RDK_CQ_REPORT_MISSED_EVENTS = 1 << 2
};

struct rdk_cq_init_attr {
	uint32_t	cqe;
	uint32_t	comp_vector;
	uint32_t	flags;
};

/*
 * Queue pairs.
 */
enum rdk_qp_type {
	RDK_QPT_SMI,
	RDK_QPT_GSI,
	RDK_QPT_RC = 2,
	RDK_QPT_UC = 3,
	RDK_QPT_UD = 4,
	RDK_QPT_MAX
};

enum rdk_sig_type {
	RDK_SIGNAL_ALL_WR,
	RDK_SIGNAL_REQ_WR
};

struct rdk_qp_cap {
	uint32_t	max_send_wr;
	uint32_t	max_recv_wr;
	uint32_t	max_send_sge;
	uint32_t	max_recv_sge;
	uint32_t	max_inline_data;
};

struct rdk_qp_init_attr {
	void			(*event_handler)(struct rdk_event *, void *);
	void			*qp_context;
	struct rdk_cq		*send_cq;
	struct rdk_cq		*recv_cq;
	struct rdk_qp_cap	cap;
	enum rdk_sig_type	sq_sig_type;
	enum rdk_qp_type	qp_type;
	uint32_t		create_flags;
	uint32_t		port_num;
};

enum rdk_qp_attr_mask {
	RDK_QP_STATE = 1,
	RDK_QP_CUR_STATE = (1 << 1),
	RDK_QP_EN_SQD_ASYNC_NOTIFY = (1 << 2),
	RDK_QP_ACCESS_FLAGS = (1 << 3),
	RDK_QP_PKEY_INDEX = (1 << 4),
	RDK_QP_PORT = (1 << 5),
	RDK_QP_QKEY = (1 << 6),
	RDK_QP_AV = (1 << 7),
	RDK_QP_PATH_MTU = (1 << 8),
	RDK_QP_TIMEOUT = (1 << 9),
	RDK_QP_RETRY_CNT = (1 << 10),
	RDK_QP_RNR_RETRY = (1 << 11),
	RDK_QP_RQ_PSN = (1 << 12),
	RDK_QP_MAX_QP_RD_ATOMIC = (1 << 13),
	RDK_QP_ALT_PATH = (1 << 14),
	RDK_QP_MIN_RNR_TIMER = (1 << 15),
	RDK_QP_SQ_PSN = (1 << 16),
	RDK_QP_MAX_DEST_RD_ATOMIC = (1 << 17),
	RDK_QP_PATH_MIG_STATE = (1 << 18),
	RDK_QP_CAP = (1 << 19),
	RDK_QP_DEST_QPN = (1 << 20),
	RDK_QP_ATTR_STANDARD_BITS = (1 << 21) - 1
};

enum rdk_qp_state {
	RDK_QPS_RESET,
	RDK_QPS_INIT,
	RDK_QPS_RTR,
	RDK_QPS_RTS,
	RDK_QPS_SQD,
	RDK_QPS_SQE,
	RDK_QPS_ERR
};

struct rdk_qp_attr {
	enum rdk_qp_state	qp_state;
	enum rdk_qp_state	cur_qp_state;
	enum rdk_mtu		path_mtu;
	uint32_t		qkey;
	uint32_t		rq_psn;
	uint32_t		sq_psn;
	uint32_t		dest_qp_num;
	int			qp_access_flags;
	struct rdk_qp_cap	cap;
	struct rdk_ah_attr	ah_attr;
	uint16_t		pkey_index;
	uint8_t			en_sqd_async_notify;
	uint8_t			max_rd_atomic;
	uint8_t			max_dest_rd_atomic;
	uint8_t			min_rnr_timer;
	uint32_t		port_num;
	uint8_t			timeout;
	uint8_t			retry_cnt;
	uint8_t			rnr_retry;
};

/*
 * Work requests.
 */
enum rdk_wr_opcode {
	RDK_WR_RDMA_WRITE = 0,
	RDK_WR_RDMA_WRITE_WITH_IMM = 1,
	RDK_WR_SEND = 2,
	RDK_WR_SEND_WITH_IMM = 3,
	RDK_WR_RDMA_READ = 4,
	RDK_WR_ATOMIC_CMP_AND_SWP = 5,
	RDK_WR_ATOMIC_FETCH_AND_ADD = 6,
	RDK_WR_LOCAL_INV = 7,
	RDK_WR_BIND_MW = 8,
	RDK_WR_SEND_WITH_INV = 9,
	RDK_WR_RDMA_READ_WITH_INV = 11,
	RDK_WR_REG_MR = 0x20
};

/* send_flags */
#define	RDK_SEND_FENCE		(1 << 0)
#define	RDK_SEND_SIGNALED	(1 << 1)
#define	RDK_SEND_SOLICITED	(1 << 2)
#define	RDK_SEND_INLINE		(1 << 3)

/* The layout matches the device's scatter-gather element. */
struct rdk_sge {
	uint64_t	addr;
	uint32_t	length;
	uint32_t	lkey;
};

struct rdk_send_wr {
	struct rdk_send_wr	*next;
	union {
		uint64_t	wr_id;
		struct rdk_cqe	*wr_cqe;
	};
	struct rdk_sge		*sg_list;
	int			num_sge;
	enum rdk_wr_opcode	opcode;
	int			send_flags;
	union {
		uint32_t	imm_data;	/* big endian */
		uint32_t	invalidate_rkey;
	} ex;
};

struct rdk_rdma_wr {
	struct rdk_send_wr	wr;
	uint64_t		remote_addr;
	uint32_t		rkey;
};

struct rdk_ud_wr {
	struct rdk_send_wr	wr;
	struct rdk_ah		*ah;
	uint32_t		remote_qpn;
	uint32_t		remote_qkey;
	uint16_t		pkey_index;
	uint32_t		port_num;
};

struct rdk_reg_wr {
	struct rdk_send_wr	wr;
	struct rdk_mr		*mr;
	uint32_t		key;
	int			access;
};

#define	RDK_RDMA_WR(w)	\
	((const struct rdk_rdma_wr *)(const void *)(w))
#define	RDK_UD_WR(w)	\
	((const struct rdk_ud_wr *)(const void *)(w))
#define	RDK_REG_WR(w)	\
	((const struct rdk_reg_wr *)(const void *)(w))

struct rdk_recv_wr {
	struct rdk_recv_wr	*next;
	union {
		uint64_t	wr_id;
		struct rdk_cqe	*wr_cqe;
	};
	struct rdk_sge		*sg_list;
	int			num_sge;
};

/*
 * Memory registration.
 */
#define	RDK_ACCESS_LOCAL_WRITE		(1 << 0)
#define	RDK_ACCESS_REMOTE_WRITE		(1 << 1)
#define	RDK_ACCESS_REMOTE_READ		(1 << 2)
#define	RDK_ACCESS_REMOTE_ATOMIC	(1 << 3)
#define	RDK_ACCESS_MW_BIND		(1 << 4)
#define	RDK_ACCESS_ZERO_BASED		(1 << 5)

enum rdk_mr_type {
	RDK_MR_TYPE_MEM_REG,
	RDK_MR_TYPE_SG_GAPS,
	RDK_MR_TYPE_DM,
	RDK_MR_TYPE_USER,
	RDK_MR_TYPE_DMA
};

/*
 * The objects.  A provider's own structure embeds each of them; the
 * framework allocates PDs, CQs, QPs and AHs with the sizes in the ops vector
 * and the provider allocates its MRs.
 */
struct rdk_pd {
	struct rdk_device	*device;
	uint32_t		local_dma_lkey;
	uint32_t		flags;
	volatile uint32_t	usecnt;
};

typedef void (*rdk_comp_handler_t)(struct rdk_cq *, void *);

enum rdk_poll_context {
	RDK_POLL_TASKQ,		/* the framework polls from its taskq */
	RDK_POLL_DIRECT		/* the consumer calls rdk_process_cq_direct() */
};

struct rdk_cq {
	struct rdk_device	*device;
	/*
	 * Neither handler may destroy the CQ: destroy waits for them.
	 * comp_handler runs in the provider's thread for the CQ's vector.
	 */
	rdk_comp_handler_t	comp_handler;
	void			(*event_handler)(struct rdk_event *, void *);
	void			*cq_context;
	int			cqe;
	volatile uint32_t	usecnt;
	enum rdk_poll_context	poll_ctx;
	struct rdk_cq_poller	*poller;	/* rdk_alloc_cq() only */
};

struct rdk_qp {
	struct rdk_device	*device;
	struct rdk_pd		*pd;
	struct rdk_cq		*send_cq;
	struct rdk_cq		*recv_cq;
	/* It must not destroy the QP: destroy waits for it to return. */
	void			(*event_handler)(struct rdk_event *, void *);
	void			*qp_context;
	uint32_t		qp_num;
	uint32_t		port;
	enum rdk_qp_type	qp_type;
	const struct rdk_gid_attr *av_sgid_attr;	/* framework */
	void			*drain_orphans;		/* framework */
};

struct rdk_mr {
	struct rdk_device	*device;
	struct rdk_pd		*pd;
	uint32_t		lkey;
	uint32_t		rkey;
	uint64_t		iova;
	uint64_t		length;
	uint32_t		page_size;
	enum rdk_mr_type	type;
};

struct rdk_ah {
	struct rdk_device	*device;
	struct rdk_pd		*pd;
	const struct rdk_gid_attr *sgid_attr;	/* framework */
	enum rdk_ah_attr_type	type;
};

/*
 * DMA memory for data a device reads or writes.  The provider allocates it,
 * and a buffer that is freed while the device might still reach it is held
 * back until the device has been reset.
 */
typedef struct rdk_dma_buf {
	caddr_t		rdb_va;
	uint64_t	rdb_pa;
	size_t		rdb_len;
	void		*rdb_priv;	/* provider */
} rdk_dma_buf_t;

/*
 * The provider interface.  Each operation returns 0 or a positive errno.
 * The destroy operations always release the object; if the device could
 * not confirm it, the provider keeps the memory the device may reach.
 */
struct rdk_device_ops {
	uint32_t	version;	/* RDK_ABI_VERSION */

	int	(*query_device)(struct rdk_device *, struct rdk_device_attr *);
	int	(*query_port)(struct rdk_device *, uint32_t,
	    struct rdk_port_attr *);
	int	(*add_gid)(const struct rdk_gid_attr *);
	void	(*del_gid)(const struct rdk_gid_attr *);

	int	(*alloc_pd)(struct rdk_pd *);
	void	(*dealloc_pd)(struct rdk_pd *);

	int	(*create_cq)(struct rdk_cq *, const struct rdk_cq_init_attr *);
	void	(*destroy_cq)(struct rdk_cq *);
	int	(*poll_cq)(struct rdk_cq *, int, struct rdk_wc *);
	int	(*req_notify_cq)(struct rdk_cq *, enum rdk_cq_notify_flags);

	int	(*create_qp)(struct rdk_qp *, struct rdk_qp_init_attr *);
	int	(*modify_qp)(struct rdk_qp *, struct rdk_qp_attr *, int);
	int	(*query_qp)(struct rdk_qp *, struct rdk_qp_attr *, int,
	    struct rdk_qp_init_attr *);
	void	(*destroy_qp)(struct rdk_qp *);
	int	(*post_send)(struct rdk_qp *, const struct rdk_send_wr *,
	    const struct rdk_send_wr **);
	int	(*post_recv)(struct rdk_qp *, const struct rdk_recv_wr *,
	    const struct rdk_recv_wr **);

	int	(*alloc_mr)(struct rdk_pd *, enum rdk_mr_type, uint32_t,
	    struct rdk_mr **);
	int	(*map_mr_sg)(struct rdk_mr *, const ddi_dma_cookie_t *, uint_t,
	    uint64_t *);
	int	(*dereg_mr)(struct rdk_mr *);

	int	(*create_ah)(struct rdk_ah *, struct rdk_ah_attr *);
	void	(*destroy_ah)(struct rdk_ah *);

	int	(*dma_alloc)(struct rdk_device *, size_t, rdk_dma_buf_t *);
	void	(*dma_free)(struct rdk_device *, rdk_dma_buf_t *);

	/*
	 * Optional: call the CQ's comp_handler again soon from the context
	 * of its completion vector.
	 */
	void	(*cq_resched)(struct rdk_cq *);
	/* Optional: hold the CQ's events up to usec; see rdk_modify_cq(). */
	int	(*modify_cq)(struct rdk_cq *, uint16_t, uint16_t);

	size_t	size_pd;
	size_t	size_cq;
	size_t	size_qp;
	size_t	size_ah;
};

struct rdk_device_priv;

struct rdk_device {
	/* Set by the provider before rdk_register_device(). */
	char				rd_name[RDK_NAME_MAX];
	dev_info_t			*rd_dip;
	const struct rdk_device_ops	*rd_ops;
	uint32_t			rd_phys_port_cnt;
	uint64_t			rd_node_guid;
	/* A CQ's comp_vector is below this; 0 is taken as 1. */
	uint32_t			rd_num_comp_vectors;

	/* Set by the framework. */
	struct rdk_device_attr		rd_attr;
	struct rdk_device_priv		*rd_priv;
};

/*
 * Clients.  add() may fail, and remove() is then not called for that
 * device.  Both run with no framework lock that a verb takes.
 */
struct rdk_client {
	const char	*name;
	int		(*add)(struct rdk_device *);
	void		(*remove)(struct rdk_device *, void *);
	list_node_t	rc_node;	/* framework */
};

/*
 * rdk_device.c
 */
extern int rdk_register_device(struct rdk_device *);
extern void rdk_unregister_device(struct rdk_device *);
extern int rdk_register_client(struct rdk_client *);
extern void rdk_unregister_client(struct rdk_client *);
extern void rdk_set_client_data(struct rdk_device *, struct rdk_client *,
    void *);
extern void *rdk_get_client_data(struct rdk_device *, struct rdk_client *);
extern void rdk_register_event_handler(struct rdk_event_handler *);
extern void rdk_unregister_event_handler(struct rdk_event_handler *);
extern void rdk_dispatch_event(const struct rdk_event *);

extern int rdk_query_port(struct rdk_device *, uint32_t,
    struct rdk_port_attr *);
extern int rdk_add_gid(struct rdk_device *, uint32_t, const rdk_gid_t *,
    uint16_t, const uint8_t *, uint16_t *);
/* Each successful rdk_add_gid() needs its own rdk_del_gid(). */
extern int rdk_del_gid(struct rdk_device *, uint32_t, uint16_t);
extern int rdk_query_gid(struct rdk_device *, uint32_t, uint16_t,
    rdk_gid_t *);
extern const struct rdk_gid_attr *rdk_get_gid_attr(struct rdk_device *,
    uint32_t, uint16_t);
extern void rdk_put_gid_attr(const struct rdk_gid_attr *);
extern void rdk_gid_from_ipv4(rdk_gid_t *, ipaddr_t);
extern boolean_t rdk_gid_to_ipv4(const rdk_gid_t *, ipaddr_t *);

extern int rdk_dma_buf_alloc(struct rdk_device *, size_t, rdk_dma_buf_t *);
extern void rdk_dma_buf_free(struct rdk_device *, rdk_dma_buf_t *);

/*
 * rdk_verbs.c
 */
extern int rdk_alloc_pd(struct rdk_device *, uint32_t, struct rdk_pd **);
extern void rdk_dealloc_pd(struct rdk_pd *);

extern int rdk_create_cq(struct rdk_device *, rdk_comp_handler_t,
    void (*)(struct rdk_event *, void *), void *,
    const struct rdk_cq_init_attr *, struct rdk_cq **);
extern void rdk_destroy_cq(struct rdk_cq *);

extern int rdk_create_qp(struct rdk_pd *, struct rdk_qp_init_attr *,
    struct rdk_qp **);
extern int rdk_modify_qp(struct rdk_qp *, struct rdk_qp_attr *, int);
extern int rdk_query_qp(struct rdk_qp *, struct rdk_qp_attr *, int,
    struct rdk_qp_init_attr *);
extern void rdk_destroy_qp(struct rdk_qp *);
extern boolean_t rdk_modify_qp_is_ok(enum rdk_qp_state, enum rdk_qp_state,
    enum rdk_qp_type, int);
/* A QP's drain and its destroy must not run at the same time. */
extern void rdk_drain_qp(struct rdk_qp *);
extern void rdk_drain_sq(struct rdk_qp *);
extern void rdk_drain_rq(struct rdk_qp *);

extern int rdk_alloc_mr(struct rdk_pd *, enum rdk_mr_type, uint32_t,
    struct rdk_mr **);
/* These two return the number of cookies mapped or a negative errno. */
extern int rdk_map_mr_sg(struct rdk_mr *, const ddi_dma_cookie_t *, uint_t,
    uint64_t *, uint32_t);
extern int rdk_dereg_mr(struct rdk_mr *);
extern int rdk_sg_to_pages(struct rdk_mr *, const ddi_dma_cookie_t *, uint_t,
    uint64_t *, int (*)(struct rdk_mr *, uint64_t));

extern int rdk_create_ah(struct rdk_pd *, struct rdk_ah_attr *,
    struct rdk_ah **);
extern void rdk_destroy_ah(struct rdk_ah *);

extern const char *rdk_wc_status_msg(enum rdk_wc_status);
extern const char *rdk_event_msg(enum rdk_event_type);

/*
 * rdk_cq.c: completion processing for consumers that put a struct rdk_cqe
 * in their work requests.
 */
extern int rdk_alloc_cq(struct rdk_device *, void *, int, int,
    enum rdk_poll_context, struct rdk_cq **);
extern void rdk_free_cq(struct rdk_cq *);
extern int rdk_process_cq_direct(struct rdk_cq *, int);

/*
 * Moderation of a RDK_POLL_TASKQ CQ, with count and usec: a poller run that
 * finds fewer than count completions polls again usec later instead of
 * arming the CQ, so completions in that time raise no interrupt and wait
 * at most usec.  A provider with modify_cq() may also hold the interrupt
 * itself for up to usec.  0 and 0 turn moderation off.
 */
#define	RDK_CQ_MOD_MAX_US	1000
extern int rdk_modify_cq(struct rdk_cq *, uint16_t, uint16_t);

/*
 * Busy polling of a RDK_POLL_TASKQ CQ.  rdk_cq_poll_begin() takes the
 * poller from the provider's thread, or returns B_FALSE if that thread has
 * it; rdk_cq_poll() handles up to budget completions; rdk_cq_poll_end()
 * arms the CQ and gives the poller back.  In between, the CQ raises at most
 * the one interrupt it was armed for.
 */
extern boolean_t rdk_cq_poll_begin(struct rdk_cq *);
extern int rdk_cq_poll(struct rdk_cq *, int);
extern void rdk_cq_poll_end(struct rdk_cq *);

/*
 * The data path goes straight to the provider.
 */
static inline int
rdk_post_send(struct rdk_qp *qp, const struct rdk_send_wr *wr,
    const struct rdk_send_wr **bad)
{
	const struct rdk_send_wr *dummy;

	return (qp->device->rd_ops->post_send(qp, wr,
	    bad != NULL ? bad : &dummy));
}

static inline int
rdk_post_recv(struct rdk_qp *qp, const struct rdk_recv_wr *wr,
    const struct rdk_recv_wr **bad)
{
	const struct rdk_recv_wr *dummy;

	return (qp->device->rd_ops->post_recv(qp, wr,
	    bad != NULL ? bad : &dummy));
}

static inline int
rdk_poll_cq(struct rdk_cq *cq, int n, struct rdk_wc *wc)
{
	return (cq->device->rd_ops->poll_cq(cq, n, wc));
}

/*
 * Returns 1 with RDK_CQ_REPORT_MISSED_EVENTS if completions may have
 * arrived before the CQ was armed.
 */
static inline int
rdk_req_notify_cq(struct rdk_cq *cq, enum rdk_cq_notify_flags flags)
{
	return (cq->device->rd_ops->req_notify_cq(cq, flags));
}

/*
 * The RoCEv2 UDP source port for a flow label, or for the QP pair when the
 * label is 0, so that both ends pick the same one.
 */
static inline uint16_t
rdk_get_udp_sport(uint32_t fl, uint32_t lqpn, uint32_t rqpn)
{
	uint32_t lo, hi;

	if (fl == 0) {
		uint64_t v = (uint64_t)lqpn * rqpn;

		v ^= v >> 20;
		v ^= v >> 40;
		fl = (uint32_t)(v & 0xfffff);
	}
	lo = fl & 0x03fff;
	hi = fl & 0xfc000;
	lo ^= hi >> 14;
	return ((uint16_t)(lo | 0xc000));
}

static inline void
rdk_update_fast_reg_key(struct rdk_mr *mr, uint8_t newkey)
{
	mr->lkey = (mr->lkey & 0xffffff00) | newkey;
	mr->rkey = (mr->rkey & 0xffffff00) | newkey;
}

static inline uint32_t
rdk_inc_rkey(uint32_t rkey)
{
	const uint32_t mask = 0x000000ff;

	return (((rkey + 1) & mask) | (rkey & ~mask));
}

#ifdef __cplusplus
}
#endif

#endif /* _RDK_H */

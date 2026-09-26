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

#ifndef _T4_RDMA_H
#define	_T4_RDMA_H

/*
 * The private interface between t4nex(4D) and the iWARP function driver that
 * attaches as its child node "iwcxgbe@0".  t4nex keeps the PCI function, the
 * firmware mailbox, every SGE queue and the connection (TID) tables; the child
 * reaches them only through the operations below.
 *
 * The child finds the interface with ddi_get_parent_data() on its own dip.
 * The structure there starts with t4_rdma_peer_hdr_t, and the child must
 * refuse to attach unless trp_version equals T4_RDMA_VERSION.
 *
 * t4nex never calls into the child with one of its own locks held.  CPL
 * handlers run in interrupt context and must not block; events run on a
 * taskq.  Every operation other than dma_free and close fails with EIO once
 * the generation returned by open is stale.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ethernet.h>
#include <sys/mac.h>
#include <sys/stream.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	T4_RDMA_VERSION		1

#define	T4_RDMA_MAX_PORTS	4
#define	T4_RDMA_NMTUS		16

/* No hardware TID in a CPL, or no L2T entry. */
#define	T4_RDMA_TID_NONE	UINT32_MAX

typedef struct t4_rdma_peer t4_rdma_peer_t;

typedef enum t4_rdma_event_type {
	T4_RDMA_EV_LINK = 1,
	T4_RDMA_EV_MTU,
	T4_RDMA_EV_RESET_PREP,
	T4_RDMA_EV_RESET_DONE,
	T4_RDMA_EV_FATAL
} t4_rdma_event_type_t;

typedef struct t4_rdma_event {
	t4_rdma_event_type_t	tre_type;
	uint8_t			tre_port;	/* LINK, MTU */
	link_state_t		tre_link;	/* LINK */
	uint64_t		tre_speed;	/* LINK, bits per second */
	uint32_t		tre_mtu;	/* MTU */
} t4_rdma_event_t;

/*
 * The queue a CPL arrived on.  RX carries connection CPLs and their payload;
 * CIQ carries the firmware's RDMA completion queue messages.
 */
typedef enum t4_rdma_queue {
	T4_RDMA_Q_RX = 1,
	T4_RDMA_Q_CIQ
} t4_rdma_queue_t;

/*
 * A CPL for the child.  t4nex has checked that the message is long enough for
 * its opcode and that every ID in it names an entry the child owns; trc_ctx
 * is the context the child bound to that entry.  The handler owns trc_mp,
 * which starts with the CPL.
 */
typedef struct t4_rdma_cpl {
	uint8_t			trc_opcode;
	uint8_t			trc_port;
	t4_rdma_queue_t		trc_queue;
	uint32_t		trc_tid;	/* hardware TID, or TID_NONE */
	uint32_t		trc_ltid;	/* atid or stid, or TID_NONE */
	void			*trc_ctx;
	mblk_t			*trc_mp;
} t4_rdma_cpl_t;

/*
 * trcl_cq is called, in interrupt context, with the ID of an RDMA completion
 * queue that has new entries.  The ID is inside the vres CQ range.
 */
typedef struct t4_rdma_client {
	void	(*trcl_event)(void *, const t4_rdma_event_t *);
	void	(*trcl_cpl)(void *, t4_rdma_cpl_t *);
	void	(*trcl_cq)(void *, uint32_t);
} t4_rdma_client_t;

typedef struct t4_rdma_range {
	uint32_t	trr_start;
	uint32_t	trr_size;
} t4_rdma_range_t;

/* The adapter memory and queue ID ranges the firmware gave RDMA. */
typedef struct t4_rdma_vres {
	t4_rdma_range_t	trv_stag;	/* bytes */
	t4_rdma_range_t	trv_pbl;	/* bytes */
	t4_rdma_range_t	trv_rq;		/* bytes */
	t4_rdma_range_t	trv_qp;		/* egress queue IDs */
	t4_rdma_range_t	trv_cq;		/* ingress queue IDs */
	t4_rdma_range_t	trv_ocq;	/* bytes */
	t4_rdma_range_t	trv_srq;	/* entries */
	uint32_t	trv_max_ordird_qp;
	uint32_t	trv_max_ird_adapter;
	uint32_t	trv_ofldq_wr_cred;	/* per connection, 16B units */
	boolean_t	trv_write_w_imm;
	boolean_t	trv_write_cmpl;
} t4_rdma_vres_t;

typedef struct t4_rdma_port {
	uint8_t		trpo_tx_chan;
	uint8_t		trpo_lport;
	uint16_t	trpo_viid;
	uint8_t		trpo_mac[ETHERADDRL];
	uint32_t	trpo_mtu;
	link_state_t	trpo_link;
	uint64_t	trpo_speed;
} t4_rdma_port_t;

/* What the child needs to run the RDMA function. */
typedef struct t4_rdma_info {
	uint32_t		tri_generation;
	uint32_t		tri_chip;	/* CHELSIO_T5, CHELSIO_T6 */
	uint32_t		tri_pf;
	uint32_t		tri_nports;
	t4_rdma_port_t		tri_port[T4_RDMA_MAX_PORTS];
	t4_rdma_vres_t		tri_vres;
	uint16_t		tri_mtus[T4_RDMA_NMTUS];
	uint16_t		tri_rxq_id;	/* abs ID, connection queue */
	uint16_t		tri_ciq_id;	/* abs ID, CQ event queue */
	uint16_t		tri_ciq_cntxt;	/* context ID, CQ event queue */
	/* BAR2 user doorbell region, for the queues the child creates. */
	caddr_t			tri_bar2;
	ddi_acc_handle_t	tri_bar2_handle;
	uint32_t		tri_sge_page_shift;
	uint32_t		tri_eq_qpp_shift;
	uint32_t		tri_iq_qpp_shift;
	boolean_t		tri_write_combine;
} t4_rdma_info_t;

typedef struct t4_rdma_listen {
	uint8_t		trl_port;
	sa_family_t	trl_family;
	in6_addr_t	trl_laddr;	/* IPv4 as a mapped address */
	in_port_t	trl_lport;	/* network order */
	uint32_t	trl_stid;
} t4_rdma_listen_t;

/* TCP options for an active or passive open. */
typedef struct t4_rdma_tcp_opts {
	uint32_t	trt_rcv_win;	/* bytes */
	uint8_t		trt_mtu_idx;
	boolean_t	trt_timestamps;
	boolean_t	trt_sack;
	boolean_t	trt_ecn;
	uint8_t		trt_ulp_mode;	/* ULP_MODE_NONE or ULP_MODE_TCPDDP */
	uint8_t		trt_tos;
} t4_rdma_tcp_opts_t;

typedef struct t4_rdma_act_open {
	uint8_t			trao_port;
	sa_family_t		trao_family;
	in6_addr_t		trao_laddr;
	in6_addr_t		trao_faddr;
	in_port_t		trao_lport;	/* network order */
	in_port_t		trao_fport;	/* network order */
	uint32_t		trao_atid;
	uint32_t		trao_l2t;
	t4_rdma_tcp_opts_t	trao_opts;
} t4_rdma_act_open_t;

typedef struct t4_rdma_accept {
	uint32_t		trac_tid;
	uint8_t			trac_port;
	uint32_t		trac_l2t;
	t4_rdma_tcp_opts_t	trac_opts;
} t4_rdma_accept_t;

/* The per connection state the firmware needs before any other work. */
typedef struct t4_rdma_flowc {
	uint32_t	trf_snd_nxt;
	uint32_t	trf_rcv_nxt;
	uint32_t	trf_sndbuf;
	uint16_t	trf_mss;
	uint8_t		trf_rcv_scale;
} t4_rdma_flowc_t;

/*
 * DMA memory t4nex allocates for the child: zeroed, physically contiguous and
 * aligned as asked.  A buffer freed while the device may still write to it is
 * kept by t4nex until the adapter is quiesced.
 */
typedef struct t4_rdma_dma {
	caddr_t		trd_va;
	uint64_t	trd_pa;
	size_t		trd_len;
} t4_rdma_dma_t;

typedef struct t4_rdma_ops {
	int	(*tro_open)(t4_rdma_peer_t *, const t4_rdma_client_t *,
	    void *, t4_rdma_info_t *);
	void	(*tro_close)(t4_rdma_peer_t *);

	int	(*tro_atid_alloc)(t4_rdma_peer_t *, void *, uint32_t *);
	void	(*tro_atid_free)(t4_rdma_peer_t *, uint32_t);
	int	(*tro_stid_alloc)(t4_rdma_peer_t *, sa_family_t, void *,
	    uint32_t *);
	void	(*tro_stid_free)(t4_rdma_peer_t *, uint32_t);
	int	(*tro_tid_bind)(t4_rdma_peer_t *, uint32_t, void *);
	int	(*tro_tid_release)(t4_rdma_peer_t *, uint32_t);

	int	(*tro_l2t_get)(t4_rdma_peer_t *, uint8_t, uint16_t,
	    const uint8_t *, uint32_t *);
	void	(*tro_l2t_put)(t4_rdma_peer_t *, uint32_t);
	int	(*tro_clip_get)(t4_rdma_peer_t *, const in6_addr_t *);
	void	(*tro_clip_put)(t4_rdma_peer_t *, const in6_addr_t *);

	int	(*tro_listen)(t4_rdma_peer_t *, const t4_rdma_listen_t *);
	int	(*tro_unlisten)(t4_rdma_peer_t *, uint32_t);
	int	(*tro_act_open)(t4_rdma_peer_t *, const t4_rdma_act_open_t *);
	int	(*tro_accept)(t4_rdma_peer_t *, const t4_rdma_accept_t *);
	int	(*tro_flowc)(t4_rdma_peer_t *, uint32_t,
	    const t4_rdma_flowc_t *);
	int	(*tro_close_con)(t4_rdma_peer_t *, uint32_t);
	int	(*tro_abort)(t4_rdma_peer_t *, uint32_t, boolean_t);
	int	(*tro_abort_rpl)(t4_rdma_peer_t *, uint32_t, boolean_t);
	int	(*tro_set_tcb_field)(t4_rdma_peer_t *, uint32_t, uint16_t,
	    uint64_t, uint64_t);
	int	(*tro_rx_credits)(t4_rdma_peer_t *, uint32_t, uint32_t);
	int	(*tro_tx_data)(t4_rdma_peer_t *, uint32_t, const void *,
	    size_t);

	int	(*tro_tpt_write)(t4_rdma_peer_t *, uint32_t, const void *,
	    size_t);
	int	(*tro_dma_alloc)(t4_rdma_peer_t *, size_t, size_t,
	    t4_rdma_dma_t **);
	void	(*tro_dma_free)(t4_rdma_peer_t *, t4_rdma_dma_t *, boolean_t);
	int	(*tro_reset)(t4_rdma_peer_t *);
	boolean_t (*tro_stopped)(t4_rdma_peer_t *);
} t4_rdma_ops_t;

typedef struct t4_rdma_peer_hdr {
	uint32_t		trp_version;
	const t4_rdma_ops_t	*trp_ops;
} t4_rdma_peer_hdr_t;

#ifdef __cplusplus
}
#endif

#endif /* _T4_RDMA_H */

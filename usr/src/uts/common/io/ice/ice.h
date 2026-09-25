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

#ifndef _ICE_H
#define	_ICE_H

#include <sys/types.h>
#include <sys/inttypes.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/debug.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/pci.h>
#include <sys/list.h>
#include <sys/ethernet.h>
#include <sys/mac_provider.h>
#include <sys/mac_ether.h>
#include <sys/ddifm.h>
#include <sys/fm/protocol.h>
#include <sys/fm/util.h>
#include <sys/fm/io/ddi.h>

#include "ice_osdep.h"
#include "ice_type.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	ICE_MODULE_NAME		"ice"

/*
 * reg property index 0 is PCI configuration space; the CSR window (BAR0) is
 * index 1.
 */
#define	ICE_REG_NUMBER		1

/*
 * MSI-X vector 0 is reserved for the "other interrupt cause" (admin queue,
 * link events, and errors); queue vectors begin at 1.
 */
#define	ICE_INTR_MSIX_MIN	2
CTASSERT(ICE_INTR_MSIX_MIN == 2);

/*
 * Ceiling and default of the num_queues property; ice_queue_limit() applies
 * the CPU, firmware and vector limits.  MAC keeps one SRS per rx ring plus
 * one for software classification in an array of MAX_RINGS_PER_GROUP
 * entries, and an RSS LUT entry is one byte.
 */
#define	ICE_MAX_QUEUES		(MAX_RINGS_PER_GROUP - 1)
#define	ICE_DEF_QUEUES		16
#define	ICE_RSS_LUT_MAX_QUEUES	256
CTASSERT(ICE_MAX_QUEUES <= ICE_RSS_LUT_MAX_QUEUES);
CTASSERT(ICE_DEF_QUEUES <= ICE_MAX_QUEUES);

/*
 * Generous hardware sanity ceilings.  Capability counts arrive from firmware
 * over the admin queue; a value beyond these bounds indicates a corrupt or
 * hostile device and is rejected outright (not clamped).  The driver's smaller
 * usable limits are applied where the counts are actually consumed.
 */
#define	ICE_HW_MAX_RXQ		2048
#define	ICE_HW_MAX_TXQ		2048
#define	ICE_HW_MAX_MSIX		2048
#define	ICE_MAX_VSI		768
#define	ICE_MIN_MTU		68		/* conventional minimum MTU */
#define	ICE_MAX_MTU	\
	(ICE_AQ_SET_MAC_FRAME_SIZE_MAX - sizeof (struct ether_vlan_header) - \
	ETHERFCSL)
#define	ICE_MAX_FRAME_SIZE	ICE_AQ_SET_MAC_FRAME_SIZE_MAX
#define	ICE_DEFAULT_MTU		ETHERMTU
#define	ICE_MAX_FUNCS		8

CTASSERT(ICE_MAX_MTU + sizeof (struct ether_vlan_header) + ETHERFCSL ==
    ICE_MAX_FRAME_SIZE);
CTASSERT(ICE_MAX_FRAME_SIZE <= UINT16_MAX);

/*
 * Every link event the Set Event Mask command (0x0613) defines.  The datasheet
 * assigns bits 1 through 12 and requires the rest of the mask to be zero, so a
 * complement taken to build a mask has to be confined to these.
 */
#define	ICE_AQ_LINK_EVENT_MASK_DEFINED					\
	(ICE_AQ_LINK_EVENT_UPDOWN | ICE_AQ_LINK_EVENT_MEDIA_NA |	\
	ICE_AQ_LINK_EVENT_LINK_FAULT | ICE_AQ_LINK_EVENT_PHY_TEMP_ALARM | \
	ICE_AQ_LINK_EVENT_EXCESSIVE_ERRORS |				\
	ICE_AQ_LINK_EVENT_SIGNAL_DETECT | ICE_AQ_LINK_EVENT_AN_COMPLETED | \
	ICE_AQ_LINK_EVENT_MODULE_QUAL_FAIL |				\
	ICE_AQ_LINK_EVENT_PORT_TX_SUSPENDED |				\
	ICE_AQ_LINK_EVENT_TOPO_CONFLICT | ICE_AQ_LINK_EVENT_MEDIA_CONFLICT | \
	ICE_AQ_LINK_EVENT_PHY_FW_LOAD_FAIL)

/*
 * E830 moved the TCLAN malicious-driver detection registers.  The imported
 * core predates the move.  The offsets match E830_GL_MDET_TX_TCLAN and
 * E830_PF_MDET_TX_TCLAN in upstream Linux and FreeBSD.
 */
#define	ICE_E830_GL_MDET_TX_TCLAN	0x000FCCC0
#define	ICE_E830_PF_MDET_TX_TCLAN	0x000FCC00
#define	ICE_GL_MDET_TX_TCLAN(hw)	\
	(ice_is_e830(hw) ? ICE_E830_GL_MDET_TX_TCLAN : GL_MDET_TX_TCLAN)
#define	ICE_PF_MDET_TX_TCLAN(hw)	\
	(ice_is_e830(hw) ? ICE_E830_PF_MDET_TX_TCLAN : PF_MDET_TX_TCLAN)

/* Standard netlb(4I) modes supported by ice_m_ioctl(). */
#define	ICE_LB_NONE		0
#define	ICE_LB_INTERNAL_MAC	1

/*
 * Datapath constants.
 */
#define	ICE_DESC_ALIGN		128		/* descriptor ring base align */
#define	ICE_DMA_ALIGNMENT	0x1000		/* packet buffer alignment */
#define	ICE_TX_MAX_BUFSZ	0x00003fff	/* per-descriptor max (16K-1) */
#define	ICE_TX_MAX_COOKIE	8		/* descs per non-LSO packet */
#define	ICE_TX_LSO_MAX_COOKIE	32		/* descs per LSO packet */
#define	ICE_TX_MAX_LSO_DESC	32		/* data descs per LSO packet */
/* The refetched header is the eighth descriptor in each hardware segment. */
#define	ICE_TX_LSO_SEG_DESCS	7		/* payload descs per segment */
/*
 * Datasheet 10.5.8.4.4: the device treats an MSS below 88 bytes as a
 * malicious-driver event and stops the queue.
 */
#define	ICE_TX_LSO_MIN_MSS	88
/* Datasheet 10.5.8.4.1: TSO header (L2+L3+L4) maximum, in bytes. */
#define	ICE_TX_LSO_MAX_HDRLEN	512
#define	ICE_LSO_MAXLEN		(64 * 1024)
#define	ICE_TX_LSO_BUFSZ	P2ROUNDUP(ICE_MAX_FRAME_SIZE, PAGESIZE)
/*
 * The general tx copy pool must hold any MTU-legal frame in one buffer: a
 * frame that failed to bind has nowhere else to go and would be dropped.
 */
#define	ICE_TX_COPY_BUFSZ	P2ROUNDUP(ICE_MAX_FRAME_SIZE, PAGESIZE)
#define	ICE_TX_SMALL_PKT	512		/* small-copy threshold */

/*
 * TX copy-buffer pools, per ring.  A ring takes the per-ring count unless that
 * would put the instance above the cap.  The LSO cap must leave every ring at
 * ICE_MAX_QUEUES enough buffers for its largest packet.
 */
#define	ICE_TX_COPY_BUFS_RING	64
#define	ICE_TX_COPY_BUFS_MAX	2048
#define	ICE_TX_SMALL_BUFS_RING	1024
#define	ICE_TX_SMALL_BUFS_MAX	16384
#define	ICE_TX_LSO_BUFS_RING	128
#define	ICE_TX_LSO_BUFS_MAX	4096
/* Page alignment would give each small buffer a page of its own. */
#define	ICE_TX_SMALL_ALIGN	128
/* GLCOMM_MIN_MAX_PKT.MIHDL reset value; a shorter frame is a TCLAN MDD. */
#define	ICE_TX_MIN_LEN		17
/* Descriptors written before a sender rings the doorbell mid-chain. */
#define	ICE_TX_DOORBELL_BATCH	32

CTASSERT(sizeof (struct ice_tx_ctx_desc) == sizeof (struct ice_tx_desc));
/* Runts take the copy path; the pad fits the smallest pool buffer. */
CTASSERT(ICE_TX_MIN_LEN <= ICE_TX_SMALL_PKT);
/*
 * ICE_TX_COPY_BUFSZ is page-rounded, so it is >= ICE_MAX_FRAME_SIZE by
 * construction and is not a constant expression a CTASSERT can read.  Assert
 * the part that matters and is constant: a whole frame still fits the length
 * field of a single tx data descriptor.
 */
CTASSERT(ICE_MAX_FRAME_SIZE <= ICE_TX_MAX_BUFSZ);
CTASSERT(ICE_LSO_MAXLEN <= (ICE_TXD_CTX_QW1_TSO_LEN_M >>
    ICE_TXD_CTX_QW1_TSO_LEN_S));
CTASSERT(ICE_TXD_CTX_MAX_MSS <= (ICE_TXD_CTX_QW1_MSS_M >>
    ICE_TXD_CTX_QW1_MSS_S));
/* An LSO header takes a small buffer, its payload this many LSO buffers. */
CTASSERT(ICE_TX_LSO_MAX_HDRLEN <= ICE_TX_SMALL_PKT);
CTASSERT(ICE_TX_LSO_BUFS_MAX / ICE_MAX_QUEUES >=
    howmany(ICE_LSO_MAXLEN, ICE_MAX_FRAME_SIZE));
CTASSERT(ICE_TX_COPY_BUFS_MAX / ICE_MAX_QUEUES >= 1);
CTASSERT(ICE_TX_SMALL_BUFS_MAX / ICE_MAX_QUEUES >= 1);
CTASSERT(ISP2(ICE_TX_SMALL_ALIGN) && ICE_TX_SMALL_ALIGN < ICE_TX_SMALL_PKT);

#define	ICE_DEF_TX_RING_SIZE	1024
#define	ICE_DEF_RX_RING_SIZE	1024
#define	ICE_MIN_RING_SIZE	64
#define	ICE_MAX_RING_SIZE	4096
#define	ICE_RX_BUF_SIZE		2048		/* posted rx data buffer */
/*
 * Spare rx buffers per ring beyond the posted ones, which bounds the frames
 * loaned up the stack at once; within a cap per instance.
 */
#define	ICE_RX_LOAN_RESERVE	1024
#define	ICE_RX_LOAN_RESERVE_MAX	16384
CTASSERT(ICE_RX_LOAN_RESERVE_MAX / ICE_MAX_QUEUES >= 1);
/*
 * Loans of replaced pools an instance may hold.  A restart adds at most every
 * ring's reserve, so the rings copy every frame instead of loaning from that
 * far below the limit, and loan again below half that point.
 */
#define	ICE_RX_ORPHAN_MAX	(3 * ICE_RX_LOAN_RESERVE_MAX / 2)
CTASSERT(ICE_RX_ORPHAN_MAX > ICE_RX_LOAN_RESERVE_MAX);
/* ceil(ICE_AQ_SET_MAC_FRAME_SIZE_MAX / ICE_RX_BUF_SIZE) */
#define	ICE_RX_MAX_DESC		5

#define	ICE_ITR_IDX_0		0		/* ITR slot for queue vectors */
#define	ICE_ITR_INDEX_NONE	3		/* "do not update the ITR" */
#define	ICE_ITR_DEFAULT_US	50		/* rx interrupt throttle */
#define	ICE_Q_ENA_MAX_WAIT	50		/* QENA_STAT poll, 20us each */

/*
 * Frames consumed per rx ring per interrupt invocation before the drain
 * yields the ring lock and interrupt context.  Residue is serviced by an
 * ITR-paced software interrupt (ice_intr.c); the values match i40e's
 * rx_limit_per_intr property.
 */
#define	ICE_DEF_RX_LIMIT_PER_INTR	256
#define	ICE_MIN_RX_LIMIT_PER_INTR	16
#define	ICE_MAX_RX_LIMIT_PER_INTR	4096

/*
 * The standard vector re-arm word: enable, clear the pending bit, and leave
 * the programmed ITR intervals alone (a real ITR index in this write would
 * reload that slot's interval from the write's zero interval field).  One
 * definition so every re-arm site stays in lockstep.
 */
#define	ICE_GLINT_DYN_CTL_REARM						\
	(GLINT_DYN_CTL_INTENA_M | GLINT_DYN_CTL_CLEARPBA_M |		\
	((ICE_ITR_INDEX_NONE << GLINT_DYN_CTL_ITR_INDX_S) &		\
	GLINT_DYN_CTL_ITR_INDX_M))

typedef enum ice_state {
	ICE_STATE_ATTACHED	= 1 << 0,
	ICE_STATE_RESET_PENDING	= 1 << 1,	/* GRST seen; rebuild owed */
	ICE_STATE_ERROR		= 1 << 2,	/* datapath fail-closed */
	ICE_STATE_STARTED	= 1 << 3,
	ICE_STATE_PFR_REQ	= 1 << 4,	/* fatal cause; PFR owed */
	ICE_STATE_RESET_FAILED	= 1 << 5,	/* rebuild failed; terminal */
	ICE_STATE_MDD_PENDING	= 1 << 6	/* MDD latched; decode owed */
} ice_state_t;

/* ice_lse_flags bits (protected by ice_lse_lock). */
#define	ICE_LSE_F_UPDATING	(1 << 1)	/* an update is in flight */

/*
 * Attach progress.  Each completed attach step records its bit; teardown
 * reverses only the steps that completed.
 */
typedef enum ice_attach_state {
	ICE_ATTACH_FM_INIT	= 1 << 0,
	ICE_ATTACH_PCI_CONFIG	= 1 << 1,
	ICE_ATTACH_REGS_MAP	= 1 << 2,
	ICE_ATTACH_HW_INIT	= 1 << 3,
	ICE_ATTACH_DDP		= 1 << 4,	/* DDP loaded or safe mode */
	ICE_ATTACH_ALLOC_INTR	= 1 << 5,
	ICE_ATTACH_ADD_INTR	= 1 << 6,
	ICE_ATTACH_OICR_TASKQ	= 1 << 7,
	ICE_ATTACH_RESET_TASKQ	= 1 << 8,	/* reset taskq */
	ICE_ATTACH_ENABLE_INTR	= 1 << 9,
	ICE_ATTACH_VSI		= 1 << 10,
	ICE_ATTACH_RINGS	= 1 << 11,	/* ring DMA allocated */
	ICE_ATTACH_QUEUE_INTR	= 1 << 12,	/* queue->vector wired */
	ICE_ATTACH_BUFS		= 1 << 13,	/* tx copy-buffer pools */
	ICE_ATTACH_STATS	= 1 << 14,	/* hardware stat kstats */
	ICE_ATTACH_MAC		= 1 << 15	/* mac_register done */
} ice_attach_state_t;

/* The driver-chosen software handle for the single PF data VSI. */
#define	ICE_PF_VSI_HANDLE	0

/*
 * The PF data VSI.  vi_handle is the driver-chosen software index into
 * hw->vsi_ctx[]; vi_hw_num is the firmware-assigned hardware VSI number,
 * stored only after it is range-checked.  vi_nrxq/vi_ntxq record the queue
 * counts this VSI was configured with.
 */
typedef struct ice_vsi {
	boolean_t		vi_added;
	uint16_t		vi_handle;
	uint16_t		vi_hw_num;
	uint16_t		vi_nrxq;
	uint16_t		vi_ntxq;

	/* Owned by ice_filter.c; other modules use the filter interface. */
	kmutex_t		vi_mac_lock;
	list_t			vi_macs;	/* for teardown */

	uint16_t		vi_max_frame;	/* posted rx frame size */
} ice_vsi_t;

/*
 * A single DMA allocation: descriptor ring or packet buffer.
 */
typedef struct ice_dma_buffer {
	caddr_t			idb_va;
	size_t			idb_len;
	ddi_acc_handle_t	idb_acc_handle;
	ddi_dma_handle_t	idb_dma_handle;
	uint_t			idb_ncookies;
	ddi_dma_cookie_t	idb_cookie;	/* single cookie (sgllen 1) */
	struct ice_buf_pool	*idb_pool;	/* TX pool, or NULL */
} ice_dma_buffer_t;

/* Shared stack implementation for each fixed-size TX copy-buffer pool. */
typedef struct ice_buf_pool {
	kmutex_t		*ibp_lock;
	ice_dma_buffer_t	*ibp_bufs;	/* backing allocation */
	ice_dma_buffer_t	**ibp_free;	/* free stack */
	uint_t			ibp_size;	/* backing array capacity */
	uint_t			ibp_nbufs;	/* initialized DMA buffers */
	uint_t			ibp_nfree;	/* free stack entries */
} ice_buf_pool_t;

#define	ICE_DMA_PA(idb)		((idb)->idb_cookie.dmac_laddress)

typedef enum ice_tcb_type {
	ITCB_NOT_USED,
	ITCB_SMALL_COPY,
	ITCB_COPY,
	ITCB_LSO_COPY,
	ITCB_BIND,
	ITCB_LSO_BIND
} ice_tcb_type_t;

/* Why ice_tx_context() refused an offload request; see the tx kstats. */
typedef enum ice_tx_hck_drop {
	ICE_TX_HCK_NONE = 0,
	ICE_TX_HCK_HDRLEN,	/* a header length overflows its field */
	ICE_TX_HCK_NOL3,	/* no usable L2/L3 metadata */
	ICE_TX_HCK_NOL4,	/* no usable L4 metadata */
	ICE_TX_HCK_BADL4,	/* L4 protocol the hardware cannot sum */
	ICE_TX_LSO_BADHDR,	/* LSO headers unusable for segmentation */
	ICE_TX_LSO_BADMSS	/* MSS outside the hardware range */
} ice_tx_hck_drop_t;

typedef struct ice_tx_ctx_t {
	uint64_t		itc_data_cmd;
	uint64_t		itc_data_off;
	boolean_t		itc_use_ctx;
	uint32_t		itc_mss;
	uint32_t		itc_tsolen;
	uint32_t		itc_hdrlen;	/* L2+L3+L4, LSO only */
	ice_tx_hck_drop_t	itc_drop;	/* reason for a DROP result */
} ice_tx_ctx_t;

typedef struct ice_tx_ctrl_block {
	struct ice_tx_ctrl_block *itcb_next;	/* completed, to release */
	ice_tcb_type_t		itcb_type;
	uint32_t		itcb_len;
	ice_dma_buffer_t	*itcb_buf;	/* copy buffer (pool) */
	mblk_t			*itcb_mp;
	ddi_dma_handle_t	itcb_dmah;
	ddi_dma_handle_t	itcb_lso_dmah;
} ice_tx_ctrl_block_t;

typedef struct ice_txq_stat {
	kstat_named_t		ictxs_bytes;
	kstat_named_t		ictxs_packets;
	kstat_named_t		ictxs_bind_bytes;
	kstat_named_t		ictxs_bind_frags;
	kstat_named_t		ictxs_copy_bytes;
	kstat_named_t		ictxs_copy_frags;
	kstat_named_t		ictxs_bind_fails;
	kstat_named_t		ictxs_no_pkt_cache;
	kstat_named_t		ictxs_drops;
	kstat_named_t		ictxs_oversize_drops;
	kstat_named_t		ictxs_blocked;
	kstat_named_t		ictxs_lso_packets;
	kstat_named_t		ictxs_lso_drops;
	kstat_named_t		ictxs_lso_pullups;
	kstat_named_t		ictxs_lso_nores;
	kstat_named_t		ictxs_hck_hdrlen;
	kstat_named_t		ictxs_hck_nol3;
	kstat_named_t		ictxs_hck_nol4;
	kstat_named_t		ictxs_hck_badl4;
	kstat_named_t		ictxs_lso_nohck;
	kstat_named_t		ictxs_lso_badhdr;
	kstat_named_t		ictxs_lso_badmss;
} ice_txq_stat_t;

typedef struct ice_tx_ring {
	struct ice		*itxr_ice;	/* RO */
	uint32_t		itxr_index;	/* absolute HW tx queue index */
	uint32_t		itxr_vec;	/* MSI-X vector index */
	/* These two are guarded by ice_rebuild_lock. */
	uint32_t		itxr_q_teid;	/* core: from ice_ena_vsi_txq */
	/*
	 * Set before the Add Tx Queues command and cleared only by a confirmed
	 * disable, so a queue the command enabled before a later step of
	 * ice_ena_vsi_txq() failed is still treated as live.
	 */
	boolean_t		itxr_programmed;

	kmutex_t		itxr_lock;
	kcondvar_t		itxr_cv;	/* stop waits for tx drain */
	boolean_t		itxr_quiesce;
	boolean_t		itxr_blocked;
	uint_t			itxr_tx_active;	/* in-flight tx calls */

	mac_ring_handle_t	itxr_mactxring;

	ice_dma_buffer_t	itxr_dma;	/* descriptor ring */
	struct ice_tx_desc	*itxr_descs;
	uint16_t		itxr_size;	/* descriptor count */
	uint16_t		itxr_avail;
	uint16_t		itxr_head;
	uint16_t		itxr_tail;
	uint16_t		itxr_unposted;	/* written since the doorbell */
	/* Slot of each in-flight packet's RS descriptor, in transmit order. */
	uint16_t		*itxr_rsq;	/* [itxr_size] */
	uint16_t		itxr_rs_pidx;
	uint16_t		itxr_rs_cidx;

	ice_tx_ctrl_block_t	*itxr_tcb_area;	/* [itxr_size] backing */
	ice_tx_ctrl_block_t	**itxr_tcbs;	/* [itxr_size], by slot */
	kmutex_t		itxr_tcb_lock;
	ice_tx_ctrl_block_t	**itxr_tcb_free_list;
	uint16_t		itxr_tcb_nfree;

	/*
	 * Copy-buffer pools; itxr_tcb_lock guards their free stacks.  With
	 * LSO enabled, the LSO pool and the TCBs' LSO bind handles exist from
	 * MAC start until MAC stop.
	 */
	ice_buf_pool_t		itxr_copy_pool;
	ice_buf_pool_t		itxr_small_pool;
	ice_buf_pool_t		itxr_lso_pool;

	kstat_t			*itxr_kstat;
	ice_txq_stat_t		itxr_stats;
} ice_tx_ring_t;

typedef enum ice_rcb_state {
	IRXB_FREE,
	IRXB_ONRING,
	IRXB_ONLOAN
} ice_rcb_state_t;

struct ice_rx_ring;
struct ice_rx_pool;

typedef struct ice_rx_ctrl_block {
	struct ice_rx_ctrl_block *ircb_next;	/* returned, to reap */
	mblk_t			*ircb_mp;
	struct ice_rx_ring	*ircb_ring;
	struct ice_rx_pool	*ircb_pool;	/* owner; set at allocation */
	ice_dma_buffer_t	ircb_dma;
	frtn_t			ircb_free_rtn;
	ice_rcb_state_t		ircb_state;
} ice_rx_ctrl_block_t;

/*
 * A ring's control-block pool.  It is built and freed without irxr_lock and
 * only exchanged under it; while a ring holds it, the ring's irxr_* fields
 * carry its free stack and counts.  Each block is on the free stack, in a
 * slot, or out on loan.  A pool replaced while loans were out keeps only those
 * blocks, each reaped once it returns, and the last reference frees the pool.
 */
typedef struct ice_rx_pool {
	ice_rx_ctrl_block_t	**irp_free;	/* [irp_nrcb] free stack */
	ice_rx_ctrl_block_t	**irp_slots;	/* [irp_size] posted, by slot */
	uint_t			irp_nrcb;
	uint_t			irp_nfree;
	uint_t			irp_nreserve;
	uint_t			irp_nloaned;
	uint16_t		irp_size;
	uint32_t		irp_refs;	/* replaced: loans + sweep */
} ice_rx_pool_t;

typedef struct ice_rxq_stat {
	kstat_named_t		icrxs_bytes;
	kstat_named_t		icrxs_packets;
	kstat_named_t		icrxs_bind_bytes;
	kstat_named_t		icrxs_bind_segs;
	kstat_named_t		icrxs_copy_bytes;
	kstat_named_t		icrxs_copy_segs;
	kstat_named_t		icrxs_desc_error;
	kstat_named_t		icrxs_copy_nomem;
	kstat_named_t		icrxs_no_rcb;
	kstat_named_t		icrxs_intr_limit;
	kstat_named_t		icrxs_orphan_pools;
	kstat_named_t		icrxs_orphan_loans;	/* still out */
	kstat_named_t		icrxs_copy_mode_enter;
	kstat_named_t		icrxs_copy_mode_exit;
	kstat_named_t		icrxs_copy_mode_segs;
	/* Receive checksum verdicts, one per delivered frame and layer. */
	kstat_named_t		icrxs_hck_v4hdr_ok;
	kstat_named_t		icrxs_hck_v4hdr_err;
	kstat_named_t		icrxs_hck_outer_err;
	kstat_named_t		icrxs_hck_l4_ok;
	kstat_named_t		icrxs_hck_l4_err;
	kstat_named_t		icrxs_hck_v6exthdr;
	kstat_named_t		icrxs_hck_nol4;
	kstat_named_t		icrxs_hck_unprocessed;
	kstat_named_t		icrxs_hck_unknown;
} ice_rxq_stat_t;

typedef struct ice_rx_ring {
	struct ice		*irxr_ice;	/* RO */
	uint32_t		irxr_index;	/* absolute HW rx queue index */
	uint32_t		irxr_vec;	/* MSI-X vector index */
	boolean_t		irxr_shutdown;
	boolean_t		irxr_started;	/* irxr_lock */
	boolean_t		irxr_intr_poll;	/* mac is polling this ring */
	/* QINT_RQCTL vector programmed */
	boolean_t		irxr_intr_routed;
	/* The lifecycle permits CAUSE_ENA. */
	boolean_t		irxr_intr_armed;
	boolean_t		irxr_intr_busy;	/* ISR is in mac_rx_ring */
	boolean_t		irxr_copy_only;	/* no loans; irxr_lock */
	uint32_t		irxr_intr_limit; /* frames per interrupt */

	kmutex_t		irxr_lock;
	kcondvar_t		irxr_cv;	/* teardown waits on loans */
	kcondvar_t		irxr_intr_cv;	/* stop waits on the ISR */
	mac_ring_handle_t	irxr_macrxring;
	uint64_t		irxr_rxgen;

	ice_dma_buffer_t	irxr_desc_dma;	/* descriptor ring */
	union ice_32b_rx_flex_desc *irxr_descs;
	ice_rx_ctrl_block_t	**irxr_rcbs;	/* [irxr_size], pool's slots */
	uint16_t		irxr_size;	/* descriptor count */
	uint16_t		irxr_head;
	uint16_t		irxr_tail;
	uint32_t		irxr_dbuf;	/* posted data buffer size */

	/* The current pool and its free stack for loaned buffers. */
	ice_rx_pool_t		*irxr_pool;
	ice_rx_ctrl_block_t	**irxr_free_rcbs;
	uint_t			irxr_nrcb;	/* size + reserve */
	uint_t			irxr_nfree;
	uint_t			irxr_nreserve;	/* loan high-water */
	uint_t			irxr_nloaned;	/* outstanding loans */

	/*
	 * Loans that ice_rx_recycle() gave back without irxr_lock; they count
	 * as outstanding until ice_rx_harvest() takes them.  The pads keep
	 * the returning CPUs off the cache lines the drain writes.
	 */
	uint64_t		irxr_returned_pad0[7];
	ice_rx_ctrl_block_t	*volatile irxr_returned;
	uint64_t		irxr_returned_pad1[7];

	kstat_t			*irxr_kstat;
	ice_rxq_stat_t		irxr_stats;
} ice_rx_ring_t;

/*
 * Physical-port hardware statistics, read from the GLPRT_* MAC counters.  The
 * counters are per logical port and therefore aggregate every VSI and VF on
 * the function, so they describe the wire rather than this interface.
 */
typedef struct ice_pf_kstats {
	kstat_named_t		ipk_rx_bytes;
	kstat_named_t		ipk_rx_unicast;
	kstat_named_t		ipk_rx_multicast;
	kstat_named_t		ipk_rx_broadcast;
	kstat_named_t		ipk_tx_bytes;
	kstat_named_t		ipk_tx_unicast;
	kstat_named_t		ipk_tx_multicast;
	kstat_named_t		ipk_tx_broadcast;
	kstat_named_t		ipk_crc_errors;
	kstat_named_t		ipk_illegal_bytes;
	kstat_named_t		ipk_mac_local_faults;
	kstat_named_t		ipk_mac_remote_faults;
	kstat_named_t		ipk_rx_len_errors;
	kstat_named_t		ipk_rx_undersize;
	kstat_named_t		ipk_rx_fragments;
	kstat_named_t		ipk_rx_oversize;
	kstat_named_t		ipk_rx_jabber;
	kstat_named_t		ipk_tx_dropped_link_down;
	kstat_named_t		ipk_link_xon_rx;
	kstat_named_t		ipk_link_xoff_rx;
	kstat_named_t		ipk_link_xon_tx;
	kstat_named_t		ipk_link_xoff_tx;
} ice_pf_kstats_t;

/*
 * Per-VSI hardware statistics, read from the GLV_* counters for this
 * interface's VSI.  Unlike the port counters these isolate traffic actually
 * switched to this VSI, so a receive that advances the port counters but not
 * these localizes a fault to the switch or VSI configuration.
 */
typedef struct ice_vsi_kstats {
	kstat_named_t		ivk_rx_bytes;
	kstat_named_t		ivk_rx_unicast;
	kstat_named_t		ivk_rx_multicast;
	kstat_named_t		ivk_rx_broadcast;
	kstat_named_t		ivk_rx_discards;
	kstat_named_t		ivk_rx_no_desc;
	kstat_named_t		ivk_rx_errors;
	kstat_named_t		ivk_tx_bytes;
	kstat_named_t		ivk_tx_unicast;
	kstat_named_t		ivk_tx_multicast;
	kstat_named_t		ivk_tx_broadcast;
	kstat_named_t		ivk_tx_errors;
} ice_vsi_kstats_t;

typedef struct ice {
	dev_info_t		*ice_dip;
	int			ice_instance;

	/*
	 * Mutated with atomic_*_32 rather than under ice_lock because it will
	 * be read locklessly from interrupt and MAC-callback context once the
	 * data path exists.
	 */
	uint32_t		ice_state;
	ice_attach_state_t	ice_attach_progress;

	kmutex_t		ice_lock;

	int			ice_fm_caps;
	/* Error-only atomic accounting protects detach's MMIO polling proof. */
	uint32_t		ice_acc_errors;
	uint32_t		ice_acc_clears;

	/*
	 * Intel common code, embedded inline.  ice_hw.back points at
	 * ice_osdep and ice_osdep.ios_ice points back here; both are wired
	 * once, early in attach, before any register or config-space access.
	 */
	struct ice_hw		ice_hw;
	struct ice_osdep	ice_osdep;

	int			ice_intr_type;
	int			ice_intr_cap;
	uint_t			ice_intr_pri;
	int			ice_intr_count;
	size_t			ice_intr_size;
	ddi_intr_handle_t	*ice_intr_handles;
	ddi_cb_handle_t		ice_intr_cb;	/* IRM registration */
	boolean_t		ice_irm_busy;	/* ice_rebuild_lock */
	uint16_t		ice_nqueues;

	/* OICR deferred async work; thread context, serialized via ice_lock. */
	ddi_taskq_t		*ice_oicr_taskq;
	ddi_periodic_t		ice_admin_periodic;
	boolean_t		ice_oicr_pending;	/* ice_lock */
	uint32_t		ice_oicr_cause;		/* ice_lock */
	uint8_t			*ice_aqbuf;		/* ARQ scratch buffer */

	/*
	 * Reset rebuild.  ice_rebuild_lock is an adaptive mutex taken only in
	 * thread context and is the outermost driver lock: it serializes a
	 * rebuild against a mac start/stop.  The rebuild runs on its own
	 * single-thread taskq so it never starves the OICR worker's ARQ drain;
	 * ice_reset_pending coalesces queued, waiting, and running work under
	 * ice_lock.  The worker atomically consumes its reset request bits,
	 * leaving later requests owed to its next pass.  ice_attaching and
	 * ice_detaching turn a queued
	 * rebuild into a no-op while the instance is not fully constructed:
	 * the rebuild frees and reinitializes scheduler and control-queue
	 * state that the attach thread is still building on, and it reports
	 * link state through a MAC handle detach is about to invalidate.  The
	 * hardware latches are one-shot, so whoever clears one of these flags
	 * must requeue a rebuild owed while it was set.
	 */
	ddi_taskq_t		*ice_reset_taskq;
	boolean_t		ice_reset_pending;	/* ice_lock */
	kmutex_t		ice_rebuild_lock;
	boolean_t		ice_attaching;		/* ice_rebuild_lock */
	boolean_t		ice_detaching;		/* ice_rebuild_lock */

	/*
	 * Link-state cache.  The authoritative state lives in
	 * ice_hw.port_info->phy.link_info, refreshed by the common code; these
	 * are the decoded carrier values, including the loopback override.
	 * MAC publication and MAC_PROP_STATUS apply the operational failure
	 * state separately.  Guarded by ice_lse_lock.
	 */
	kmutex_t		ice_lse_lock;
	kcondvar_t		ice_lse_cv;
	uint32_t		ice_lse_flags;
	link_state_t		ice_link_state;
	uint64_t		ice_link_speed;
	link_duplex_t		ice_link_duplex;
	link_flowctrl_t		ice_link_fctl;
	uint16_t		ice_phy_speeds_supp;
	uint16_t		ice_phy_speeds_adv;
	link_fec_t		ice_fec_neg;
	/* Adaptive mutex: loopback transitions run only in thread context. */
	kmutex_t		ice_loopback_lock;
	uint32_t		ice_loopback_mode;

	uint32_t		ice_mtu;
	boolean_t		ice_tx_lso_enable;	/* LSO advertised */
	boolean_t		ice_led_ident;		/* ice_rebuild_lock */
	/* E830 PHY setup waits for the PHY firmware; ice_rebuild_lock. */
	boolean_t		ice_phy_fw_pending;
	boolean_t		ice_phy_fw_fault;
	boolean_t		ice_promisc_on;		/* replay on reset */
	ice_vsi_t		ice_pf_vsi;		/* the PF's data VSI */

	/*
	 * Datapath rings.  Counts derive from the VSI queue configuration.
	 */
	uint_t			ice_num_txr;
	uint_t			ice_num_rxr;
	/* Loans of replaced rx pools still out; see ICE_RX_ORPHAN_MAX. */
	volatile uint32_t	ice_rx_orphan_loans;
	/* Their returned blocks, freed on ice_rx_reap_taskq. */
	ice_rx_ctrl_block_t	*volatile ice_rx_reap;
	volatile uint32_t	ice_rx_reap_queued;
	ddi_taskq_t		*ice_rx_reap_taskq;
	uint_t			ice_num_rx_groups;
	ice_tx_ring_t		*ice_txr;	/* [ice_num_txr] */
	ice_rx_ring_t		*ice_rxr;	/* [ice_num_rxr] */

	uint32_t		ice_tx_ring_size;
	uint32_t		ice_rx_ring_size;
	uint32_t		ice_rx_limit_per_intr;

	/* DDP firmware. */
	/* Attach-only: MAC caches mi_capab at mac_register(). */
	boolean_t		ice_safe_mode;
	enum ice_ddp_state	ice_ddp_state;

	/*
	 * Hardware statistics.  The MAC counters do not reset on a PF reset, so
	 * the common-code helpers subtract a first-read baseline; ice_stat_lock
	 * serializes the kstat and mac_stat readers that share that baseline.
	 */
	kmutex_t		ice_stat_lock;
	boolean_t		ice_stat_port_loaded;
	boolean_t		ice_stat_vsi_loaded;
	hrtime_t		ice_stat_port_last_update;
	struct ice_hw_port_stats ice_stat_port_cur;
	struct ice_hw_port_stats ice_stat_port_prev;
	struct ice_eth_stats	ice_stat_vsi_cur;
	struct ice_eth_stats	ice_stat_vsi_prev;
	kstat_t			*ice_pf_kstat;
	kstat_t			*ice_vsi_kstat;

	/*
	 * Firmware log events queued for ICE_IOC_FWLOG_READ (ice_ioctl.c).
	 * The ring is allocated when log delivery is first enabled.
	 */
	kmutex_t		ice_fwlog_lock;
	uint8_t			*ice_fwlog_buf;
	size_t			ice_fwlog_head;
	size_t			ice_fwlog_len;
	uint32_t		ice_fwlog_dropped;

	mac_handle_t		ice_mac_hdl;	/* set by mac_register() */
} ice_t;

/*
 * ice.c
 */
/*PRINTFLIKE2*/
extern void ice_error(ice_t *, const char *, ...);
extern int ice_check_acc_handle(ice_t *, ddi_acc_handle_t);
extern int ice_status_to_errno(ice_t *, int);
extern void ice_update_mtu(ice_t *);
/* MAC lifecycle intent; these acquire ice_rebuild_lock internally. */
extern int ice_start(ice_t *);
extern void ice_stop(ice_t *);
extern void ice_reset_task(void *);
extern void ice_reset_redispatch(ice_t *);
#ifdef DEBUG
extern void ice_test_request_reset(ice_t *);
#endif

/*
 * ice_hw.c: device bring-up shared by attach and the reset rebuild.
 */
extern void ice_fm_init(ice_t *);
extern void ice_fm_fini(ice_t *);
extern void ice_identify_hardware(ice_t *);
extern boolean_t ice_regs_map(ice_t *);
extern boolean_t ice_validate_caps(ice_t *);
extern boolean_t ice_hw_init(ice_t *);
typedef enum ice_fw_state {
	ICE_FW_USABLE,
	ICE_FW_RECOVERY,	/* firmware needs an NVM update */
	ICE_FW_UNREADABLE	/* register access faulted */
} ice_fw_state_t;
extern ice_fw_state_t ice_fw_state(ice_t *, uint32_t *);
extern void ice_fw_recovery_report(ice_t *, uint32_t);
extern boolean_t ice_reset_empr_slow(struct ice_hw *);
typedef enum ice_phy_fw_state {
	ICE_PHY_FW_READY,
	ICE_PHY_FW_LOADING,
	ICE_PHY_FW_UNREADABLE	/* register access faulted */
} ice_phy_fw_state_t;
extern ice_phy_fw_state_t ice_phy_fw_state(ice_t *);
extern ice_phy_fw_state_t ice_phy_fw_wait(ice_t *);
extern boolean_t ice_alloc_intrs(ice_t *);
extern void ice_free_intrs(ice_t *);
extern boolean_t ice_add_intr_handlers(ice_t *);
extern boolean_t ice_rem_intr_handlers(ice_t *);
extern int ice_intr_adjust(ice_t *, ddi_cb_action_t, int);

/*
 * ice_intr.c
 */
extern uint_t ice_intr_msix(caddr_t, caddr_t);
extern uint32_t ice_ring_vector(const ice_t *, uint_t);
extern uint32_t ice_rx_intr_limit(const ice_t *);
extern void ice_intr_rings_map(ice_t *);
extern boolean_t ice_intr_enable(ice_t *);
extern boolean_t ice_intr_disable(ice_t *);
extern void ice_intr_oicr_setup(ice_t *, boolean_t);
extern void ice_intr_oicr_disable(ice_t *);
extern boolean_t ice_set_link_events(ice_t *);
extern void ice_link_status_update(ice_t *);
extern void ice_oicr_resync(ice_t *);
extern void ice_admin_periodic_start(ice_t *);
extern void ice_admin_periodic_stop(ice_t *);
extern void ice_setup_link(ice_t *);
extern void ice_phy_setup(ice_t *);
extern void ice_phy_caps_update(ice_t *);
extern void ice_reset_dispatch(ice_t *);
extern void ice_link_report(ice_t *, link_state_t);

/*
 * ice_vsi.c
 */
extern boolean_t ice_vsi_init(ice_t *);
extern void ice_vsi_fini(ice_t *);
extern int ice_vsi_rebuild(ice_t *);

/*
 * ice_filter.c: setup and address replay return ICE status; setters and
 * promiscuous replay return errno.
 * Init/setup/fini run with exclusive lifecycle ownership.  Replay operations
 * require ice_rebuild_lock; callback setters acquire it themselves.
 */
extern void ice_filters_init(ice_t *);
extern void ice_filters_fini(ice_t *);
extern int ice_filters_setup(ice_t *);
extern int ice_filters_replay(ice_t *);
extern int ice_filters_replay_promisc(ice_t *);
extern int ice_filters_set_mac(ice_t *, const uint8_t *, boolean_t);
extern int ice_filters_set_promisc(ice_t *, boolean_t);

/*
 * ice_ddp.c
 */
extern boolean_t ice_ddp_load(ice_t *);

/*
 * ice_dma.c
 */
extern void ice_dma_acc_attr(ice_t *, ddi_device_acc_attr_t *);
extern void ice_dma_ring_attr(ice_t *, ddi_dma_attr_t *);
extern void ice_pkt_dma_attr(ice_t *, ddi_dma_attr_t *);
extern void ice_pkt_txbind_attr(ice_t *, ddi_dma_attr_t *);
extern void ice_pkt_txbind_lso_attr(ice_t *, ddi_dma_attr_t *);
extern boolean_t ice_dma_alloc(ice_t *, ice_dma_buffer_t *, ddi_dma_attr_t *,
    ddi_device_acc_attr_t *, boolean_t, size_t, boolean_t);
extern void ice_dma_free(ice_dma_buffer_t *);
extern int ice_check_dma_handle(ddi_dma_handle_t);
extern uint_t ice_tx_pool_bufs(uint_t, uint_t, uint_t);
extern ice_dma_buffer_t *ice_buf_take(ice_buf_pool_t *);
extern void ice_buf_put(ice_dma_buffer_t *);
extern boolean_t ice_buf_init(ice_t *);
extern void ice_buf_fini(ice_t *);
extern boolean_t ice_tx_lso_alloc(ice_t *);
extern void ice_tx_lso_free(ice_t *);

/*
 * ice_tx.c
 */
extern boolean_t ice_tx_rings_alloc(ice_t *);
extern void ice_tx_rings_free(ice_t *);
extern int ice_tx_ring_program(ice_t *, ice_tx_ring_t *);
extern int ice_tx_ring_unprogram(ice_t *, ice_tx_ring_t *);
extern void ice_map_txq_vector(ice_t *, ice_tx_ring_t *);
extern boolean_t ice_tcb_lso_handles_alloc(ice_t *, ice_tx_ring_t *);
extern void ice_tcb_lso_handles_free(ice_tx_ring_t *);

/*
 * ice_rx.c
 */
extern boolean_t ice_rx_rings_alloc(ice_t *);
extern void ice_rx_rings_free(ice_t *);
extern int ice_rx_ring_program(ice_t *, ice_rx_ring_t *);
extern int ice_rx_ring_unprogram(ice_t *, ice_rx_ring_t *);
/*
 * Lifecycle transitions of an rx queue's interrupt cause routing.  MAC's
 * poll-mode callbacks share the same register; see ice_rx_ring_intr_program().
 */
typedef enum ice_rx_intr_route {
	ICE_RX_INTR_UNMAP,	/* clear routing and cause (reset, teardown) */
	ICE_RX_INTR_DISSOCIATE,	/* keep routing, clear cause (queue disable) */
	ICE_RX_INTR_MAP		/* route and arm; MAC's poll state still wins */
} ice_rx_intr_route_t;
extern void ice_rx_ring_intr_route(ice_rx_ring_t *, ice_rx_intr_route_t);
extern void ice_cfg_itr(ice_t *, uint32_t);

/*
 * ice_tx.c -- packet datapath
 */
extern mblk_t *ice_ring_tx(void *, mblk_t *);
extern void ice_tx_start(ice_t *);
extern void ice_tx_quiesce(ice_t *);
extern void ice_tx_reclaim(ice_t *);
extern void ice_tx_stop(ice_t *);
extern void ice_tx_wake(ice_t *);
extern void ice_tx_ring_intr(ice_tx_ring_t *);
extern int ice_ring_tx_stat(mac_ring_driver_t, uint_t, uint64_t *);

/*
 * ice_rx.c -- packet datapath
 */
extern void ice_rx_recycle(caddr_t);
extern boolean_t ice_rx_start(ice_t *);
extern boolean_t ice_rx_quiesce(ice_t *);
extern boolean_t ice_rx_orphans_drain(ice_t *);
extern void ice_rx_reclaim(ice_t *);
extern boolean_t ice_rx_stop(ice_t *);
extern boolean_t ice_rx_rings_resume(ice_t *);
extern boolean_t ice_rx_ring_intr(ice_rx_ring_t *);
extern mblk_t *ice_ring_rx_poll(void *, int);
extern int ice_ring_rx_start(mac_ring_driver_t, uint64_t);
extern void ice_ring_rx_stop(mac_ring_driver_t);
extern int ice_ring_rx_stat(mac_ring_driver_t, uint_t, uint64_t *);
extern int ice_ring_rx_intr_enable(mac_intr_handle_t);
extern int ice_ring_rx_intr_disable(mac_intr_handle_t);

/*
 * ice_gld.c
 */
extern boolean_t ice_mac_register(ice_t *);
extern int ice_mac_unregister(ice_t *);
extern void ice_mac_intr_set(ice_t *, boolean_t);
extern void ice_link_state_publish(ice_t *);
extern link_state_t ice_link_state_effective(ice_t *, link_state_t);

/*
 * ice_port.c: transceiver and LED capabilities.  The replay requires
 * ice_rebuild_lock; the other entry points acquire it.
 */
extern int ice_transceiver_info(void *, uint_t, mac_transceiver_info_t *);
extern int ice_transceiver_read(void *, uint_t, uint_t, void *, size_t, off_t,
    size_t *);
extern int ice_led_set(void *, mac_led_mode_t, uint_t);
extern void ice_led_replay(ice_t *);
extern void ice_led_fini(ice_t *);

/*
 * ice_ioctl.c: firmware diagnostic ioctls.
 */
extern void ice_diag_init(ice_t *);
extern void ice_diag_fini(ice_t *);
extern void ice_diag_fwlog_event(ice_t *, const uint8_t *, size_t);
extern boolean_t ice_diag_ioctl(ice_t *, queue_t *, mblk_t *);

/*
 * Hardware statistics (ice_stats.c).
 */
extern boolean_t ice_stats_init(ice_t *);
extern void ice_stats_fini(ice_t *);
extern int ice_stats_read(ice_t *, uint_t, uint64_t *);
/* Requires ice_rebuild_lock; preserves accumulated counters. */
extern void ice_stats_reset(ice_t *);
extern int ice_vsi_loopback_set(ice_t *, boolean_t);
extern void ice_link_loopback_update(ice_t *, uint32_t);
extern void ice_loopback_replay(ice_t *);
extern void ice_loopback_fini(ice_t *);

#ifdef __cplusplus
}
#endif

#endif /* _ICE_H */

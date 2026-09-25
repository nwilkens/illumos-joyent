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

#ifndef _ICE_RDMA_H
#define	_ICE_RDMA_H

/*
 * The private interface between ice(4D) and the RDMA function driver that
 * attaches as its child (irdma).  ice keeps the PCI function: BAR0, the admin
 * queue, the MSI-X vectors, resets and the VSI.  The child reaches them only
 * through the operations below.
 *
 * The child finds the interface with ddi_get_parent_data() on its own dip.
 * The structure there starts with ice_rdma_peer_hdr_t, and the child must
 * refuse to attach unless irp_version equals ICE_RDMA_VERSION.
 *
 * A PF reset takes the child offline before the reset and attaches it again
 * after the rebuild.  ice never calls into the child with one of its own
 * locks held, and the child may call any operation from its event callback.
 * Every operation other than dma_free, close and intr_get fails with EIO once
 * the generation returned by open is stale.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ethernet.h>
#include <sys/mac.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	ICE_RDMA_VERSION	1

#define	ICE_RDMA_MAX_TC		8
#define	ICE_RDMA_MAX_UP		8
#define	ICE_RDMA_DSCP_NUM	64
/* Qsets one call may add or remove. */
#define	ICE_RDMA_MAX_QSETS	8

typedef struct ice_rdma_peer ice_rdma_peer_t;

typedef enum ice_rdma_event_type {
	ICE_RDMA_EV_LINK = 1,
	ICE_RDMA_EV_MTU,
	ICE_RDMA_EV_TC,
	ICE_RDMA_EV_RESET_PREP,
	ICE_RDMA_EV_RESET_DONE,
	ICE_RDMA_EV_CRIT_ERR
} ice_rdma_event_type_t;

typedef struct ice_rdma_tc {
	uint8_t		irt_rel_bw;
	uint8_t		irt_prio_type;
} ice_rdma_tc_t;

/* The port's quality of service; ice programs only TC0 today. */
typedef struct ice_rdma_qos {
	uint8_t		irq_num_tc;
	uint8_t		irq_pfc_mode;
	uint8_t		irq_up2tc[ICE_RDMA_MAX_UP];
	ice_rdma_tc_t	irq_tc[ICE_RDMA_MAX_TC];
	uint8_t		irq_dscp_map[ICE_RDMA_DSCP_NUM];
} ice_rdma_qos_t;

typedef struct ice_rdma_event {
	ice_rdma_event_type_t	ire_type;
	link_state_t		ire_link;	/* LINK */
	uint64_t		ire_speed;	/* LINK, bits per second */
	uint32_t		ire_mtu;	/* MTU */
	uint32_t		ire_oicr;	/* CRIT_ERR: PFINT_OICR bits */
	ice_rdma_qos_t		ire_qos;	/* TC */
} ice_rdma_event_t;

typedef struct ice_rdma_client {
	void	(*irc_event)(void *, const ice_rdma_event_t *);
} ice_rdma_client_t;

/* What the child needs to run the RDMA function. */
typedef struct ice_rdma_info {
	uint32_t		iri_generation;
	caddr_t			iri_bar0;
	size_t			iri_bar0_size;
	ddi_acc_handle_t	iri_bar0_handle;
	uint8_t			iri_pf_id;
	uint16_t		iri_vsi_num;
	uint32_t		iri_mtu;
	uint8_t			iri_mac[ETHERADDRL];
	link_state_t		iri_link;
	uint64_t		iri_speed;
	ice_rdma_qos_t		iri_qos;
} ice_rdma_info_t;

/*
 * The reserved block of MSI-X vectors.  irin_first is the PF-relative vector
 * number of irin_handles[0], which the child writes to the queue interrupt
 * control registers.  The handles stay allocated until ice detaches.
 */
typedef struct ice_rdma_intr {
	ddi_intr_handle_t	*irin_handles;
	uint_t			irin_count;
	uint_t			irin_first;
	uint_t			irin_pri;
	int			irin_cap;
} ice_rdma_intr_t;

typedef struct ice_rdma_qset {
	uint16_t	irqs_handle;	/* the child's work scheduler node */
	uint16_t	irqs_vsi_num;
	uint8_t		irqs_tc;
	uint32_t	irqs_teid;	/* set by qset_add */
} ice_rdma_qset_t;

typedef enum ice_rdma_reset {
	ICE_RDMA_RESET_PF = 1
} ice_rdma_reset_t;

/*
 * DMA memory ice allocates for the child: zeroed, physically contiguous and
 * aligned as asked.  A buffer freed while the device may still write to it is
 * kept by ice until the next reset completes.
 */
typedef struct ice_rdma_dma {
	caddr_t		ird_va;
	uint64_t	ird_pa;
	size_t		ird_len;
} ice_rdma_dma_t;

typedef enum ice_rdma_test {
	ICE_RDMA_TEST_IRM_REMOVE = 1,
	ICE_RDMA_TEST_IRM_ADD
} ice_rdma_test_t;

typedef struct ice_rdma_ops {
	int	(*iro_open)(ice_rdma_peer_t *, const ice_rdma_client_t *,
	    void *, ice_rdma_info_t *);
	void	(*iro_close)(ice_rdma_peer_t *);
	int	(*iro_intr_get)(ice_rdma_peer_t *, ice_rdma_intr_t *);
	int	(*iro_qset_add)(ice_rdma_peer_t *, ice_rdma_qset_t *, uint_t);
	int	(*iro_qset_del)(ice_rdma_peer_t *, ice_rdma_qset_t *, uint_t);
	int	(*iro_pe_filter)(ice_rdma_peer_t *, boolean_t);
	int	(*iro_reset)(ice_rdma_peer_t *, ice_rdma_reset_t);
	boolean_t (*iro_resetting)(ice_rdma_peer_t *);
	int	(*iro_dma_alloc)(ice_rdma_peer_t *, size_t, size_t,
	    ice_rdma_dma_t **);
	void	(*iro_dma_free)(ice_rdma_peer_t *, ice_rdma_dma_t *, boolean_t);
	int	(*iro_acc_check)(ice_rdma_peer_t *);
	void	(*iro_fm_report)(ice_rdma_peer_t *, const char *, int);
	int	(*iro_devmap)(ice_rdma_peer_t *, devmap_cookie_t, offset_t,
	    size_t, uint_t);
	int	(*iro_test)(ice_rdma_peer_t *, ice_rdma_test_t, uint_t);
} ice_rdma_ops_t;

typedef struct ice_rdma_peer_hdr {
	uint32_t		irp_version;
	const ice_rdma_ops_t	*irp_ops;
} ice_rdma_peer_hdr_t;

#ifdef __cplusplus
}
#endif

#endif /* _ICE_RDMA_H */

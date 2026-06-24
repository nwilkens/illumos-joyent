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
 * Copyright 2026 MNX Cloud, Inc.
 */

#ifndef _ICE_H
#define	_ICE_H

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/pci.h>
#include <sys/list.h>
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
#define	ICE_MAX_MTU		9728		/* E810 jumbo frame maximum */
#define	ICE_MAX_FUNCS		8

typedef enum ice_state {
	ICE_STATE_ATTACHED	= 1 << 0,
	ICE_STATE_RESET_PENDING	= 1 << 1,	/* GRST seen; recovery is M7 */
	ICE_STATE_ERROR		= 1 << 2	/* acc-handle fault latched */
} ice_state_t;

/* ice_lse_flags bits (protected by ice_lse_lock). */
#define	ICE_LSE_F_ENABLE	(1 << 0)	/* link status events wanted */
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
	ICE_ATTACH_ALLOC_INTR	= 1 << 4,
	ICE_ATTACH_ADD_INTR	= 1 << 5,
	ICE_ATTACH_OICR_TASKQ	= 1 << 6,
	ICE_ATTACH_ENABLE_INTR	= 1 << 7
} ice_attach_state_t;

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
	list_node_t		ice_glink;

	int			ice_fm_caps;

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

	/* OICR deferred async work; thread context, serialized via ice_lock. */
	ddi_taskq_t		*ice_oicr_taskq;
	boolean_t		ice_oicr_pending;	/* ice_lock */
	uint8_t			*ice_aqbuf;		/* ARQ scratch buffer */

	/*
	 * Link-state cache.  The authoritative state lives in
	 * ice_hw.port_info->phy.link_info, refreshed by the common code; these
	 * are the decoded values MAC will consume once mac_register lands.
	 * Guarded by ice_lse_lock.
	 */
	kmutex_t		ice_lse_lock;
	kcondvar_t		ice_lse_cv;
	uint32_t		ice_lse_flags;
	link_state_t		ice_link_state;
	uint64_t		ice_link_speed;
	link_duplex_t		ice_link_duplex;
	link_flowctrl_t		ice_link_fctl;
	mac_handle_t		ice_mac_hdl;		/* NULL until M6 */
} ice_t;

/*
 * ice.c
 */
/*PRINTFLIKE2*/
extern void ice_error(ice_t *, const char *, ...);
extern int ice_check_acc_handle(ddi_acc_handle_t);

/*
 * ice_intr.c
 */
extern uint_t ice_intr_msix(caddr_t, caddr_t);
extern boolean_t ice_intr_enable(ice_t *);
extern void ice_intr_disable(ice_t *);
extern void ice_intr_oicr_setup(ice_t *);
extern void ice_intr_oicr_disable(ice_t *);
extern boolean_t ice_set_link_events(ice_t *);
extern void ice_link_status_update(ice_t *);

#ifdef __cplusplus
}
#endif

#endif /* _ICE_H */

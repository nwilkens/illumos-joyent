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

/*
 * Interrupt handling for ice(4D): MSI-X enable/disable, the "other interrupt
 * cause" (OICR) vector that carries admin-queue async events, link-status
 * changes, and error causes, and the taskq that drains the admin receive
 * queue off the interrupt.  Link state is read through the common code and
 * cached in the softc; it is not reported to MAC until mac_register lands.
 */

#include <sys/atomic.h>

#include "ice.h"
#include "ice_common.h"

/*
 * OICR is always MSI-X vector 0; queue vectors start at 1.  The other cause
 * uses a dedicated ITR slot; ICE_ITR_INDEX_NONE is the "do not update the ITR"
 * encoding used when simply re-arming the vector.
 */
#define	ICE_OICR_VECTOR		0
#define	ICE_ITR_INDEX_OTHER	2
#define	ICE_ITR_INDEX_NONE	3

/*
 * Hard cap on a single ARQ drain pass so a wedged ring cannot spin forever.
 * Set above the ring depth (ICE_AQ_LEN, 1023) so a merely busy ring still
 * drains fully in one pass.
 */
#define	ICE_ARQ_MAX_ELEMS	2048

/*
 * Causes enabled in PFINT_OICR_ENA.  Link changes and the error causes; the
 * generic INTEVENT bit (0) is reserved in this register and is delivered via
 * control-queue routing instead.
 */
#define	ICE_OICR_ENA_MASK	\
	(PFINT_OICR_LINK_STAT_CHANGE_M | PFINT_OICR_GRST_M |	\
	PFINT_OICR_ECC_ERR_M | PFINT_OICR_MAL_DETECT_M |	\
	PFINT_OICR_PCI_EXCEPTION_M | PFINT_OICR_HMC_ERR_M |	\
	PFINT_OICR_PE_CRITERR_M | PFINT_OICR_VFLR_M)

/* Causes that imply deferred (taskq) work. */
#define	ICE_OICR_TASKQ_MASK	\
	(PFINT_OICR_INTEVENT_M | PFINT_OICR_LINK_STAT_CHANGE_M |	\
	PFINT_OICR_GRST_M)

static void
ice_link_state_set(ice_t *ice, link_state_t state)
{
	ASSERT(MUTEX_HELD(&ice->ice_lse_lock));

	/*
	 * Link state is cached only until MAC is registered (M6); mac_link_
	 * update() must not be called with a NULL handle.
	 */
	if (ice->ice_mac_hdl == NULL)
		return;

	mac_link_update(ice->ice_mac_hdl, state);
}

/*
 * Decode the firmware-supplied link_info into the cached link state.  Every
 * field here originates in a firmware-written DMA buffer and is treated as
 * untrusted: the speed bitmap is masked and zero-guarded before being turned
 * into an index, and the speed lookup itself (ice_get_link_speed) is bounds
 * checked by the common code.
 */
static void
ice_link_prop_update(ice_t *ice)
{
	struct ice_link_status *li = &ice->ice_hw.port_info->phy.link_info;
	link_flowctrl_t fctl = LINK_FLOWCTRL_NONE;
	uint64_t speed = 0;
	uint16_t spd_bits;

	ASSERT(MUTEX_HELD(&ice->ice_lse_lock));

	if ((li->link_info & ICE_AQ_LINK_UP) == 0) {
		ice->ice_link_state = LINK_STATE_DOWN;
		ice->ice_link_speed = 0;
		ice->ice_link_duplex = LINK_DUPLEX_UNKNOWN;
		ice->ice_link_fctl = LINK_FLOWCTRL_NONE;
		ice_link_state_set(ice, LINK_STATE_DOWN);
		return;
	}

	/*
	 * link_speed is a one-hot bitmap (bits 0-11 valid).  Mask to the valid
	 * range and guard zero before highbit(), then decode the bit index
	 * through the bounds-checked common-code helper.
	 */
	spd_bits = li->link_speed & ICE_AQ_LINK_SPEED_M;
	if (spd_bits != 0)
		speed = ice_get_link_speed((u16)(highbit(spd_bits) - 1));

	if (li->an_info & ICE_AQ_LINK_PAUSE_TX) {
		fctl = (li->an_info & ICE_AQ_LINK_PAUSE_RX) ?
		    LINK_FLOWCTRL_BI : LINK_FLOWCTRL_TX;
	} else if (li->an_info & ICE_AQ_LINK_PAUSE_RX) {
		fctl = LINK_FLOWCTRL_RX;
	}

	/* PHY/serdes links are point-to-point: always full duplex. */
	ice->ice_link_state = LINK_STATE_UP;
	ice->ice_link_speed = speed;
	ice->ice_link_duplex = LINK_DUPLEX_FULL;
	ice->ice_link_fctl = fctl;

	ice_link_state_set(ice, LINK_STATE_UP);
}

/*
 * Refresh the cached link state from hardware.  The blocking common-code read
 * runs outside ice_lse_lock; a single in-flight update is enforced with the
 * UPDATING flag so concurrent callers (taskq and attach) serialize.
 */
void
ice_link_status_update(ice_t *ice)
{
	struct ice_port_info *pi = ice->ice_hw.port_info;
	int rc;

	if (pi == NULL)
		return;

	mutex_enter(&ice->ice_lse_lock);
	while (ice->ice_lse_flags & ICE_LSE_F_UPDATING)
		cv_wait(&ice->ice_lse_cv, &ice->ice_lse_lock);
	ice->ice_lse_flags |= ICE_LSE_F_UPDATING;
	mutex_exit(&ice->ice_lse_lock);

	pi->phy.get_link_info = 1;
	rc = ice_update_link_info(pi);

	mutex_enter(&ice->ice_lse_lock);
	if (rc == 0)
		ice_link_prop_update(ice);
	else
		ice_error(ice, "link info update failed: %d", rc);
	ice->ice_lse_flags &= ~ICE_LSE_F_UPDATING;
	cv_broadcast(&ice->ice_lse_cv);
	mutex_exit(&ice->ice_lse_lock);
}

/*
 * Taskq worker: drain the admin receive queue and act on link-status events.
 * The loop is bounded both by the firmware-reported pending count and by a
 * hard element cap.
 */
static void
ice_oicr_task(void *arg)
{
	ice_t *ice = arg;
	struct ice_hw *hw = &ice->ice_hw;
	struct ice_rq_event_info evt;
	uint16_t pending;
	uint_t guard = 0;
	int rc;

	mutex_enter(&ice->ice_lock);
	ice->ice_oicr_pending = B_FALSE;
	mutex_exit(&ice->ice_lock);

	bzero(&evt, sizeof (evt));
	evt.buf_len = ICE_AQ_MAX_BUF_LEN;
	evt.msg_buf = ice->ice_aqbuf;

	do {
		rc = ice_clean_rq_elem(hw, &hw->adminq, &evt, &pending);
		if (rc == ICE_ERR_AQ_NO_WORK)
			break;
		if (rc != 0) {
			ice_error(ice, "admin receive queue clean failed: %d",
			    rc);
			break;
		}

		switch (LE_16(evt.desc.opcode)) {
		case ice_aqc_opc_get_link_status:
			ice_link_status_update(ice);
			break;
		default:
			break;
		}
	} while (pending != 0 && ++guard < ICE_ARQ_MAX_ELEMS);

	if (guard >= ICE_ARQ_MAX_ELEMS) {
		ice_error(ice, "admin receive queue drain cap hit; pending %u",
		    pending);
	}

	/*
	 * A detected global reset is latched in ICE_STATE_RESET_PENDING; full
	 * reset recovery is deferred until the data path exists (M7).
	 */
}

static void
ice_intr_oicr_enable(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;

	wr32(hw, GLINT_DYN_CTL(ICE_OICR_VECTOR),
	    GLINT_DYN_CTL_INTENA_M | GLINT_DYN_CTL_CLEARPBA_M |
	    ((ICE_ITR_INDEX_NONE << GLINT_DYN_CTL_ITR_INDX_S) &
	    GLINT_DYN_CTL_ITR_INDX_M));
	ice_flush(hw);
}

void
ice_intr_oicr_disable(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;

	wr32(hw, GLINT_DYN_CTL(ICE_OICR_VECTOR),
	    (ICE_ITR_INDEX_NONE << GLINT_DYN_CTL_ITR_INDX_S) &
	    GLINT_DYN_CTL_ITR_INDX_M);
	ice_flush(hw);
}

void
ice_intr_oicr_setup(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t reg;

	/* Mask all causes and clear any stale status (the read clears it). */
	wr32(hw, PFINT_OICR_ENA, 0);
	(void) rd32(hw, PFINT_OICR);

	/* Route the FW/admin control-queue cause to vector 0, OTHER ITR. */
	reg = ((ICE_OICR_VECTOR << PFINT_FW_CTL_MSIX_INDX_S) &
	    PFINT_FW_CTL_MSIX_INDX_M) |
	    ((ICE_ITR_INDEX_OTHER << PFINT_FW_CTL_ITR_INDX_S) &
	    PFINT_FW_CTL_ITR_INDX_M) | PFINT_FW_CTL_CAUSE_ENA_M;
	wr32(hw, PFINT_FW_CTL, reg);

	/* Route the generic OICR cause (link change + errors) to vector 0. */
	reg = ((ICE_OICR_VECTOR << PFINT_OICR_CTL_MSIX_INDX_S) &
	    PFINT_OICR_CTL_MSIX_INDX_M) |
	    ((ICE_ITR_INDEX_OTHER << PFINT_OICR_CTL_ITR_INDX_S) &
	    PFINT_OICR_CTL_ITR_INDX_M) | PFINT_OICR_CTL_CAUSE_ENA_M;
	wr32(hw, PFINT_OICR_CTL, reg);

	wr32(hw, PFINT_OICR_ENA, ICE_OICR_ENA_MASK);

	ice_intr_oicr_enable(ice);
}

static uint_t
ice_intr_oicr(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	boolean_t dispatch = B_FALSE;
	uint32_t oicr;

	oicr = rd32(hw, PFINT_OICR);

	if (ice_check_acc_handle(ice->ice_osdep.ios_reg_handle) != DDI_FM_OK) {
		ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_DEGRADED);
		atomic_or_32(&ice->ice_state, ICE_STATE_ERROR);
		return (DDI_INTR_CLAIMED);
	}

	if (oicr == 0)
		return (DDI_INTR_CLAIMED);

	if (oicr & PFINT_OICR_GRST_M)
		atomic_or_32(&ice->ice_state, ICE_STATE_RESET_PENDING);

	/*
	 * ice has no dedicated admin-queue cause bit, so any control-queue or
	 * link cause dispatches the worker, which decides if there is work.  A
	 * single in-flight worker coalesces a storm of interrupts.
	 */
	if (oicr & ICE_OICR_TASKQ_MASK) {
		mutex_enter(&ice->ice_lock);
		if (!ice->ice_oicr_pending) {
			ice->ice_oicr_pending = B_TRUE;
			dispatch = B_TRUE;
		}
		mutex_exit(&ice->ice_lock);
	}

	if (dispatch && ddi_taskq_dispatch(ice->ice_oicr_taskq, ice_oicr_task,
	    ice, DDI_NOSLEEP) != DDI_SUCCESS) {
		mutex_enter(&ice->ice_lock);
		ice->ice_oicr_pending = B_FALSE;
		mutex_exit(&ice->ice_lock);
		ice_error(ice, "OICR taskq dispatch failed");
	}

	ice_intr_oicr_enable(ice);
	return (DDI_INTR_CLAIMED);
}

static uint_t
ice_intr_queue(ice_t *ice, uint_t vector)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint_t i;

	/*
	 * A queue vector services the rx and tx rings mapped to it.  Rx
	 * delivery is suppressed while mac polls the ring (ice_rx_ring_intr).
	 */
	for (i = 0; i < ice->ice_num_rxr; i++) {
		if (ice->ice_rxr[i].irxr_vec == vector)
			ice_rx_ring_intr(&ice->ice_rxr[i]);
	}
	for (i = 0; i < ice->ice_num_txr; i++) {
		if (ice->ice_txr[i].itxr_vec == vector)
			ice_tx_ring_intr(&ice->ice_txr[i]);
	}

	wr32(hw, GLINT_DYN_CTL(vector),
	    GLINT_DYN_CTL_INTENA_M | GLINT_DYN_CTL_CLEARPBA_M |
	    ((ICE_ITR_IDX_0 << GLINT_DYN_CTL_ITR_INDX_S) &
	    GLINT_DYN_CTL_ITR_INDX_M));
	ice_flush(hw);

	return (DDI_INTR_CLAIMED);
}

uint_t
ice_intr_msix(caddr_t arg1, caddr_t arg2)
{
	ice_t *ice = (ice_t *)arg1;
	uint_t vector = (uint_t)(uintptr_t)arg2;

	if (vector == ICE_OICR_VECTOR)
		return (ice_intr_oicr(ice));

	return (ice_intr_queue(ice, vector));
}

boolean_t
ice_intr_enable(ice_t *ice)
{
	int i, rc;

	if (ice->ice_intr_cap & DDI_INTR_FLAG_BLOCK) {
		rc = ddi_intr_block_enable(ice->ice_intr_handles,
		    ice->ice_intr_count);
		if (rc != DDI_SUCCESS) {
			ice_error(ice, "interrupt block-enable failed: %d", rc);
			return (B_FALSE);
		}
		return (B_TRUE);
	}

	for (i = 0; i < ice->ice_intr_count; i++) {
		rc = ddi_intr_enable(ice->ice_intr_handles[i]);
		if (rc != DDI_SUCCESS) {
			ice_error(ice, "interrupt enable %d failed: %d", i, rc);
			while (--i >= 0) {
				(void) ddi_intr_disable(
				    ice->ice_intr_handles[i]);
			}
			return (B_FALSE);
		}
	}

	return (B_TRUE);
}

void
ice_intr_disable(ice_t *ice)
{
	int i;

	if (ice->ice_intr_handles == NULL)
		return;

	if (ice->ice_intr_cap & DDI_INTR_FLAG_BLOCK) {
		(void) ddi_intr_block_disable(ice->ice_intr_handles,
		    ice->ice_intr_count);
	} else {
		for (i = 0; i < ice->ice_intr_count; i++)
			(void) ddi_intr_disable(ice->ice_intr_handles[i]);
	}
}

/*
 * Ask firmware to deliver link up/down events on the admin receive queue.  The
 * event mask is inverted: a clear bit means "deliver".  Only link up/down is
 * requested; all other link events are masked off because the driver does not
 * act on them.
 */
boolean_t
ice_set_link_events(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	struct ice_port_info *pi = hw->port_info;
	uint16_t mask;
	int rc;

	if (pi == NULL)
		return (B_FALSE);

	mask = (uint16_t)~ICE_AQ_LINK_EVENT_UPDOWN;

	mutex_enter(&ice->ice_lse_lock);
	ice->ice_lse_flags |= ICE_LSE_F_ENABLE;
	mutex_exit(&ice->ice_lse_lock);

	rc = ice_aq_set_event_mask(hw, pi->lport, mask, NULL);
	if (rc != 0) {
		ice_error(ice, "failed to set link event mask: %d", rc);
		return (B_FALSE);
	}

	return (B_TRUE);
}

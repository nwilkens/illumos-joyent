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

/*
 * Receive ring DMA allocation and receive queue-context programming for
 * ice(4D).  Each rx ring owns a descriptor ring (a contiguous block of
 * union ice_32b_rx_flex_desc) and a per-slot array of receive control blocks;
 * the control blocks' data buffers and the desballoc loaners are posted by the
 * data path in a later milestone, so only the array is allocated here.
 *
 * The hardware queue context (struct ice_rlan_ctx) is packed and written by the
 * Intel common code (ice_write_rxq_ctx) under core/; the per-queue enable and
 * the queue-to-MSI-X-vector wiring are raw register operations the common code
 * does not wrap.  No packets flow until the data path posts buffers and rings
 * the tail doorbell.
 */

#include "ice.h"
#include "ice_common.h"
#include "ice_lan_tx_rx.h"

/*
 * Free a single rx ring's DMA and per-slot state.  Safe to call on a ring that
 * was only partially set up: each step is gated on what ice_rx_ring_alloc()
 * actually completed.
 */
static void
ice_rx_ring_free(ice_rx_ring_t *irr)
{
	if (irr->irxr_rcbs != NULL) {
		kmem_free(irr->irxr_rcbs,
		    irr->irxr_size * sizeof (ice_rx_ctrl_block_t *));
		irr->irxr_rcbs = NULL;
	}

	if (irr->irxr_descs != NULL) {
		ice_dma_free(&irr->irxr_desc_dma);
		irr->irxr_descs = NULL;
	}

	mutex_destroy(&irr->irxr_lock);
}

static boolean_t
ice_rx_ring_alloc(ice_t *ice, ice_rx_ring_t *irr, uint_t index)
{
	ddi_dma_attr_t dma_attr;
	ddi_device_acc_attr_t acc_attr;
	size_t desc_len;

	irr->irxr_ice = ice;

	/* Absolute HW rx queue index; the single PF VSI's queues start at 0. */
	irr->irxr_index = index;

	/*
	 * MSI-X vector 0 is the OICR; queue vectors begin at 1.  A 1:1 mapping
	 * is used while there are at least as many vectors as rings; otherwise
	 * the last vector absorbs the overflow.
	 */
	irr->irxr_vec = 1 + index;
	if (irr->irxr_vec >= (uint32_t)ice->ice_intr_count)
		irr->irxr_vec = ice->ice_intr_count - 1;

	irr->irxr_size = ice->ice_rx_ring_size;
	irr->irxr_dbuf = ICE_RX_BUF_SIZE;
	irr->irxr_head = 0;
	irr->irxr_tail = 0;

	mutex_init(&irr->irxr_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(ice->ice_intr_pri));

	desc_len = (size_t)irr->irxr_size *
	    sizeof (union ice_32b_rx_flex_desc);

	ice_dma_ring_attr(ice, &dma_attr);
	ice_dma_acc_attr(ice, &acc_attr);
	if (!ice_dma_alloc(ice, &irr->irxr_desc_dma, &dma_attr, &acc_attr,
	    B_FALSE, desc_len, B_TRUE)) {
		ice_error(ice, "failed to allocate rx descriptor ring for "
		    "queue %u", index);
		mutex_destroy(&irr->irxr_lock);
		return (B_FALSE);
	}
	irr->irxr_descs =
	    (union ice_32b_rx_flex_desc *)irr->irxr_desc_dma.idb_va;

	/*
	 * The control blocks themselves (data buffers, desballoc loaners) are
	 * posted by the data path; only the by-slot array is allocated now.
	 */
	irr->irxr_rcbs = kmem_zalloc(
	    irr->irxr_size * sizeof (ice_rx_ctrl_block_t *), KM_SLEEP);

	return (B_TRUE);
}

boolean_t
ice_rx_rings_alloc(ice_t *ice)
{
	uint_t i;

	ASSERT3U(ice->ice_num_rxr, >, 0);

	ice->ice_rxr = kmem_zalloc(
	    ice->ice_num_rxr * sizeof (ice_rx_ring_t), KM_SLEEP);

	for (i = 0; i < ice->ice_num_rxr; i++) {
		if (!ice_rx_ring_alloc(ice, &ice->ice_rxr[i], i)) {
			while (i-- > 0)
				ice_rx_ring_free(&ice->ice_rxr[i]);
			kmem_free(ice->ice_rxr,
			    ice->ice_num_rxr * sizeof (ice_rx_ring_t));
			ice->ice_rxr = NULL;
			return (B_FALSE);
		}
	}

	return (B_TRUE);
}

void
ice_rx_rings_free(ice_t *ice)
{
	uint_t i;

	if (ice->ice_rxr == NULL)
		return;

	for (i = 0; i < ice->ice_num_rxr; i++)
		ice_rx_ring_free(&ice->ice_rxr[i]);

	kmem_free(ice->ice_rxr, ice->ice_num_rxr * sizeof (ice_rx_ring_t));
	ice->ice_rxr = NULL;
}

/*
 * Tie an rx queue to its MSI-X vector and arm the cause.  There is no common
 * code helper for this; the driver writes QINT_RQCTL directly.
 */
void
ice_map_rxq_vector(ice_t *ice, ice_rx_ring_t *irr)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t reg;

	reg = ((irr->irxr_vec << QINT_RQCTL_MSIX_INDX_S) &
	    QINT_RQCTL_MSIX_INDX_M) |
	    ((ICE_ITR_IDX_0 << QINT_RQCTL_ITR_INDX_S) &
	    QINT_RQCTL_ITR_INDX_M) |
	    QINT_RQCTL_CAUSE_ENA_M;
	wr32(hw, QINT_RQCTL(irr->irxr_index), reg);
	ice_flush(hw);
}

/*
 * Program the per-vector interrupt throttle interval and arm the vector.  Used
 * by both the tx and rx queue->vector wiring, so it is keyed on the vector
 * rather than a ring.
 */
void
ice_cfg_itr(ice_t *ice, uint32_t vector)
{
	struct ice_hw *hw = &ice->ice_hw;

	wr32(hw, GLINT_ITR(ICE_ITR_IDX_0, vector),
	    ICE_ITR_DEFAULT_INTERVAL & GLINT_ITR_INTERVAL_M);
	wr32(hw, GLINT_DYN_CTL(vector),
	    GLINT_DYN_CTL_INTENA_M | GLINT_DYN_CTL_CLEARPBA_M |
	    ((ICE_ITR_IDX_0 << GLINT_DYN_CTL_ITR_INDX_S) &
	    GLINT_DYN_CTL_ITR_INDX_M));
	ice_flush(hw);
}

/*
 * Program an rx queue's context through the common code, then request the
 * queue enable and poll for it.  No buffers are posted here, so the queue is
 * armed but idle until the data path writes the tail doorbell.
 */
int
ice_rx_ring_program(ice_t *ice, ice_rx_ring_t *irr)
{
	struct ice_hw *hw = &ice->ice_hw;
	struct ice_rlan_ctx rlan;
	uint32_t reg;
	uint_t i;
	int status;

	bzero(&rlan, sizeof (rlan));

	/* base and dbuf are expressed in 128-byte units (>> _S of 7). */
	rlan.base = ICE_DMA_PA(&irr->irxr_desc_dma) >> ICE_RLAN_BASE_S;
	rlan.qlen = irr->irxr_size;
	rlan.dbuf = irr->irxr_dbuf >> ICE_RLAN_CTX_DBUF_S;
	rlan.hbuf = 0;				/* no header split */
	rlan.dtype = 0;				/* no descriptor split */
	rlan.dsize = 1;				/* 32-byte descriptors */
	rlan.crcstrip = 1;			/* strip the Ethernet FCS */
	rlan.l2tsel = 1;
	rlan.hsplit_0 = 0;
	rlan.hsplit_1 = 0;
	rlan.showiv = 0;
	/*
	 * Single-buffer receive: the maximum frame must not exceed the posted
	 * data buffer, or hardware could write past the end of it.
	 */
	rlan.rxmax = MIN(ice->ice_pf_vsi.vi_max_frame, irr->irxr_dbuf);
	rlan.lrxqthresh = 1;
	/* ice_write_rxq_ctx forces prefena = 1; set it for clarity. */
	rlan.prefena = 1;
	/* TPH descriptor/data hints are left off, matching both references. */

	status = ice_write_rxq_ctx(hw, &rlan, irr->irxr_index);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to write rx queue context for queue "
		    "%u: %d", irr->irxr_index, status);
		return (status);
	}

	/*
	 * Select the Flex-NIC receive descriptor profile so the hardware
	 * writeback layout matches the descriptor the data path decodes.  This
	 * is not part of the queue context and is reset only by a core reset,
	 * so it is programmed explicitly per queue.
	 */
	reg = rd32(hw, QRXFLXP_CNTXT(irr->irxr_index));
	reg &= ~(QRXFLXP_CNTXT_RXDID_IDX_M | QRXFLXP_CNTXT_RXDID_PRIO_M);
	reg |= (ICE_RXDID_FLEX_NIC << QRXFLXP_CNTXT_RXDID_IDX_S) &
	    QRXFLXP_CNTXT_RXDID_IDX_M;
	reg |= (0x3 << QRXFLXP_CNTXT_RXDID_PRIO_S) & QRXFLXP_CNTXT_RXDID_PRIO_M;
	wr32(hw, QRXFLXP_CNTXT(irr->irxr_index), reg);

	/* Request the enable, then poll QENA_STAT for the queue to come up. */
	reg = rd32(hw, QRX_CTRL(irr->irxr_index));
	reg |= QRX_CTRL_QENA_REQ_M;
	wr32(hw, QRX_CTRL(irr->irxr_index), reg);

	for (i = 0; i < ICE_Q_ENA_MAX_WAIT; i++) {
		reg = rd32(hw, QRX_CTRL(irr->irxr_index));
		if ((reg & QRX_CTRL_QENA_STAT_M) != 0)
			break;
		drv_usecwait(20);
	}

	if ((reg & QRX_CTRL_QENA_STAT_M) == 0) {
		ice_error(ice, "rx queue %u failed to enable", irr->irxr_index);
		return (ICE_ERR_CFG);
	}

	return (ICE_SUCCESS);
}

/*
 * Disable an rx queue and clear its context.  Best effort: teardown proceeds
 * even if the queue does not acknowledge the disable, so the result is not
 * propagated.
 */
void
ice_rx_ring_unprogram(ice_t *ice, ice_rx_ring_t *irr)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t reg;
	uint_t i;

	reg = rd32(hw, QRX_CTRL(irr->irxr_index));
	reg &= ~QRX_CTRL_QENA_REQ_M;
	wr32(hw, QRX_CTRL(irr->irxr_index), reg);

	for (i = 0; i < ICE_Q_ENA_MAX_WAIT; i++) {
		reg = rd32(hw, QRX_CTRL(irr->irxr_index));
		if ((reg & QRX_CTRL_QENA_STAT_M) == 0)
			break;
		drv_usecwait(20);
	}

	if ((reg & QRX_CTRL_QENA_STAT_M) != 0) {
		ice_error(ice, "rx queue %u failed to disable",
		    irr->irxr_index);
	}

	(void) ice_clear_rxq_ctx(hw, irr->irxr_index);
}

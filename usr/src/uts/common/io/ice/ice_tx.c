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
 * Transmit ring DMA allocation and Tx queue-context programming.
 *
 * This milestone builds the per-ring state and hands the LAN Tx queue context
 * to the Intel common code: the descriptor ring is allocated as physically
 * contiguous DMA, the sparse ice_tlan_ctx is filled and bit-packed into the
 * Add Tx LAN Queues command buffer, and ice_ena_vsi_txq() programs and enables
 * the queue in one admin-queue round trip (there is no separate per-queue Tx
 * enable register).  The scheduler element TEID the firmware returns is saved;
 * ice_dis_vsi_txq() requires it at teardown.  The packet datapath (descriptor
 * fill, doorbell, completion reclaim) is a later milestone.
 */

#include "ice.h"
#include "ice_common.h"
#include "ice_lan_tx_rx.h"

/*
 * Map a Tx queue to its MSI-X vector and arm the queue's interrupt cause.
 * There is no common-code helper for this; the driver writes QINT_TQCTL
 * directly.  Exposed via a prototype in ice.h so the attach path can wire the
 * vectors as its own step (ICE_ATTACH_QUEUE_INTR), after the queues have been
 * programmed and enabled (ICE_ATTACH_QUEUES_ENA).
 */
void
ice_map_txq_vector(ice_t *ice, ice_tx_ring_t *itr)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t reg;

	reg = ((itr->itxr_vec << QINT_TQCTL_MSIX_INDX_S) &
	    QINT_TQCTL_MSIX_INDX_M) |
	    ((ICE_ITR_IDX_0 << QINT_TQCTL_ITR_INDX_S) &
	    QINT_TQCTL_ITR_INDX_M) |
	    QINT_TQCTL_CAUSE_ENA_M;
	wr32(hw, QINT_TQCTL(itr->itxr_index), reg);
	ice_flush(hw);
}

static void
ice_tx_ring_fini(ice_t *ice, ice_tx_ring_t *itr)
{
	ice_dma_free(&itr->itxr_dma);
	itr->itxr_descs = NULL;

	if (itr->itxr_tcb_free_list != NULL) {
		kmem_free(itr->itxr_tcb_free_list,
		    itr->itxr_size * sizeof (ice_tx_ctrl_block_t *));
		itr->itxr_tcb_free_list = NULL;
	}
	if (itr->itxr_tcbs != NULL) {
		uint16_t i;

		for (i = 0; i < itr->itxr_size; i++) {
			if (itr->itxr_tcbs[i] != NULL)
				kmem_free(itr->itxr_tcbs[i],
				    sizeof (ice_tx_ctrl_block_t));
		}
		kmem_free(itr->itxr_tcbs,
		    itr->itxr_size * sizeof (ice_tx_ctrl_block_t *));
		itr->itxr_tcbs = NULL;
	}

	mutex_destroy(&itr->itxr_tcb_lock);
	cv_destroy(&itr->itxr_cv);
	mutex_destroy(&itr->itxr_lock);
}

static boolean_t
ice_tx_ring_alloc(ice_t *ice, ice_tx_ring_t *itr, uint_t index)
{
	ddi_dma_attr_t attr;
	ddi_device_acc_attr_t acc;
	size_t descsz;
	uint16_t i;

	itr->itxr_ice = ice;

	/*
	 * Absolute HW Tx queue index.  The single PF data VSI's queues start at
	 * zero, so ring i is queue i; with multiple VSIs sharing the function
	 * this becomes the VSI's first queue plus i.
	 */
	itr->itxr_index = index;

	/*
	 * Vector 0 is the OICR; queue vectors begin at 1.  A simple 1:1 map,
	 * capped so queues beyond the vector count share the last vector.
	 */
	itr->itxr_vec = 1 + index;
	if (itr->itxr_vec >= (uint32_t)ice->ice_intr_count)
		itr->itxr_vec = ice->ice_intr_count - 1;

	itr->itxr_size = ice->ice_tx_ring_size != 0 ?
	    ice->ice_tx_ring_size : ICE_DEF_TX_RING_SIZE;

	mutex_init(&itr->itxr_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(ice->ice_intr_pri));
	cv_init(&itr->itxr_cv, NULL, CV_DRIVER, NULL);
	mutex_init(&itr->itxr_tcb_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(ice->ice_intr_pri));

	descsz = (size_t)itr->itxr_size * sizeof (struct ice_tx_desc);
	ice_dma_ring_attr(ice, &attr);
	ice_dma_acc_attr(ice, &acc);
	if (!ice_dma_alloc(ice, &itr->itxr_dma, &attr, &acc, B_TRUE, descsz,
	    B_TRUE)) {
		ice_error(ice, "failed to allocate tx descriptor ring %u",
		    index);
		goto fail;
	}
	itr->itxr_descs = (struct ice_tx_desc *)itr->itxr_dma.idb_va;

	itr->itxr_tcbs = kmem_zalloc(itr->itxr_size *
	    sizeof (ice_tx_ctrl_block_t *), KM_SLEEP);
	itr->itxr_tcb_free_list = kmem_zalloc(itr->itxr_size *
	    sizeof (ice_tx_ctrl_block_t *), KM_SLEEP);

	for (i = 0; i < itr->itxr_size; i++) {
		ice_tx_ctrl_block_t *itcb;

		itcb = kmem_zalloc(sizeof (ice_tx_ctrl_block_t), KM_SLEEP);
		itcb->itcb_ring = itr;
		itcb->itcb_type = ITCB_NOT_USED;
		itr->itxr_tcbs[i] = itcb;
		itr->itxr_tcb_free_list[i] = itcb;
	}
	itr->itxr_tcb_nfree = itr->itxr_size;
	itr->itxr_avail = itr->itxr_size;

	return (B_TRUE);

fail:
	ice_tx_ring_fini(ice, itr);
	return (B_FALSE);
}

boolean_t
ice_tx_rings_alloc(ice_t *ice)
{
	uint_t i;

	ice->ice_txr = kmem_zalloc(ice->ice_num_txr * sizeof (ice_tx_ring_t),
	    KM_SLEEP);

	for (i = 0; i < ice->ice_num_txr; i++) {
		if (!ice_tx_ring_alloc(ice, &ice->ice_txr[i], i)) {
			while (i-- > 0)
				ice_tx_ring_fini(ice, &ice->ice_txr[i]);
			kmem_free(ice->ice_txr,
			    ice->ice_num_txr * sizeof (ice_tx_ring_t));
			ice->ice_txr = NULL;
			return (B_FALSE);
		}
	}

	return (B_TRUE);
}

void
ice_tx_rings_free(ice_t *ice)
{
	uint_t i;

	if (ice->ice_txr == NULL)
		return;

	for (i = 0; i < ice->ice_num_txr; i++)
		ice_tx_ring_fini(ice, &ice->ice_txr[i]);

	kmem_free(ice->ice_txr, ice->ice_num_txr * sizeof (ice_tx_ring_t));
	ice->ice_txr = NULL;
}

int
ice_tx_ring_program(ice_t *ice, ice_tx_ring_t *itr)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_tlan_ctx tlan;
	struct ice_aqc_add_tx_qgrp *qg;
	uint8_t ctx[ICE_TXQ_CTX_SZ];
	uint16_t qg_size;
	int status;

	bzero(&tlan, sizeof (tlan));

	/* base is the ring DMA address in 128-byte units. */
	tlan.base = ICE_DMA_PA(&itr->itxr_dma) >> ICE_TLAN_CTX_BASE_S;
	tlan.port_num = hw->port_info->lport;
	tlan.pf_num = hw->pf_id;
	tlan.vmvf_type = ICE_TLAN_CTX_VMVF_TYPE_PF;
	tlan.src_vsi = ice_get_hw_vsi_num(hw, vsi->vi_handle);
	tlan.qlen = itr->itxr_size;

	/*
	 * The LAN Tx queue context requires these for normal (legacy,
	 * non-context-descriptor) operation; int_q_state is hardware-owned and
	 * left zero.  Segmentation offload is not advertised yet, but tso_ena
	 * is part of the required legacy configuration.
	 */
	tlan.tso_ena = 1;
	tlan.tso_qnum = (uint16_t)itr->itxr_index;
	tlan.internal_usage_flag = 1;
	tlan.legacy_int = 1;

	/*
	 * ice_ena_vsi_txq() does not pack the context for us: bit-pack the
	 * sparse ice_tlan_ctx into the dense LAN Tx context and copy the valid
	 * leading bytes into the command's per-queue context field.
	 */
	bzero(ctx, sizeof (ctx));
	status = ice_set_ctx(hw, (uint8_t *)&tlan, ctx, ice_tlan_ctx_info);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to pack tx queue %u context: %d",
		    itr->itxr_index, status);
		return (status);
	}

	qg_size = ice_struct_size(qg, txqs, 1);
	qg = kmem_zalloc(qg_size, KM_SLEEP);
	qg->num_txqs = 1;
	qg->txqs[0].txq_id = CPU_TO_LE16((uint16_t)itr->itxr_index);
	bcopy(ctx, qg->txqs[0].txq_ctx, sizeof (qg->txqs[0].txq_ctx));

	/*
	 * One queue group, one queue.  The queue handle is the per-VSI queue
	 * index (ring i for the single PF VSI); it must be within the queue
	 * count reserved by ice_cfg_vsi_lan().
	 */
	mutex_enter(&ice->ice_lock);
	status = ice_ena_vsi_txq(hw->port_info, vsi->vi_handle, 0,
	    (uint16_t)itr->itxr_index, 1, qg, qg_size, NULL);
	if (status == ICE_SUCCESS)
		itr->itxr_q_teid = LE32_TO_CPU(qg->txqs[0].q_teid);
	mutex_exit(&ice->ice_lock);

	kmem_free(qg, qg_size);

	if (status != ICE_SUCCESS) {
		ice_error(ice, "ice_ena_vsi_txq failed for tx queue %u: %d",
		    itr->itxr_index, status);
		return (status);
	}

	return (ICE_SUCCESS);
}

void
ice_tx_ring_unprogram(ice_t *ice, ice_tx_ring_t *itr)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint16_t q_handle, q_id;
	uint32_t q_teid;
	int status;

	q_handle = (uint16_t)itr->itxr_index;
	q_id = (uint16_t)itr->itxr_index;
	q_teid = itr->itxr_q_teid;

	mutex_enter(&ice->ice_lock);
	status = ice_dis_vsi_txq(hw->port_info, ice->ice_pf_vsi.vi_handle, 0, 1,
	    &q_handle, &q_id, &q_teid, ICE_NO_RESET, 0, NULL);
	mutex_exit(&ice->ice_lock);

	if (status != ICE_SUCCESS) {
		ice_error(ice, "!ice_dis_vsi_txq failed for tx queue %u: %d",
		    itr->itxr_index, status);
	}
}

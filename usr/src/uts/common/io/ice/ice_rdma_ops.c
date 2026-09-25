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
 * The operations of the RDMA peer interface (ice_rdma.h), which the irdma
 * child calls.  Each one that sends an admin queue command takes
 * ice_rebuild_lock and fails while a reset is owed or running.
 */

#include <sys/atomic.h>
#include <sys/mman.h>

#include "ice_rdma_impl.h"
#include "ice_common.h"
#include "ice_switch.h"
#include "ice_sched.h"

/*
 * The admin queue gate for a peer operation.  The caller holds
 * ice_rebuild_lock.
 */
static int
ice_rdma_hw_ok(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;
	int ret = 0;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	mutex_enter(&ir->ir_lock);
	if (!ice_rdma_client_ok(ir))
		ret = EIO;
	mutex_exit(&ir->ir_lock);
	if (ret == 0 && (ice->ice_detaching || ice->ice_hw.reset_ongoing ||
	    (ice->ice_state & ICE_RDMA_DOWN) != 0 || !ice->ice_pf_vsi.vi_added))
		ret = EAGAIN;
	return (ret);
}
/*
 * Peer operations.
 */

static void
ice_rdma_qos_tc0(ice_rdma_qos_t *qos)
{
	bzero(qos, sizeof (*qos));
	qos->irq_num_tc = 1;
	qos->irq_tc[0].irt_rel_bw = 100;
}

static int
ice_rdma_op_open(ice_rdma_peer_t *peer, const ice_rdma_client_t *client,
    void *arg, ice_rdma_info_t *info)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	struct ice_hw *hw = &ice->ice_hw;
	int ret = 0;

	if (client == NULL || client->irc_event == NULL || info == NULL)
		return (EINVAL);

	mutex_enter(&ice->ice_rebuild_lock);
	if (ice->ice_attaching || ice->ice_detaching || hw->reset_ongoing ||
	    (ice->ice_state & ICE_RDMA_DOWN) != 0 ||
	    !ice->ice_pf_vsi.vi_added || hw->port_info == NULL) {
		mutex_exit(&ice->ice_rebuild_lock);
		return (EAGAIN);
	}

	mutex_enter(&ir->ir_lock);
	if (ir->ir_vectors == 0) {
		ret = ENOTSUP;
	} else if (ir->ir_stopping || ir->ir_resetting) {
		ret = EAGAIN;
	} else if (ir->ir_client != NULL) {
		ret = EBUSY;
	}
	if (ret != 0) {
		mutex_exit(&ir->ir_lock);
		mutex_exit(&ice->ice_rebuild_lock);
		return (ret);
	}

	bzero(info, sizeof (*info));
	info->iri_generation = ir->ir_gen;
	info->iri_bar0 = (caddr_t)hw->hw_addr;
	info->iri_bar0_size = ice->ice_osdep.ios_reg_size;
	info->iri_bar0_handle = ice->ice_osdep.ios_reg_handle;
	info->iri_pf_id = hw->pf_id;
	info->iri_vsi_num = ice->ice_pf_vsi.vi_hw_num;
	info->iri_mtu = ice->ice_mtu;
	bcopy(hw->port_info->mac.perm_addr, info->iri_mac, ETHERADDRL);
	ice_rdma_qos_tc0(&info->iri_qos);

	mutex_enter(&ice->ice_lse_lock);
	ir->ir_link = ice_link_state_effective(ice, ice->ice_link_state);
	ir->ir_speed = ice->ice_link_speed;
	mutex_exit(&ice->ice_lse_lock);
	info->iri_link = ir->ir_link;
	info->iri_speed = ir->ir_speed;

	ir->ir_client = client;
	ir->ir_client_arg = arg;
	ir->ir_client_gen = ir->ir_gen;
	ir->ir_ev_pending = 0;
	ir->ir_ev_oicr = 0;
	mutex_exit(&ir->ir_lock);
	mutex_exit(&ice->ice_rebuild_lock);

	return (0);
}

/*
 * The client is going away.  Wait out callbacks, remove what it left behind,
 * and quarantine any buffer it did not free.
 */
static void
ice_rdma_op_close(ice_rdma_peer_t *peer)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	struct ice_hw *hw = &ice->ice_hw;
	ice_rdma_buf_t *irb;
	boolean_t hw_ok;
	uint_t i;

	mutex_enter(&ir->ir_lock);
	ir->ir_client = NULL;
	ir->ir_ev_pending = 0;
	while (ir->ir_cb_busy != 0)
		cv_wait(&ir->ir_cv, &ir->ir_lock);
	mutex_exit(&ir->ir_lock);

	mutex_enter(&ice->ice_rebuild_lock);
	mutex_enter(&ir->ir_lock);
	hw_ok = !ir->ir_resetting && !ice->ice_hw.reset_ongoing &&
	    (ice->ice_state & ICE_RDMA_DOWN) == 0 && ice->ice_pf_vsi.vi_added;
	mutex_exit(&ir->ir_lock);

	for (i = 0; i < ICE_RDMA_QSET_TABLE; i++) {
		ice_rdma_qrec_t *q = &ir->ir_qsets[i];

		if (!q->iqr_used)
			continue;
		if (hw_ok && ice_dis_vsi_rdma_qset(hw->port_info, 1,
		    &q->iqr_teid, &q->iqr_handle) != ICE_SUCCESS) {
			atomic_or_32(&ice->ice_state, ICE_STATE_PFR_REQ);
			hw_ok = B_FALSE;
		}
		bzero(q, sizeof (*q));
	}
	ir->ir_nqsets = 0;

	if (ir->ir_pe_fltr) {
		if (hw_ok && ice_cfg_iwarp_fltr(hw, ICE_PF_VSI_HANDLE,
		    false) != ICE_SUCCESS)
			atomic_or_32(&ice->ice_state, ICE_STATE_PFR_REQ);
		ir->ir_pe_fltr = B_FALSE;
	}
	ice_reset_redispatch(ice);
	mutex_exit(&ice->ice_rebuild_lock);

	mutex_enter(&ir->ir_lock);
	while ((irb = list_remove_head(&ir->ir_bufs)) != NULL) {
		ir->ir_nbufs--;
		ir->ir_dma_bytes -= irb->irb_pub.ird_len;
		ir->ir_nquar++;
		ir->ir_quar_bytes += irb->irb_pub.ird_len;
		list_insert_tail(&ir->ir_quarantine, irb);
	}
	mutex_exit(&ir->ir_lock);
}

static int
ice_rdma_op_intr_get(ice_rdma_peer_t *peer, ice_rdma_intr_t *intr)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;

	if (intr == NULL)
		return (EINVAL);
	if (ir->ir_vectors == 0)
		return (ENOTSUP);

	bzero(intr, sizeof (*intr));
	intr->irin_handles = &ice->ice_intr_handles[ICE_RDMA_FIRST_VECTOR];
	intr->irin_count = ir->ir_vectors;
	intr->irin_first = ICE_RDMA_FIRST_VECTOR;
	intr->irin_pri = ice->ice_intr_pri;
	intr->irin_cap = ice->ice_intr_cap;
	return (0);
}

static int
ice_rdma_qset_find(ice_rdma_t *ir, const ice_rdma_qset_t *qs)
{
	uint_t i;

	for (i = 0; i < ICE_RDMA_QSET_TABLE; i++) {
		const ice_rdma_qrec_t *q = &ir->ir_qsets[i];

		if (q->iqr_used && q->iqr_handle == qs->irqs_handle)
			return ((int)i);
	}
	return (-1);
}

/* Only TC0 is enabled; ice does not program DCB. */
static boolean_t
ice_rdma_tc_valid(uint8_t tc)
{
	return (tc < ICE_MAX_TRAFFIC_CLASS && tc == 0);
}

static int
ice_rdma_op_qset_add(ice_rdma_peer_t *peer, ice_rdma_qset_t *qs, uint_t n)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	struct ice_hw *hw = &ice->ice_hw;
	uint16_t max_rdmaqs[ICE_MAX_TRAFFIC_CLASS];
	uint_t i, j, slot[ICE_RDMA_MAX_QSETS];
	int status, ret;

	if (qs == NULL || n == 0 || n > ICE_RDMA_MAX_QSETS)
		return (EINVAL);

	mutex_enter(&ice->ice_rebuild_lock);
	if ((ret = ice_rdma_hw_ok(ice)) != 0)
		goto out;

	ret = EINVAL;
	if (ir->ir_nqsets + n > ICE_RDMA_QSET_TABLE) {
		ret = ENOSPC;
		goto out;
	}
	for (i = 0; i < n; i++) {
		if (!ice_rdma_tc_valid(qs[i].irqs_tc) ||
		    qs[i].irqs_vsi_num != ice->ice_pf_vsi.vi_hw_num ||
		    ice_rdma_qset_find(ir, &qs[i]) >= 0)
			goto out;
		for (j = 0; j < i; j++) {
			if (qs[j].irqs_handle == qs[i].irqs_handle)
				goto out;
		}
	}

	bzero(max_rdmaqs, sizeof (max_rdmaqs));
	max_rdmaqs[0] = (uint16_t)(ir->ir_nqsets + n);
	status = ice_cfg_vsi_rdma(hw->port_info, ICE_PF_VSI_HANDLE, BIT(0),
	    max_rdmaqs);
	if (status != ICE_SUCCESS) {
		ret = ice_status_to_errno(ice, status);
		goto out;
	}

	for (i = 0, j = 0; i < n; i++) {
		uint32_t teid = 0;
		uint16_t handle = qs[i].irqs_handle;

		status = ice_ena_vsi_rdma_qset(hw->port_info, ICE_PF_VSI_HANDLE,
		    qs[i].irqs_tc, &handle, 1, &teid);
		if (status != ICE_SUCCESS) {
			ret = ice_status_to_errno(ice, status);
			break;
		}
		while (ir->ir_qsets[j].iqr_used)
			j++;
		ir->ir_qsets[j].iqr_used = B_TRUE;
		ir->ir_qsets[j].iqr_handle = qs[i].irqs_handle;
		ir->ir_qsets[j].iqr_tc = qs[i].irqs_tc;
		ir->ir_qsets[j].iqr_teid = teid;
		ir->ir_nqsets++;
		slot[i] = j;
		qs[i].irqs_teid = teid;
	}

	if (i == n) {
		ret = 0;
		goto out;
	}

	/* Remove the qsets this call added. */
	while (i-- > 0) {
		ice_rdma_qrec_t *q = &ir->ir_qsets[slot[i]];

		if (ice_dis_vsi_rdma_qset(hw->port_info, 1, &q->iqr_teid,
		    &q->iqr_handle) != ICE_SUCCESS) {
			atomic_or_32(&ice->ice_state, ICE_STATE_PFR_REQ);
			ice_reset_redispatch(ice);
		}
		bzero(q, sizeof (*q));
		ir->ir_nqsets--;
		qs[i].irqs_teid = 0;
	}
out:
	mutex_exit(&ice->ice_rebuild_lock);
	return (ret);
}

/*
 * Remove qsets the client added.  Every entry must name a qset this client
 * owns, with the TEID ice returned for it, or nothing is removed.  A failed
 * removal retires the record and owes a PF reset.
 */
static int
ice_rdma_op_qset_del(ice_rdma_peer_t *peer, ice_rdma_qset_t *qs, uint_t n)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	struct ice_hw *hw = &ice->ice_hw;
	int idx[ICE_RDMA_MAX_QSETS];
	uint_t i, j;
	int ret;

	if (qs == NULL || n == 0 || n > ICE_RDMA_MAX_QSETS)
		return (EINVAL);

	mutex_enter(&ice->ice_rebuild_lock);
	if ((ret = ice_rdma_hw_ok(ice)) != 0)
		goto out;

	for (i = 0; i < n; i++) {
		idx[i] = ice_rdma_qset_find(ir, &qs[i]);
		if (idx[i] < 0 ||
		    ir->ir_qsets[idx[i]].iqr_teid != qs[i].irqs_teid ||
		    ir->ir_qsets[idx[i]].iqr_tc != qs[i].irqs_tc ||
		    qs[i].irqs_vsi_num != ice->ice_pf_vsi.vi_hw_num) {
			ret = EINVAL;
			goto out;
		}
		for (j = 0; j < i; j++) {
			if (idx[j] == idx[i]) {
				ret = EINVAL;
				goto out;
			}
		}
	}

	for (i = 0; i < n; i++) {
		ice_rdma_qrec_t *q = &ir->ir_qsets[idx[i]];

		if (ice_dis_vsi_rdma_qset(hw->port_info, 1, &q->iqr_teid,
		    &q->iqr_handle) != ICE_SUCCESS) {
			ret = EIO;
			atomic_or_32(&ice->ice_state, ICE_STATE_PFR_REQ);
			ice_reset_redispatch(ice);
		}
		bzero(q, sizeof (*q));
		ir->ir_nqsets--;
	}
out:
	mutex_exit(&ice->ice_rebuild_lock);
	return (ret);
}

static int
ice_rdma_op_pe_filter(ice_rdma_peer_t *peer, boolean_t enable)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	int status, ret;

	mutex_enter(&ice->ice_rebuild_lock);
	if ((ret = ice_rdma_hw_ok(ice)) == 0) {
		status = ice_cfg_iwarp_fltr(&ice->ice_hw, ICE_PF_VSI_HANDLE,
		    enable != B_FALSE);
		if (status == ICE_SUCCESS)
			ir->ir_pe_fltr = enable;
		else
			ret = ice_status_to_errno(ice, status);
	}
	mutex_exit(&ice->ice_rebuild_lock);
	return (ret);
}

static int
ice_rdma_op_reset(ice_rdma_peer_t *peer, ice_rdma_reset_t type)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;

	if (type != ICE_RDMA_RESET_PF)
		return (EINVAL);

	mutex_enter(&ir->ir_lock);
	if (ir->ir_client == NULL || ir->ir_client_gen != ir->ir_gen) {
		mutex_exit(&ir->ir_lock);
		return (EIO);
	}
	ir->ir_reset_requests++;
	mutex_exit(&ir->ir_lock);

	atomic_or_32(&ice->ice_state, ICE_STATE_ERROR | ICE_STATE_PFR_REQ);
	ice_reset_dispatch(ice);
	return (0);
}

static boolean_t
ice_rdma_op_resetting(ice_rdma_peer_t *peer)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	boolean_t ret;

	mutex_enter(&ir->ir_lock);
	ret = ir->ir_resetting || ir->ir_client_gen != ir->ir_gen;
	mutex_exit(&ir->ir_lock);

	return (ret || ice->ice_hw.reset_ongoing ||
	    (ice->ice_state & ICE_RDMA_DOWN) != 0);
}

static int
ice_rdma_op_dma_alloc(ice_rdma_peer_t *peer, size_t len, size_t align,
    ice_rdma_dma_t **dmap)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	ddi_device_acc_attr_t acc;
	ddi_dma_attr_t attr;
	ice_rdma_buf_t *irb;

	if (dmap == NULL || len == 0 || len > ICE_RDMA_DMA_MAX_LEN ||
	    align == 0 || !ISP2(align) || align > ICE_RDMA_DMA_MAX_ALIGN)
		return (EINVAL);
	*dmap = NULL;

	mutex_enter(&ir->ir_lock);
	if (!ice_rdma_client_ok(ir)) {
		mutex_exit(&ir->ir_lock);
		return (EIO);
	}
	if (ir->ir_dma_bytes + ir->ir_quar_bytes + len > ICE_RDMA_DMA_LIMIT) {
		mutex_exit(&ir->ir_lock);
		return (ENOMEM);
	}
	/* Hold the bytes against the limit while allocating. */
	ir->ir_dma_bytes += len;
	mutex_exit(&ir->ir_lock);

	irb = kmem_zalloc(sizeof (*irb), KM_SLEEP);
	ice_dma_ring_attr(ice, &attr);
	attr.dma_attr_align = MAX(align, ICE_DESC_ALIGN);
	ice_dma_acc_attr(ice, &acc);
	if (!ice_dma_alloc(ice, &irb->irb_dma, &attr, &acc, B_TRUE, len,
	    B_FALSE)) {
		kmem_free(irb, sizeof (*irb));
		mutex_enter(&ir->ir_lock);
		ir->ir_dma_bytes -= len;
		mutex_exit(&ir->ir_lock);
		return (ENOMEM);
	}
	irb->irb_pub.ird_va = irb->irb_dma.idb_va;
	irb->irb_pub.ird_pa = ICE_DMA_PA(&irb->irb_dma);
	irb->irb_pub.ird_len = len;

	mutex_enter(&ir->ir_lock);
	list_insert_tail(&ir->ir_bufs, irb);
	ir->ir_nbufs++;
	mutex_exit(&ir->ir_lock);

	*dmap = &irb->irb_pub;
	return (0);
}

/*
 * Free a buffer now if the client says the device is done with it and no
 * reset is under way; otherwise keep it until the next reset completes.  An
 * unknown pointer is ignored.
 */
static void
ice_rdma_op_dma_free(ice_rdma_peer_t *peer, ice_rdma_dma_t *dma,
    boolean_t quiesced)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	ice_rdma_buf_t *irb;
	boolean_t now;

	if (dma == NULL)
		return;

	mutex_enter(&ir->ir_lock);
	for (irb = list_head(&ir->ir_bufs); irb != NULL;
	    irb = list_next(&ir->ir_bufs, irb)) {
		if (&irb->irb_pub == dma)
			break;
	}
	if (irb == NULL) {
		mutex_exit(&ir->ir_lock);
		ice_error(ice, "RDMA freed an unknown DMA buffer %p",
		    (void *)dma);
		return;
	}
	list_remove(&ir->ir_bufs, irb);
	ir->ir_nbufs--;
	ir->ir_dma_bytes -= irb->irb_pub.ird_len;

	now = quiesced && !ir->ir_resetting && !ice->ice_hw.reset_ongoing &&
	    (ice->ice_state & ICE_RDMA_DOWN) == 0;
	if (!now) {
		ir->ir_nquar++;
		ir->ir_quar_bytes += irb->irb_pub.ird_len;
		list_insert_tail(&ir->ir_quarantine, irb);
	}
	mutex_exit(&ir->ir_lock);

	if (now)
		ice_rdma_buf_free(irb);
}

static int
ice_rdma_op_acc_check(ice_rdma_peer_t *peer)
{
	ice_t *ice = ice_rdma_peer_ice(peer);

	return (ice_check_acc_handle(ice, ice->ice_osdep.ios_reg_handle));
}

/*
 * Post an ereport and a service impact on behalf of the child, which has no
 * FMA state of its own.  Only a few classes are accepted.
 */
static void
ice_rdma_op_fm_report(ice_rdma_peer_t *peer, const char *detail, int impact)
{
	static const char *classes[] = {
		DDI_FM_DEVICE_NO_RESPONSE,
		DDI_FM_DEVICE_INVAL_STATE,
		DDI_FM_DEVICE_INTERN_UNCORR,
		DDI_FM_DEVICE_STALL
	};
	ice_t *ice = ice_rdma_peer_ice(peer);
	char class[FM_MAX_CLASS];
	uint_t i;

	for (i = 0; i < ARRAY_SIZE(classes); i++) {
		if (detail != NULL && strcmp(detail, classes[i]) == 0)
			break;
	}
	if (i < ARRAY_SIZE(classes) && DDI_FM_EREPORT_CAP(ice->ice_fm_caps)) {
		(void) snprintf(class, sizeof (class), "%s.%s", DDI_FM_DEVICE,
		    classes[i]);
		ddi_fm_ereport_post(ice->ice_dip, class,
		    fm_ena_generate(0, FM_ENA_FMT1), DDI_NOSLEEP,
		    FM_VERSION, DATA_TYPE_UINT8, FM_EREPORT_VERS0,
		    "detail", DATA_TYPE_STRING, "rdma", NULL);
	}

	switch (impact) {
	case DDI_SERVICE_LOST:
	case DDI_SERVICE_DEGRADED:
	case DDI_SERVICE_UNAFFECTED:
	case DDI_SERVICE_RESTORED:
		ddi_fm_service_impact(ice->ice_dip, impact);
		break;
	default:
		break;
	}
}

/*
 * Map part of the doorbell and push page window of BAR0 for a later user
 * context.  The range must be whole pages inside the window.
 */
static int
ice_rdma_op_devmap(ice_rdma_peer_t *peer, devmap_cookie_t dhp, offset_t off,
    size_t len, uint_t flags)
{
	ice_t *ice = ice_rdma_peer_ice(peer);
	ice_rdma_t *ir = ice->ice_rdma;
	ddi_device_acc_attr_t acc;
	boolean_t ok;

	if (off < ICE_RDMA_DEVMAP_BASE || len == 0 ||
	    (off & PAGEOFFSET) != 0 || (len & PAGEOFFSET) != 0 ||
	    len > ICE_RDMA_DEVMAP_END - ICE_RDMA_DEVMAP_BASE ||
	    off > ICE_RDMA_DEVMAP_END - len ||
	    off + len > ice->ice_osdep.ios_reg_size)
		return (EINVAL);
	if ((flags & ~(DEVMAP_DEFAULTS | DEVMAP_MAPPING_INVALID)) != 0)
		return (EINVAL);

	mutex_enter(&ir->ir_lock);
	ok = ice_rdma_client_ok(ir);
	mutex_exit(&ir->ir_lock);
	if (!ok)
		return (EIO);

	acc.devacc_attr_version = DDI_DEVICE_ATTR_V0;
	acc.devacc_attr_endian_flags = DDI_STRUCTURE_LE_ACC;
	acc.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
	acc.devacc_attr_access = DDI_DEFAULT_ACC;

	return (devmap_devmem_setup(dhp, ice->ice_dip, NULL, ICE_REG_NUMBER,
	    off, len, PROT_READ | PROT_WRITE | PROT_USER, flags, &acc));
}

static int
ice_rdma_op_test(ice_rdma_peer_t *peer, ice_rdma_test_t test, uint_t count)
{
#ifdef DEBUG
	ice_t *ice = ice_rdma_peer_ice(peer);

	switch (test) {
	case ICE_RDMA_TEST_IRM_REMOVE:
		return (ice_intr_adjust(ice, DDI_CB_INTR_REMOVE, (int)count) ==
		    DDI_SUCCESS ? 0 : EIO);
	case ICE_RDMA_TEST_IRM_ADD:
		return (ice_intr_adjust(ice, DDI_CB_INTR_ADD, (int)count) ==
		    DDI_SUCCESS ? 0 : EIO);
	default:
		return (EINVAL);
	}
#else
	_NOTE(ARGUNUSED(peer, test, count));
	return (ENOTSUP);
#endif
}

const ice_rdma_ops_t ice_rdma_ops = {
	.iro_open = ice_rdma_op_open,
	.iro_close = ice_rdma_op_close,
	.iro_intr_get = ice_rdma_op_intr_get,
	.iro_qset_add = ice_rdma_op_qset_add,
	.iro_qset_del = ice_rdma_op_qset_del,
	.iro_pe_filter = ice_rdma_op_pe_filter,
	.iro_reset = ice_rdma_op_reset,
	.iro_resetting = ice_rdma_op_resetting,
	.iro_dma_alloc = ice_rdma_op_dma_alloc,
	.iro_dma_free = ice_rdma_op_dma_free,
	.iro_acc_check = ice_rdma_op_acc_check,
	.iro_fm_report = ice_rdma_op_fm_report,
	.iro_devmap = ice_rdma_op_devmap,
	.iro_test = ice_rdma_op_test
};

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
 * The RDMA peer interface: ice(4D) side.  ice_rdma.h describes the contract.
 * The RDMA function driver attaches as the child node "irdma@0" of the PF.
 *
 * RDMA is used only when firmware reports the capability, the DDP package is
 * loaded and the rdma_enable property is set.  ice_alloc_intrs() then keeps
 * MSI-X vectors 1 through ice_intr_rdma for the child; the LAN queue vectors
 * follow them, so interrupt resource management trims LAN vectors first
 * (ice_intr_adjust_locked()).
 *
 * Locks: ir_cfg_lock serializes the child's online and offline and is taken
 * with no other ice lock held.  ir_lock follows ice_rebuild_lock and
 * precedes ice_lse_lock.  Neither is held across a call into the child.
 *
 * Resets: the reset worker marks the peer resetting, takes the child offline
 * with no lifecycle lock held, runs the rebuild, and brings the child back.
 * A buffer the child frees while the device may still use it goes to the
 * quarantine, which ice empties only after a reset completes.  If the child
 * cannot be taken offline, it gets RESET_PREP before the reset and
 * RESET_DONE after it, and its generation is stale from then on.
 */

#include <sys/sunndi.h>
#include <sys/atomic.h>

#include "ice_rdma_impl.h"
#include "ice_common.h"
#include "ice_switch.h"

#define	ICE_RDMA_NODE_NAME	"irdma"

/*
 * Whether the peer may issue commands for the client now.  The caller holds
 * ir_lock.
 */
boolean_t
ice_rdma_client_ok(ice_rdma_t *ir)
{
	ASSERT(MUTEX_HELD(&ir->ir_lock));
	return (ir->ir_client != NULL && ir->ir_client_gen == ir->ir_gen &&
	    !ir->ir_resetting && !ir->ir_stopping);
}


void
ice_rdma_buf_free(ice_rdma_buf_t *irb)
{
	ice_dma_free(&irb->irb_dma);
	kmem_free(irb, sizeof (*irb));
}

/*
 * Free the quarantined buffers.  The caller has proven that the device can
 * no longer reach them: a reset completed.
 */
static void
ice_rdma_quarantine_free(ice_rdma_t *ir)
{
	ice_rdma_buf_t *irb;
	list_t done;

	list_create(&done, sizeof (ice_rdma_buf_t),
	    offsetof(ice_rdma_buf_t, irb_node));
	mutex_enter(&ir->ir_lock);
	list_move_tail(&done, &ir->ir_quarantine);
	ir->ir_quar_freed += ir->ir_nquar;
	ir->ir_nquar = 0;
	ir->ir_quar_bytes = 0;
	mutex_exit(&ir->ir_lock);

	while ((irb = list_remove_head(&done)) != NULL)
		ice_rdma_buf_free(irb);
	list_destroy(&done);
}

static int
ice_rdma_kstat_update(kstat_t *ksp, int rw)
{
	ice_rdma_t *ir = ksp->ks_private;
	ice_rdma_kstats_t *k = &ir->ir_kstats;

	if (rw == KSTAT_WRITE)
		return (EACCES);

	mutex_enter(&ir->ir_lock);
	k->irk_state.value.ui32 = ir->ir_client != NULL ? 2 :
	    (ir->ir_resetting ? 1 : 0);
	k->irk_vectors.value.ui32 = ir->ir_vectors;
	k->irk_generation.value.ui32 = ir->ir_gen;
	k->irk_qsets.value.ui32 = ir->ir_nqsets;
	k->irk_dma_bufs.value.ui32 = ir->ir_nbufs;
	k->irk_dma_bytes.value.ui64 = ir->ir_dma_bytes;
	k->irk_quar_bufs.value.ui32 = ir->ir_nquar;
	k->irk_quar_bytes.value.ui64 = ir->ir_quar_bytes;
	k->irk_quar_freed.value.ui64 = ir->ir_quar_freed;
	k->irk_events.value.ui64 = ir->ir_events;
	k->irk_reset_requests.value.ui64 = ir->ir_reset_requests;
	k->irk_offline_fail.value.ui64 = ir->ir_offline_fail;
	k->irk_crit_errors.value.ui64 = ir->ir_crit_errors;
	mutex_exit(&ir->ir_lock);

	return (0);
}

static void
ice_rdma_kstat_init(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;
	ice_rdma_kstats_t *k = &ir->ir_kstats;
	kstat_t *ksp;

	ksp = kstat_create(ICE_MODULE_NAME, ice->ice_instance, "rdma", "net",
	    KSTAT_TYPE_NAMED, sizeof (*k) / sizeof (kstat_named_t), 0);
	if (ksp == NULL) {
		ice_error(ice, "failed to create the rdma kstat");
		return;
	}
	ksp->ks_data = k;
	ksp->ks_private = ir;
	ksp->ks_update = ice_rdma_kstat_update;

	kstat_named_init(&k->irk_state, "state", KSTAT_DATA_UINT32);
	kstat_named_init(&k->irk_vectors, "vectors", KSTAT_DATA_UINT32);
	kstat_named_init(&k->irk_generation, "generation", KSTAT_DATA_UINT32);
	kstat_named_init(&k->irk_qsets, "qsets", KSTAT_DATA_UINT32);
	kstat_named_init(&k->irk_dma_bufs, "dma_bufs", KSTAT_DATA_UINT32);
	kstat_named_init(&k->irk_dma_bytes, "dma_bytes", KSTAT_DATA_UINT64);
	kstat_named_init(&k->irk_quar_bufs, "quarantine_bufs",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&k->irk_quar_bytes, "quarantine_bytes",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&k->irk_quar_freed, "quarantine_freed",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&k->irk_events, "events", KSTAT_DATA_UINT64);
	kstat_named_init(&k->irk_reset_requests, "reset_requests",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&k->irk_offline_fail, "offline_failures",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&k->irk_crit_errors, "crit_errors", KSTAT_DATA_UINT64);

	kstat_install(ksp);
	ir->ir_kstat = ksp;
}

/*
 * Allocate the peer state once ice_alloc_intrs() has granted the RDMA block;
 * without it RDMA stays off.
 */
void
ice_rdma_attach(ice_t *ice)
{
	ice_rdma_t *ir;

	if (ice->ice_intr_rdma == 0)
		return;

	ir = kmem_zalloc(sizeof (*ir), KM_SLEEP);
	ir->ir_peer.irp_hdr.irp_version = ICE_RDMA_VERSION;
	ir->ir_peer.irp_hdr.irp_ops = &ice_rdma_ops;
	ir->ir_peer.irp_ice = ice;
	ir->ir_vectors = ice->ice_intr_rdma;
	mutex_init(&ir->ir_cfg_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&ir->ir_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ir->ir_cv, NULL, CV_DRIVER, NULL);
	list_create(&ir->ir_bufs, sizeof (ice_rdma_buf_t),
	    offsetof(ice_rdma_buf_t, irb_node));
	list_create(&ir->ir_quarantine, sizeof (ice_rdma_buf_t),
	    offsetof(ice_rdma_buf_t, irb_node));
	ir->ir_link = LINK_STATE_UNKNOWN;
	ice->ice_rdma = ir;
}

/*
 * Finish RDMA setup at the end of attach and bring the child online from the
 * reset taskq, which also serializes it with the reset worker.
 */
void
ice_rdma_start(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;

	if (ir == NULL)
		return;

	ir->ir_evtq = ddi_taskq_create(ice->ice_dip, "ice_rdma_ev", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (ir->ir_evtq == NULL) {
		ice_error(ice, "failed to create the RDMA event taskq; "
		    "RDMA disabled");
		ir->ir_vectors = 0;
		return;
	}
	ice_rdma_kstat_init(ice);

	if (ddi_taskq_dispatch(ice->ice_reset_taskq, ice_rdma_online_task,
	    ice, DDI_SLEEP) != DDI_SUCCESS)
		ice_error(ice, "failed to dispatch the RDMA online task");
}

/*
 * Create the child node if needed and attach it.  A missing irdma driver
 * leaves the node unbound until the driver is added.
 */
static void
ice_rdma_online(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;
	dev_info_t *cdip;
	boolean_t stop;

	ASSERT(MUTEX_HELD(&ir->ir_cfg_lock));

	mutex_enter(&ir->ir_lock);
	stop = ir->ir_stopping || ir->ir_resetting;
	mutex_exit(&ir->ir_lock);
	if (stop)
		return;

	if ((cdip = ir->ir_cdip) == NULL) {
		if (ndi_devi_alloc(ice->ice_dip, ICE_RDMA_NODE_NAME,
		    (pnode_t)DEVI_SID_NODEID, &cdip) != NDI_SUCCESS) {
			ice_error(ice, "failed to allocate the RDMA node");
			return;
		}
		ddi_set_parent_data(cdip, &ir->ir_peer);
		ir->ir_cdip = cdip;
	}

	if (ndi_devi_online(cdip, 0) != NDI_SUCCESS) {
		dev_err(ice->ice_dip, CE_NOTE, "!RDMA function not attached; "
		    "is the irdma driver installed?");
	}
}

void
ice_rdma_online_task(void *arg)
{
	ice_t *ice = arg;
	ice_rdma_t *ir = ice->ice_rdma;

	mutex_enter(&ir->ir_cfg_lock);
	ice_rdma_online(ice);
	mutex_exit(&ir->ir_cfg_lock);
}

/*
 * Take the child offline.  Returns B_FALSE if it stays attached.  With
 * remove, the node is also freed.
 */
static boolean_t
ice_rdma_offline(ice_t *ice, boolean_t remove)
{
	ice_rdma_t *ir = ice->ice_rdma;
	int rc;

	ASSERT(MUTEX_HELD(&ir->ir_cfg_lock));

	if (ir->ir_cdip == NULL)
		return (B_TRUE);

	rc = ndi_devi_offline(ir->ir_cdip, remove ? NDI_DEVI_REMOVE : 0);
	if (rc != NDI_SUCCESS) {
		mutex_enter(&ir->ir_lock);
		ir->ir_offline_fail++;
		mutex_exit(&ir->ir_lock);
		ice_error(ice, "failed to take the RDMA function offline: %d",
		    rc);
		return (B_FALSE);
	}
	if (remove)
		ir->ir_cdip = NULL;
	return (B_TRUE);
}

/*
 * Deliver one event to the client now.  The caller holds no ice lock.
 */
static void
ice_rdma_deliver(ice_rdma_t *ir, const ice_rdma_event_t *ev)
{
	const ice_rdma_client_t *client;
	void *arg;

	mutex_enter(&ir->ir_lock);
	if ((client = ir->ir_client) == NULL) {
		mutex_exit(&ir->ir_lock);
		return;
	}
	arg = ir->ir_client_arg;
	ir->ir_cb_busy++;
	ir->ir_events++;
	mutex_exit(&ir->ir_lock);

	client->irc_event(arg, ev);

	mutex_enter(&ir->ir_lock);
	if (--ir->ir_cb_busy == 0)
		cv_broadcast(&ir->ir_cv);
	mutex_exit(&ir->ir_lock);
}

static void
ice_rdma_event_task(void *arg)
{
	ice_t *ice = arg;
	ice_rdma_t *ir = ice->ice_rdma;
	ice_rdma_event_t ev;
	uint32_t pending, oicr;
	link_state_t link;
	uint64_t speed;

	mutex_enter(&ir->ir_lock);
	pending = ir->ir_ev_pending;
	oicr = ir->ir_ev_oicr;
	link = ir->ir_link;
	speed = ir->ir_speed;
	ir->ir_ev_pending = 0;
	ir->ir_ev_oicr = 0;
	ir->ir_ev_queued = B_FALSE;
	mutex_exit(&ir->ir_lock);

	if ((pending & ICE_RDMA_EVP_CRIT) != 0) {
		bzero(&ev, sizeof (ev));
		ev.ire_type = ICE_RDMA_EV_CRIT_ERR;
		ev.ire_oicr = oicr;
		ice_rdma_deliver(ir, &ev);
	}
	if ((pending & ICE_RDMA_EVP_LINK) != 0) {
		bzero(&ev, sizeof (ev));
		ev.ire_type = ICE_RDMA_EV_LINK;
		ev.ire_link = link;
		ev.ire_speed = speed;
		ice_rdma_deliver(ir, &ev);
	}
	if ((pending & ICE_RDMA_EVP_MTU) != 0) {
		bzero(&ev, sizeof (ev));
		ev.ire_type = ICE_RDMA_EV_MTU;
		ev.ire_mtu = ice->ice_mtu;
		ice_rdma_deliver(ir, &ev);
	}
}

/* Queue coalesced events.  The caller holds ir_lock. */
static void
ice_rdma_event_post(ice_t *ice, uint32_t what)
{
	ice_rdma_t *ir = ice->ice_rdma;

	ASSERT(MUTEX_HELD(&ir->ir_lock));

	if (ir->ir_client == NULL || ir->ir_evtq == NULL)
		return;
	ir->ir_ev_pending |= what;
	if (ir->ir_ev_queued)
		return;
	if (ddi_taskq_dispatch(ir->ir_evtq, ice_rdma_event_task, ice,
	    DDI_NOSLEEP) == DDI_SUCCESS)
		ir->ir_ev_queued = B_TRUE;
}

/* Report the cached link state if it changed.  Thread context. */
void
ice_rdma_link_notify(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;
	link_state_t link;
	uint64_t speed;

	if (ir == NULL)
		return;

	mutex_enter(&ice->ice_lse_lock);
	link = ice_link_state_effective(ice, ice->ice_link_state);
	speed = ice->ice_link_speed;
	mutex_exit(&ice->ice_lse_lock);

	mutex_enter(&ir->ir_lock);
	if (link != ir->ir_link || speed != ir->ir_speed) {
		ir->ir_link = link;
		ir->ir_speed = speed;
		ice_rdma_event_post(ice, ICE_RDMA_EVP_LINK);
	}
	mutex_exit(&ir->ir_lock);
}

void
ice_rdma_mtu_notify(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;

	if (ir == NULL)
		return;

	mutex_enter(&ir->ir_lock);
	ice_rdma_event_post(ice, ICE_RDMA_EVP_MTU);
	mutex_exit(&ir->ir_lock);
}

/*
 * Tell the child about a PE critical or HMC error.  ice still resets the
 * function itself, so the child's own reset request is not needed.
 */
void
ice_rdma_crit_notify(ice_t *ice, uint32_t oicr)
{
	ice_rdma_t *ir = ice->ice_rdma;

	oicr &= PFINT_OICR_PE_CRITERR_M | PFINT_OICR_HMC_ERR_M;
	if (ir == NULL || oicr == 0)
		return;

	mutex_enter(&ir->ir_lock);
	ir->ir_crit_errors++;
	ir->ir_ev_oicr |= oicr;
	ice_rdma_event_post(ice, ICE_RDMA_EVP_CRIT);
	mutex_exit(&ir->ir_lock);
}

/*
 * Reset worker, before the rebuild, with no ice lock held.  Bump the
 * generation so the client's handles go stale, then take the child offline.
 * If it stays attached, tell it to stop.
 */
void
ice_rdma_reset_prepare(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;
	ice_rdma_event_t ev;
	boolean_t attached;

	if (ir == NULL)
		return;

	mutex_enter(&ir->ir_cfg_lock);
	mutex_enter(&ir->ir_lock);
	ir->ir_resetting = B_TRUE;
	ir->ir_gen++;
	attached = ir->ir_client != NULL;
	mutex_exit(&ir->ir_lock);

	if (attached) {
		ir->ir_online_owed = B_TRUE;
		if (!ice_rdma_offline(ice, B_FALSE)) {
			bzero(&ev, sizeof (ev));
			ev.ire_type = ICE_RDMA_EV_RESET_PREP;
			ice_rdma_deliver(ir, &ev);
		}
	}
	mutex_exit(&ir->ir_cfg_lock);
}

/*
 * Called from the rebuild once the reset is known to have completed: no
 * quarantined buffer can be reached by the device any longer.
 */
void
ice_rdma_reset_barrier(ice_t *ice)
{
	if (ice->ice_rdma != NULL)
		ice_rdma_quarantine_free(ice->ice_rdma);
}

/*
 * Reset worker, after the rebuild, with no ice lock held.  Bring the child
 * back unless the reset failed.  A child that never went offline gets
 * RESET_DONE and is offered the offline again.
 */
void
ice_rdma_reset_done(ice_t *ice, boolean_t ok)
{
	ice_rdma_t *ir = ice->ice_rdma;
	ice_rdma_event_t ev;
	boolean_t stale;

	if (ir == NULL)
		return;

	mutex_enter(&ir->ir_cfg_lock);
	mutex_enter(&ir->ir_lock);
	ir->ir_resetting = B_FALSE;
	stale = ir->ir_client != NULL && ir->ir_client_gen != ir->ir_gen;
	mutex_exit(&ir->ir_lock);

	if (stale) {
		bzero(&ev, sizeof (ev));
		ev.ire_type = ICE_RDMA_EV_RESET_DONE;
		ice_rdma_deliver(ir, &ev);
		(void) ice_rdma_offline(ice, B_FALSE);
	}

	if (ok && ir->ir_online_owed) {
		ir->ir_online_owed = B_FALSE;
		ice_rdma_online(ice);
	}
	mutex_exit(&ir->ir_cfg_lock);
}

/*
 * Replay the peer's VSI state after the rebuild added the VSI again.  A
 * failure leaves the filter off; the child finds out when it opens again.
 * The caller holds ice_rebuild_lock.
 */
void
ice_rdma_vsi_replay(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;
	int status;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	if (ir == NULL)
		return;

	/* The reset removed every qset; the records are stale. */
	bzero(ir->ir_qsets, sizeof (ir->ir_qsets));
	ir->ir_nqsets = 0;

	if (!ir->ir_pe_fltr)
		return;
	status = ice_cfg_iwarp_fltr(&ice->ice_hw, ICE_PF_VSI_HANDLE, true);
	if (status != ICE_SUCCESS) {
		ir->ir_pe_fltr = B_FALSE;
		ice_error(ice, "failed to replay the PE filter: %d", status);
	}
}

/*
 * Detach, before anything else: remove the child.  Fails if the child
 * cannot be detached.
 */
boolean_t
ice_rdma_detach(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;
	boolean_t ok;

	if (ir == NULL)
		return (B_TRUE);

	mutex_enter(&ir->ir_lock);
	ir->ir_stopping = B_TRUE;
	mutex_exit(&ir->ir_lock);

	mutex_enter(&ir->ir_cfg_lock);
	ok = ice_rdma_offline(ice, B_TRUE);
	mutex_exit(&ir->ir_cfg_lock);

	if (!ok)
		ice_rdma_detach_undo(ice);
	return (ok);
}

/* A later detach step failed; bring the child back. */
void
ice_rdma_detach_undo(ice_t *ice)
{
	ice_rdma_t *ir = ice->ice_rdma;

	if (ir == NULL || ir->ir_vectors == 0 || ir->ir_evtq == NULL)
		return;

	mutex_enter(&ir->ir_lock);
	ir->ir_stopping = B_FALSE;
	mutex_exit(&ir->ir_lock);

	if (ddi_taskq_dispatch(ice->ice_reset_taskq, ice_rdma_online_task,
	    ice, DDI_SLEEP) != DDI_SUCCESS)
		ice_error(ice, "failed to dispatch the RDMA online task");
}

/*
 * Release the RDMA state.  reset_ok says whether the cleanup PF reset
 * completed; without it the quarantined buffers are leaked, not freed.
 */
void
ice_rdma_fini(ice_t *ice, boolean_t reset_ok)
{
	ice_rdma_t *ir = ice->ice_rdma;
	ice_rdma_buf_t *irb;

	if (ir == NULL)
		return;

	VERIFY3P(ir->ir_cdip, ==, NULL);
	VERIFY3P(ir->ir_client, ==, NULL);

	if (ir->ir_kstat != NULL)
		kstat_delete(ir->ir_kstat);
	if (ir->ir_evtq != NULL)
		ddi_taskq_destroy(ir->ir_evtq);

	/* close() moved every live buffer to the quarantine. */
	VERIFY(list_is_empty(&ir->ir_bufs));
	if (reset_ok) {
		ice_rdma_quarantine_free(ir);
	} else if (ir->ir_nquar != 0) {
		ice_error(ice, "leaking %u RDMA buffers (%" PRIu64 " bytes) "
		    "that the device may still reach", ir->ir_nquar,
		    ir->ir_quar_bytes);
		while ((irb = list_remove_head(&ir->ir_quarantine)) != NULL)
			;
	}

	list_destroy(&ir->ir_bufs);
	list_destroy(&ir->ir_quarantine);
	cv_destroy(&ir->ir_cv);
	mutex_destroy(&ir->ir_lock);
	mutex_destroy(&ir->ir_cfg_lock);
	kmem_free(ir, sizeof (*ir));
	ice->ice_rdma = NULL;
}

/*
 * Nexus operations for the child node.
 */

static int
ice_bus_ctl(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t op, void *arg,
    void *result)
{
	dev_info_t *child = arg;

	switch (op) {
	case DDI_CTLOPS_REPORTDEV:
		if (rdip == NULL)
			return (DDI_FAILURE);
		cmn_err(CE_CONT, "?%s%d at %s%d\n", ddi_driver_name(rdip),
		    ddi_get_instance(rdip), ddi_driver_name(dip),
		    ddi_get_instance(dip));
		return (DDI_SUCCESS);
	case DDI_CTLOPS_INITCHILD:
		if (ddi_get_parent_data(child) == NULL)
			return (DDI_NOT_WELL_FORMED);
		ddi_set_name_addr(child, "0");
		return (DDI_SUCCESS);
	case DDI_CTLOPS_UNINITCHILD:
		ddi_set_name_addr(child, NULL);
		return (DDI_SUCCESS);
	case DDI_CTLOPS_ATTACH:
	case DDI_CTLOPS_DETACH:
		return (DDI_SUCCESS);
	default:
		return (ddi_ctlops(dip, rdip, op, arg, result));
	}
}

static int
ice_bus_config(dev_info_t *dip, uint_t flags, ddi_bus_config_op_t op,
    void *arg, dev_info_t **cdipp)
{
	if (op == BUS_CONFIG_ONE || op == BUS_CONFIG_ALL ||
	    op == BUS_CONFIG_DRIVER)
		flags |= NDI_ONLINE_ATTACH;
	return (ndi_busop_bus_config(dip, flags, op, arg, cdipp, 0));
}

static int
ice_bus_unconfig(dev_info_t *dip, uint_t flags, ddi_bus_config_op_t op,
    void *arg)
{
	if (op == BUS_UNCONFIG_ONE || op == BUS_UNCONFIG_ALL ||
	    op == BUS_UNCONFIG_DRIVER)
		flags |= NDI_UNCONFIG;
	return (ndi_busop_bus_unconfig(dip, flags, op, arg));
}

struct bus_ops ice_bus_ops = {
	.busops_rev = BUSO_REV,
	.bus_ctl = ice_bus_ctl,
	.bus_prop_op = ddi_bus_prop_op,
	.bus_config = ice_bus_config,
	.bus_unconfig = ice_bus_unconfig
};

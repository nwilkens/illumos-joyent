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
 * Own the PF VSI's accepted address set and promiscuous policy.  GLD callers
 * submit intent; only this module translates it into imported switch requests.
 * VSI setup/rebuild owns the surrounding hardware lifecycle.  MAC callbacks
 * acquire ice_rebuild_lock here; replay already holds it.  The address-list
 * lock is always dropped before a blocking firmware command.
 */

#include "ice.h"
#include "ice_common.h"
#include "ice_switch.h"

typedef struct ice_mac_filter {
	list_node_t		imf_node;
	uint8_t			imf_addr[ETHERADDRL];
} ice_mac_filter_t;

static const uint8_t ice_bcast_addr[ETHERADDRL] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/*
 * Initialize a caller-owned, unlinked MAC filter entry.  This shared request
 * shape is private to callback, attach, reset, and teardown operations.
 * No allocation, locking, or firmware I/O occurs here; callers own submission.
 */
static void
ice_fltr_entry_init(struct ice_fltr_list_entry *e, uint16_t handle,
    const uint8_t *addr)
{
	bzero(e, sizeof (*e));
	e->fltr_info.flag = ICE_FLTR_TX;
	e->fltr_info.lkup_type = ICE_SW_LKUP_MAC;
	e->fltr_info.fltr_act = ICE_FWD_TO_VSI;
	e->fltr_info.vsi_handle = handle;
	e->fltr_info.src_id = ICE_SRC_ID_VSI;
	bcopy(addr, e->fltr_info.l_data.mac.mac_addr, ETHERADDRL);
}

static void
ice_mac_filter_track(ice_vsi_t *vsi, const uint8_t *addr)
{
	ice_mac_filter_t *imf;

	imf = kmem_zalloc(sizeof (*imf), KM_SLEEP);
	bcopy(addr, imf->imf_addr, ETHERADDRL);
	mutex_enter(&vsi->vi_mac_lock);
	list_insert_tail(&vsi->vi_macs, imf);
	mutex_exit(&vsi->vi_mac_lock);
}

static ice_mac_filter_t *
ice_filter_find_mac(ice_vsi_t *vsi, const uint8_t *addr)
{
	ice_mac_filter_t *imf;

	ASSERT(MUTEX_HELD(&vsi->vi_mac_lock));

	for (imf = list_head(&vsi->vi_macs); imf != NULL;
	    imf = list_next(&vsi->vi_macs, imf)) {
		if (bcmp(imf->imf_addr, addr, ETHERADDRL) == 0)
			return (imf);
	}

	return (NULL);
}

/*
 * MAC callbacks cannot introduce new ownership while filter recovery is owed.
 * Replay applies accepted policy after the worker claims its reset requests;
 * a later request must not prevent that replay.
 */
static boolean_t
ice_filters_blocked(ice_t *ice)
{
	const uint32_t blocked = ICE_STATE_RESET_FAILED | ICE_STATE_PFR_REQ |
	    ICE_STATE_RESET_PENDING;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	return ((ice->ice_state & blocked) != 0);
}

/*
 * A failed switch operation may already have changed hardware or common-code
 * bookkeeping.  Block software traffic and arrange a reset to clear that
 * uncertain state before replaying accepted ownership.  This is deferred
 * filter cleanup, not an acknowledgment that hardware or DMA has stopped.
 */
static void
ice_filters_recover(ice_t *ice)
{
	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	atomic_or_32(&ice->ice_state, ICE_STATE_ERROR | ICE_STATE_PFR_REQ);
	ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_LOST);
	ice_link_report(ice, LINK_STATE_DOWN);
	ice_reset_redispatch(ice);
}

/*
 * Add or remove a unicast/multicast MAC filter on the PF data VSI through the
 * switch.  vi_macs records addresses to replay or retire, not MAC reference
 * counts or hardware readback.  Failed removals retire ownership and request
 * recovery because MAC client teardown cannot retain it.  The blocking admin
 * queue command runs with the list lock dropped.
 */
static int
ice_filters_set_mac_locked(ice_t *ice, const uint8_t *addr, boolean_t add)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_fltr_list_entry e;
	struct LIST_HEAD_TYPE m_list;
	ice_mac_filter_t *imf;
	int status;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	if (add && ice_filters_blocked(ice))
		return (EIO);

	mutex_enter(&vsi->vi_mac_lock);
	imf = ice_filter_find_mac(vsi, addr);
	if (add && imf != NULL) {
		/* Already present; nothing to do. */
		mutex_exit(&vsi->vi_mac_lock);
		return (0);
	}
	if (!add && imf == NULL) {
		mutex_exit(&vsi->vi_mac_lock);
		return (ENOENT);
	}
	mutex_exit(&vsi->vi_mac_lock);

	/*
	 * Terminal failure cannot replay.  An owed reset will discard uncertain
	 * hardware rules and replay the remaining addresses, so removals in
	 * either state retire ownership without another switch command.
	 */
	if (!ice_filters_blocked(ice)) {
		INIT_LIST_HEAD(&m_list);
		ice_fltr_entry_init(&e, vsi->vi_handle, addr);
		LIST_ADD(&e.list_entry, &m_list);

		if (add)
			status = ice_add_mac(hw, &m_list);
		else
			status = ice_remove_mac(hw, &m_list);

		if (status != ICE_SUCCESS) {
			int error = ice_status_to_errno(ice, status);

			ice_error(ice, "failed to %s MAC filter: %d",
			    add ? "add" : "remove", status);
			ice_filters_recover(ice);
			if (add)
				return (error);
		}
	} else if (add) {
		/* A reset request may have arrived while checking the list. */
		return (EIO);
	}

	mutex_enter(&vsi->vi_mac_lock);
	if (add) {
		imf = kmem_zalloc(sizeof (*imf), KM_SLEEP);
		bcopy(addr, imf->imf_addr, ETHERADDRL);
		list_insert_tail(&vsi->vi_macs, imf);
	} else if ((imf = ice_filter_find_mac(vsi, addr)) != NULL) {
		list_remove(&vsi->vi_macs, imf);
		kmem_free(imf, sizeof (*imf));
	}
	mutex_exit(&vsi->vi_mac_lock);

	return (0);
}

/*
 * ice_rebuild_lock is the outermost lock: hold it across the admin-queue filter
 * command so a reset rebuild cannot shut down and reinitialize the control
 * queue underneath ice_add_mac/ice_remove_mac.  It is taken before the
 * vi_mac_lock the inner routine uses, matching the rebuild's own lock order.
 */
int
ice_filters_set_mac(ice_t *ice, const uint8_t *addr, boolean_t add)
{
	int ret;

	mutex_enter(&ice->ice_rebuild_lock);
	ret = ice_filters_set_mac_locked(ice, addr, add);
	mutex_exit(&ice->ice_rebuild_lock);

	return (ret);
}

/*
 * Apply or clear unicast + multicast promiscuous mode in both directions.
 * Broadcast is already forwarded
 * by the default filters installed in ice_filters_setup().
 */
static int
ice_filters_promisc_apply(ice_t *ice, boolean_t on)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	ice_declare_bitmap(mask, ICE_PROMISC_MAX);
	boolean_t prev;
	int rollback, status, ret;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	/* MAC also needs to release promiscuous ownership after failure. */
	if ((ice->ice_state & ICE_STATE_RESET_FAILED) != 0) {
		if (on)
			return (EIO);
		ice->ice_promisc_on = B_FALSE;
		return (0);
	}

	ice_zero_bitmap(mask, ICE_PROMISC_MAX);
	ice_set_bit(ICE_PROMISC_UCAST_RX, mask);
	ice_set_bit(ICE_PROMISC_UCAST_TX, mask);
	ice_set_bit(ICE_PROMISC_MCAST_RX, mask);
	ice_set_bit(ICE_PROMISC_MCAST_TX, mask);

	/* The accepted policy is separate from partial switch programming. */
	prev = ice->ice_promisc_on;

	if (!on) {
		status = ice_clear_vsi_promisc(hw, vsi->vi_handle, mask, 0);
		ice->ice_promisc_on = B_FALSE;
		if (status != ICE_SUCCESS) {
			ice_error(ice, "failed to disable promiscuous "
			    "mode: %d", status);
			ice_filters_recover(ice);
		}
		return (0);
	}

	status = ice_set_vsi_promisc(hw, vsi->vi_handle, mask, 0);
	if (status == ICE_SUCCESS) {
		ice->ice_promisc_on = B_TRUE;
		return (0);
	}

	ice_error(ice, "failed to enable promiscuous mode: %d", status);

	/*
	 * The rollback below issues admin queue commands that overwrite
	 * sq_last_status, so decode this failure before it runs.
	 */
	ret = ice_status_to_errno(ice, status);

	/*
	 * The setter applies individual rules and stops on the first error.
	 * Clear the whole mask to remove recorded rules installed before that
	 * failure.  Reset below also clears any unrecorded hardware state.
	 */
	rollback = ice_clear_vsi_promisc(hw, vsi->vi_handle, mask, 0);
	ice->ice_promisc_on = prev;
	if (rollback != ICE_SUCCESS) {
		ice_error(ice, "failed to roll back promiscuous mode: %d",
		    rollback);
	}
	/*
	 * Even a successful rollback only removes recorded rules.  A failed
	 * admin queue completion can leave an unrecorded hardware rule, so
	 * reset before allowing another callback to enable traffic.
	 */
	ice_filters_recover(ice);

	return (ret);
}

int
ice_filters_set_promisc(ice_t *ice, boolean_t on)
{
	int ret;

	/*
	 * ice_rebuild_lock is the outermost lock: hold it so the
	 * admin-queue command cannot overlap control-queue reconstruction.
	 * Replay uses the private lock-held operation.
	 */
	mutex_enter(&ice->ice_rebuild_lock);
	if (ice_filters_blocked(ice)) {
		ret = on ? EIO : 0;
		if (!on)
			ice->ice_promisc_on = B_FALSE;
	} else if (on == ice->ice_promisc_on) {
		ret = 0;
	} else {
		ret = ice_filters_promisc_apply(ice, on);
	}
	mutex_exit(&ice->ice_rebuild_lock);

	return (ret);
}

/* Reapply accepted policy even when the cached boolean is unchanged. */
int
ice_filters_replay_promisc(ice_t *ice)
{
	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	if (ice->ice_promisc_on)
		return (ice_filters_promisc_apply(ice, B_TRUE));
	return (0);
}

/* Construct software ownership before VSI creation can fail. */
void
ice_filters_init(ice_t *ice)
{
	ice_vsi_t *vsi = &ice->ice_pf_vsi;

	mutex_init(&vsi->vi_mac_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&vsi->vi_macs, sizeof (ice_mac_filter_t),
	    offsetof(ice_mac_filter_t, imf_node));
}

int
ice_filters_setup(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_fltr_list_entry uc, bc;
	struct LIST_HEAD_TYPE m_list;
	const uint8_t *mac;
	int status;

	mac = hw->port_info->mac.perm_addr;
	if (IS_ZERO_ETHER_ADDR(mac)) {
		ice_error(ice, "firmware reported a zero station MAC address");
		return (ICE_ERR_PARAM);
	}

	INIT_LIST_HEAD(&m_list);
	ice_fltr_entry_init(&uc, vsi->vi_handle, mac);
	ice_fltr_entry_init(&bc, vsi->vi_handle, ice_bcast_addr);
	LIST_ADD(&uc.list_entry, &m_list);
	LIST_ADD(&bc.list_entry, &m_list);

	/*
	 * ice_add_mac()'s return is the authoritative result for the batch.
	 * On failure roll back any rule it did install (best effort; the VSI
	 * free and ice_deinit_hw() reclaim the rest).
	 */
	status = ice_add_mac(hw, &m_list);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to add MAC filters: %d", status);
		(void) ice_remove_mac(hw, &m_list);
		return (status);
	}

	ice_mac_filter_track(vsi, mac);
	ice_mac_filter_track(vsi, ice_bcast_addr);
	return (ICE_SUCCESS);
}

/* Tear down accepted filters after callbacks and rebuild work have drained. */
void
ice_filters_fini(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_fltr_list_entry *ents = NULL;
	ice_mac_filter_t *imf;
	uint_t n = 0, i = 0;

	/*
	 * Build the removal list under the lock, but issue the (blocking) admin
	 * queue command with the lock dropped.
	 */
	mutex_enter(&vsi->vi_mac_lock);
	for (imf = list_head(&vsi->vi_macs); imf != NULL;
	    imf = list_next(&vsi->vi_macs, imf))
		n++;
	if (n > 0) {
		struct LIST_HEAD_TYPE rm;

		ents = kmem_zalloc(n * sizeof (*ents), KM_SLEEP);
		INIT_LIST_HEAD(&rm);
		for (imf = list_head(&vsi->vi_macs); imf != NULL;
		    imf = list_next(&vsi->vi_macs, imf)) {
			ice_fltr_entry_init(&ents[i], vsi->vi_handle,
			    imf->imf_addr);
			LIST_ADD(&ents[i].list_entry, &rm);
			i++;
		}
		mutex_exit(&vsi->vi_mac_lock);

		(void) ice_remove_mac(hw, &rm);
		kmem_free(ents, n * sizeof (*ents));

		mutex_enter(&vsi->vi_mac_lock);
	}
	while ((imf = list_remove_head(&vsi->vi_macs)) != NULL)
		kmem_free(imf, sizeof (*imf));
	mutex_exit(&vsi->vi_mac_lock);

	list_destroy(&vsi->vi_macs);
	mutex_destroy(&vsi->vi_mac_lock);
}

/* Rebuild has prepared the imported replay lists and holds its lock. */
int
ice_filters_replay(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_fltr_list_entry *ents = NULL;
	ice_mac_filter_t *imf;
	uint_t n = 0, i = 0;
	int status;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	/*
	 * Replay every tracked MAC filter.  Build the list under the lock but
	 * issue the blocking admin-queue command with it dropped, matching
	 * ice_filters_fini().
	 */
	mutex_enter(&vsi->vi_mac_lock);
	for (imf = list_head(&vsi->vi_macs); imf != NULL;
	    imf = list_next(&vsi->vi_macs, imf))
		n++;
	if (n > 0) {
		struct LIST_HEAD_TYPE add;

		ents = kmem_zalloc(n * sizeof (*ents), KM_SLEEP);
		INIT_LIST_HEAD(&add);
		for (imf = list_head(&vsi->vi_macs); imf != NULL;
		    imf = list_next(&vsi->vi_macs, imf)) {
			ice_fltr_entry_init(&ents[i], vsi->vi_handle,
			    imf->imf_addr);
			LIST_ADD(&ents[i].list_entry, &add);
			i++;
		}
		mutex_exit(&vsi->vi_mac_lock);

		status = ice_add_mac(hw, &add);
		kmem_free(ents, n * sizeof (*ents));
		if (status != ICE_SUCCESS) {
			ice_error(ice, "failed to replay MAC filters: %d",
			    status);
			return (status);
		}
	} else {
		mutex_exit(&vsi->vi_mac_lock);
	}

	return (ICE_SUCCESS);
}

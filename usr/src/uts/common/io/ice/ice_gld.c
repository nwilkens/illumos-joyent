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
 * GLDv3 (MAC) registration and callbacks for ice(4D).
 *
 * The driver advertises a single static rx group backing the PF data VSI and
 * one ring per tx/rx queue.  Per-ring datapath callbacks (start/stop/poll/tx/
 * stat/intr) are wired directly to the entry points in ice_tx.c and ice_rx.c;
 * there is no driver-level interrupt dispatch table -- each ring already knows
 * its MSI-X vector (itxr_vec/irxr_vec), so the mac_intr_t handles point at the
 * ring itself.
 *
 * Management callbacks (unicast/multicast filters, promiscuous mode) go through
 * the Intel common-code switch APIs, matching the filter bookkeeping already
 * established by ice_vsi.c (ice_add_mac/ice_remove_mac with the fltr list
 * entry, tracked on the VSI's vi_macs list).  Link state is reported from cache
 * ice_intr.c maintains; the MAC path never blocks on hardware.
 *
 * Hardware checksum and LSO offloads are not advertised in this milestone:
 * ice_m_getcapab returns B_FALSE for MAC_CAPAB_HCKSUM and MAC_CAPAB_LSO.  The
 * rings carry traffic; the offload descriptor paths land later.
 */

#include <sys/mac_provider.h>
#include <sys/mac_ether.h>
#include <sys/vlan.h>

#include "ice.h"
#include "ice_common.h"
#include "ice_switch.h"

/*
 * Build a switch MAC filter list entry for the PF data VSI.  Mirrors the helper
 * in ice_vsi.c so the unicast/multicast paths produce filters the common code
 * and ice_vsi_teardown() agree on.
 */
static void
ice_gld_fltr_init(struct ice_fltr_list_entry *e, uint16_t handle,
    const uint8_t *addr)
{
	bzero(e, sizeof (*e));
	e->fltr_info.lkup_type = ICE_SW_LKUP_MAC;
	e->fltr_info.fltr_act = ICE_FWD_TO_VSI;
	e->fltr_info.vsi_handle = handle;
	e->fltr_info.src_id = ICE_SRC_ID_VSI;
	bcopy(addr, e->fltr_info.l_data.mac.mac_addr, ETHERADDRL);
}

static ice_mac_filter_t *
ice_gld_find_mac(ice_vsi_t *vsi, const uint8_t *addr)
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
 * Add or remove a unicast/multicast MAC filter on the PF data VSI through the
 * switch.  The vi_macs list is the authoritative software record; the admin
 * queue command runs with the list lock dropped because it can block.
 */
static int
ice_gld_set_mac(ice_t *ice, const uint8_t *addr, boolean_t add)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_fltr_list_entry e;
	struct LIST_HEAD_TYPE m_list;
	ice_mac_filter_t *imf;
	int status;

	mutex_enter(&vsi->vi_mac_lock);
	imf = ice_gld_find_mac(vsi, addr);
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

	INIT_LIST_HEAD(&m_list);
	ice_gld_fltr_init(&e, vsi->vi_handle, addr);
	LIST_ADD(&e.list_entry, &m_list);

	if (add)
		status = ice_add_mac(hw, &m_list);
	else
		status = ice_remove_mac(hw, &m_list);

	if (status != ICE_SUCCESS) {
		ice_error(ice, "!failed to %s MAC filter: %d",
		    add ? "add" : "remove", status);
		return (EIO);
	}

	mutex_enter(&vsi->vi_mac_lock);
	if (add) {
		imf = kmem_zalloc(sizeof (*imf), KM_SLEEP);
		bcopy(addr, imf->imf_addr, ETHERADDRL);
		list_insert_tail(&vsi->vi_macs, imf);
	} else if ((imf = ice_gld_find_mac(vsi, addr)) != NULL) {
		list_remove(&vsi->vi_macs, imf);
		kmem_free(imf, sizeof (*imf));
	}
	mutex_exit(&vsi->vi_mac_lock);

	return (0);
}

/*
 * Ring callbacks.
 */
static int
ice_group_add_mac(void *arg, const uint8_t *mac_addr)
{
	ice_t *ice = arg;

	return (ice_gld_set_mac(ice, mac_addr, B_TRUE));
}

static int
ice_group_remove_mac(void *arg, const uint8_t *mac_addr)
{
	ice_t *ice = arg;

	return (ice_gld_set_mac(ice, mac_addr, B_FALSE));
}

static void
ice_fill_rx_ring(void *arg, mac_ring_type_t rtype, const int group_index,
    const int ring_index, mac_ring_info_t *infop, mac_ring_handle_t rh)
{
	ice_t *ice = arg;
	ice_rx_ring_t *rxr;

	ASSERT3S(rtype, ==, MAC_RING_TYPE_RX);
	ASSERT3S(group_index, ==, 0);
	ASSERT3U(ring_index, <, ice->ice_num_rxr);

	rxr = &ice->ice_rxr[ring_index];
	rxr->irxr_macrxring = rh;

	infop->mri_driver = (mac_ring_driver_t)rxr;
	infop->mri_start = ice_ring_rx_start;
	infop->mri_stop = ice_ring_rx_stop;
	infop->mri_poll = ice_ring_rx_poll;
	infop->mri_stat = ice_ring_rx_stat;
	infop->mri_intr.mi_handle = (mac_intr_handle_t)rxr;
	infop->mri_intr.mi_enable = ice_ring_rx_intr_enable;
	infop->mri_intr.mi_disable = ice_ring_rx_intr_disable;
}

static void
ice_fill_tx_ring(void *arg, mac_ring_type_t rtype, const int group_index,
    const int ring_index, mac_ring_info_t *infop, mac_ring_handle_t rh)
{
	ice_t *ice = arg;
	ice_tx_ring_t *txr;

	ASSERT3S(rtype, ==, MAC_RING_TYPE_TX);
	ASSERT3S(group_index, ==, -1);	/* tx rings are groupless */
	ASSERT3U(ring_index, <, ice->ice_num_txr);

	txr = &ice->ice_txr[ring_index];
	txr->itxr_mactxring = rh;

	infop->mri_driver = (mac_ring_driver_t)txr;
	infop->mri_start = NULL;
	infop->mri_stop = NULL;
	infop->mri_tx = ice_ring_tx;
	infop->mri_stat = ice_ring_tx_stat;

	/*
	 * Tx completion runs off the queue's MSI-X vector; expose the handle so
	 * MAC can retarget it.  Tx rings are not polled, so they carry no
	 * enable/disable callbacks.
	 */
	if ((ice->ice_intr_type & DDI_INTR_TYPE_MSIX) != 0) {
		infop->mri_intr.mi_ddi_handle =
		    ice->ice_intr_handles[txr->itxr_vec];
	}
}

static void
ice_fill_group(void *arg, mac_ring_type_t rtype, const int index,
    mac_group_info_t *infop, mac_group_handle_t gh)
{
	ice_t *ice = arg;

	if (rtype != MAC_RING_TYPE_RX)
		return;

	ASSERT3S(index, ==, 0);

	infop->mgi_driver = (mac_group_driver_t)ice;
	infop->mgi_start = NULL;
	infop->mgi_stop = NULL;
	infop->mgi_addmac = ice_group_add_mac;
	infop->mgi_remmac = ice_group_remove_mac;
	infop->mgi_count = ice->ice_num_rxr;
}

/*
 * MAC callbacks.
 */
static int
ice_m_start(void *arg)
{
	ice_t *ice = arg;

	/*
	 * Program and enable the queues on each start so the hardware ring head
	 * resets in lockstep with software, then allocate the rx control-block
	 * pool and admit traffic.  The per-ring start callbacks post buffers.
	 */
	if (ice_queues_program(ice) != ICE_SUCCESS)
		return (EIO);
	if (!ice_rx_start(ice)) {
		ice_queues_disable(ice);
		return (EIO);
	}
	ice_tx_start(ice);

	return (0);
}

static void
ice_m_stop(void *arg)
{
	ice_t *ice = arg;

	/*
	 * Disable the queues first so hardware stops touching descriptors and
	 * buffers, then reclaim the tx control blocks and the rx pool (the
	 * latter waits for any loaned buffers to return).
	 */
	ice_queues_disable(ice);
	ice_tx_stop(ice);
	ice_rx_stop(ice);
}

static int
ice_m_promisc(void *arg, boolean_t on)
{
	ice_t *ice = arg;
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	ice_declare_bitmap(mask, ICE_PROMISC_MAX);
	int status;

	/*
	 * Unicast + multicast promiscuous in both directions.  Broadcast is
	 * already forwarded by the default filters installed in M5.
	 */
	ice_zero_bitmap(mask, ICE_PROMISC_MAX);
	ice_set_bit(ICE_PROMISC_UCAST_RX, mask);
	ice_set_bit(ICE_PROMISC_UCAST_TX, mask);
	ice_set_bit(ICE_PROMISC_MCAST_RX, mask);
	ice_set_bit(ICE_PROMISC_MCAST_TX, mask);

	if (on)
		status = ice_set_vsi_promisc(hw, vsi->vi_handle, mask, 0);
	else
		status = ice_clear_vsi_promisc(hw, vsi->vi_handle, mask, 0);

	if (status != ICE_SUCCESS) {
		ice_error(ice, "!failed to %s promiscuous mode: %d",
		    on ? "enable" : "disable", status);
		return (EIO);
	}

	return (0);
}

static int
ice_m_multicst(void *arg, boolean_t add, const uint8_t *addr)
{
	ice_t *ice = arg;

	return (ice_gld_set_mac(ice, addr, add));
}

static int
ice_m_stat(void *arg, uint_t stat, uint64_t *val)
{
	ice_t *ice = arg;
	int ret = 0;

	mutex_enter(&ice->ice_lse_lock);
	switch (stat) {
	case MAC_STAT_IFSPEED:
		/* Cached speed is in Mbit/s; MAC wants bits/s. */
		*val = ice->ice_link_speed * 1000000ULL;
		break;
	case ETHER_STAT_LINK_DUPLEX:
		*val = ice->ice_link_duplex;
		break;
	default:
		ret = ENOTSUP;
		break;
	}
	mutex_exit(&ice->ice_lse_lock);

	return (ret);
}

static boolean_t
ice_m_getcapab(void *arg, mac_capab_t capab, void *cap_data)
{
	ice_t *ice = arg;
	mac_capab_rings_t *cap_rings;

	switch (capab) {
	case MAC_CAPAB_RINGS:
		cap_rings = cap_data;
		cap_rings->mr_group_type = MAC_GROUP_TYPE_STATIC;
		switch (cap_rings->mr_type) {
		case MAC_RING_TYPE_TX:
			cap_rings->mr_gnum = 0;
			cap_rings->mr_rnum = ice->ice_num_txr;
			cap_rings->mr_rget = ice_fill_tx_ring;
			cap_rings->mr_gget = NULL;
			cap_rings->mr_gaddring = NULL;
			cap_rings->mr_gremring = NULL;
			break;
		case MAC_RING_TYPE_RX:
			cap_rings->mr_gnum = ice->ice_num_rx_groups;
			cap_rings->mr_rnum = ice->ice_num_rxr;
			cap_rings->mr_rget = ice_fill_rx_ring;
			cap_rings->mr_gget = ice_fill_group;
			cap_rings->mr_gaddring = NULL;
			cap_rings->mr_gremring = NULL;
			break;
		default:
			return (B_FALSE);
		}
		break;

	/* Offloads are not advertised this milestone. */
	case MAC_CAPAB_HCKSUM:
	case MAC_CAPAB_LSO:
	default:
		return (B_FALSE);
	}

	return (B_TRUE);
}

static int
ice_m_setprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, const void *pr_val)
{
	_NOTE(ARGUNUSED(arg, pr_name, pr_valsize, pr_val));

	/*
	 * Link configuration (autoneg, per-speed advertisement, flow control)
	 * is not settable yet, and MTU resizing requires reprogramming the rx
	 * rings, which is out of this milestone's scope.
	 */
	switch (pr_num) {
	default:
		return (ENOTSUP);
	}
}

static int
ice_m_getprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, void *pr_val)
{
	ice_t *ice = arg;
	int ret = 0;
	uint64_t speed;
	uint8_t *u8;

	_NOTE(ARGUNUSED(pr_name));

	mutex_enter(&ice->ice_lse_lock);

	switch (pr_num) {
	case MAC_PROP_DUPLEX:
		if (pr_valsize < sizeof (link_duplex_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&ice->ice_link_duplex, pr_val, sizeof (link_duplex_t));
		break;
	case MAC_PROP_SPEED:
		if (pr_valsize < sizeof (uint64_t)) {
			ret = EOVERFLOW;
			break;
		}
		speed = ice->ice_link_speed * 1000000ULL;
		bcopy(&speed, pr_val, sizeof (speed));
		break;
	case MAC_PROP_STATUS:
		if (pr_valsize < sizeof (link_state_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&ice->ice_link_state, pr_val, sizeof (link_state_t));
		break;
	case MAC_PROP_AUTONEG:
		if (pr_valsize < sizeof (uint8_t)) {
			ret = EOVERFLOW;
			break;
		}
		u8 = pr_val;
		*u8 = 1;
		break;
	case MAC_PROP_FLOWCTRL:
		if (pr_valsize < sizeof (link_flowctrl_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&ice->ice_link_fctl, pr_val, sizeof (link_flowctrl_t));
		break;
	default:
		ret = ENOTSUP;
		break;
	}

	mutex_exit(&ice->ice_lse_lock);

	return (ret);
}

static void
ice_m_propinfo(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    mac_prop_info_handle_t prh)
{
	_NOTE(ARGUNUSED(arg, pr_name));

	switch (pr_num) {
	case MAC_PROP_DUPLEX:
	case MAC_PROP_SPEED:
	case MAC_PROP_STATUS:
	case MAC_PROP_AUTONEG:
	case MAC_PROP_FLOWCTRL:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		break;
	case MAC_PROP_MTU:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		mac_prop_info_set_range_uint32(prh, ICE_MIN_MTU,
		    ICE_RX_BUF_SIZE - sizeof (struct ether_vlan_header));
		break;
	default:
		break;
	}
}

static mac_callbacks_t ice_m_callbacks = {
	.mc_callbacks = MC_GETCAPAB | MC_SETPROP | MC_GETPROP | MC_PROPINFO,
	.mc_getstat = ice_m_stat,
	.mc_start = ice_m_start,
	.mc_stop = ice_m_stop,
	.mc_setpromisc = ice_m_promisc,
	.mc_multicst = ice_m_multicst,
	.mc_unicst = NULL,		/* rx groups: unicast via addmac */
	.mc_tx = NULL,			/* tx is per-ring (MAC_CAPAB_RINGS) */
	.mc_getcapab = ice_m_getcapab,
	.mc_setprop = ice_m_setprop,
	.mc_getprop = ice_m_getprop,
	.mc_propinfo = ice_m_propinfo
};

boolean_t
ice_mac_register(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	mac_register_t *mac;
	int status;

	if ((mac = mac_alloc(MAC_VERSION)) == NULL) {
		ice_error(ice, "failed to allocate MAC handle");
		return (B_FALSE);
	}

	mac->m_type_ident = MAC_PLUGIN_IDENT_ETHER;
	mac->m_driver = ice;
	mac->m_dip = ice->ice_dip;
	mac->m_instance = ice->ice_instance;
	/* Advertise the address the unicast filter was installed from. */
	mac->m_src_addr = hw->port_info->mac.perm_addr;
	mac->m_dst_addr = NULL;
	mac->m_callbacks = &ice_m_callbacks;
	mac->m_min_sdu = ICE_MIN_MTU;
	/*
	 * Single-buffer receive bounds the frame to one posted buffer until
	 * multi-buffer/jumbo rx lands; advertise the matching payload size.
	 */
	mac->m_max_sdu = ICE_RX_BUF_SIZE - sizeof (struct ether_vlan_header);
	mac->m_pdata = NULL;
	mac->m_pdata_size = 0;
	mac->m_priv_props = NULL;
	mac->m_margin = VLAN_TAGSZ;
	mac->m_v12n = MAC_VIRT_LEVEL1;

	status = mac_register(mac, &ice->ice_mac_hdl);
	mac_free(mac);

	if (status != 0) {
		ice_error(ice, "failed to register with MAC: %d", status);
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * Returns the mac_unregister() status so attach/detach can honor a bound
 * client (mac_unregister fails with EBUSY while a client is attached).
 */
int
ice_mac_unregister(ice_t *ice)
{
	int status;

	if (ice->ice_mac_hdl == NULL)
		return (0);

	status = mac_unregister(ice->ice_mac_hdl);
	if (status != 0) {
		ice_error(ice, "!failed to unregister from MAC: %d", status);
		return (status);
	}

	ice->ice_mac_hdl = NULL;
	return (0);
}

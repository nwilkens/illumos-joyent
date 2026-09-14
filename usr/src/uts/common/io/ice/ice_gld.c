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
 * GLDv3 (MAC) registration and callbacks for ice(4D).
 *
 * The driver advertises a single static rx group backing the PF data VSI and
 * one ring per tx/rx queue.  Per-ring datapath callbacks (start/stop/poll/tx/
 * stat/intr) are wired directly to the entry points in ice_tx.c and ice_rx.c;
 * there is no driver-level interrupt dispatch table -- each ring already knows
 * its MSI-X vector (itxr_vec/irxr_vec), so the mac_intr_t handles point at the
 * ring itself.
 *
 * Management filter callbacks delegate to ice_filter.c, which owns accepted
 * address/promiscuous policy and its switch programming.  Link state is
 * reported from the cache ice_intr.c maintains.  Filter and control callbacks
 * can block on firmware; cached link property getters do not issue commands.
 *
 * Hardware checksum offload is advertised.  LSO remains dark unless the
 * operator enables the validation property before attach.
 */

#include <sys/mac_provider.h>
#include <sys/mac_ether.h>
#include <sys/vlan.h>
#include <sys/dlpi.h>
#include <sys/netlb.h>
#include <sys/policy.h>
#include <sys/stream.h>
#include <sys/strsun.h>

#include "ice.h"
#include "ice_common.h"

/*
 * Ring callbacks.
 */
static int
ice_group_add_mac(void *arg, const uint8_t *mac_addr)
{
	ice_t *ice = arg;

	return (ice_filters_set_mac(ice, mac_addr, B_TRUE));
}

static int
ice_group_remove_mac(void *arg, const uint8_t *mac_addr)
{
	ice_t *ice = arg;

	return (ice_filters_set_mac(ice, mac_addr, B_FALSE));
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
	if ((ice->ice_intr_type & DDI_INTR_TYPE_MSIX) != 0) {
		infop->mri_intr.mi_ddi_handle =
		    ice->ice_intr_handles[rxr->irxr_vec];
	}
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
 * Standard netlb(4I) loopback modes.  The firmware command implements an
 * internal MAC loopback; no physical media is involved.
 */
static const lb_property_t ice_loopback_modes[] = {
	{ normal, "normal", ICE_LB_NONE },
	{ internal, "MAC", ICE_LB_INTERNAL_MAC }
};

static int
ice_loopback_enable(ice_t *ice)
{
	int rollback, status;

	status = ice_vsi_loopback_set(ice, B_TRUE);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to permit VSI loopback: %d", status);
		return (EIO);
	}

	status = ice_aq_set_mac_loopback(&ice->ice_hw, true, NULL);
	if (status == ICE_SUCCESS)
		return (0);

	rollback = ice_vsi_loopback_set(ice, B_FALSE);
	if (rollback != ICE_SUCCESS) {
		ice_error(ice, "failed to roll back VSI loopback: %d",
		    rollback);
	}
	ice_error(ice, "failed to enable MAC loopback: %d", status);
	return (EIO);
}

static int
ice_loopback_disable(ice_t *ice)
{
	int rollback, status;

	status = ice_aq_set_mac_loopback(&ice->ice_hw, false, NULL);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to disable MAC loopback: %d", status);
		return (EIO);
	}

	status = ice_vsi_loopback_set(ice, B_FALSE);
	if (status == ICE_SUCCESS)
		return (0);

	rollback = ice_aq_set_mac_loopback(&ice->ice_hw, true, NULL);
	if (rollback != ICE_SUCCESS) {
		ice_error(ice, "failed to restore MAC loopback: %d", rollback);
	}
	ice_error(ice, "failed to revoke VSI loopback: %d", status);
	return (EIO);
}

static int
ice_loopback_mode_set_locked(ice_t *ice, uint32_t mode)
{
	boolean_t enable;
	uint32_t current;
	int error, rollback;

	if (mode != ICE_LB_NONE && mode != ICE_LB_INTERNAL_MAC)
		return (EINVAL);

	mutex_enter(&ice->ice_loopback_lock);
	mutex_enter(&ice->ice_lse_lock);
	current = ice->ice_loopback_mode;
	mutex_exit(&ice->ice_lse_lock);
	if (mode == current) {
		mutex_exit(&ice->ice_loopback_lock);
		return (0);
	}

	enable = mode == ICE_LB_INTERNAL_MAC;
	error = enable ? ice_loopback_enable(ice) : ice_loopback_disable(ice);
	if (error != 0) {
		mutex_exit(&ice->ice_loopback_lock);
		return (error);
	}
	if (ice_check_acc_handle(ice, ice->ice_osdep.ios_reg_handle) !=
	    DDI_FM_OK) {
		if (enable) {
			rollback = ice_loopback_disable(ice);
		} else {
			rollback = ice_loopback_enable(ice);
		}
		if (rollback != 0) {
			ice_error(ice, "failed to restore loopback after "
			    "register access fault");
		}
		mutex_exit(&ice->ice_loopback_lock);
		ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_DEGRADED);
		return (EIO);
	}

	ice_link_loopback_update(ice, mode);
	if (!enable)
		ice_link_status_update(ice);
	mutex_exit(&ice->ice_loopback_lock);

	return (0);
}

/*
 * ice_rebuild_lock is the outermost lock: hold it across the loopback
 * admin-queue commands (ice_aq_set_mac_loopback / ice_vsi_loopback_set) so a
 * reset rebuild cannot tear the control queue down underneath them.  Both the
 * netlb(4I) LB_SET_MODE ioctl and ice_loopback_fini() reach the hardware only
 * through here.
 */
static int
ice_loopback_mode_set(ice_t *ice, uint32_t mode)
{
	int ret;

	mutex_enter(&ice->ice_rebuild_lock);
	ret = ice_loopback_mode_set_locked(ice, mode);
	mutex_exit(&ice->ice_rebuild_lock);

	return (ret);
}

/*
 * Restore internal MAC loopback after a reset.  Both halves are cleared: the
 * VSI switch flags are re-derived from ice_vsi_ctx_fill()'s SRC_PRUNE default
 * and the PF reset drops the firmware loopback setting.  A failure downgrades
 * to ICE_LB_NONE so netlb(4I) never reports a loopback the hardware is not in;
 * taking the instance terminally offline over a transient diagnostic mode
 * would be worse than the bug.  Called from ice_rebuild() under
 * ice_rebuild_lock.
 */
void
ice_loopback_replay(ice_t *ice)
{
	uint32_t mode;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	mutex_enter(&ice->ice_loopback_lock);
	mutex_enter(&ice->ice_lse_lock);
	mode = ice->ice_loopback_mode;
	mutex_exit(&ice->ice_lse_lock);

	if (mode == ICE_LB_INTERNAL_MAC && ice_loopback_enable(ice) != 0) {
		ice_error(ice, "loopback not restored after reset; "
		    "reverting to normal mode");
		ice_link_loopback_update(ice, ICE_LB_NONE);
	}
	mutex_exit(&ice->ice_loopback_lock);
}

void
ice_loopback_fini(ice_t *ice)
{
	uint32_t mode;

	mutex_enter(&ice->ice_lse_lock);
	mode = ice->ice_loopback_mode;
	mutex_exit(&ice->ice_lse_lock);
	if (mode != ICE_LB_NONE)
		(void) ice_loopback_mode_set(ice, ICE_LB_NONE);
}

static boolean_t
ice_loopback_payload(mblk_t *mp, size_t size)
{
	return (mp->b_cont != NULL && MBLKL(mp->b_cont) >= size);
}

static void
ice_m_ioctl(void *arg, queue_t *q, mblk_t *mp)
{
	ice_t *ice = arg;
	struct iocblk *iocp;
	lb_info_sz_t infosz;
	uint32_t mode;
	size_t size;
	int error = 0;

	if (MBLKL(mp) < sizeof (*iocp)) {
		miocnak(q, mp, 0, EINVAL);
		return;
	}
	iocp = (struct iocblk *)(uintptr_t)mp->b_rptr;

	switch (iocp->ioc_cmd) {
	case LB_GET_INFO_SIZE:
		size = sizeof (lb_info_sz_t);
		if (iocp->ioc_count != size ||
		    !ice_loopback_payload(mp, size)) {
			error = EINVAL;
			break;
		}
		infosz = sizeof (ice_loopback_modes);
		bcopy(&infosz, mp->b_cont->b_rptr, size);
		break;
	case LB_GET_INFO:
		size = sizeof (ice_loopback_modes);
		if (iocp->ioc_count != size ||
		    !ice_loopback_payload(mp, size)) {
			error = EINVAL;
			break;
		}
		bcopy(ice_loopback_modes, mp->b_cont->b_rptr, size);
		break;
	case LB_GET_MODE:
		size = sizeof (uint32_t);
		if (iocp->ioc_count != size ||
		    !ice_loopback_payload(mp, size)) {
			error = EINVAL;
			break;
		}
		mutex_enter(&ice->ice_lse_lock);
		mode = ice->ice_loopback_mode;
		mutex_exit(&ice->ice_lse_lock);
		bcopy(&mode, mp->b_cont->b_rptr, size);
		break;
	case LB_SET_MODE:
		size = 0;
		error = secpolicy_net_config(iocp->ioc_cr, B_FALSE);
		if (error != 0)
			break;
		if (iocp->ioc_count != sizeof (uint32_t) ||
		    !ice_loopback_payload(mp, sizeof (uint32_t))) {
			error = EINVAL;
			break;
		}
		bcopy(mp->b_cont->b_rptr, &mode, sizeof (mode));
		error = ice_loopback_mode_set(ice, mode);
		break;
	default:
		error = EINVAL;
		break;
	}

	if (error != 0) {
		miocnak(q, mp, 0, error);
		return;
	}

	iocp->ioc_count = size;
	iocp->ioc_error = 0;
	iocp->ioc_rval = 0;
	mp->b_datap->db_type = M_IOCACK;
	qreply(q, mp);
}

/*
 * MAC callbacks.
 */

static int
ice_m_start(void *arg)
{
	return (ice_start(arg));
}

static void
ice_m_stop(void *arg)
{
	ice_stop(arg);
}

static int
ice_m_promisc(void *arg, boolean_t on)
{
	return (ice_filters_set_promisc(arg, on));
}

static int
ice_m_multicst(void *arg, boolean_t add, const uint8_t *addr)
{
	ice_t *ice = arg;

	return (ice_filters_set_mac(ice, addr, add));
}

static int
ice_m_stat(void *arg, uint_t stat, uint64_t *val)
{
	ice_t *ice = arg;
	uint16_t speed = 0;
	boolean_t advertised = B_FALSE;

	/* Link properties are served from the cached link state. */
	switch (stat) {
	case MAC_STAT_IFSPEED:
		mutex_enter(&ice->ice_lse_lock);
		/* Cached speed is in Mbit/s; MAC wants bits/s. */
		*val = ice->ice_link_speed * 1000000ULL;
		mutex_exit(&ice->ice_lse_lock);
		return (0);
	case ETHER_STAT_LINK_DUPLEX:
		mutex_enter(&ice->ice_lse_lock);
		*val = ice->ice_link_duplex;
		mutex_exit(&ice->ice_lse_lock);
		return (0);
	case ETHER_STAT_CAP_1000FDX:
		speed = ICE_AQ_LINK_SPEED_1000MB;
		break;
	case ETHER_STAT_CAP_2500FDX:
		speed = ICE_AQ_LINK_SPEED_2500MB;
		break;
	case ETHER_STAT_CAP_5000FDX:
		speed = ICE_AQ_LINK_SPEED_5GB;
		break;
	case ETHER_STAT_CAP_10GFDX:
		speed = ICE_AQ_LINK_SPEED_10GB;
		break;
	case ETHER_STAT_CAP_25GFDX:
		speed = ICE_AQ_LINK_SPEED_25GB;
		break;
	case ETHER_STAT_CAP_40GFDX:
		speed = ICE_AQ_LINK_SPEED_40GB;
		break;
	case ETHER_STAT_CAP_50GFDX:
		speed = ICE_AQ_LINK_SPEED_50GB;
		break;
	case ETHER_STAT_CAP_100GFDX:
		speed = ICE_AQ_LINK_SPEED_100GB;
		break;
	case ETHER_STAT_ADV_CAP_1000FDX:
		speed = ICE_AQ_LINK_SPEED_1000MB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_ADV_CAP_2500FDX:
		speed = ICE_AQ_LINK_SPEED_2500MB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_ADV_CAP_5000FDX:
		speed = ICE_AQ_LINK_SPEED_5GB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_ADV_CAP_10GFDX:
		speed = ICE_AQ_LINK_SPEED_10GB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_ADV_CAP_25GFDX:
		speed = ICE_AQ_LINK_SPEED_25GB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_ADV_CAP_40GFDX:
		speed = ICE_AQ_LINK_SPEED_40GB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_ADV_CAP_50GFDX:
		speed = ICE_AQ_LINK_SPEED_50GB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_ADV_CAP_100GFDX:
		speed = ICE_AQ_LINK_SPEED_100GB;
		advertised = B_TRUE;
		break;
	case ETHER_STAT_CAP_AUTONEG:
	case ETHER_STAT_ADV_CAP_AUTONEG:
		*val = 1;
		return (0);
	default:
		break;
	}

	if (speed != 0) {
		mutex_enter(&ice->ice_lse_lock);
		if (advertised) {
			*val = (ice->ice_phy_speeds_adv & speed) != 0;
		} else {
			*val = (ice->ice_phy_speeds_supp & speed) != 0;
		}
		mutex_exit(&ice->ice_lse_lock);
		return (0);
	}

	return (ice_stats_read(ice, stat, val));
}

/*
 * SFF module (transceiver) access.  Pages 0xa0/0xa2 are the I2C device
 * addresses of the SFF-8472 diagnostic memory; the admin-queue command reads at
 * most 16 bytes per request.
 */
#define	ICE_SFF_8472_BASE	0xa0
#define	ICE_SFF_8472_DIAG	0xa2
#define	ICE_SFF_PAGE_LEN	256
#define	ICE_SFF_READ_CHUNK	16

static int
ice_transceiver_info(void *arg, uint_t id, mac_transceiver_info_t *infop)
{
	ice_t *ice = arg;
	struct ice_link_status *li;
	boolean_t present, usable;

	if (id != 0 || infop == NULL)
		return (EINVAL);

	/*
	 * ice_rebuild_lock is the outermost lock: hold it so a reset
	 * rebuild cannot reinitialize port_info underneath this
	 * read.  Read link_info under the lock rather than snapshotting the
	 * pointer earlier.
	 */
	mutex_enter(&ice->ice_rebuild_lock);
	mutex_enter(&ice->ice_lock);
	li = &ice->ice_hw.port_info->phy.link_info;
	present = (li->link_info & ICE_AQ_MEDIA_AVAILABLE) != 0;
	usable = present && (li->an_info & ICE_AQ_QUALIFIED_MODULE) != 0;
	mutex_exit(&ice->ice_lock);
	mutex_exit(&ice->ice_rebuild_lock);

	mac_transceiver_info_set_present(infop, present);
	mac_transceiver_info_set_usable(infop, usable);

	return (0);
}

static int
ice_transceiver_read(void *arg, uint_t id, uint_t page, void *buf,
    size_t nbytes, off_t offset, size_t *nread)
{
	ice_t *ice = arg;
	struct ice_hw *hw = &ice->ice_hw;
	uint8_t *out = buf;
	size_t i;

	if (id != 0 || buf == NULL || nbytes == 0 || nread == NULL ||
	    (page != ICE_SFF_8472_BASE && page != ICE_SFF_8472_DIAG) ||
	    offset < 0)
		return (EINVAL);
	if (nbytes > ICE_SFF_PAGE_LEN || offset >= ICE_SFF_PAGE_LEN ||
	    offset + nbytes > ICE_SFF_PAGE_LEN)
		return (EINVAL);

	/*
	 * ice_rebuild_lock is the outermost lock: hold it across the
	 * admin-queue SFF reads so a reset rebuild cannot tear the control
	 * queue down underneath ice_aq_sff_eeprom().  ice_lock is not taken:
	 * it is an interrupt-priority mutex, each command can poll firmware
	 * for up to a second, and any /dev/dld user in the link's zone can
	 * issue this read.  The core's sq_lock serializes the commands.
	 */
	mutex_enter(&ice->ice_rebuild_lock);
	for (i = 0; i < nbytes; ) {
		uint8_t len = (uint8_t)MIN(nbytes - i, ICE_SFF_READ_CHUNK);

		if (ice_aq_sff_eeprom(hw, 0, (uint8_t)page,
		    (uint16_t)(offset + i), 0, 0, &out[i], len, false,
		    NULL) != ICE_SUCCESS) {
			mutex_exit(&ice->ice_rebuild_lock);
			return (EIO);
		}
		i += len;
	}
	mutex_exit(&ice->ice_rebuild_lock);

	*nread = nbytes;
	return (0);
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

	case MAC_CAPAB_HCKSUM: {
		uint32_t *txflags = cap_data;

		/*
		 * Without the DDP package the pipeline cannot compute
		 * checksums; see ice_set_safe_mode_caps().
		 */
		if (ice->ice_safe_mode)
			return (B_FALSE);

		*txflags = HCKSUM_INET_PARTIAL | HCKSUM_IPHDRCKSUM;
		break;
	}

	case MAC_CAPAB_TRANSCEIVER: {
		mac_capab_transceiver_t *mct = cap_data;

		mct->mct_flags = 0;
		mct->mct_ntransceivers = 1;
		mct->mct_info = ice_transceiver_info;
		mct->mct_read = ice_transceiver_read;
		break;
	}

	case MAC_CAPAB_LSO: {
		mac_capab_lso_t *cap_lso = cap_data;

		/*
		 * LSO also depends on the Tx checksum offload that safe mode
		 * withholds, so it has to go at the same time.
		 */
		if (ice->ice_safe_mode || !ice->ice_tx_lso_enable)
			return (B_FALSE);

		cap_lso->lso_flags = LSO_TX_BASIC_TCP_IPV4 |
		    LSO_TX_BASIC_TCP_IPV6;
		cap_lso->lso_basic_tcp_ipv4.lso_max = ICE_LSO_MAXLEN;
		cap_lso->lso_basic_tcp_ipv6.lso_max = ICE_LSO_MAXLEN;
		break;
	}

	default:
		return (B_FALSE);
	}

	return (B_TRUE);
}

static int
ice_m_setprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, const void *pr_val)
{
	ice_t *ice = arg;
	uint32_t mtu;
	int ret;

#ifdef DEBUG
	/*
	 * Test hook: "dladm set-linkprop -p _reset=1 ice0" injects a reset
	 * rebuild so the recovery path can be exercised without a hardware
	 * fault.  DEBUG builds only; production resets arrive through the OICR
	 * fatal-cause/global-reset path.
	 */
	if (pr_num == MAC_PROP_PRIVATE && strcmp(pr_name, "_reset") == 0) {
		atomic_or_32(&ice->ice_state, ICE_STATE_PFR_REQ);
		ice_reset_dispatch(ice);
		return (0);
	}
#else
	_NOTE(ARGUNUSED(pr_name));
#endif

	switch (pr_num) {
	case MAC_PROP_MTU: {
		if (pr_valsize < sizeof (mtu))
			return (EINVAL);
		bcopy(pr_val, &mtu, sizeof (mtu));
		if (mtu == ice->ice_mtu)
			return (0);
		if (mtu < ICE_MIN_MTU || mtu > ICE_MAX_MTU)
			return (EINVAL);
		if (ice->ice_state & ICE_STATE_STARTED)
			return (EBUSY);

		ret = mac_maxsdu_update(ice->ice_mac_hdl, mtu);
		if (ret != 0)
			return (ret);
		ice->ice_mtu = mtu;
		ice_update_mtu(ice);
		return (0);
	}
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
	link_state_t state;
	uint16_t phy_speed = 0;
	boolean_t advertised = B_FALSE;
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
		state = ice_link_state_effective(ice, ice->ice_link_state);
		bcopy(&state, pr_val, sizeof (state));
		break;
	case MAC_PROP_AUTONEG:
		if (pr_valsize < sizeof (uint8_t)) {
			ret = EOVERFLOW;
			break;
		}
		u8 = pr_val;
		*u8 = 1;
		break;
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_EN_100FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_100FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_100MB;
		break;
	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_EN_1000FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_1000FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_1000MB;
		break;
	case MAC_PROP_ADV_2500FDX_CAP:
	case MAC_PROP_EN_2500FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_2500FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_2500MB;
		break;
	case MAC_PROP_ADV_5000FDX_CAP:
	case MAC_PROP_EN_5000FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_5000FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_5GB;
		break;
	case MAC_PROP_ADV_10GFDX_CAP:
	case MAC_PROP_EN_10GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_10GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_10GB;
		break;
	case MAC_PROP_ADV_25GFDX_CAP:
	case MAC_PROP_EN_25GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_25GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_25GB;
		break;
	case MAC_PROP_ADV_40GFDX_CAP:
	case MAC_PROP_EN_40GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_40GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_40GB;
		break;
	case MAC_PROP_ADV_50GFDX_CAP:
	case MAC_PROP_EN_50GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_50GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_50GB;
		break;
	case MAC_PROP_ADV_100GFDX_CAP:
	case MAC_PROP_EN_100GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_100GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_100GB;
		break;
	case MAC_PROP_ADV_FEC_CAP:
		if (pr_valsize < sizeof (link_fec_t)) {
			ret = EOVERFLOW;
			break;
		}
		*(link_fec_t *)pr_val = ice->ice_fec_neg;
		break;
	case MAC_PROP_EN_FEC_CAP:
		if (pr_valsize < sizeof (link_fec_t)) {
			ret = EOVERFLOW;
			break;
		}
		*(link_fec_t *)pr_val = LINK_FEC_AUTO;
		break;
	case MAC_PROP_FLOWCTRL:
		if (pr_valsize < sizeof (link_flowctrl_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&ice->ice_link_fctl, pr_val, sizeof (link_flowctrl_t));
		break;
	case MAC_PROP_MTU:
		if (pr_valsize < sizeof (uint32_t)) {
			ret = EOVERFLOW;
			break;
		}
		bcopy(&ice->ice_mtu, pr_val, sizeof (uint32_t));
		break;
	default:
		ret = ENOTSUP;
		break;
	}

	if (phy_speed != 0) {
		if (pr_valsize < sizeof (uint8_t)) {
			ret = EOVERFLOW;
		} else {
			u8 = pr_val;
			if (advertised) {
				*u8 = (ice->ice_phy_speeds_adv &
				    phy_speed) != 0;
			} else {
				*u8 = (ice->ice_phy_speeds_supp &
				    phy_speed) != 0;
			}
		}
	}

	mutex_exit(&ice->ice_lse_lock);

	return (ret);
}

static void
ice_m_propinfo(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    mac_prop_info_handle_t prh)
{
	ice_t *ice = arg;
	uint16_t phy_speed = 0;
	boolean_t advertised = B_FALSE;

	_NOTE(ARGUNUSED(pr_name));

	mutex_enter(&ice->ice_lse_lock);

	switch (pr_num) {
	case MAC_PROP_DUPLEX:
	case MAC_PROP_SPEED:
	case MAC_PROP_STATUS:
	case MAC_PROP_FLOWCTRL:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		break;
	case MAC_PROP_AUTONEG:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		mac_prop_info_set_default_uint8(prh, 1);
		break;
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_EN_100FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_100FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_100MB;
		break;
	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_EN_1000FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_1000FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_1000MB;
		break;
	case MAC_PROP_ADV_2500FDX_CAP:
	case MAC_PROP_EN_2500FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_2500FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_2500MB;
		break;
	case MAC_PROP_ADV_5000FDX_CAP:
	case MAC_PROP_EN_5000FDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_5000FDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_5GB;
		break;
	case MAC_PROP_ADV_10GFDX_CAP:
	case MAC_PROP_EN_10GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_10GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_10GB;
		break;
	case MAC_PROP_ADV_25GFDX_CAP:
	case MAC_PROP_EN_25GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_25GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_25GB;
		break;
	case MAC_PROP_ADV_40GFDX_CAP:
	case MAC_PROP_EN_40GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_40GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_40GB;
		break;
	case MAC_PROP_ADV_50GFDX_CAP:
	case MAC_PROP_EN_50GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_50GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_50GB;
		break;
	case MAC_PROP_ADV_100GFDX_CAP:
	case MAC_PROP_EN_100GFDX_CAP:
		advertised = pr_num == MAC_PROP_ADV_100GFDX_CAP;
		phy_speed = ICE_AQ_LINK_SPEED_100GB;
		break;
	case MAC_PROP_ADV_FEC_CAP:
	case MAC_PROP_EN_FEC_CAP:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		mac_prop_info_set_default_fec(prh, LINK_FEC_AUTO);
		break;
	case MAC_PROP_MTU:
		mac_prop_info_set_range_uint32(prh, ICE_MIN_MTU,
		    ICE_MAX_MTU);
		break;
	default:
		break;
	}

	if (phy_speed != 0) {
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		if (advertised) {
			mac_prop_info_set_default_uint8(prh,
			    (ice->ice_phy_speeds_adv & phy_speed) != 0);
		} else {
			mac_prop_info_set_default_uint8(prh,
			    (ice->ice_phy_speeds_supp & phy_speed) != 0);
		}
	}

	mutex_exit(&ice->ice_lse_lock);
}

static mac_callbacks_t ice_m_callbacks = {
	.mc_callbacks = MC_IOCTL | MC_GETCAPAB | MC_SETPROP | MC_GETPROP |
	    MC_PROPINFO,
	.mc_getstat = ice_m_stat,
	.mc_start = ice_m_start,
	.mc_stop = ice_m_stop,
	.mc_setpromisc = ice_m_promisc,
	.mc_multicst = ice_m_multicst,
	.mc_unicst = NULL,		/* rx groups: unicast via addmac */
	.mc_tx = NULL,			/* tx is per-ring (MAC_CAPAB_RINGS) */
	.mc_ioctl = ice_m_ioctl,
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
	mac->m_max_sdu = ice->ice_mtu;
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

	/* A concurrent MAC start must finish before initial publication. */
	mutex_enter(&ice->ice_rebuild_lock);
	ice_link_state_publish(ice);
	mutex_exit(&ice->ice_rebuild_lock);

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
		ice_error(ice, "failed to unregister from MAC: %d", status);
		return (status);
	}

	/*
	 * Clear the handle under ice_lse_lock: ice_link_state_set() reads
	 * ice_mac_hdl under the same lock, so a concurrent async link update
	 * cannot observe a torn (half-cleared) handle.
	 */
	mutex_enter(&ice->ice_lse_lock);
	ice->ice_mac_hdl = NULL;
	mutex_exit(&ice->ice_lse_lock);
	return (0);
}

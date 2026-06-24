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
 * Control-plane setup for the PF's data VSI: create the VSI, program the
 * default unicast and broadcast MAC filters through the switch, and configure
 * RSS (hash key, redirection table, and the hashed flow types), all through
 * the Intel common code under core/.  The transmit and receive rings, queue
 * contexts, and MAC registration are a later milestone; without them the VSI
 * receives no traffic yet.
 *
 * Identifier values that originate in firmware are validated before the driver
 * uses them as an index: the VSI handle is trusted only after ice_is_vsi_valid,
 * and the firmware-assigned hardware VSI number is bounds-checked before it is
 * stored.
 */

#include <sys/random.h>

#include "ice.h"
#include "ice_common.h"
#include "ice_switch.h"
#include "ice_flow.h"

static const uint8_t ice_bcast_addr[ETHERADDRL] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/*
 * The flow types hashed for RSS: IPv4 and IPv6, each over TCP and UDP.
 */
static const struct {
	uint64_t	irf_hash;
	uint32_t	irf_hdrs;
} ice_rss_flows[] = {
	{ ICE_HASH_TCP_IPV4, ICE_FLOW_SEG_HDR_IPV4 | ICE_FLOW_SEG_HDR_TCP },
	{ ICE_HASH_UDP_IPV4, ICE_FLOW_SEG_HDR_IPV4 | ICE_FLOW_SEG_HDR_UDP },
	{ ICE_HASH_TCP_IPV6, ICE_FLOW_SEG_HDR_IPV6 | ICE_FLOW_SEG_HDR_TCP },
	{ ICE_HASH_UDP_IPV6, ICE_FLOW_SEG_HDR_IPV6 | ICE_FLOW_SEG_HDR_UDP }
};

static void
ice_fltr_entry_init(struct ice_fltr_list_entry *e, uint16_t handle,
    const uint8_t *addr)
{
	bzero(e, sizeof (*e));
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

/*
 * Tear down everything ice_vsi_setup() may have created.  Safe to call on a
 * partially-initialized VSI: each step is gated on the state it undoes.  The
 * Tx scheduler nodes reserved by ice_cfg_vsi_lan() and the switch filter state
 * are reclaimed by ice_deinit_hw() (a lower attach-progress bit, undone later
 * in ice_unconfigure()); the best-effort removals here keep state tidy on a
 * normal detach.
 */
static void
ice_vsi_teardown(ice_t *ice)
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

	if (vsi->vi_added) {
		struct ice_vsi_ctx *ctx = ice_get_vsi_ctx(hw, vsi->vi_handle);

		(void) ice_free_vsi(hw, vsi->vi_handle, ctx, false, NULL);
		vsi->vi_added = B_FALSE;
	}
}

/*
 * Build the VSI context for a basic PF data VSI: a contiguous block of queues
 * on TC0 with a per-VSI RSS LUT and Toeplitz hashing.  The buffer is sent in
 * full, so it must start zeroed.
 */
static void
ice_vsi_ctx_fill(ice_t *ice, struct ice_vsi_ctx *ctx)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	uint16_t nq = vsi->vi_nrxq;

	bzero(ctx, sizeof (*ctx));

	ctx->flags = ICE_AQ_VSI_TYPE_PF;
	ctx->alloc_from_pool = 1;	/* firmware assigns the HW VSI number */
	ctx->vf_num = 0;

	ctx->info.valid_sections = CPU_TO_LE16(ICE_AQ_VSI_PROP_SW_VALID |
	    ICE_AQ_VSI_PROP_RXQ_MAP_VALID | ICE_AQ_VSI_PROP_Q_OPT_VALID);

	/* switch ids are small; the field is a u8 */
	ctx->info.sw_id = (u8)hw->port_info->sw_id;
	ctx->info.sw_flags = ICE_AQ_VSI_SW_FLAG_ALLOW_LB;
	ctx->info.sw_flags2 = ICE_AQ_VSI_SW_FLAG_LAN_ENA;

	ctx->info.mapping_flags = CPU_TO_LE16(ICE_AQ_VSI_Q_MAP_CONTIG);
	ctx->info.q_mapping[0] = CPU_TO_LE16(0);
	ctx->info.tc_mapping[0] = CPU_TO_LE16(
	    (0 << ICE_AQ_VSI_TC_Q_OFFSET_S) |
	    ((uint16_t)ice_ilog2(nq) << ICE_AQ_VSI_TC_Q_NUM_S));

	ctx->info.q_opt_rss = (ICE_AQ_VSI_Q_OPT_RSS_LUT_VSI &
	    ICE_AQ_VSI_Q_OPT_RSS_LUT_M) | ICE_AQ_VSI_Q_OPT_RSS_TPLZ;
}

static int
ice_vsi_setup(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_vsi_ctx ctx;
	uint16_t max_lanqs[ICE_MAX_TRAFFIC_CLASS];
	int status;

	vsi->vi_handle = ICE_PF_VSI_HANDLE;

	/*
	 * One rx and one tx queue for this milestone; the data path grows
	 * these later.  The count must be a power of two for the VSI context
	 * and the scheduler.
	 */
	vsi->vi_nrxq = 1;
	vsi->vi_ntxq = 1;

	ice_vsi_ctx_fill(ice, &ctx);

	status = ice_add_vsi(hw, vsi->vi_handle, &ctx, NULL);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to add PF VSI: %d", status);
		return (status);
	}
	vsi->vi_added = B_TRUE;

	/*
	 * Trust the handle only after ice_is_vsi_valid(), and bounds-check the
	 * firmware-assigned HW VSI number before storing it: it is firmware
	 * input the driver will later use to index per-VSI state.
	 */
	if (!ice_is_vsi_valid(hw, vsi->vi_handle)) {
		ice_error(ice, "PF VSI handle %u invalid after add",
		    vsi->vi_handle);
		ice_vsi_teardown(ice);
		return (ICE_ERR_PARAM);
	}
	vsi->vi_hw_num = ice_get_hw_vsi_num(hw, vsi->vi_handle);
	if (vsi->vi_hw_num >= ICE_MAX_VSI) {
		ice_error(ice, "firmware returned out-of-range VSI number %u",
		    vsi->vi_hw_num);
		ice_vsi_teardown(ice);
		return (ICE_ERR_PARAM);
	}

	/*
	 * Reserve scheduler nodes for the planned tx queue count.  max_lanqs is
	 * indexed per traffic class by the common code, so it must be a full
	 * ICE_MAX_TRAFFIC_CLASS array even though only TC0 is enabled.
	 */
	bzero(max_lanqs, sizeof (max_lanqs));
	max_lanqs[0] = vsi->vi_ntxq;
	status = ice_cfg_vsi_lan(hw->port_info, vsi->vi_handle, BIT(0),
	    max_lanqs);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to configure VSI scheduler: %d", status);
		ice_vsi_teardown(ice);
		return (status);
	}

	return (ICE_SUCCESS);
}

static int
ice_add_mac_filters(ice_t *ice)
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

static int
ice_rss_setup(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_vsi_t *vsi = &ice->ice_pf_vsi;
	struct ice_aqc_get_set_rss_keys key;
	struct ice_aq_get_set_rss_lut_params lp;
	uint8_t lut[ICE_LUT_VSI_SIZE];
	uint_t i;
	int status;

	(void) random_get_pseudo_bytes((uint8_t *)&key, sizeof (key));
	status = ice_aq_set_rss_key(hw, vsi->vi_handle, &key);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to set RSS key: %d", status);
		return (status);
	}

	/* Round-robin the redirection table over the planned rx queues. */
	ASSERT3U(vsi->vi_nrxq, >, 0);
	for (i = 0; i < ICE_LUT_VSI_SIZE; i++)
		lut[i] = (uint8_t)(i % vsi->vi_nrxq);

	bzero(&lp, sizeof (lp));
	lp.vsi_handle = vsi->vi_handle;
	lp.lut_type = ICE_LUT_VSI;
	lp.lut_size = ICE_LUT_VSI_SIZE;
	lp.lut = lut;
	status = ice_aq_set_rss_lut(hw, &lp);
	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to set RSS LUT: %d", status);
		return (status);
	}

	for (i = 0; i < ARRAY_SIZE(ice_rss_flows); i++) {
		struct ice_rss_hash_cfg cfg;

		bzero(&cfg, sizeof (cfg));
		cfg.hash_flds = ice_rss_flows[i].irf_hash;
		cfg.addl_hdrs = ice_rss_flows[i].irf_hdrs;
		cfg.hdr_type = ICE_RSS_OUTER_HEADERS;
		cfg.symm = false;

		status = ice_add_rss_cfg(hw, vsi->vi_handle, &cfg);
		if (status != ICE_SUCCESS) {
			ice_error(ice, "failed to add RSS flow %u: %d", i,
			    status);
			return (status);
		}
	}

	vsi->vi_rss_set = B_TRUE;
	return (ICE_SUCCESS);
}

boolean_t
ice_vsi_init(ice_t *ice)
{
	ice_vsi_t *vsi = &ice->ice_pf_vsi;

	mutex_init(&vsi->vi_mac_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&vsi->vi_macs, sizeof (ice_mac_filter_t),
	    offsetof(ice_mac_filter_t, imf_node));

	if (ice_vsi_setup(ice) != ICE_SUCCESS)
		goto fail;
	if (ice_add_mac_filters(ice) != ICE_SUCCESS)
		goto fail;
	if (ice_rss_setup(ice) != ICE_SUCCESS)
		goto fail;

	return (B_TRUE);

fail:
	ice_vsi_teardown(ice);
	list_destroy(&vsi->vi_macs);
	mutex_destroy(&vsi->vi_mac_lock);
	return (B_FALSE);
}

void
ice_vsi_fini(ice_t *ice)
{
	ice_vsi_t *vsi = &ice->ice_pf_vsi;

	ice_vsi_teardown(ice);
	list_destroy(&vsi->vi_macs);
	mutex_destroy(&vsi->vi_mac_lock);
}

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
 * Copyright 2026 RackTop Systems, Inc.
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Device bring-up that attach and the reset rebuild share: FMA setup,
 * register mapping, firmware checks, capability validation, the per-family
 * decisions, and interrupt vector allocation.  None of it holds lifecycle
 * policy; ice.c decides when each step runs.
 */

#include <sys/cmn_err.h>
#include <sys/cpuvar.h>

#include "ice.h"
#include "ice_common.h"

/*
 * Control-queue depths and buffer sizes.  ice_init_hw() requires the caller to
 * size the queues.  These depths mirror the FreeBSD ice driver's ICE_AQ_LEN,
 * ICE_MBXQ_LEN, and ICE_SBQ_LEN (sys/dev/ice/ice_lib.h); the ICE_*_MAX_BUF_LEN
 * buffer sizes come from the common code (core/ice_controlq.h).
 */
#define	ICE_AQ_LEN		1023
#define	ICE_MBXQ_LEN		512
#define	ICE_SBQ_LEN		512

/* Bound on the E830 PHY firmware load wait; see ice_phy_fw_wait(). */
#define	ICE_PHY_FW_WAIT_MS	30000
#define	ICE_PHY_FW_POLL_MS	100

/* GL_MNG_FWSM.FW_MODES bit 1: firmware runs in recovery mode. */
#define	ICE_FWSM_MODE_RECOVERY	BIT(1)

static int
ice_fm_error_cb(dev_info_t *dip, ddi_fm_error_t *err, const void *arg __unused)
{
	pci_ereport_post(dip, err, NULL);
	return (err->fme_status);
}

void
ice_fm_init(ice_t *ice)
{
	ddi_iblock_cookie_t iblk;

	/*
	 * The data path checks DMA handles after synchronization, and all DMA
	 * attributes honor the negotiated capability with DDI_DMA_FLAGERR.
	 */
	ice->ice_fm_caps = ddi_prop_get_int(DDI_DEV_T_ANY, ice->ice_dip,
	    DDI_PROP_DONTPASS, "fm-capable",
	    DDI_FM_EREPORT_CAPABLE | DDI_FM_ACCCHK_CAPABLE |
	    DDI_FM_DMACHK_CAPABLE | DDI_FM_ERRCB_CAPABLE);

	if (ice->ice_fm_caps < 0)
		ice->ice_fm_caps = 0;
	ice->ice_fm_caps &= (DDI_FM_EREPORT_CAPABLE | DDI_FM_ACCCHK_CAPABLE |
	    DDI_FM_DMACHK_CAPABLE | DDI_FM_ERRCB_CAPABLE);

	if (ice->ice_fm_caps == 0)
		return;

	ddi_fm_init(ice->ice_dip, &ice->ice_fm_caps, &iblk);

	if (DDI_FM_EREPORT_CAP(ice->ice_fm_caps) ||
	    DDI_FM_ERRCB_CAP(ice->ice_fm_caps))
		pci_ereport_setup(ice->ice_dip);
	if (DDI_FM_ERRCB_CAP(ice->ice_fm_caps))
		ddi_fm_handler_register(ice->ice_dip, ice_fm_error_cb, ice);
}

void
ice_fm_fini(ice_t *ice)
{
	if (ice->ice_fm_caps == 0)
		return;

	if (DDI_FM_ERRCB_CAP(ice->ice_fm_caps))
		ddi_fm_handler_unregister(ice->ice_dip);
	if (DDI_FM_EREPORT_CAP(ice->ice_fm_caps) ||
	    DDI_FM_ERRCB_CAP(ice->ice_fm_caps))
		pci_ereport_teardown(ice->ice_dip);

	ddi_fm_fini(ice->ice_dip);
}

/*
 * Read the PCI identity straight into struct ice_hw; ice_init_hw() ->
 * ice_set_mac_type() consumes the vendor/device IDs, so they must be in place
 * before bring-up.
 */
void
ice_identify_hardware(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	ddi_acc_handle_t cfg = ice->ice_osdep.ios_cfg_handle;

	hw->vendor_id = pci_config_get16(cfg, PCI_CONF_VENID);
	hw->device_id = pci_config_get16(cfg, PCI_CONF_DEVID);
	hw->revision_id = pci_config_get8(cfg, PCI_CONF_REVID);
	hw->subsystem_vendor_id = pci_config_get16(cfg, PCI_CONF_SUBVENID);
	hw->subsystem_device_id = pci_config_get16(cfg, PCI_CONF_SUBSYSID);
}

boolean_t
ice_regs_map(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	struct ice_osdep *osdep = &ice->ice_osdep;
	ddi_device_acc_attr_t attr;
	off_t memsize;
	int ret;

	if (ddi_dev_regsize(ice->ice_dip, ICE_REG_NUMBER, &memsize) !=
	    DDI_SUCCESS) {
		ice_error(ice, "failed to get BAR0 register set size");
		return (B_FALSE);
	}

	attr.devacc_attr_version = DDI_DEVICE_ATTR_V1;
	attr.devacc_attr_endian_flags = DDI_STRUCTURE_LE_ACC;
	attr.devacc_attr_dataorder = DDI_STRICTORDER_ACC;
	if ((ice->ice_fm_caps & DDI_FM_ACCCHK_CAPABLE) != 0)
		attr.devacc_attr_access = DDI_FLAGERR_ACC;
	else
		attr.devacc_attr_access = DDI_DEFAULT_ACC;

	ret = ddi_regs_map_setup(ice->ice_dip, ICE_REG_NUMBER,
	    (caddr_t *)&hw->hw_addr, 0, memsize, &attr, &osdep->ios_reg_handle);
	if (ret != DDI_SUCCESS) {
		ice_error(ice, "failed to map BAR0 registers: %d", ret);
		return (B_FALSE);
	}

	osdep->ios_reg_size = memsize;
	return (B_TRUE);
}

/*
 * Sanity-check and clamp the firmware-supplied capabilities the driver will
 * later use to size allocations or index arrays.  ice_init_hw() has already
 * consumed the raw capability data; the Intel common code is the trusted
 * in-tree consumer of that path, so this guards only the driver's own
 * subsequent use.  Only the fields below are validated here -- any other
 * firmware-supplied count, length, or base id (notably the *_first_id bases
 * and rss_table_size) must be re-checked at its point of use.
 */
boolean_t
ice_validate_caps(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	struct ice_hw_common_caps *c = &hw->func_caps.common_cap;

	if (c->num_rxq == 0 || c->num_rxq > ICE_HW_MAX_RXQ ||
	    c->num_txq == 0 || c->num_txq > ICE_HW_MAX_TXQ ||
	    c->num_msix_vectors == 0 ||
	    c->num_msix_vectors > ICE_HW_MAX_MSIX) {
		ice_error(ice, "implausible queue/vector counts from firmware "
		    "(rxq %u txq %u msix %u)", c->num_rxq, c->num_txq,
		    c->num_msix_vectors);
		return (B_FALSE);
	}

	if (hw->func_caps.guar_num_vsi == 0 ||
	    hw->func_caps.guar_num_vsi > ICE_MAX_VSI) {
		ice_error(ice, "implausible guaranteed VSI count %u",
		    hw->func_caps.guar_num_vsi);
		return (B_FALSE);
	}

	if (c->max_mtu < ICE_MIN_MTU ||
	    c->max_mtu > ICE_MAX_FRAME_SIZE) {
		ice_error(ice, "implausible maximum MTU %u", c->max_mtu);
		return (B_FALSE);
	}

	if (hw->dev_caps.num_funcs == 0 ||
	    hw->dev_caps.num_funcs > ICE_MAX_FUNCS) {
		ice_error(ice, "implausible function count %u",
		    hw->dev_caps.num_funcs);
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * The control-queue depths and buffer sizes are the caller's responsibility;
 * ice_init_hw() rejects an unconfigured queue.  The sideband queue is only
 * brought up on the parts that support it, but sizing it is harmless.
 */
static void
ice_set_ctrlq_len(struct ice_hw *hw)
{
	hw->adminq.num_rq_entries = ICE_AQ_LEN;
	hw->adminq.num_sq_entries = ICE_AQ_LEN;
	hw->adminq.rq_buf_size = ICE_AQ_MAX_BUF_LEN;
	hw->adminq.sq_buf_size = ICE_AQ_MAX_BUF_LEN;

	hw->mailboxq.num_rq_entries = ICE_MBXQ_LEN;
	hw->mailboxq.num_sq_entries = ICE_MBXQ_LEN;
	hw->mailboxq.rq_buf_size = ICE_MBXQ_MAX_BUF_LEN;
	hw->mailboxq.sq_buf_size = ICE_MBXQ_MAX_BUF_LEN;

	hw->sbq.num_rq_entries = ICE_SBQ_LEN;
	hw->sbq.num_sq_entries = ICE_SBQ_LEN;
	hw->sbq.rq_buf_size = ICE_SBQ_MAX_BUF_LEN;
	hw->sbq.sq_buf_size = ICE_SBQ_MAX_BUF_LEN;
}

/*
 * Report whether firmware can run the device.  ice_get_fw_mode() tests the
 * debug bit first, so a device in both debug and recovery mode reads as DBG;
 * the recovery bit is tested as well, within the per-MAC width of the field.
 * A faulted read proves nothing either way, and the check consumes the fault,
 * so the caller must fail on ICE_FW_UNREADABLE itself.
 */
ice_fw_state_t
ice_fw_state(ice_t *ice, uint32_t *fwsmp)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t fwsm;

	fwsm = rd32(hw, GL_MNG_FWSM);
	*fwsmp = fwsm;
	if (ice_check_acc_handle(ice, ice->ice_osdep.ios_reg_handle) !=
	    DDI_FM_OK)
		return (ICE_FW_UNREADABLE);

	if (ice_get_fw_mode(hw) == ICE_FW_MODE_REC ||
	    (fwsm & GL_MNG_FWSM_FW_MODES_M_BY_MAC(hw) &
	    ICE_FWSM_MODE_RECOVERY) != 0)
		return (ICE_FW_RECOVERY);

	return (ICE_FW_USABLE);
}

/*
 * Firmware in recovery mode cannot run the device; only an NVM update can
 * repair it.  Post one ereport, mark the service lost, and tell the operator
 * what to do.  The callers then fail closed.
 */
void
ice_fw_recovery_report(ice_t *ice, uint32_t fwsm)
{
	char class[FM_MAX_CLASS];

	if (DDI_FM_EREPORT_CAP(ice->ice_fm_caps)) {
		(void) snprintf(class, sizeof (class), "%s.%s", DDI_FM_DEVICE,
		    DDI_FM_DEVICE_FW_CORRUPT);
		ddi_fm_ereport_post(ice->ice_dip, class,
		    fm_ena_generate(0, FM_ENA_FMT1), DDI_NOSLEEP,
		    FM_VERSION, DATA_TYPE_UINT8, FM_EREPORT_VERS0,
		    "fw_mode", DATA_TYPE_STRING, "recovery",
		    "mng_fwsm", DATA_TYPE_UINT32, fwsm, NULL);
	}
	ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_LOST);
	dev_err(ice->ice_dip, CE_WARN, "firmware is in recovery mode "
	    "(GL_MNG_FWSM 0x%x); the device is not usable.  Update the NVM "
	    "with the Intel NVM update tool, then power cycle the system",
	    fwsm);
}

/*
 * Name the family for messages.  The common code keys each family difference
 * on hw->mac_type.  Only the E822 and E823 parts share a MAC type, and the
 * device ID separates them.  NULL marks a MAC type that no supported device
 * ID produces.
 */
static const char *
ice_family_name(struct ice_hw *hw)
{
	switch (hw->mac_type) {
	case ICE_MAC_E810:
		return ("E810");
	case ICE_MAC_GENERIC:
		return (ice_is_e823(hw) ? "E823" : "E822");
	case ICE_MAC_GENERIC_3K_E825:
		return ("E825-C");
	case ICE_MAC_E830:
		return ("E830");
	default:
		return (NULL);
	}
}

boolean_t
ice_hw_init(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	const char *family;
	uint32_t fwsm;
	int rc;

	/*
	 * An alias added by hand can bind a device ID that the common code
	 * does not map.  Reject it before any register access: the common
	 * code selects registers, queue types and firmware formats from
	 * the MAC type.
	 */
	if (ice_set_mac_type(hw) != 0 ||
	    (family = ice_family_name(hw)) == NULL) {
		ice_error(ice, "unsupported device %04x:%04x",
		    hw->vendor_id, hw->device_id);
		return (B_FALSE);
	}

	/*
	 * Recovery firmware does not accept the normal initialization, and
	 * nothing the driver builds on it is usable.  Detect it before any
	 * admin queue work.
	 */
	switch (ice_fw_state(ice, &fwsm)) {
	case ICE_FW_USABLE:
		break;
	case ICE_FW_RECOVERY:
		ice_fw_recovery_report(ice, fwsm);
		return (B_FALSE);
	default:
		ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_LOST);
		ice_error(ice, "cannot read the firmware state");
		return (B_FALSE);
	}

	ice_set_ctrlq_len(hw);

	/*
	 * ice_init_hw() performs the PF reset, brings up the admin queue, and
	 * reads NVM and capabilities.  On failure it has already unwound its
	 * own state, so ice_deinit_hw() must not be called.
	 */
	rc = ice_init_hw(hw);
	if (rc == ICE_ERR_FW_API_VER) {
		dev_err(ice->ice_dip, CE_WARN, "firmware API version %u.%u is "
		    "not supported by this driver; update the NVM or the "
		    "driver", hw->api_maj_ver, hw->api_min_ver);
		return (B_FALSE);
	}
	if (rc != 0) {
		ice_error(ice, "hardware initialization failed: %d", rc);
		return (B_FALSE);
	}

	if (!ice_validate_caps(ice)) {
		ice_deinit_hw(hw);
		return (B_FALSE);
	}

	dev_err(ice->ice_dip, CE_NOTE, "!%s device %04x, PF %u, firmware "
	    "%u.%u.%u, API %u.%u.%u", family, hw->device_id, hw->pf_id,
	    hw->fw_maj_ver, hw->fw_min_ver, hw->fw_patch, hw->api_maj_ver,
	    hw->api_min_ver, hw->api_patch);

	return (B_TRUE);
}

/*
 * After an EMPR, E825-C and E830 firmware can take longer to finish than
 * ice_check_reset() waits.  FreeBSD pauses for 20 seconds before it polls on
 * those parts (ICE_EMPR_ADDL_WAIT_MSEC_SLOW in if_ice_iflib.c).
 */
boolean_t
ice_reset_empr_slow(struct ice_hw *hw)
{
	uint32_t type;

	if (!ice_is_e830(hw) && !ice_is_e825c(hw))
		return (B_FALSE);

	type = (rd32(hw, GLGEN_RSTAT) & GLGEN_RSTAT_RESET_TYPE_M) >>
	    GLGEN_RSTAT_RESET_TYPE_S;
	return (type == ICE_RESET_EMPR);
}

/*
 * E830 firmware loads the PHY firmware after the PF comes up, and PHY
 * configuration fails until the load completes.  No interrupt reports the
 * completion.  The access check consumes a fault, so the caller must fail on
 * ICE_PHY_FW_UNREADABLE itself.
 */
ice_phy_fw_state_t
ice_phy_fw_state(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t fwsm;

	if (!ice_is_e830(hw))
		return (ICE_PHY_FW_READY);

	fwsm = rd32(hw, GL_MNG_FWSM);
	if (ice_check_acc_handle(ice, ice->ice_osdep.ios_reg_handle) !=
	    DDI_FM_OK)
		return (ICE_PHY_FW_UNREADABLE);
	if ((fwsm & GL_MNG_FWSM_FW_LOADING_M) != 0)
		return (ICE_PHY_FW_LOADING);
	return (ICE_PHY_FW_READY);
}

/*
 * Wait a bounded time for the E830 PHY firmware, as Linux does
 * (ice_wait_fw_load()).  On ICE_PHY_FW_LOADING the caller leaves the PHY
 * setup to the admin worker (ice_phy_fw_poll()).
 */
ice_phy_fw_state_t
ice_phy_fw_wait(ice_t *ice)
{
	ice_phy_fw_state_t state;
	uint_t waited = 0;

	while ((state = ice_phy_fw_state(ice)) == ICE_PHY_FW_LOADING) {
		if (waited >= ICE_PHY_FW_WAIT_MS) {
			ice_error(ice, "PHY firmware still loading after "
			    "%u ms; link setup deferred", waited);
			break;
		}
		delay(drv_usectohz(ICE_PHY_FW_POLL_MS * (MICROSEC / MILLISEC)));
		waited += ICE_PHY_FW_POLL_MS;
	}

	return (state);
}

/* The ice.conf ceiling on the queue pair count. */
static uint32_t
ice_prop_get_num_queues(ice_t *ice)
{
	int value, clamped;

	value = ddi_prop_get_int(DDI_DEV_T_ANY, ice->ice_dip, 0, "num_queues",
	    ICE_DEF_QUEUES);
	clamped = MIN(MAX(value, 1), ICE_MAX_QUEUES);
	if (clamped != value) {
		ice_error(ice, "num_queues %d is outside 1 to %d; using %d",
		    value, ICE_MAX_QUEUES, clamped);
	}
	return ((uint32_t)clamped);
}

/*
 * The queue pair count before the vector grant.  Each queue pair owns one
 * MSI-X vector, and vector 0 serves the other causes.  The count follows the
 * CPUs and need not be a power of two: the VSI TC map rounds it up
 * (ice_vsi_ctx_fill()), but the rings and the RSS table use the exact count.
 */
static uint32_t
ice_queue_limit(ice_t *ice)
{
	struct ice_hw_common_caps *c = &ice->ice_hw.func_caps.common_cap;
	uint32_t n, cpus, conf;

	/*
	 * Attach can observe one CPU before the rest of the boot CPUs are
	 * online.
	 */
	cpus = (ncpus >= 2) ? (uint32_t)ncpus :
	    ((boot_max_ncpus == -1) ? (uint32_t)max_ncpus :
	    (uint32_t)boot_max_ncpus);

	n = MIN(c->num_rxq, c->num_txq);
	n = MIN(n, cpus);
	if (c->num_msix_vectors > 1)
		n = MIN(n, c->num_msix_vectors - 1);
	/* A narrow RSS entry cannot name every queue. */
	if (c->rss_table_entry_width > 0 && c->rss_table_entry_width < 8)
		n = MIN(n, 1u << c->rss_table_entry_width);
	n = MIN(n, ICE_MAX_QUEUES);
	conf = ice_prop_get_num_queues(ice);
	n = MIN(n, conf);

	return (MAX(n, 1));
}

void
ice_free_intrs(ice_t *ice)
{
	int i;

	if (ice->ice_intr_handles == NULL)
		return;

	for (i = 0; i < ice->ice_intr_count; i++)
		(void) ddi_intr_free(ice->ice_intr_handles[i]);

	kmem_free(ice->ice_intr_handles, ice->ice_intr_size);
	ice->ice_intr_handles = NULL;
	ice->ice_intr_count = 0;
	ice->ice_intr_size = 0;
	ice->ice_intr_type = 0;
	ice->ice_intr_cap = 0;
	ice->ice_intr_pri = 0;
}

boolean_t
ice_alloc_intrs(ice_t *ice)
{
	dev_info_t *dip = ice->ice_dip;
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t nvec = hw->func_caps.common_cap.num_msix_vectors;
	uint32_t nreq;
	int types, nintrs, navail, actual, request, rc;

	if (ddi_intr_get_supported_types(dip, &types) != DDI_SUCCESS ||
	    (types & DDI_INTR_TYPE_MSIX) == 0) {
		ice_error(ice, "MSI-X interrupts are not supported");
		return (B_FALSE);
	}

	if (ddi_intr_get_nintrs(dip, DDI_INTR_TYPE_MSIX, &nintrs) !=
	    DDI_SUCCESS || nintrs < ICE_INTR_MSIX_MIN) {
		ice_error(ice, "too few MSI-X interrupts supported: %d",
		    nintrs);
		return (B_FALSE);
	}

	if (ddi_intr_get_navail(dip, DDI_INTR_TYPE_MSIX, &navail) !=
	    DDI_SUCCESS || navail < ICE_INTR_MSIX_MIN) {
		ice_error(ice, "too few MSI-X interrupts available: %d",
		    navail);
		return (B_FALSE);
	}

	if (nvec < ICE_INTR_MSIX_MIN) {
		ice_error(ice, "firmware reports too few MSI-X vectors: %u",
		    nvec);
		return (B_FALSE);
	}

	nreq = ice_queue_limit(ice);
	request = (int)MIN(1 + nreq, (uint32_t)MIN(nintrs, navail));
	ice->ice_intr_size = request * sizeof (ddi_intr_handle_t);
	ice->ice_intr_handles = kmem_zalloc(ice->ice_intr_size, KM_SLEEP);

	rc = ddi_intr_alloc(dip, ice->ice_intr_handles, DDI_INTR_TYPE_MSIX, 0,
	    request, &actual, DDI_INTR_ALLOC_NORMAL);
	if (rc != DDI_SUCCESS) {
		ice_error(ice, "failed to allocate MSI-X interrupts: %d", rc);
		kmem_free(ice->ice_intr_handles, ice->ice_intr_size);
		ice->ice_intr_handles = NULL;
		ice->ice_intr_size = 0;
		return (B_FALSE);
	}

	/* Set before ice_free_intrs() so cleanup frees the real handles. */
	ice->ice_intr_count = actual;
	ice->ice_intr_type = DDI_INTR_TYPE_MSIX;

	if (actual < ICE_INTR_MSIX_MIN) {
		ice_error(ice, "too few MSI-X interrupts allocated: %d",
		    actual);
		ice_free_intrs(ice);
		return (B_FALSE);
	}
	ice->ice_nqueues = (uint16_t)MIN(nreq, (uint32_t)actual - 1);

	/*
	 * The direct vector->ring ISR dispatch (ice_intr_queue) and the 1:1
	 * ring-to-vector map (irxr_vec/itxr_vec = 1 + index) require every data
	 * queue to own a distinct vector.  Enforce it at the source so a future
	 * sizing change cannot silently fold rings onto a shared vector.
	 */
	ASSERT3U((uint_t)ice->ice_nqueues, <=, (uint_t)ice->ice_intr_count - 1);

	/*
	 * Report the vector accounting so the scaling ceiling is visible:
	 * firmware advertised, platform available, requested, granted, and the
	 * resulting data-queue count.
	 */
	dev_err(ice->ice_dip, CE_NOTE, "!MSI-X vectors: fw=%u avail=%d "
	    "requested=%d granted=%d data-queues=%u", nvec, navail, request,
	    actual, ice->ice_nqueues);

	if (ddi_intr_get_pri(ice->ice_intr_handles[0], &ice->ice_intr_pri) !=
	    DDI_SUCCESS ||
	    ddi_intr_get_cap(ice->ice_intr_handles[0], &ice->ice_intr_cap) !=
	    DDI_SUCCESS) {
		ice_error(ice, "failed to read MSI-X priority/capabilities");
		ice_free_intrs(ice);
		return (B_FALSE);
	}

	/*
	 * ice_lock and ice_lse_lock are taken from the OICR interrupt and its
	 * taskq, so they must be held at MSI-X priority.  ice_lock was created
	 * earlier with a NULL cookie before the priority was known; recreate it
	 * now that ddi_intr_get_pri() has run.
	 */
	mutex_destroy(&ice->ice_lock);
	mutex_init(&ice->ice_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(ice->ice_intr_pri));
	mutex_init(&ice->ice_lse_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(ice->ice_intr_pri));
	cv_init(&ice->ice_lse_cv, NULL, CV_DRIVER, NULL);

	/*
	 * The copy-buffer pool locks are taken from the tx completion path via
	 * ice_tcb_free(), which runs under the MSI-X priority itxr_lock, so
	 * they need the same interrupt cookie.
	 */
	mutex_init(&ice->ice_buf_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(ice->ice_intr_pri));
	mutex_init(&ice->ice_small_buf_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(ice->ice_intr_pri));

	return (B_TRUE);
}

void
ice_rem_intr_handlers(ice_t *ice)
{
	int i;

	for (i = 0; i < ice->ice_intr_count; i++)
		(void) ddi_intr_remove_handler(ice->ice_intr_handles[i]);
}

boolean_t
ice_add_intr_handlers(ice_t *ice)
{
	int i, rc;

	for (i = 0; i < ice->ice_intr_count; i++) {
		rc = ddi_intr_add_handler(ice->ice_intr_handles[i],
		    ice_intr_msix, ice, (caddr_t)(uintptr_t)i);
		if (rc != DDI_SUCCESS) {
			ice_error(ice, "failed to add MSI-X handler %d: %d",
			    i, rc);
			while (--i >= 0) {
				(void) ddi_intr_remove_handler(
				    ice->ice_intr_handles[i]);
			}
			return (B_FALSE);
		}
	}

	return (B_TRUE);
}

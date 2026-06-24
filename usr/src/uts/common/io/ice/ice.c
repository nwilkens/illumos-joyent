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
 * Intel Ethernet 800 series (E810) "ice" driver.
 *
 * This file is the illumos device-driver glue: DDI attach/detach, FMA, PCI
 * configuration and register mapping, interrupt allocation, and bring-up of
 * the hardware through the Intel common code under core/.  The common code
 * reaches illumos services through the osdep shim (ice_osdep.[ch]) by way of
 * hw->back; that wiring is established at the top of attach.
 */

#include <sys/atomic.h>
#include <sys/cmn_err.h>
#include <sys/varargs.h>

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

#define	ICE_ERRBUF_LEN		512

static int ice_attach(dev_info_t *, ddi_attach_cmd_t);
static int ice_detach(dev_info_t *, ddi_detach_cmd_t);

static void *ice_state_p;

/*
 * All attached instances.  The list is maintained now; its consumer (per-device
 * state shared across the PFs of a multi-function device) arrives with a later
 * milestone.
 */
static kmutex_t ice_glock;
static list_t ice_glist;

static char ice_ident[] = "Intel E810 Ethernet";

static struct cb_ops ice_cb_ops = {
	.cb_open = nulldev,
	.cb_close = nulldev,
	.cb_strategy = nodev,
	.cb_print = nodev,
	.cb_dump = nodev,
	.cb_read = nodev,
	.cb_write = nodev,
	.cb_ioctl = nodev,
	.cb_devmap = nodev,
	.cb_mmap = nodev,
	.cb_segmap = nodev,
	.cb_chpoll = nochpoll,
	.cb_prop_op = ddi_prop_op,
	.cb_str = NULL,
	.cb_flag = D_MP | D_HOTPLUG,
	.cb_rev = CB_REV,
	.cb_aread = nodev,
	.cb_awrite = nodev
};

static struct dev_ops ice_dev_ops = {
	.devo_rev = DEVO_REV,
	.devo_refcnt = 0,
	.devo_getinfo = NULL,
	.devo_identify = nulldev,
	.devo_probe = nulldev,
	.devo_attach = ice_attach,
	.devo_detach = ice_detach,
	.devo_reset = nodev,
	.devo_cb_ops = &ice_cb_ops,
	.devo_bus_ops = NULL,
	.devo_power = NULL,
	.devo_quiesce = ddi_quiesce_not_supported
};

static struct modldrv ice_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = ice_ident,
	.drv_dev_ops = &ice_dev_ops
};

static struct modlinkage ice_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &ice_modldrv, NULL }
};

int
_init(void)
{
	int status;

	status = ddi_soft_state_init(&ice_state_p, sizeof (ice_t), 1);
	if (status != DDI_SUCCESS)
		return (status);

	mutex_init(&ice_glock, NULL, MUTEX_DRIVER, NULL);
	list_create(&ice_glist, sizeof (ice_t), offsetof(ice_t, ice_glink));

	mac_init_ops(&ice_dev_ops, ICE_MODULE_NAME);

	status = mod_install(&ice_modlinkage);
	if (status != DDI_SUCCESS) {
		mac_fini_ops(&ice_dev_ops);
		list_destroy(&ice_glist);
		mutex_destroy(&ice_glock);
		ddi_soft_state_fini(&ice_state_p);
	}

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ice_modlinkage, modinfop));
}

int
_fini(void)
{
	int status;

	status = mod_remove(&ice_modlinkage);
	if (status == DDI_SUCCESS) {
		mac_fini_ops(&ice_dev_ops);
		list_destroy(&ice_glist);
		mutex_destroy(&ice_glock);
		ddi_soft_state_fini(&ice_state_p);
	}

	return (status);
}

/*PRINTFLIKE2*/
void
ice_error(ice_t *ice, const char *fmt, ...)
{
	va_list ap;
	char buf[ICE_ERRBUF_LEN];

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);

	if (ice != NULL && ice->ice_dip != NULL)
		dev_err(ice->ice_dip, CE_WARN, "%s", buf);
	else
		cmn_err(CE_WARN, "ice: %s", buf);
}

int
ice_check_acc_handle(ddi_acc_handle_t h)
{
	ddi_fm_error_t de;

	ddi_fm_acc_err_get(h, &de, DDI_FME_VERSION);
	ddi_fm_acc_err_clear(h, DDI_FME_VERSION);
	return (de.fme_status);
}

static int
ice_fm_error_cb(dev_info_t *dip, ddi_fm_error_t *err, const void *arg __unused)
{
	pci_ereport_post(dip, err, NULL);
	return (err->fme_status);
}

static void
ice_fm_init(ice_t *ice)
{
	ddi_iblock_cookie_t iblk;

	/*
	 * DMACHK is advertised with the data path (M6b), where the per-buffer
	 * ddi_fm_dma_err_get checks that honor it live.  Advertising it here,
	 * with no checker wired, would misrepresent the FMA posture.
	 */
	ice->ice_fm_caps = ddi_prop_get_int(DDI_DEV_T_ANY, ice->ice_dip,
	    DDI_PROP_DONTPASS, "fm-capable",
	    DDI_FM_EREPORT_CAPABLE | DDI_FM_ACCCHK_CAPABLE |
	    DDI_FM_ERRCB_CAPABLE);

	if (ice->ice_fm_caps < 0)
		ice->ice_fm_caps = 0;
	ice->ice_fm_caps &= (DDI_FM_EREPORT_CAPABLE | DDI_FM_ACCCHK_CAPABLE |
	    DDI_FM_ERRCB_CAPABLE);

	if (ice->ice_fm_caps == 0)
		return;

	ddi_fm_init(ice->ice_dip, &ice->ice_fm_caps, &iblk);

	if (DDI_FM_EREPORT_CAP(ice->ice_fm_caps) ||
	    DDI_FM_ERRCB_CAP(ice->ice_fm_caps))
		pci_ereport_setup(ice->ice_dip);
	if (DDI_FM_ERRCB_CAP(ice->ice_fm_caps))
		ddi_fm_handler_register(ice->ice_dip, ice_fm_error_cb, ice);
}

static void
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
static void
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

static boolean_t
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
static boolean_t
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

	if (c->max_mtu < ICE_MIN_MTU || c->max_mtu > ICE_MAX_MTU) {
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

static boolean_t
ice_hw_init(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	int rc;

	ice_set_ctrlq_len(hw);

	/*
	 * ice_init_hw() performs the PF reset, brings up the admin queue, and
	 * reads NVM and capabilities.  On failure it has already unwound its
	 * own state, so ice_deinit_hw() must not be called.
	 */
	rc = ice_init_hw(hw);
	if (rc != 0) {
		ice_error(ice, "hardware initialization failed: %d", rc);
		return (B_FALSE);
	}

	if (!ice_validate_caps(ice)) {
		ice_deinit_hw(hw);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static void
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

static boolean_t
ice_alloc_intrs(ice_t *ice)
{
	dev_info_t *dip = ice->ice_dip;
	uint32_t nvec = ice->ice_hw.func_caps.common_cap.num_msix_vectors;
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

	/*
	 * Exactly ICE_INTR_MSIX_MIN vectors (one other-cause, one queue) are
	 * requested at this milestone; per-queue vectors are added with the
	 * data path.  Both navail and the firmware count were already confirmed
	 * to meet this minimum above.
	 */
	request = ICE_INTR_MSIX_MIN;
	if (nvec < (uint32_t)request) {
		ice_error(ice, "firmware reports too few MSI-X vectors: %u",
		    nvec);
		return (B_FALSE);
	}

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

	return (B_TRUE);
}

static void
ice_rem_intr_handlers(ice_t *ice)
{
	int i;

	for (i = 0; i < ice->ice_intr_count; i++)
		(void) ddi_intr_remove_handler(ice->ice_intr_handles[i]);
}

static boolean_t
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

/*
 * Program and enable every tx/rx queue.  Called from mac start so each plumb
 * cycle re-adds the tx scheduler node and rewrites the rx context, resetting
 * the hardware ring head to zero in lockstep with the software pointers.  On
 * partial failure the queues programmed so far are unwound.
 */
int
ice_queues_program(ice_t *ice)
{
	uint_t i, j;
	int status;

	for (i = 0; i < ice->ice_num_txr; i++) {
		status = ice_tx_ring_program(ice, &ice->ice_txr[i]);
		if (status != ICE_SUCCESS) {
			while (i-- > 0)
				ice_tx_ring_unprogram(ice, &ice->ice_txr[i]);
			return (status);
		}
	}
	for (i = 0; i < ice->ice_num_rxr; i++) {
		status = ice_rx_ring_program(ice, &ice->ice_rxr[i]);
		if (status != ICE_SUCCESS) {
			while (i-- > 0)
				ice_rx_ring_unprogram(ice, &ice->ice_rxr[i]);
			for (j = 0; j < ice->ice_num_txr; j++)
				ice_tx_ring_unprogram(ice, &ice->ice_txr[j]);
			return (status);
		}
	}

	return (ICE_SUCCESS);
}

void
ice_queues_disable(ice_t *ice)
{
	uint_t i;

	for (i = 0; i < ice->ice_num_txr; i++)
		ice_tx_ring_unprogram(ice, &ice->ice_txr[i]);
	for (i = 0; i < ice->ice_num_rxr; i++)
		ice_rx_ring_unprogram(ice, &ice->ice_rxr[i]);
}

static void
ice_queues_intr_map(ice_t *ice)
{
	uint_t i;

	for (i = 0; i < ice->ice_num_txr; i++)
		ice_map_txq_vector(ice, &ice->ice_txr[i]);
	for (i = 0; i < ice->ice_num_rxr; i++) {
		ice_map_rxq_vector(ice, &ice->ice_rxr[i]);
		ice_cfg_itr(ice, ice->ice_rxr[i].irxr_vec);
	}
}

static void
ice_queues_intr_unmap(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint_t i;

	for (i = 0; i < ice->ice_num_txr; i++)
		wr32(hw, QINT_TQCTL(ice->ice_txr[i].itxr_index), 0);
	for (i = 0; i < ice->ice_num_rxr; i++)
		wr32(hw, QINT_RQCTL(ice->ice_rxr[i].irxr_index), 0);
	ice_flush(hw);
}

static void
ice_unconfigure(ice_t *ice)
{
	/*
	 * The MAC handle (a higher progress bit) is unregistered by ice_detach
	 * before this runs, so the datapath is already quiesced: mac_stop drove
	 * ice_rx_stop()/ice_tx_stop(), draining loans and reclaiming TCBs.  The
	 * shared copy-buffer pools can now be freed.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_BUFS)
		ice_buf_fini(ice);

	/*
	 * Tear down the datapath before the VSI: the queues belong to the VSI
	 * and the queue disables ride the admin queue, which a lower progress
	 * bit (ice_deinit_hw) undoes later.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_QUEUE_INTR)
		ice_queues_intr_unmap(ice);

	/*
	 * Fence the interrupt handlers before freeing anything they touch.
	 * Masking (ice_intr_disable) only stops new deliveries; removing the
	 * handler is what waits out one already running on another CPU.  The
	 * queue ISR dereferences the ring arrays, so the handlers must be
	 * removed before the rings are freed and before the OICR taskq, which
	 * the OICR handler dispatches onto, is destroyed.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_ENABLE_INTR) {
		ice_intr_disable(ice);
		ice_intr_oicr_disable(ice);
		wr32(&ice->ice_hw, PFINT_OICR_ENA, 0);
		ice_flush(&ice->ice_hw);
	}

	if (ice->ice_attach_progress & ICE_ATTACH_ADD_INTR)
		ice_rem_intr_handlers(ice);

	if (ice->ice_attach_progress & ICE_ATTACH_RINGS) {
		ice_tx_rings_free(ice);
		ice_rx_rings_free(ice);
	}

	/*
	 * Tear down the VSI: ice_free_vsi() and ice_remove_mac() ride the
	 * admin queue, which ice_deinit_hw() (a lower progress bit, undone
	 * later) tears down.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_VSI)
		ice_vsi_fini(ice);

	if (ice->ice_attach_progress & ICE_ATTACH_OICR_TASKQ) {
		/* No handler can dispatch now; drain then free. */
		ddi_taskq_destroy(ice->ice_oicr_taskq);
		ice->ice_oicr_taskq = NULL;
		kmem_free(ice->ice_aqbuf, ICE_AQ_MAX_BUF_LEN);
		ice->ice_aqbuf = NULL;
	}

	if (ice->ice_attach_progress & ICE_ATTACH_ALLOC_INTR) {
		ice_free_intrs(ice);
		cv_destroy(&ice->ice_lse_cv);
		mutex_destroy(&ice->ice_lse_lock);
	}

	if (ice->ice_attach_progress & ICE_ATTACH_HW_INIT)
		ice_deinit_hw(&ice->ice_hw);

	if (ice->ice_attach_progress & ICE_ATTACH_REGS_MAP) {
		ddi_regs_map_free(&ice->ice_osdep.ios_reg_handle);
		ice->ice_osdep.ios_reg_handle = NULL;
		ice->ice_hw.hw_addr = NULL;
	}

	if (ice->ice_attach_progress & ICE_ATTACH_PCI_CONFIG) {
		pci_config_teardown(&ice->ice_osdep.ios_cfg_handle);
		ice->ice_osdep.ios_cfg_handle = NULL;
	}

	if (ice->ice_attach_progress & ICE_ATTACH_FM_INIT)
		ice_fm_fini(ice);

	mutex_destroy(&ice->ice_lock);
	ice->ice_attach_progress = 0;
}

static int
ice_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	ice_t *ice;
	struct ice_hw *hw;
	struct ice_osdep *osdep;
	int instance;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(ice_state_p, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	ice = ddi_get_soft_state(ice_state_p, instance);

	ice->ice_dip = dip;
	ice->ice_instance = instance;
	mutex_init(&ice->ice_lock, NULL, MUTEX_DRIVER, NULL);

	/*
	 * Wire the common code to the osdep and back to the softc before any
	 * register or config-space access takes place.
	 */
	hw = &ice->ice_hw;
	osdep = &ice->ice_osdep;
	hw->back = osdep;
	osdep->ios_ice = ice;
	osdep->ios_dip = dip;

	ice_fm_init(ice);
	ice->ice_attach_progress |= ICE_ATTACH_FM_INIT;

	if (pci_config_setup(dip, &osdep->ios_cfg_handle) != DDI_SUCCESS) {
		ice_error(ice, "failed to set up PCI configuration space");
		goto fail;
	}
	ice->ice_attach_progress |= ICE_ATTACH_PCI_CONFIG;
	ice_identify_hardware(ice);

	if (!ice_regs_map(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_REGS_MAP;

	if (ice_check_acc_handle(osdep->ios_cfg_handle) != DDI_FM_OK ||
	    ice_check_acc_handle(osdep->ios_reg_handle) != DDI_FM_OK) {
		ddi_fm_service_impact(dip, DDI_SERVICE_LOST);
		goto fail;
	}

	if (!ice_hw_init(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_HW_INIT;

	/*
	 * ice_init_hw() performed extensive firmware interaction; confirm no
	 * register or config-space access faulted during bring-up.  The
	 * progress bit is already set, so teardown undoes the hardware init.
	 */
	if (ice_check_acc_handle(osdep->ios_reg_handle) != DDI_FM_OK ||
	    ice_check_acc_handle(osdep->ios_cfg_handle) != DDI_FM_OK) {
		ddi_fm_service_impact(dip, DDI_SERVICE_LOST);
		goto fail;
	}

	if (!ice_alloc_intrs(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_ALLOC_INTR;

	if (!ice_add_intr_handlers(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_ADD_INTR;

	/*
	 * One worker thread by design: the single ice_aqbuf scratch buffer and
	 * the ice_oicr_pending coalescing both assume one task runs at a time.
	 */
	ice->ice_oicr_taskq = ddi_taskq_create(dip, "ice_oicr", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (ice->ice_oicr_taskq == NULL) {
		ice_error(ice, "failed to create OICR taskq");
		goto fail;
	}
	ice->ice_aqbuf = kmem_zalloc(ICE_AQ_MAX_BUF_LEN, KM_SLEEP);
	ice->ice_attach_progress |= ICE_ATTACH_OICR_TASKQ;

	ice_intr_oicr_setup(ice);

	if (!ice_set_link_events(ice))
		goto fail;

	/*
	 * Enabling interrupts is the final hardware step so no handler runs
	 * before its state is ready.  MAC registration follows in a later
	 * milestone; until then link state is cached but not reported.
	 */
	if (!ice_intr_enable(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_ENABLE_INTR;

	ice_link_status_update(ice);

	/*
	 * Load the DDP package before the VSI: a missing or rejected package
	 * drops the device into safe mode, which clamps the queue counts the
	 * VSI configuration reads.
	 */
	if (!ice_ddp_load(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_DDP;

	if (!ice_vsi_init(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_VSI;

	/*
	 * Allocate the datapath rings and program the tx/rx queue contexts.
	 * The queue counts come from the VSI configuration.  The progress bit
	 * is set before programming so a partial failure still tears every
	 * queue back down.
	 */
	ice->ice_num_rxr = ice->ice_pf_vsi.vi_nrxq;
	ice->ice_num_txr = ice->ice_pf_vsi.vi_ntxq;
	ice->ice_num_rx_groups = 1;
	ice->ice_tx_ring_size = ICE_DEF_TX_RING_SIZE;
	ice->ice_rx_ring_size = ICE_DEF_RX_RING_SIZE;
	ice->ice_pf_vsi.vi_max_frame = ICE_RX_BUF_SIZE;

	if (!ice_tx_rings_alloc(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_RINGS;
	if (!ice_rx_rings_alloc(ice))
		goto fail;

	/*
	 * Wire the queue->vector routing now; the queues themselves are
	 * programmed and enabled by mac start so each plumb cycle resets the
	 * hardware ring head.  The routing is keyed on the queue index and is
	 * inert until a queue is enabled.
	 */
	ice_queues_intr_map(ice);
	ice->ice_attach_progress |= ICE_ATTACH_QUEUE_INTR;

	ice_buf_init(ice);
	ice->ice_attach_progress |= ICE_ATTACH_BUFS;

	/*
	 * Register with MAC last: once this returns the datapath is reachable
	 * by clients, so everything it touches must already be live.
	 */
	if (!ice_mac_register(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_MAC;

	mutex_enter(&ice_glock);
	list_insert_tail(&ice_glist, ice);
	mutex_exit(&ice_glock);

	atomic_or_32(&ice->ice_state, ICE_STATE_ATTACHED);
	return (DDI_SUCCESS);

fail:
	ice_unconfigure(ice);
	ddi_soft_state_free(ice_state_p, instance);
	return (DDI_FAILURE);
}

static int
ice_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	ice_t *ice;
	int instance;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	instance = ddi_get_instance(dip);
	ice = ddi_get_soft_state(ice_state_p, instance);
	if (ice == NULL)
		return (DDI_FAILURE);

	/*
	 * Unregister from MAC first: it fails if a client is still bound, in
	 * which case the driver must remain attached.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_MAC) {
		if (ice_mac_unregister(ice) != 0)
			return (DDI_FAILURE);
		ice->ice_attach_progress &= ~ICE_ATTACH_MAC;
	}

	mutex_enter(&ice_glock);
	list_remove(&ice_glist, ice);
	mutex_exit(&ice_glock);

	ice_unconfigure(ice);
	ddi_soft_state_free(ice_state_p, instance);
	return (DDI_SUCCESS);
}

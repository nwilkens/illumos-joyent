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
#include <sys/cpuvar.h>
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
static uint32_t ice_prop_get_num_queues(ice_t *);

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

void
ice_update_mtu(ice_t *ice)
{
	ice->ice_pf_vsi.vi_max_frame = ice->ice_mtu +
	    sizeof (struct ether_vlan_header) + ETHERFCSL;
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
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t nvec = hw->func_caps.common_cap.num_msix_vectors;
	uint32_t qcap, cpus, vcap, nprop, nreq;
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

	qcap = MIN(hw->func_caps.common_cap.num_rxq,
	    hw->func_caps.common_cap.num_txq);
	/*
	 * Attach can observe one CPU before the rest of the boot CPUs are
	 * online.
	 */
	cpus = (ncpus >= 2) ? (uint32_t)ncpus :
	    ((boot_max_ncpus == -1) ? (uint32_t)max_ncpus :
	    (uint32_t)boot_max_ncpus);
	vcap = (nvec > 1) ? (uint32_t)nvec - 1 : 1;
	nprop = ice_prop_get_num_queues(ice);
	nreq = MIN(MIN(MIN(qcap, cpus),
	    MIN(vcap, (uint32_t)ICE_MAX_INTR_QUEUES)), nprop);
	nreq = (nreq < 1) ? 1 : (1u << ice_ilog2(nreq));
	request = (int)(1 + nreq);
	if (request < ICE_INTR_MSIX_MIN)
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
	ice->ice_nqueues = (uint16_t)MIN(nreq,
	    1u << ice_ilog2((uint32_t)actual - 1));

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
	 * Delete the kstats first: their update callbacks read hardware
	 * registers, so they must stop before the register mapping is torn
	 * down.  kstat_delete() waits out any in-progress read.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_STATS)
		ice_stats_fini(ice);

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

	/*
	 * Drain the OICR taskq before the reset taskq.  With the handlers gone
	 * no new OICR fires, but an already-queued ice_oicr_task can still
	 * dispatch a rebuild, so the OICR worker must be quiesced first.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_OICR_TASKQ) {
		ddi_taskq_destroy(ice->ice_oicr_taskq);
		ice->ice_oicr_taskq = NULL;
		kmem_free(ice->ice_aqbuf, ICE_AQ_MAX_BUF_LEN);
		ice->ice_aqbuf = NULL;
	}

	/*
	 * ddi_taskq_destroy drains any in-flight rebuild.  It runs after the
	 * handlers and OICR worker are gone (nothing can dispatch a new one)
	 * and before the rings and VSI the rebuild touches are freed.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_RESET_TASKQ) {
		ddi_taskq_destroy(ice->ice_reset_taskq);
		ice->ice_reset_taskq = NULL;
		mutex_destroy(&ice->ice_rebuild_lock);
	}

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

	if (ice->ice_attach_progress & ICE_ATTACH_ALLOC_INTR) {
		ice_free_intrs(ice);
		cv_destroy(&ice->ice_lse_cv);
		mutex_destroy(&ice->ice_lse_lock);
		mutex_destroy(&ice->ice_small_buf_lock);
		mutex_destroy(&ice->ice_buf_lock);
	}

	if (ice->ice_attach_progress & ICE_ATTACH_HW_INIT) {
		/*
		 * ice_deinit_hw() also releases the DDP package copy.  It runs
		 * after interrupt teardown because DDP now precedes interrupt
		 * allocation during attach.
		 */
		ice_deinit_hw(&ice->ice_hw);
		/*
		 * Quiesce the function with a PF reset so a later re-attach
		 * inherits clean hardware state (scheduler tree, queue and VSI
		 * contexts, PHY config, in-flight DMA) not stale config.
		 */
		(void) ice_reset(&ice->ice_hw, ICE_RESET_PFR);
	}

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

	mutex_destroy(&ice->ice_loopback_lock);
	mutex_destroy(&ice->ice_lock);
	ice->ice_attach_progress = 0;
}

static uint32_t
ice_prop_get_num_queues(ice_t *ice)
{
	int value;

	value = ddi_prop_get_int(DDI_DEV_T_ANY, ice->ice_dip, 0, "num_queues",
	    ICE_MAX_INTR_QUEUES);
	value = MIN(MAX(value, 1), ICE_MAX_INTR_QUEUES);

	/* A power-of-two count keeps the VSI TC encoding exact. */
	return (1u << ice_ilog2((uint32_t)value));
}

static int
ice_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	ice_t *ice;
	struct ice_hw *hw;
	struct ice_osdep *osdep;
	int mtu;
	int instance;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	instance = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(ice_state_p, instance) != DDI_SUCCESS)
		return (DDI_FAILURE);
	ice = ddi_get_soft_state(ice_state_p, instance);

	ice->ice_dip = dip;
	ice->ice_instance = instance;
	ice->ice_link_state = LINK_STATE_UNKNOWN;
	ice->ice_fec_neg = LINK_FEC_NONE;
	mutex_init(&ice->ice_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&ice->ice_loopback_lock, NULL, MUTEX_DRIVER, NULL);

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

	/*
	 * Load DDP before sizing queues and vectors because safe mode rewrites
	 * the queue and MSI-X capabilities that ice_alloc_intrs() consumes.
	 */
	if (!ice_ddp_load(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_DDP;

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

	/*
	 * The reset rebuild runs on its own single-thread taskq so a multi-
	 * second rebuild cannot starve the OICR worker's admin-queue drain.
	 * ice_rebuild_lock is adaptive (NULL cookie): it is taken only in
	 * thread context and never at interrupt priority.
	 */
	ice->ice_reset_taskq = ddi_taskq_create(dip, "ice_reset", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (ice->ice_reset_taskq == NULL) {
		ice_error(ice, "failed to create reset taskq");
		goto fail;
	}
	mutex_init(&ice->ice_rebuild_lock, NULL, MUTEX_DRIVER, NULL);
	ice->ice_attach_progress |= ICE_ATTACH_RESET_TASKQ;

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

	/* Enable the PHY; firmware will not bring the link up on its own. */
	ice_setup_link(ice);
	ice_phy_caps_update(ice);

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
	mtu = ddi_prop_get_int(DDI_DEV_T_ANY, ice->ice_dip,
	    DDI_PROP_DONTPASS, "default_mtu", ICE_DEFAULT_MTU);
	if (mtu < ICE_MIN_MTU)
		mtu = ICE_MIN_MTU;
	else if (mtu > ICE_MAX_MTU)
		mtu = ICE_MAX_MTU;
	ice->ice_mtu = mtu;
	ice->ice_tx_lso_enable = ddi_prop_get_int(DDI_DEV_T_ANY,
	    ice->ice_dip, DDI_PROP_DONTPASS, "tx_lso_enable", 0) != 0;
	ice_update_mtu(ice);

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

	if (!ice_buf_init(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_BUFS;

	if (!ice_stats_init(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_STATS;

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

	/*
	 * Mark the device detaching under the rebuild lock before any
	 * admin-queue teardown.  Acquiring the lock waits out an in-flight
	 * rebuild, and the flag makes a not-yet-started ice_reset_task a no-op;
	 * ice_unconfigure then drains the reset taskq for good.  This must
	 * precede ice_loopback_fini(), whose admin-queue commands would
	 * otherwise race a rebuild tearing the control queue down.
	 */
	mutex_enter(&ice->ice_rebuild_lock);
	ice->ice_detaching = B_TRUE;
	mutex_exit(&ice->ice_rebuild_lock);

	ice_loopback_fini(ice);

	mutex_enter(&ice_glock);
	list_remove(&ice_glist, ice);
	mutex_exit(&ice_glock);

	ice_unconfigure(ice);
	ddi_soft_state_free(ice_state_p, instance);
	return (DDI_SUCCESS);
}

/*
 * Mark the reset terminally failed and fail the datapath closed.  Shared by the
 * rebuild's per-step failure path and by ice_reset_task when the pre-reset
 * quiesce cannot drain outstanding rx loans.  The device stays down until the
 * driver is reloaded.
 */
static void
ice_reset_set_failed(ice_t *ice)
{
	atomic_or_32(&ice->ice_state,
	    ICE_STATE_RESET_FAILED | ICE_STATE_ERROR);
	ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_LOST);
	ice_link_report(ice, LINK_STATE_DOWN);
	ice_error(ice, "reset recovery failed; reload the ice driver");
}

/*
 * Quiesce the function ahead of the rebuild.  Modeled on the FreeBSD ice
 * driver's ice_prepare_for_reset().  Runs under ice_rebuild_lock.  Returns
 * B_FALSE when a loaned rx buffer did not return within the bounded wait, in
 * which case the rings are left intact and the caller must fail closed rather
 * than reinitialize over buffers still up the stack.
 */
static boolean_t
ice_prepare_for_reset(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	boolean_t drained = B_TRUE;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	/*
	 * Idempotent: the GRST path set this in the ISR, the fatal/PFR-request
	 * path did not.  It makes concurrent admin-queue commands soft-fail
	 * rather than hang on resetting hardware while we quiesce.
	 */
	hw->reset_ongoing = true;

	/*
	 * Quiesce the datapath if it was running.  ice_tx_stop() waits for
	 * in-flight transmits and ice_rx_stop() for loaned buffers to
	 * return under a bounded deadline, so the autonomous reset taskq cannot
	 * wedge on a lost loan; the queue disable and interrupt unmap are best
	 * effort on hardware that may already be resetting.  ICE_STATE_STARTED
	 * is left set so the rebuild knows to restart the datapath.
	 */
	if ((ice->ice_state & ICE_STATE_STARTED) != 0) {
		ice_tx_stop(ice);
		drained = ice_rx_stop(ice);
		ice_queues_disable(ice);
		ice_queues_intr_unmap(ice);
	}

	/* Report the link down for the duration of the rebuild. */
	ice_link_report(ice, LINK_STATE_DOWN);

	/* Silence the OICR so no new cause fires mid-reset. */
	ice_intr_oicr_disable(ice);
	wr32(hw, PFINT_OICR_ENA, 0);
	ice_flush(hw);

	/*
	 * Force the PF VSI to be recreated by the rebuild.  The vi_macs list is
	 * left intact: it is the authoritative record the rebuild replays.
	 */
	ice->ice_pf_vsi.vi_added = B_FALSE;

	return (drained);
}

/*
 * Reinitialize the function after a reset and restore the datapath.  Modeled
 * on the FreeBSD ice driver's ice_rebuild().  Runs under ice_rebuild_lock.
 * Each failing step jumps to reset_failed, which fails closed until the driver
 * is reloaded.
 */
static void
ice_rebuild(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	/*
	 * Tear the common code down and reinitialize it.  reset_ongoing must be
	 * cleared before the reinit: every admin-queue command soft-fails with
	 * ICE_ERR_RESET_ONGOING while it is set, and ice_init_hw() drives the
	 * PF reset and re-reads capabilities over that queue.
	 *
	 * ICE_ATTACH_HW_INIT is cleared across the teardown and reinit so a
	 * failure between them leaves the bit clear: detach then skips a second
	 * common-code teardown of already-freed state (which also owns the DDP
	 * copy).  The bit is restored only after the reinit succeeds, so detach
	 * tears the HW down exactly once.
	 */
	ice->ice_attach_progress &= ~ICE_ATTACH_HW_INIT;
	ice_deinit_hw(hw);
	hw->reset_ongoing = false;
	if (!ice_hw_init(ice))
		goto reset_failed;
	ice->ice_attach_progress |= ICE_ATTACH_HW_INIT;

	/*
	 * A global or core reset can zero the MAC counters, so drop the
	 * baselines and let the next read re-establish them.
	 */
	ice->ice_stat_port_loaded = B_FALSE;
	ice->ice_stat_vsi_loaded = B_FALSE;

	/* Re-read the DDP package; safe mode is tolerated as at attach. */
	if (!ice_ddp_load(ice))
		goto reset_failed;

	if (ice_vsi_rebuild(ice) != ICE_SUCCESS)
		goto reset_failed;

	/*
	 * Clear the reset-owed bits before re-enabling the OICR below: the owed
	 * rebuild has been performed by the steps above, so a link-change cause
	 * arriving right after the OICR is re-armed must not observe a stale
	 * RESET_PENDING/PFR_REQ and dispatch a redundant rebuild.  The
	 * fail-closed bit stays set until the datapath is confirmed restored.
	 */
	atomic_and_32(&ice->ice_state,
	    ~(ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ));

	/* Re-route and re-arm the interrupts a reset clears. */
	ice_intr_oicr_setup(ice);
	if (!ice_set_link_events(ice))
		goto reset_failed;
	ice_queues_intr_map(ice);

	/* Refresh the cached link and re-enable the PHY. */
	ice_link_status_update(ice);
	ice_setup_link(ice);
	ice_phy_caps_update(ice);

	/*
	 * The rebuild steps succeeded: clear the fail-closed bit before
	 * restoring the datapath.
	 */
	atomic_and_32(&ice->ice_state, ~ICE_STATE_ERROR);

	if ((ice->ice_state & ICE_STATE_STARTED) != 0 &&
	    (ice_start_datapath(ice) != 0 || !ice_rx_rings_resume(ice))) {
		/*
		 * The reset recovered but the datapath did not restart.  Leave
		 * it fail-closed (a later mac stop/start recovers) without
		 * marking the reset terminally failed.  ice_rx_rings_resume()
		 * reposts the rx buffers and reopens the rings, which MAC would
		 * otherwise drive through the per-ring start callbacks.
		 */
		atomic_or_32(&ice->ice_state, ICE_STATE_ERROR);
		ice_link_report(ice, LINK_STATE_DOWN);
		ice_error(ice, "reset recovered but datapath restart failed");
		return;
	}

	ice_link_state_publish(ice);
	dev_err(ice->ice_dip, CE_NOTE, "!reset recovery complete");
	return;

reset_failed:
	ice_reset_set_failed(ice);
}

/*
 * Reset taskq worker: consume the coalesced request and run the rebuild.  A
 * detach in progress makes it a no-op.
 */
void
ice_reset_task(void *arg)
{
	ice_t *ice = arg;

	mutex_enter(&ice->ice_lock);
	ice->ice_reset_pending = B_FALSE;
	mutex_exit(&ice->ice_lock);

	/*
	 * ice_rebuild_lock is the outermost lock and is taken only after
	 * ice_lock is dropped.
	 */
	mutex_enter(&ice->ice_rebuild_lock);
	if (ice->ice_detaching) {
		mutex_exit(&ice->ice_rebuild_lock);
		return;
	}
	if (!ice_prepare_for_reset(ice)) {
		/*
		 * A loaned rx buffer never returned within the bounded wait.
		 * Reinitializing would require freeing rings that still have
		 * buffers up the stack, so fail closed terminally and leave the
		 * rings intact for detach to drain.
		 */
		ice_reset_set_failed(ice);
		mutex_exit(&ice->ice_rebuild_lock);
		return;
	}
	ice_rebuild(ice);
	mutex_exit(&ice->ice_rebuild_lock);
}

#ifdef DEBUG
/*
 * Test hook: drive a PFR rebuild from mdb -kw via ::call without a hardware
 * GLOBR.  Not compiled into production builds.
 */
void
ice_test_request_reset(ice_t *ice)
{
	atomic_or_32(&ice->ice_state, ICE_STATE_PFR_REQ);
	ice_reset_dispatch(ice);
}
#endif

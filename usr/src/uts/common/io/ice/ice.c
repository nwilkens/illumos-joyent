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
 * ice - Intel Ethernet 800 series driver
 *
 * Each PCI physical function (PF) of an E800 series device is one instance
 * of this driver and one MAC.  Firmware runs on the device and the driver
 * programs it through an admin queue.  The Intel common code under core/
 * encodes those commands and keeps the switch, scheduler and VSI bookkeeping.
 * It reaches illumos through ice_osdep.[ch] by way of hw->back; attach wires
 * that first.  core/README.illumos lists the few local changes to it.
 *
 * ------------
 * Organization
 * ------------
 *
 * ice.c		DDI entry points; the lifecycle: attach, detach, MAC
 *			start and stop, reset and rebuild.
 * ice_hw.c		Bring-up steps that attach and rebuild share: FMA,
 *			registers, firmware checks, per-family decisions,
 *			interrupt vectors.
 * ice_intr.c		Interrupt handlers, the admin queue worker, link state.
 * ice_gld.c		MAC callbacks and properties.
 * ice_port.c		Transceiver access and the identification LED.
 * ice_vsi.c		The PF's VSI and RSS.
 * ice_filter.c		Accepted MAC addresses and promiscuous policy.
 * ice_tx.c, ice_rx.c	The data path.
 * ice_dma.c		DMA attributes and the TX copy-buffer pools.
 * ice_stats.c		Hardware counters and kstats.
 * ice_ddp.c		The DDP package load and safe mode.
 * ice_ioctl.c		Firmware logging and debug dump ioctls.
 *
 * ---------------
 * Device families
 * ---------------
 *
 * The common code maps each device ID to a MAC type and keys every family
 * difference on it, so the driver binds every ID that ice_set_mac_type()
 * maps and rejects any other at attach.  Only the E810-C (0x1592) has been
 * tested on hardware.  The differences the driver itself must handle:
 *
 * E810		The base case.
 * E822, E823	ICE_MAC_GENERIC.  These have a sideband queue.  The common
 *		code sizes, starts and stops it with the other control
 *		queues; the admin worker discards the messages firmware posts
 *		to it (ice_sbq_drain()) so that its ring cannot fill.  SGMII
 *		ports link at 100 Mb/s.
 * E825-C	ICE_MAC_GENERIC_3K_E825: the sideband queue as above, a
 *		separate DDP signature that the common code selects, and a
 *		slow EMPR (below).
 * E830, E835	ICE_MAC_E830.  The TCLAN malicious-driver registers moved
 *		(ICE_GL_MDET_TX_TCLAN()); PHY firmware loads after the PF is
 *		up, so attach waits for it (ice_phy_fw_wait()); links reach
 *		200 Gb/s; the common code uses the longer Get Link Status
 *		response and the E830 DDP segment.  After an EMPR, E825-C and
 *		E830 firmware needs more time than ice_check_reset() allows,
 *		so the rebuild waits first (ice_reset_empr_slow()).
 *
 * The data path, queue contexts and descriptors are the same for all of
 * them.  Features that differ by family but that the driver does not use
 * (PTP, Tx time, DCB, SR-IOV) are not handled.
 *
 * -------------------
 * State shared by PFs
 * -------------------
 *
 * The PFs of one card share no driver state.  A card-wide reset (CORER,
 * GLOBR, EMPR) reaches every PF as its own OICR cause, and each PF rebuilds
 * itself; the driver issues only PF resets.  Firmware serializes the DDP
 * download with its global configuration lock, and a later PF finds the
 * package loaded.  Nothing else needs one owner per card, so there is no
 * per-card structure.
 *
 * -----
 * Locks
 * -----
 *
 * ice_rebuild_lock is the outermost lock.  It is adaptive and is taken only in
 * thread context: MAC start and stop, the reset worker, detach, and every
 * management operation that sends a firmware command.  No interrupt handler
 * takes it; handlers record causes and defer the work.  Then, in order:
 *
 *	ice_lock	interrupt priority; OICR causes and worker dispatch.
 *			Never hold it across a firmware command, which can poll
 *			for a second.
 *	ring locks	each ring's descriptors, pool and interrupt routing.
 *	vi_mac_lock	the accepted address list (ice_filter.c); it is
 *			dropped around switch commands.
 *	ice_loopback_lock, then ice_lse_lock	the link state cache.
 *	ice_stat_lock	the counter baselines (ice_stats.c).
 *	ice_fwlog_lock	the firmware log ring (ice_ioctl.c).
 *
 * Never wait for a worker while holding a lock that the worker takes.
 *
 * ---------------------
 * Start, stop and reset
 * ---------------------
 *
 * ICE_STATE_STARTED records a successful MAC start and whether a rebuild must
 * start the data path again.  It does not mean that carrier is up or that DMA
 * has stopped.  ICE_STATE_ERROR closes the data path.  MAC start refuses to
 * run while a reset is owed or after a failed one, and clears an ordinary
 * ERROR because it programs every queue again.
 *
 * MAC stop cannot fail.  If a queue does not confirm its disable, the queue
 * can still write to memory, so stop releases nothing, closes the software
 * paths, and requests a PF reset.  Packet memory is reclaimed only after a
 * queue disable is confirmed or a reset completes.
 *
 * A reset request is an atomic cause bit: RESET_PENDING for a reset firmware
 * or another PF started, PFR_REQ for one the driver owes.  The reset worker
 * claims the bits under ice_rebuild_lock, prepares (quiesce, never release),
 * waits for or issues the reset, and rebuilds.  A request that arrives after
 * the claim stays owed and the worker dispatches again.  If a hardware or
 * firmware step of the rebuild fails, ICE_STATE_RESET_FAILED is set; it is
 * terminal and the device stays down until the driver is reloaded.  If only
 * the data path restart fails, ERROR stays set and a later MAC start can
 * recover.
 *
 * Firmware in recovery mode cannot run the device.  Attach checks for it
 * before its first admin queue command, and the rebuild checks right after
 * the reset.  Either posts an ereport.io.device.fw_corrupt, marks the service
 * lost, tells the operator to update the NVM, and fails closed.
 *
 * Detach refuses a started interface.  Before MAC unregister it must prove
 * that packet DMA has stopped: every queue confirmed its disable, or a PF
 * reset completed, and no register access fault occurred meanwhile.  If that
 * proof fails, or MAC refuses to unregister, detach keeps every resource and
 * can be tried again.  Attach and detach record each completed step in
 * ice_attach_progress, and ice_unconfigure() undoes only those steps.
 *
 * ------------------
 * Filters and replay
 * ------------------
 *
 * MAC owns the reference counts of addresses and promiscuous mode.  The
 * driver records only what it accepted (vi_macs, ice_promisc_on) so that a
 * rebuild can program it again.  The common code keeps its own rule records,
 * and the hardware is a third copy; a failed command can leave the three out
 * of step.  So a failed add returns its errno and requests a reset; a failed
 * remove retires the address, requests a reset and succeeds, because MAC
 * drops its reference anyway.  While a reset is owed or after one failed,
 * adds fail with EIO and removals retire the record without a command.
 *
 * -------------------
 * Queues and offloads
 * -------------------
 *
 * The queue pair count is the lowest of the CPU count, the queues and vectors
 * firmware gives this PF, the vectors the platform grants less the OICR
 * vector, MAX_RINGS_PER_GROUP - 1, and the num_queues property.  It need not
 * be a power of two: the VSI TC map rounds up, while the rings and the RSS
 * table use the exact count.
 *
 * Checksum offload and LSO are advertised unless the DDP package is missing
 * (safe mode), which also leaves one queue pair.  For LSO the MSS comes from
 * mac_lso_get() and must be at least 88 bytes; the header is copied into one
 * descriptor so that each segment uses at most eight.
 *
 * -----------------
 * Diagnostic ioctls
 * -----------------
 *
 * ice_ioctl.h describes the firmware logging and debug dump ioctls.  They
 * reach card-wide firmware state, so only the global zone with
 * {PRIV_SYS_DEVICES} and {PRIV_SYS_CONFIG} can use them, even when a zone
 * owns the link.  No ioctl can start a card-wide reset.
 *
 * ----------
 * Validation
 * ----------
 *
 * usr/src/test/ice-tests runs the lifecycle, filter, reset, offload and
 * ioctl code with controlled boundaries on any host.  Those checks cannot
 * show memory ordering, device timing or interrupt delivery; the on-system
 * tests in usr/src/test/ice-tests/runfiles cover those on hardware.
 */

#include <sys/atomic.h>
#include <sys/cmn_err.h>
#include <sys/varargs.h>

#include "ice.h"
#include "ice_common.h"
#include "ice_ddp_common.h"
#include "ice_flex_pipe.h"
#include "ice_sched.h"

#define	ICE_ERRBUF_LEN		512

/* Extra settle time for an EMPR on E825-C and E830; see ice_rebuild(). */
#define	ICE_EMPR_SLOW_WAIT_SEC	20

static int ice_attach(dev_info_t *, ddi_attach_cmd_t);
static int ice_detach(dev_info_t *, ddi_detach_cmd_t);

static void *ice_state_p;

static char ice_ident[] = "Intel E800 Series Ethernet";

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

	mac_init_ops(&ice_dev_ops, ICE_MODULE_NAME);

	status = mod_install(&ice_modlinkage);
	if (status != DDI_SUCCESS) {
		mac_fini_ops(&ice_dev_ops);
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

	/*
	 * Syslog only: a recurring hardware fault must not be able to render
	 * the console unusable.  Matches i40e_error() (i40e_main.c:431), which
	 * likewise passes console = B_FALSE for CE_WARN.
	 */
	if (ice != NULL && ice->ice_dip != NULL)
		dev_err(ice->ice_dip, CE_WARN, "!%s", buf);
	else
		cmn_err(CE_WARN, "!ice: %s", buf);
}

int
ice_check_acc_handle(ice_t *ice, ddi_acc_handle_t h)
{
	ddi_fm_error_t de;

	ddi_fm_acc_err_get(h, &de, DDI_FME_VERSION);
	if (de.fme_status != DDI_FM_OK) {
		/*
		 * Record an observer before it consumes the shared error.
		 * Detach must also reject a clear already in flight when its
		 * polling begins: that clear could erase a newer error.
		 */
		atomic_inc_32(&ice->ice_acc_clears);
		atomic_inc_32(&ice->ice_acc_errors);
		membar_enter();
		ddi_fm_acc_err_clear(h, DDI_FME_VERSION);
		membar_exit();
		atomic_dec_32(&ice->ice_acc_clears);
	}
	/* An OK observation must not clear an error arriving after the GET. */
	return (de.fme_status);
}

/*
 * Translate a common-code status into an errno.  mac(9E) recovers from ENOSPC
 * on the unicast add path by falling back to promiscuous mode plus software
 * classification; collapsing every failure to EIO forfeits that.  Firmware
 * reports a filter it could not allocate as ICE_AQ_RC_ENOSPC (E810 datasheet
 * Table 7-78, Add Switch Rules Response), which the common code records in
 * hw->adminq.sq_last_status while returning ICE_ERR_AQ_ERROR.  The next admin
 * queue command overwrites sq_last_status, so callers must decode a failure
 * before issuing anything else.
 */
int
ice_status_to_errno(ice_t *ice, int status)
{
	switch (status) {
	case ICE_SUCCESS:
		return (0);
	case ICE_ERR_NO_MEMORY:
		return (ENOMEM);
	case ICE_ERR_RESET_ONGOING:
		return (EAGAIN);
	case ICE_ERR_AQ_ERROR:
		break;
	default:
		return (EIO);
	}

	switch (ice->ice_hw.adminq.sq_last_status) {
	case ICE_AQ_RC_ENOSPC:
	case ICE_AQ_RC_ENOMEM:
		return (ENOSPC);
	default:
		return (EIO);
	}
}

void
ice_update_mtu(ice_t *ice)
{
	ice->ice_pf_vsi.vi_max_frame = ice->ice_mtu +
	    sizeof (struct ether_vlan_header) + ETHERFCSL;
}

/*
 * Program and enable every tx/rx queue.  Called from mac start so each plumb
 * cycle re-adds the tx scheduler node and rewrites the rx context, resetting
 * the hardware ring head to zero in lockstep with the software pointers.  On
 * partial failure the queues programmed so far are unwound.
 */
static int
ice_queues_program(ice_t *ice)
{
	uint_t i, j;
	int status;

	for (i = 0; i < ice->ice_num_txr; i++) {
		status = ice_tx_ring_program(ice, &ice->ice_txr[i]);
		if (status != ICE_SUCCESS) {
			while (i-- > 0) {
				(void) ice_tx_ring_unprogram(ice,
				    &ice->ice_txr[i]);
			}
			return (status);
		}
	}
	for (i = 0; i < ice->ice_num_rxr; i++) {
		status = ice_rx_ring_program(ice, &ice->ice_rxr[i]);
		if (status != ICE_SUCCESS) {
			while (i-- > 0) {
				(void) ice_rx_ring_unprogram(ice,
				    &ice->ice_rxr[i]);
			}
			for (j = 0; j < ice->ice_num_txr; j++) {
				(void) ice_tx_ring_unprogram(ice,
				    &ice->ice_txr[j]);
			}
			return (status);
		}
	}

	return (ICE_SUCCESS);
}

/*
 * Disable every tx/rx queue.  Every ring is attempted even after a failure, so
 * a queue that can be stopped is stopped.  Returns B_FALSE if any queue did not
 * confirm the disable: its DMA may still be live, so the caller must not
 * release anything the hardware can still reach.
 */
static boolean_t
ice_queues_disable(ice_t *ice)
{
	boolean_t ok = B_TRUE;
	uint_t i;

	for (i = 0; i < ice->ice_num_txr; i++) {
		if (ice_tx_ring_unprogram(ice, &ice->ice_txr[i]) != ICE_SUCCESS)
			ok = B_FALSE;
	}
	for (i = 0; i < ice->ice_num_rxr; i++) {
		if (ice_rx_ring_unprogram(ice, &ice->ice_rxr[i]) != ICE_SUCCESS)
			ok = B_FALSE;
	}

	return (ok);
}

static void
ice_queues_intr_map(ice_t *ice)
{
	uint_t i;

	for (i = 0; i < ice->ice_num_txr; i++)
		ice_map_txq_vector(ice, &ice->ice_txr[i]);
	for (i = 0; i < ice->ice_num_rxr; i++) {
		ice_rx_ring_intr_route(&ice->ice_rxr[i], ICE_RX_INTR_MAP);
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
		ice_rx_ring_intr_route(&ice->ice_rxr[i], ICE_RX_INTR_UNMAP);
	ice_flush(hw);
}

/*
 * Dissociate every queue's interrupt cause from its vector before the queues
 * are disabled: clear CAUSE_ENA, then trigger a software interrupt on the
 * vector so a cause already in flight is retired (datasheet 9.1.3.1.2).  A
 * queue disable issued without this is not guaranteed to complete.  The MSI-X
 * and ITR routing is left in place, unlike ice_queues_intr_unmap(), so
 * ice_queues_intr_map() is what re-arms the cause on the next start.
 */
static void
ice_queues_intr_dissociate(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t reg;
	uint_t i;

	for (i = 0; i < ice->ice_num_txr; i++) {
		ice_tx_ring_t *itr = &ice->ice_txr[i];

		reg = rd32(hw, QINT_TQCTL(itr->itxr_index));
		reg &= ~QINT_TQCTL_CAUSE_ENA_M;
		wr32(hw, QINT_TQCTL(itr->itxr_index), reg);
		ice_flush(hw);
		wr32(hw, GLINT_DYN_CTL(itr->itxr_vec),
		    GLINT_DYN_CTL_SWINT_TRIG_M | GLINT_DYN_CTL_INTENA_MSK_M);
	}

	for (i = 0; i < ice->ice_num_rxr; i++) {
		ice_rx_ring_t *irr = &ice->ice_rxr[i];

		ice_rx_ring_intr_route(irr, ICE_RX_INTR_DISSOCIATE);
		wr32(hw, GLINT_DYN_CTL(irr->irxr_vec),
		    GLINT_DYN_CTL_SWINT_TRIG_M | GLINT_DYN_CTL_INTENA_MSK_M);
	}

	ice_flush(hw);
}

/*
 * Program queues and buffers with the lifecycle lock already held. Both MAC
 * start and reset use this operation; reset has its own admission/completion
 * policy and additionally resumes MAC-started RX rings. Programming resets
 * hardware ring heads in lockstep with the software pointers.
 */
static int
ice_start_datapath(ice_t *ice)
{
	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	/*
	 * Re-arm the queue interrupt causes first: ice_stop() cleared
	 * CAUSE_ENA to dissociate them for the queue disable, and nothing else
	 * restores it.  Idempotent, and inert until a queue is enabled below.
	 */
	ice_queues_intr_map(ice);

	if (ice_queues_program(ice) != ICE_SUCCESS)
		return (EIO);
	if (!ice_rx_start(ice)) {
		(void) ice_queues_disable(ice);
		return (EIO);
	}
	ice_tx_start(ice);
	atomic_or_32(&ice->ice_state, ICE_STATE_STARTED);

	return (0);
}

int
ice_start(ice_t *ice)
{
	uint32_t blocked = ICE_STATE_RESET_FAILED | ICE_STATE_PFR_REQ |
	    ICE_STATE_RESET_PENDING;
	int ret;

	/*
	 * ice_rebuild_lock is the outermost lock and is uncontended in normal
	 * operation; it brackets the callback so a reset rebuild cannot
	 * interleave with a plumb.
	 */
	mutex_enter(&ice->ice_rebuild_lock);

	/*
	 * Refuse to start while the hardware is untrustworthy: a terminally
	 * failed reset (reload needed), or a fatal cause or reset still owed a
	 * rebuild.  A replumb must not clear the fail-closed state or reprogram
	 * queues on stale hardware; the rebuild alone clears these bits.
	 */
	if (ice->ice_detaching || (ice->ice_state & blocked) != 0) {
		mutex_exit(&ice->ice_rebuild_lock);
		return (EIO);
	}

	/*
	 * Clear any latched datapath error: mac start fully re-programs the
	 * queues and rings below, so it is the recovery point for a device that
	 * faulted while plumbed and was then replumbed.
	 */
	atomic_and_32(&ice->ice_state, ~ICE_STATE_ERROR);

	ret = ice_start_datapath(ice);
	if (ret != 0)
		atomic_or_32(&ice->ice_state, ICE_STATE_ERROR);
	ice_link_state_publish(ice);
	mutex_exit(&ice->ice_rebuild_lock);

	return (ret);
}

void
ice_stop(ice_t *ice)
{
	boolean_t disabled;

	mutex_enter(&ice->ice_rebuild_lock);

	/*
	 * Mark the device stopped, dissociate the queues from their interrupt
	 * causes, then disable the queues so hardware stops touching
	 * descriptors and buffers.
	 */
	atomic_and_32(&ice->ice_state, ~ICE_STATE_STARTED);
	ice_queues_intr_dissociate(ice);
	disabled = ice_queues_disable(ice);

	if (disabled) {
		ice_tx_stop(ice);
		/*
		 * mac stop cannot fail and cannot wait forever.  A loan the
		 * stack never returns leaves ice_rx_stop() short of a full
		 * drain; it deliberately leaves that ring's pool intact rather
		 * than freeing buffers still held upstream, and ice_rx_start()
		 * re-checks before reusing it.
		 */
		(void) ice_rx_stop(ice);
	} else {
		/*
		 * A queue that did not confirm the disable can still master
		 * into the rings, so nothing may be released (datasheet
		 * 10.4.3.1.2 step 9).  mac stop cannot fail, but it can
		 * decline to reclaim: quiesce the software side only and
		 * request a PF reset, which is the barrier ice_rebuild()
		 * reclaims behind, as the reset path already does.
		 */
		ice_tx_quiesce(ice);
		(void) ice_rx_quiesce(ice);
		atomic_or_32(&ice->ice_state,
		    ICE_STATE_ERROR | ICE_STATE_PFR_REQ);
		ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_LOST);
		ice_reset_redispatch(ice);
	}

	mutex_exit(&ice->ice_rebuild_lock);
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
	 * Detach has closed the packet paths before unregistering MAC.  The
	 * remaining handlers can still touch ring storage and dispatch admin
	 * work, so mask and remove them before releasing those resources.
	 * Admin queue commands below are polled and need no interrupts.
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
	 * ice_detach_quiesce() isolated packet DMA and reclaimed TX descriptors
	 * before unregistering MAC, returning their copy buffers to the pools.
	 * The pools can therefore be destroyed before the remaining ring/TCB
	 * storage. Attach failure never exposed the datapath or enabled queues.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_BUFS)
		ice_buf_fini(ice);

	/*
	 * Remove the remaining queue-to-vector routing.  Packet DMA is already
	 * isolated; the VSI and control queue still exist for later cleanup.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_QUEUE_INTR)
		ice_queues_intr_unmap(ice);

	/*
	 * Stop the admin periodic before the taskq it dispatches into.
	 * ddi_periodic_delete waits for an in-flight callout, so no new
	 * ice_oicr_task can be queued once this returns.
	 */
	ice_admin_periodic_stop(ice);

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

	/*
	 * ice_rx_rings_free() also reclaims a control-block pool that an
	 * ice_rx_stop() timeout left behind.  Reaching it here rather than
	 * earlier in detach is deliberate: by now the taskqs are drained and
	 * ice_rx_quiesce() has confirmed no loans remain, so nothing can be
	 * reposting or reading the pool as it is freed.
	 */
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
		 * Best-effort cleanup for a later attach.  Packet DMA was
		 * already stopped before any resource release; this reset is
		 * not part of that isolation proof.
		 */
		if (ice_reset(&ice->ice_hw, ICE_RESET_PFR) != ICE_SUCCESS)
			ice_error(ice, "cleanup PF reset failed");
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

	ice_diag_fini(ice);
	mutex_destroy(&ice->ice_loopback_lock);
	mutex_destroy(&ice->ice_lock);
	ice->ice_attach_progress = 0;
}

/*
 * Hand an owed rebuild to the reset taskq when a lifetime gate lifts or a
 * caller observes a request it cannot service itself.  The hardware causes
 * are one-shot; persistent request bits retain the work.  Redispatch promptly
 * here, while the admin periodic provides a retry if taskq dispatch fails.
 *
 * Runs under ice_rebuild_lock, which ice_reset_dispatch() does not take; it
 * only sets a flag and queues onto the reset taskq, and that worker waits on
 * this lock, so there is neither recursion nor a self-deadlock.  A terminally
 * failed reset is deliberately not requeued.
 */
void
ice_reset_redispatch(ice_t *ice)
{
	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	if ((ice->ice_state & ICE_STATE_RESET_FAILED) != 0)
		return;

	if ((ice->ice_state &
	    (ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ)) != 0)
		ice_reset_dispatch(ice);
}

static int
ice_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	ice_t *ice;
	struct ice_hw *hw;
	struct ice_osdep *osdep;
	int mtu;
	int limit;
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
	/*
	 * Set before the reset taskq exists so no rebuild can ever observe it
	 * clear on a half-constructed instance; it is cleared under
	 * ice_rebuild_lock once attach is complete.
	 */
	ice->ice_attaching = B_TRUE;
	mutex_init(&ice->ice_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&ice->ice_loopback_lock, NULL, MUTEX_DRIVER, NULL);
	ice_diag_init(ice);

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

	if (ice_check_acc_handle(ice, osdep->ios_cfg_handle) != DDI_FM_OK ||
	    ice_check_acc_handle(ice, osdep->ios_reg_handle) != DDI_FM_OK) {
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
	if (ice_check_acc_handle(ice, osdep->ios_reg_handle) != DDI_FM_OK ||
	    ice_check_acc_handle(ice, osdep->ios_cfg_handle) != DDI_FM_OK) {
		ddi_fm_service_impact(dip, DDI_SERVICE_LOST);
		goto fail;
	}

	/*
	 * PFINT_OICR has reset source CORER, so it survives the PF reset
	 * ice_init_hw() issued: anything firmware or a previous driver instance
	 * latched before this driver owned the function is still pending and is
	 * not ours to act on.  Drop it here so the harvest in
	 * ice_intr_oicr_setup() below reports only causes from the window this
	 * driver did own.
	 */
	(void) rd32(hw, PFINT_OICR);

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

	/*
	 * Ask firmware for link events before the PHY is enabled below, so a
	 * transition during the rest of attach is queued on the ARQ rather than
	 * lost.  This only configures an event mask; the OICR that delivers it
	 * is armed much later.
	 */
	if (!ice_set_link_events(ice))
		goto fail;

	ice_link_status_update(ice);

	/* Enable the PHY; firmware will not bring the link up on its own. */
	ice_phy_fw_wait(ice);
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
	    ice->ice_dip, DDI_PROP_DONTPASS, "tx_lso_enable", 1) != 0;
	limit = ddi_prop_get_int(DDI_DEV_T_ANY, ice->ice_dip,
	    DDI_PROP_DONTPASS, "rx_limit_per_intr", ICE_DEF_RX_LIMIT_PER_INTR);
	if (limit < ICE_MIN_RX_LIMIT_PER_INTR)
		limit = ICE_MIN_RX_LIMIT_PER_INTR;
	else if (limit > ICE_MAX_RX_LIMIT_PER_INTR)
		limit = ICE_MAX_RX_LIMIT_PER_INTR;
	ice->ice_rx_limit_per_intr = limit;
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
	 * Arm the interrupts only now, as FreeBSD's ice_if_attach_post() does.
	 * An OICR delivered earlier dispatches a reset rebuild that frees and
	 * reinitializes the scheduler tree, control queues and PF VSI while
	 * this thread is still building on them.  Everything above drives the
	 * admin queue by polling, so none of it needs the OICR.  This must
	 * still precede ice_mac_register(): MAC can call ice_start() as soon
	 * as registration returns, and the queue vectors have to be live then.
	 *
	 * Everything latched since the pre-drain above happened on this
	 * driver's watch, so harvest it into persistent state rather than
	 * discarding it: PFINT_OICR is read-clear and nothing re-derives a
	 * cause afterwards.  The attaching gate still keeps the resulting
	 * rebuild off the half-built instance; ice_reset_redispatch() below
	 * runs it once the gate lifts.
	 */
	ice_intr_oicr_setup(ice, B_TRUE);
	if (!ice_intr_enable(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_ENABLE_INTR;

	/*
	 * Register with MAC last: once this returns the datapath is reachable
	 * by clients, so everything it touches must already be live.
	 */
	if (!ice_mac_register(ice))
		goto fail;
	ice->ice_attach_progress |= ICE_ATTACH_MAC;

	/*
	 * Refresh carrier after registration and interrupt setup, before
	 * opening the instance to rebuilds.  The lifecycle lock serializes
	 * this admin-queue query and its publication with MAC start and reset.
	 * The periodic refresh handles later events, including a link cause
	 * whose notification was consumed while attach gated the worker.
	 */
	mutex_enter(&ice->ice_rebuild_lock);
	ice_link_status_update(ice);
	ice->ice_attaching = B_FALSE;
	ice_reset_redispatch(ice);
	mutex_exit(&ice->ice_rebuild_lock);

	ice_oicr_resync(ice);
	ice_admin_periodic_start(ice);

	atomic_or_32(&ice->ice_state, ICE_STATE_ATTACHED);
	return (DDI_SUCCESS);

fail:
	ice_unconfigure(ice);
	ddi_soft_state_free(ice_state_p, instance);
	return (DDI_FAILURE);
}

/*
 * Close the stopped datapath and establish the packet DMA barrier before
 * unregistering MAC.  A failure releases nothing and leaves detach retryable.
 * The caller holds ice_rebuild_lock and has barred new starts and rebuilds.
 */
static boolean_t
ice_detach_quiesce(ice_t *ice)
{
	uint32_t acc_errors;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));
	ASSERT((ice->ice_state & ICE_STATE_STARTED) == 0);

	if ((ice->ice_attach_progress & ICE_ATTACH_RINGS) != 0) {
		ice_tx_quiesce(ice);
		if (!ice_rx_quiesce(ice)) {
			ice_error(ice, "timed out draining rx loans; "
			    "detach deferred");
			return (B_FALSE);
		}
	}

	if ((ice->ice_attach_progress & ICE_ATTACH_QUEUE_INTR) == 0)
		return (B_TRUE);

	acc_errors = atomic_add_32_nv(&ice->ice_acc_errors, 0);
	if (atomic_add_32_nv(&ice->ice_acc_clears, 0) != 0)
		goto access_failed;
	membar_enter();

	ice_queues_intr_dissociate(ice);
	if (!ice_queues_disable(ice)) {
		/*
		 * A reset invalidates the cached AQ/VSI configuration.  If
		 * unregister later refuses an open control client, the gate
		 * rollback must rebuild it before admitting another start.
		 */
		atomic_or_32(&ice->ice_state,
		    ICE_STATE_ERROR | ICE_STATE_PFR_REQ);
		ice->ice_hw.reset_ongoing = true;
		if (ice_reset(&ice->ice_hw, ICE_RESET_PFR) != ICE_SUCCESS) {
			ice_error(ice, "cannot stop packet DMA; "
			    "detach deferred");
			ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_LOST);
			return (B_FALSE);
		}
	}

	/* An interrupt observer must not hide a fault in the polled reads. */
	if (ice_check_acc_handle(ice, ice->ice_osdep.ios_reg_handle) !=
	    DDI_FM_OK)
		goto access_failed;
	membar_exit();
	if (atomic_add_32_nv(&ice->ice_acc_clears, 0) != 0 ||
	    atomic_add_32_nv(&ice->ice_acc_errors, 0) != acc_errors)
		goto access_failed;

	ice_tx_reclaim(ice);
	return (B_TRUE);

access_failed:
	atomic_or_32(&ice->ice_state, ICE_STATE_ERROR | ICE_STATE_PFR_REQ);
	ddi_fm_service_impact(ice->ice_dip, DDI_SERVICE_LOST);
	ice_error(ice, "cannot verify packet DMA stop; detach deferred");
	return (B_FALSE);
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
	 * Leave an active datapath alone.  The same lock makes the detaching
	 * gate atomic with ice_start(), and waits out a stop or rebuild.
	 * Workers honor the gate until teardown or the failure rollback below.
	 */
	mutex_enter(&ice->ice_rebuild_lock);
	if ((ice->ice_state & ICE_STATE_STARTED) != 0) {
		mutex_exit(&ice->ice_rebuild_lock);
		return (DDI_FAILURE);
	}
	ice->ice_detaching = B_TRUE;
	if (!ice_detach_quiesce(ice)) {
		mutex_exit(&ice->ice_rebuild_lock);
		goto fail;
	}
	mutex_exit(&ice->ice_rebuild_lock);

	/*
	 * All fallible hardware work and the loan/upcall drain precede this
	 * irreversible step.  Stopped control clients can still refuse it;
	 * preserve resources and recover any reset-invalidated state then.
	 */
	if (ice->ice_attach_progress & ICE_ATTACH_MAC) {
		if (ice_mac_unregister(ice) != 0)
			goto fail;
		ice->ice_attach_progress &= ~ICE_ATTACH_MAC;
	}

	ice_loopback_fini(ice);
	ice_led_fini(ice);

	ice_unconfigure(ice);
	ddi_soft_state_free(ice_state_p, instance);
	return (DDI_SUCCESS);

fail:
	mutex_enter(&ice->ice_rebuild_lock);
	ice->ice_detaching = B_FALSE;
	ice_reset_redispatch(ice);
	mutex_exit(&ice->ice_rebuild_lock);
	return (DDI_FAILURE);
}

/*
 * Mark the reset terminally failed and fail the datapath closed.  Reserved for
 * the rebuild's per-step hardware and firmware failures, matching every
 * ICE_STATE_RESET_FAILED site in the FreeBSD driver; software-side buffer
 * ownership never reaches here.  The device stays down until the driver is
 * reloaded.
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
 * driver's ice_prepare_for_reset(), which likewise cannot fail.  Runs under
 * ice_rebuild_lock.
 *
 * A loan the stack does not return within the bounded wait is deliberately not
 * an error here.  ice_rx_quiesce() leaves such a ring fully intact and
 * ice_rx_start() refuses to reuse a pool with loans outstanding, so the rebuild
 * fails soft at ice_start_datapath() and recovers on the next mac start.
 * Escalating instead would take the NIC terminally offline over buffers that
 * were about to come back.
 */
static void
ice_prepare_for_reset(ice_t *ice)
{
	struct ice_hw *hw = &ice->ice_hw;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	/*
	 * Quiesce the datapath if it was running, but release nothing.  The PFR
	 * is not issued until ice_rebuild(), and E810 has no MMIO tx queue
	 * disable, so a tx queue can still master DMA right up to the reset;
	 * the packet DMA is reclaimed past that barrier instead.  Interrupt
	 * causes are dissociated before the queues are disabled, as FreeBSD
	 * requires (ice_lib.c:1473, ice_lib.c:1510) and i40e_stop() does
	 * (i40e_main.c:3119).  ice_tx_quiesce() waits for in-flight transmits
	 * and ice_rx_quiesce() for loaned buffers under a bounded deadline, so
	 * the autonomous reset taskq cannot wedge on a lost loan.
	 * ICE_STATE_STARTED is left set so the rebuild restarts the datapath.
	 */
	if ((ice->ice_state & ICE_STATE_STARTED) != 0) {
		ice_queues_intr_unmap(ice);
		(void) ice_queues_disable(ice);
		ice_tx_quiesce(ice);
		(void) ice_rx_quiesce(ice);
	}

	/*
	 * Only now: ice_dis_vsi_txq() above rides the admin queue, which
	 * soft-fails with ICE_ERR_RESET_ONGOING once this is set, so the tx
	 * queue disable has to be issued first.  Everything below deliberately
	 * runs with the admin queue fenced off.  Idempotent: the GRST path set
	 * this in the ISR, the fatal/PFR-request path did not.
	 */
	hw->reset_ongoing = true;

	/* Report the link down for the duration of the rebuild. */
	ice_link_report(ice, LINK_STATE_DOWN);

	/* Silence the OICR so no new cause fires mid-reset. */
	ice_intr_oicr_disable(ice);
	wr32(hw, PFINT_OICR_ENA, 0);
	ice_flush(hw);

	/*
	 * Drop the state a reset invalidates without freeing anything a
	 * concurrent reader still holds a pointer to.  The control queue is
	 * shut down, not destroyed: ice_shutdown_sq/rq zero the ring count
	 * under the queue lock, so an admin-queue caller racing this gets
	 * ICE_ERR_NOT_READY instead of touching a destroyed mutex.  port_info
	 * and the VSI contexts survive the reset, as they do on FreeBSD.
	 */
	ice_clear_hw_tbls(hw);
	if (hw->port_info != NULL)
		ice_sched_cleanup_all(hw);
	ice_shutdown_all_ctrlq(hw, false);

	/*
	 * Force the PF VSI to be recreated by the rebuild.  The vi_macs list is
	 * left intact: it is the authoritative record the rebuild replays.
	 */
	ice->ice_pf_vsi.vi_added = B_FALSE;
}

/*
 * Claim the requests this worker will service without erasing a concurrently
 * latched cause.  The returned mask selects the reset type; requests arriving
 * after the successful CAS remain owed to the next worker.
 */
static uint32_t
ice_reset_take_requests(ice_t *ice)
{
	const uint32_t mask = ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ;
	uint32_t old, requests;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	do {
		old = ice->ice_state;
		requests = old & mask;
		if (requests == 0)
			return (0);
	} while (atomic_cas_32(&ice->ice_state, old,
	    (old & ~requests) | ICE_STATE_ERROR) != old);

	return (requests);
}

/*
 * The completed rebuild can reopen the datapath only when no later reset is
 * owed.  A CAS keeps a new request's fail-closed state intact if it races the
 * final transition.  The worker will redispatch that request on exit.
 */
static boolean_t
ice_reset_complete(ice_t *ice)
{
	uint32_t old;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	do {
		old = ice->ice_state;
		if ((old & (ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ)) != 0)
			return (B_FALSE);
	} while (atomic_cas_32(&ice->ice_state, old,
	    old & ~ICE_STATE_ERROR) != old);

	return (B_TRUE);
}

/*
 * Reinitialize the function after a reset and restore the datapath.  Modeled
 * on the FreeBSD ice driver's ice_rebuild().  Runs under ice_rebuild_lock.
 * Each failing step jumps to reset_failed, which fails closed until the driver
 * is reloaded.
 */
static void
ice_rebuild(ice_t *ice, uint32_t requests)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint32_t fwsm;
	int rc;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	/*
	 * Nothing below re-runs ice_init_hw(), which used to be what waited out
	 * (or issued) the hardware reset, so that has to happen explicitly.
	 * ice_pf_reset() polls a global reset already in flight to completion
	 * and otherwise drives a real PF reset, which is what the fatal-cause
	 * and test-hook PFR_REQ paths need.
	 */
	if ((requests & ICE_STATE_RESET_PENDING) != 0) {
		if (ice_reset_empr_slow(hw))
			delay(drv_usectohz(ICE_EMPR_SLOW_WAIT_SEC * MICROSEC));
		rc = ice_check_reset(hw);
	} else {
		rc = ice_reset(hw, ICE_RESET_PFR);
	}
	if (rc != 0) {
		ice_error(ice, "device never came out of reset: %d", rc);
		goto reset_failed;
	}

	/*
	 * An EMPR can leave the firmware in recovery mode.  The rebuild then
	 * stops here, fail-closed, before any queue or filter programming.
	 */
	switch (ice_fw_state(ice, &fwsm)) {
	case ICE_FW_USABLE:
		break;
	case ICE_FW_RECOVERY:
		ice_fw_recovery_report(ice, fwsm);
		goto reset_failed;
	default:
		ice_error(ice, "cannot read the firmware state after reset");
		goto reset_failed;
	}

	/*
	 * The reset has completed, so no queue can master against the old
	 * contexts.  E810 has no MMIO tx queue disable, so this is the first
	 * point at which releasing packet DMA is safe; ice_prepare_for_reset()
	 * deliberately only quiesced.  Both are no-ops when the datapath was
	 * already down.
	 */
	ice_tx_reclaim(ice);
	ice_rx_reclaim(ice);

	/*
	 * Every step below rides the admin queue, which soft-fails with
	 * ICE_ERR_RESET_ONGOING while this is set.
	 */
	hw->reset_ongoing = false;

	/*
	 * Restore only what the reset cleared.  The common-code state that
	 * outlives a reset (port_info, the VSI contexts, the DDP copy) is never
	 * freed here, so concurrent readers holding ice_rebuild_lock keep
	 * seeing valid memory and detach remains the only common-code teardown.
	 */
	rc = ice_init_all_ctrlq(hw);
	if (rc != 0) {
		ice_error(ice, "control queue reinit failed: %d", rc);
		goto reset_failed;
	}

	rc = ice_sched_query_res_alloc(hw);
	if (rc != 0) {
		ice_error(ice, "scheduler resource query failed: %d", rc);
		goto reset_failed;
	}

	rc = ice_clear_pf_cfg(hw);
	if (rc != 0) {
		ice_error(ice, "failed to clear PF configuration: %d", rc);
		goto reset_failed;
	}

	ice_clear_pxe_mode(hw);

	/*
	 * ice_validate_caps() is a bounds gate only: the ring and queue counts
	 * stay as attach derived them, since resizing them here would race the
	 * per-ring loaned-buffer accounting.
	 */
	rc = ice_get_caps(hw);
	if (rc != 0) {
		ice_error(ice, "failed to re-read capabilities: %d", rc);
		goto reset_failed;
	}
	if (!ice_validate_caps(ice))
		goto reset_failed;

	rc = ice_sched_init_port(hw->port_info);
	if (rc != 0) {
		ice_error(ice, "failed to reinitialize the port: %d", rc);
		goto reset_failed;
	}

	/*
	 * A global or core reset can zero the MAC counters, so drop the
	 * baselines and let the next read re-establish them.
	 */
	ice_stats_reset(ice);

	/*
	 * Replay the DDP package the common code already holds a copy of.  The
	 * attach path instead re-reads the file and copies it in, which would
	 * overwrite hw->pkg_copy and leak the previous copy: nothing frees it
	 * now that no segment teardown runs across a reset.
	 *
	 * A failed reload cannot be absorbed by entering safe mode here.  MAC
	 * caches mi_capab at mac_register() and the framework performs no
	 * client quiescing on a capability change, so the stack would keep
	 * handing down partial-checksum and LSO frames to a pipeline with no
	 * parser profiles.  Fail closed instead; a driver reload enters safe
	 * mode coherently at attach, before mac_register().
	 */
	if (!ice->ice_safe_mode) {
		enum ice_ddp_state state;

		if (hw->pkg_copy == NULL) {
			ice_error(ice, "no DDP package to replay");
			goto reset_failed;
		}

		state = ice_init_pkg(hw, hw->pkg_copy, hw->pkg_size);
		ice->ice_ddp_state = state;
		if (!ice_is_init_pkg_successful(state)) {
			ice_error(ice, "ice.pkg reload failed (%d); offloads "
			    "cannot be withdrawn on a live instance", state);
			goto reset_failed;
		}
	}

	if (ice_vsi_rebuild(ice) != ICE_SUCCESS)
		goto reset_failed;

	ice_loopback_replay(ice);
	ice_led_replay(ice);

	/*
	 * Re-route and re-arm the interrupts a reset clears.  Discard the
	 * hardware causes that requested this rebuild; the worker already
	 * consumed their software request bits.  Leave later software requests
	 * intact for its next pass.
	 */
	ice_intr_oicr_setup(ice, B_FALSE);
	if (!ice_set_link_events(ice))
		goto reset_failed;
	ice_queues_intr_map(ice);

	/* Refresh the cached link and re-enable the PHY. */
	ice_link_status_update(ice);
	ice_setup_link(ice);
	ice_phy_caps_update(ice);

	/* A later request keeps the datapath closed until its own rebuild. */
	if (!ice_reset_complete(ice))
		return;

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
	/*
	 * Silence the OICR again: the failure may have come from after
	 * ice_intr_oicr_setup() re-armed it, and a terminally failed instance
	 * must stop taking interrupts.  This covers only the interrupt-driven
	 * path; ice_oicr_task() gates the admin periodic on the terminal bit.
	 */
	ice_intr_oicr_disable(ice);
	wr32(hw, PFINT_OICR_ENA, 0);
	ice_flush(hw);

	/*
	 * Leave the control queue quiesced rather than destroyed: port_info and
	 * the VSI contexts stay allocated, so the readers that serialize on
	 * ice_rebuild_lock remain valid for the fail-closed life of the
	 * instance and detach still tears the HW down exactly once.
	 */
	ice_shutdown_all_ctrlq(hw, false);

	/*
	 * Every path here has issued or waited out a reset, so the hardware
	 * loopback is gone: stop reporting a mode that cannot exist.  This
	 * precedes ice_reset_set_failed() so its link-down report is the last
	 * state published.
	 */
	ice_link_loopback_update(ice, ICE_LB_NONE);

	/*
	 * Retire any later requests on terminal failure.  They cannot be
	 * serviced until reload, and ice_start() blocks independently on
	 * ICE_STATE_RESET_FAILED.  Ordinary success leaves later requests
	 * untouched so the worker can redispatch them.
	 */
	atomic_and_32(&ice->ice_state,
	    ~(ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ));
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
	uint32_t requests;

	/* The queued/running ownership flag stays set while this lock waits. */
	mutex_enter(&ice->ice_rebuild_lock);
	/*
	 * Terminal failure cannot recover without reload.  The attaching and
	 * detaching gates leave requests owed; the gate-lifting caller
	 * redispatches them once the instance is usable again.
	 */
	if (ice->ice_attaching || ice->ice_detaching ||
	    (ice->ice_state & ICE_STATE_RESET_FAILED) != 0)
		goto done;

	requests = ice_reset_take_requests(ice);
	if (requests != 0) {
		ice_prepare_for_reset(ice);
		ice_rebuild(ice, requests);
	}

done:
	mutex_enter(&ice->ice_lock);
	ice->ice_reset_pending = B_FALSE;
	mutex_exit(&ice->ice_lock);
	if (!ice->ice_attaching && !ice->ice_detaching)
		ice_reset_redispatch(ice);
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

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
 * Runtime reset of an xHCI controller after a fatal error.
 *
 * Every fatal condition the driver can detect (FM register or DMA faults, the
 * HSE, HCE and SRE status bits, a command ring that will not abort, events
 * that point outside the memory we own) ends in xhci_fm_runtime_reset(). That
 * marks the controller unusable, retires every device slot by bumping
 * xhci_gen, and dispatches xhci_reset_task() here.
 *
 * The recovery keeps the driver, the HCDI registration and the root hub
 * attached and resets the controller underneath them:
 *
 *   1. Quiesce: interrupts off, controller halted. Entry points and the
 *      interrupt handler already refuse to touch the hardware while
 *      XHCI_S_ERROR is set.
 *   2. Drain: every queued command and transfer is failed back to its owner.
 *      Nothing here talks to the controller.
 *   3. Reset: HCRST (issued before the drain so that no DMA is in flight
 *      when memory is freed), then the same configuration and start sequence
 *      attach uses. Port power is restored when the controller manages it.
 *   4. Resume: XHCI_S_ERROR is cleared and the root hub reports every port as
 *      disconnected and then reconnected, so hubd removes each stale child
 *      and enumerates whatever is really present with a fresh slot.
 *
 * If the reset fails, or too many happen in a short window, the controller is
 * taken offline instead: XHCI_S_OFFLINE stays set, every entry point fails
 * fast, and hubd still removes the children through the same synthetic port
 * events. The host keeps running either way. Setting the driver.conf
 * property xhci-fatal-panic, or the global xhci_fatal_panic, restores the old
 * behaviour of panicking for a crash dump.
 */

#include <sys/pci.h>
#include <sys/usb/hcd/xhci/xhci.h>

/*
 * More than xhci_reset_max resets within xhci_reset_window_sec seconds means
 * the controller is not recovering and is taken offline.
 */
uint_t xhci_reset_max = 3;
uint_t xhci_reset_window_sec = 60;
int xhci_fatal_panic = 0;

/*
 * A controller that could not be halted or reset may still be a DMA master.
 * Turning bus mastering off in PCI configuration space stops it from writing
 * into memory the driver will free.
 */
static void
xhci_reset_bus_master_off(xhci_t *xhcip)
{
	uint16_t cmd;

	cmd = pci_config_get16(xhcip->xhci_cfg_handle, PCI_CONF_COMM);
	cmd &= ~PCI_COMM_ME;
	pci_config_put16(xhcip->xhci_cfg_handle, PCI_CONF_COMM, cmd);
}

static void
xhci_reset_offline(xhci_t *xhcip, const char *why)
{
	xhci_error(xhcip, "controller taken offline: %s", why);
	xhci_reset_bus_master_off(xhcip);

	mutex_enter(&xhcip->xhci_lock);
	xhcip->xhci_state |= XHCI_S_OFFLINE;
	xhcip->xhci_state &= ~XHCI_S_ERROR;
	cv_broadcast(&xhcip->xhci_statecv);
	mutex_exit(&xhcip->xhci_lock);

	/*
	 * Children are removed through the root hub, which answers from
	 * software while the controller is offline.
	 */
	xhci_root_hub_psc_callback(xhcip);
}

static boolean_t
xhci_reset_too_many(xhci_t *xhcip)
{
	hrtime_t now = gethrtime();
	hrtime_t window = (hrtime_t)xhci_reset_window_sec * NANOSEC;

	if (xhcip->xhci_reset_first == 0 ||
	    now - xhcip->xhci_reset_first > window) {
		xhcip->xhci_reset_first = now;
		xhcip->xhci_reset_count = 0;
	}
	xhcip->xhci_reset_count++;

	return (xhcip->xhci_reset_count > xhci_reset_max);
}

/*
 * HCRST clears port power on controllers with port power control. hubd only
 * powers ports when the root hub attaches, so restore it here.
 */
static int
xhci_reset_power_ports(xhci_t *xhcip)
{
	uint_t i;

	if ((xhcip->xhci_caps.xcap_flags & XCAP_PPC) == 0)
		return (0);

	for (i = 1; i <= xhcip->xhci_caps.xcap_max_ports; i++) {
		uint32_t reg;

		reg = xhci_get32(xhcip, XHCI_R_OPER, XHCI_PORTSC(i));
		reg &= ~XHCI_PS_CLEAR;
		reg |= XHCI_PS_PP;
		xhci_put32(xhcip, XHCI_R_OPER, XHCI_PORTSC(i), reg);
	}

	if (xhci_check_regs_acc(xhcip) != DDI_FM_OK) {
		ddi_fm_service_impact(xhcip->xhci_dip, DDI_SERVICE_LOST);
		return (EIO);
	}

	return (0);
}

static int
xhci_reset_hardware(xhci_t *xhcip)
{
	int ret;

	if ((ret = xhci_controller_configure(xhcip)) != 0)
		return (ret);
	xhci_controller_reroute(xhcip);
	if ((ret = xhci_controller_start(xhcip)) != 0)
		return (ret);

	return (xhci_reset_power_ports(xhcip));
}

void
xhci_reset_task(void *arg)
{
	xhci_t *xhcip = arg;
	int ret;

	if (xhci_fatal_panic != 0 || xhcip->xhci_fatal_panic != 0)
		panic("XHCI runtime reset required");

	xhci_error(xhcip, "fatal controller error: resetting controller");

	if (xhci_ddi_intr_disable(xhcip) == B_FALSE) {
		xhci_reset_offline(xhcip, "could not disable interrupts");
		xhci_command_ring_drain(xhcip);
		xhci_hcdi_reset_drain(xhcip);
		return;
	}

	/*
	 * The drain below frees memory the controller may still be using, so
	 * the controller has to be stopped before it runs. A halt that fails
	 * is not fatal by itself; HCRST follows and its own status polling
	 * decides whether the controller responded. If neither works the
	 * controller is cut off from the bus before anything is freed.
	 */
	(void) xhci_controller_stop(xhcip);
	if ((ret = xhci_controller_reset(xhcip)) != 0) {
		xhci_reset_offline(xhcip, ret == EIO ?
		    "fatal FM error during reset" :
		    "controller did not respond to reset");
		xhci_command_ring_drain(xhcip);
		xhci_hcdi_reset_drain(xhcip);
		return;
	}

	xhci_command_ring_drain(xhcip);
	xhci_hcdi_reset_drain(xhcip);

	if (xhci_reset_too_many(xhcip)) {
		xhci_reset_offline(xhcip, "too many resets in a short time");
		return;
	}

	if ((ret = xhci_reset_hardware(xhcip)) != 0) {
		xhci_reset_offline(xhcip, ret == EIO ?
		    "fatal FM error during reset" :
		    "controller did not come back after reset");
		return;
	}

	if (xhci_ddi_intr_enable(xhcip) == B_FALSE) {
		xhci_reset_offline(xhcip, "could not enable interrupts");
		return;
	}

	mutex_enter(&xhcip->xhci_lock);
	xhcip->xhci_state &= ~XHCI_S_ERROR;
	cv_broadcast(&xhcip->xhci_statecv);
	mutex_exit(&xhcip->xhci_lock);

	ddi_fm_service_impact(xhcip->xhci_dip, DDI_SERVICE_RESTORED);
	xhci_log(xhcip, "controller reset complete, %u reset%s in the last "
	    "%u seconds", xhcip->xhci_reset_count,
	    xhcip->xhci_reset_count == 1 ? "" : "s", xhci_reset_window_sec);

	xhci_root_hub_psc_callback(xhcip);
}

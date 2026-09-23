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
 * Copyright 2019, Joyent, Inc.
 * Copyright 2026 RackTop Systems, Inc.
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Port controls that MAC exposes through capabilities: transceiver (SFF)
 * access and the port identification LED.  Each one sends admin queue
 * commands, so each holds ice_rebuild_lock to keep a reset rebuild from
 * shutting the control queue down under it.
 */

#include <sys/mac_provider.h>

#include "ice.h"
#include "ice_common.h"

/*
 * SFF module (transceiver) access.  Pages 0xa0/0xa2 are the I2C device
 * addresses of the SFF-8472 diagnostic memory; the admin-queue command reads at
 * most 16 bytes per request.
 */
#define	ICE_SFF_8472_BASE	0xa0
#define	ICE_SFF_8472_DIAG	0xa2
#define	ICE_SFF_PAGE_LEN	256
#define	ICE_SFF_READ_CHUNK	16

int
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

int
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

/*
 * Blink the port LED, or return it to firmware control.  MAC serializes the
 * callbacks through its perimeter.  ice_led_ident records the accepted mode
 * so that a reset rebuild and detach can restore it.
 */
int
ice_led_set(void *arg, mac_led_mode_t mode, uint_t flags)
{
	ice_t *ice = arg;
	boolean_t ident;
	int status;

	if (flags != 0)
		return (EINVAL);

	switch (mode) {
	case MAC_LED_DEFAULT:
		ident = B_FALSE;
		break;
	case MAC_LED_IDENT:
		ident = B_TRUE;
		break;
	default:
		return (ENOTSUP);
	}

	mutex_enter(&ice->ice_rebuild_lock);
	status = ice_aq_set_port_id_led(ice->ice_hw.port_info, !ident, NULL);
	if (status == ICE_SUCCESS)
		ice->ice_led_ident = ident;
	mutex_exit(&ice->ice_rebuild_lock);

	if (status != ICE_SUCCESS) {
		ice_error(ice, "failed to set the port LED: %d", status);
		return (EIO);
	}

	return (0);
}

/*
 * A reset can return the LED to firmware control while MAC still records
 * IDENT.  Blink it again so the two agree.  Called from ice_rebuild().
 */
void
ice_led_replay(ice_t *ice)
{
	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));

	if (ice->ice_led_ident && ice_aq_set_port_id_led(ice->ice_hw.port_info,
	    false, NULL) != ICE_SUCCESS)
		ice_error(ice, "port LED not restored after reset");
}

/*
 * Give the LED back to firmware at detach.  MAC is unregistered, so no
 * callback can race this.
 */
void
ice_led_fini(ice_t *ice)
{
	mutex_enter(&ice->ice_rebuild_lock);
	if (ice->ice_led_ident) {
		(void) ice_aq_set_port_id_led(ice->ice_hw.port_info, true,
		    NULL);
		ice->ice_led_ident = B_FALSE;
	}
	mutex_exit(&ice->ice_rebuild_lock);
}

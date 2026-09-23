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
 * Run the imported MAC type mapping and the driver's per-family decisions for
 * each device ID.  The Python driver passes "id:family" pairs.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define	ICE_INTEL_VENDOR_ID		0x8086
#define	ICE_ERR_DEVICE_NOT_SUPPORTED	-8
#define	ICE_DBG_TRACE			0
#define	ICE_DBG_INIT			0
#define	ice_debug(hw, mask, ...)	((void)(hw), (void)(mask))

#include "ice_devids.h"

enum ice_mac_type {
	ICE_MAC_UNKNOWN = 0,
	ICE_MAC_VF,
	ICE_MAC_E810,
	ICE_MAC_E830,
	ICE_MAC_GENERIC,
	ICE_MAC_GENERIC_3K,
	ICE_MAC_GENERIC_3K_E825,
};

struct ice_hw {
	u16 vendor_id, device_id;
	enum ice_mac_type mac_type;
};

#include "ice_family_body.h"

static void
check(unsigned id, const char *want)
{
	struct ice_hw hw = { ICE_INTEL_VENDOR_ID, (u16)id, ICE_MAC_UNKNOWN };
	const char *got;

	assert(ice_set_mac_type(&hw) == 0);
	got = ice_family_name(&hw);
	if (strcmp(want, "none") == 0) {
		assert(got == NULL);
		return;
	}
	if (got == NULL || strcmp(got, want) != 0) {
		(void) fprintf(stderr, "device %04x: family %s, want %s\n",
		    id, got == NULL ? "none" : got, want);
		exit(1);
	}
	FAMILY_CHECKS(&hw, want);
}

int
main(int argc, char **argv)
{
	struct ice_hw other = { 0x8087, ICE_DEV_ID_E810C_QSFP,
	    ICE_MAC_UNKNOWN };
	int i;

	/* Another vendor never reaches a supported family. */
	assert(ice_set_mac_type(&other) == ICE_ERR_DEVICE_NOT_SUPPORTED);
	assert(ice_family_name(&other) == NULL);

	for (i = 1; i < argc; i++) {
		char *sep = strchr(argv[i], ':');

		assert(sep != NULL);
		*sep = '\0';
		check((unsigned)strtoul(argv[i], NULL, 16), sep + 1);
	}
	(void) printf("PASS: %d device IDs map to their families\n",
	    argc - 1);
	return (0);
}

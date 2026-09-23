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
typedef int boolean_t;
typedef unsigned int uint_t;

#define	B_TRUE				1
#define	B_FALSE				0
#define	ICE_INTEL_VENDOR_ID		0x8086
#define	ICE_ERR_DEVICE_NOT_SUPPORTED	-8
#define	ICE_ERR_AQ_NO_WORK		-102
#define	ICE_DBG_TRACE			0
#define	ICE_DBG_INIT			0
#define	ice_debug(hw, mask, ...)	((void)(hw), (void)(mask))
#define	MAKEMASK(m, s)			((m) << (s))
#define	ICE_ARQ_MAX_ELEMS		2048
#define	ICE_RESET_EMPR			3

#include "ice_devids.h"
#include "ice_family_regs.h"

enum ice_mac_type {
	ICE_MAC_UNKNOWN = 0,
	ICE_MAC_VF,
	ICE_MAC_E810,
	ICE_MAC_E830,
	ICE_MAC_GENERIC,
	ICE_MAC_GENERIC_3K,
	ICE_MAC_GENERIC_3K_E825,
};

struct ice_ctl_q_info {
	unsigned pending;
};

struct ice_rq_event_info {
	int unused;
};

struct ice_hw {
	u16 vendor_id, device_id;
	enum ice_mac_type mac_type;
	struct ice_ctl_q_info adminq, sbq;
};

typedef struct ice {
	struct ice_hw ice_hw;
} ice_t;

/* Register and control-queue boundary. */
static u32 rstat;
static unsigned sbq_cleans, other_cleans;

static u32
rd32(struct ice_hw *hw, u32 reg)
{
	(void) hw;
	assert(reg == GLGEN_RSTAT);
	return (rstat);
}

static int
ice_clean_rq_elem(struct ice_hw *hw, struct ice_ctl_q_info *cq,
    struct ice_rq_event_info *evt, u16 *pending)
{
	(void) evt;
	if (cq != &hw->sbq) {
		other_cleans++;
		return (ICE_ERR_AQ_NO_WORK);
	}
	sbq_cleans++;
	if (cq->pending == 0)
		return (ICE_ERR_AQ_NO_WORK);
	*pending = (u16)--cq->pending;
	return (0);
}

static void
ice_error(ice_t *ice, const char *fmt, ...)
{
	(void) ice;
	(void) fmt;
	abort();
}

#include "ice_family_body.h"

static bool
is(const char *want, const char *family)
{
	return (strcmp(want, family) == 0);
}

static void
family_checks(struct ice_hw *hw, const char *want)
{
	ice_t ice;
	struct ice_rq_event_info evt;
	bool slow = is(want, "E825-C") || is(want, "E830");
	bool sbq = is(want, "E822") || is(want, "E823") || is(want, "E825-C");

	/* Only an EMPR is slow, and only on E825-C and E830. */
	rstat = ICE_RESET_EMPR << GLGEN_RSTAT_RESET_TYPE_S;
	assert(ice_reset_empr_slow(hw) == slow);
	rstat = 2u << GLGEN_RSTAT_RESET_TYPE_S;
	assert(!ice_reset_empr_slow(hw));

	/* The sideband receive ring is drained only where it exists. */
	(void) memset(&ice, 0, sizeof (ice));
	ice.ice_hw = *hw;
	ice.ice_hw.sbq.pending = 3;
	sbq_cleans = other_cleans = 0;
	ice_sbq_drain(&ice, &evt);
	assert(other_cleans == 0);
	assert(sbq_cleans == (sbq ? 3u : 0u));
}

static void
check(unsigned id, const char *want)
{
	struct ice_hw hw;
	const char *got;

	(void) memset(&hw, 0, sizeof (hw));
	hw.vendor_id = ICE_INTEL_VENDOR_ID;
	hw.device_id = (u16)id;
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
	family_checks(&hw, want);
}

int
main(int argc, char **argv)
{
	struct ice_hw other;
	int i;

	/* Another vendor never reaches a supported family. */
	(void) memset(&other, 0, sizeof (other));
	other.vendor_id = 0x8087;
	other.device_id = ICE_DEV_ID_E810C_QSFP;
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

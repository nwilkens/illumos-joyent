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
#define	DDI_FM_OK			0
#define	DDI_FM_NONFATAL			1
#define	DDI_SERVICE_LOST		-1
#define	ICE_STATE_ERROR			4
#define	MICROSEC			1000000
#define	MILLISEC			1000
#define	BIT(n)				(1u << (n))
#define	ICE_GET_LINK_STATUS_DATA_V1	1
#define	ICE_GET_LINK_STATUS_DATA_V2	2
#define	ICE_GET_LINK_STATUS_DATALEN_V1	32
#define	ICE_GET_LINK_STATUS_DATALEN_V2	56
#define	SEGMENT_TYPE_ICE_E810		0x00000010
#define	SEGMENT_TYPE_ICE_E830		0x00000017
#define	SEGMENT_SIGN_TYPE_RSA2K		0x00000001
#define	SEGMENT_SIGN_TYPE_RSA3K		0x00000002
#define	SEGMENT_SIGN_TYPE_RSA3K_SBB	0x00000003
#define	SEGMENT_SIGN_TYPE_RSA3K_E825	0x00000005

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
	struct {
		int ios_reg_handle;
	} ice_osdep;
	void *ice_dip;
	uint32_t ice_state;
	int ice_rebuild_lock;
	boolean_t ice_phy_fw_pending;
	boolean_t ice_phy_fw_fault;
} ice_t;

#define	ASSERT(x)	assert(x)
#define	MUTEX_HELD(m)	(*(m) != 0)

static unsigned phy_setups;

static void
ice_phy_setup(ice_t *ice)
{
	assert(!ice->ice_phy_fw_pending);
	phy_setups++;
}

/* Register and control-queue boundary. */
static u32 rstat;
static unsigned sbq_cleans, other_cleans;
static unsigned fw_loading, fw_reads, delays, errors, fw_faults, impacts;
static long delayed_us;

static u32
rd32(struct ice_hw *hw, u32 reg)
{
	(void) hw;
	if (reg == GL_MNG_FWSM) {
		fw_reads++;
		if (fw_loading == 0)
			return (0);
		fw_loading--;
		return (GL_MNG_FWSM_FW_LOADING_M);
	}
	assert(reg == GLGEN_RSTAT);
	return (rstat);
}

static int
ice_check_acc_handle(ice_t *ice, int handle)
{
	(void) ice;
	(void) handle;
	if (fw_faults == 0)
		return (DDI_FM_OK);
	fw_faults--;
	return (DDI_FM_NONFATAL);
}

static void
ddi_fm_service_impact(void *dip, int impact)
{
	(void) dip;
	assert(impact == DDI_SERVICE_LOST);
	impacts++;
}

static void
atomic_or_32(uint32_t *target, uint32_t bits)
{
	*target |= bits;
}

static long
drv_usectohz(long usec)
{
	return (usec);
}

static void
delay(long ticks)
{
	delays++;
	delayed_us += ticks;
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
	errors++;
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
	bool e830 = is(want, "E830");

	/* Only an EMPR is slow, and only on E825-C and E830. */
	rstat = ICE_RESET_EMPR << GLGEN_RSTAT_RESET_TYPE_S;
	assert(ice_reset_empr_slow(hw) == slow);
	rstat = 2u << GLGEN_RSTAT_RESET_TYPE_S;
	assert(!ice_reset_empr_slow(hw));

	/* The sideband receive ring is drained only where it exists. */
	(void) memset(&ice, 0, sizeof (ice));
	ice.ice_hw = *hw;
	ice.ice_hw.sbq.pending = 3;
	sbq_cleans = other_cleans = errors = 0;
	ice_sbq_drain(&ice, &evt);
	assert(other_cleans == 0 && errors == 0);
	assert(sbq_cleans == (sbq ? 3u : 0u));

	/* E830 moved the TCLAN detection registers. */
	assert(ICE_GL_MDET_TX_TCLAN(hw) == (e830 ? 0x000FCCC0u : 0x000FC068u));
	assert(ICE_PF_MDET_TX_TCLAN(hw) == (e830 ? 0x000FCC00u : 0x000FC000u));

	/* Only E830 answers Get Link Status with the longer v2 data. */
	assert(ice_get_link_status_datalen(hw) ==
	    (e830 ? ICE_GET_LINK_STATUS_DATALEN_V2 :
	    ICE_GET_LINK_STATUS_DATALEN_V1));

	/* The core selects the package segment and signature per family. */
	assert(ice_get_pkg_segment_id(hw->mac_type) ==
	    (e830 ? SEGMENT_TYPE_ICE_E830 : SEGMENT_TYPE_ICE_E810));
	assert(ice_get_pkg_sign_type(hw->mac_type) ==
	    (e830 ? SEGMENT_SIGN_TYPE_RSA3K_SBB :
	    is(want, "E825-C") ? SEGMENT_SIGN_TYPE_RSA3K_E825 :
	    SEGMENT_SIGN_TYPE_RSA2K));

	/* Only E830 waits for its PHY firmware, and the wait is bounded. */
	fw_loading = 3;
	fw_reads = delays = errors = 0;
	delayed_us = 0;
	assert(ice_phy_fw_wait(&ice) == ICE_PHY_FW_READY);
	assert(delays == (e830 ? 3u : 0u) && errors == 0);
	assert(fw_reads == (e830 ? 4u : 0u));
	fw_loading = 1000000;
	delays = errors = 0;
	delayed_us = 0;
	if (e830) {
		assert(ice_phy_fw_wait(&ice) == ICE_PHY_FW_LOADING);
		assert(errors == 1);
		assert(delayed_us == (long)ICE_PHY_FW_WAIT_MS * 1000);
	} else {
		assert(ice_phy_fw_wait(&ice) == ICE_PHY_FW_READY);
		assert(delays == 0 && errors == 0);
	}

	/* A faulted read ends the wait and is not a finished load. */
	fw_loading = 2;
	fw_faults = 1;
	delays = errors = 0;
	assert(ice_phy_fw_wait(&ice) ==
	    (e830 ? ICE_PHY_FW_UNREADABLE : ICE_PHY_FW_READY));
	assert(delays == 0 && errors == 0);
	fw_faults = 0;

	/* The admin worker finishes a deferred setup once the load ends. */
	ice.ice_rebuild_lock = 1;
	ice.ice_phy_fw_pending = e830;
	phy_setups = 0;
	fw_loading = 1;
	ice_phy_fw_poll(&ice);
	assert(phy_setups == 0 && ice.ice_phy_fw_pending == e830);
	ice_phy_fw_poll(&ice);
	assert(phy_setups == (e830 ? 1u : 0u) && !ice.ice_phy_fw_pending);
	ice_phy_fw_poll(&ice);
	assert(phy_setups == (e830 ? 1u : 0u));
	fw_loading = 0;

	/*
	 * A register fault keeps the setup pending, fails the datapath closed
	 * and is reported once; the setup runs after a clean read.
	 */
	ice.ice_phy_fw_pending = e830;
	ice.ice_state = 0;
	phy_setups = errors = impacts = 0;
	fw_faults = 2;
	ice_phy_fw_poll(&ice);
	ice_phy_fw_poll(&ice);
	assert(phy_setups == 0 && ice.ice_phy_fw_pending == e830);
	assert(impacts == (e830 ? 1u : 0u) && errors == (e830 ? 1u : 0u));
	assert(ice.ice_state == (e830 ? (uint32_t)ICE_STATE_ERROR : 0u));
	ice_phy_fw_poll(&ice);
	assert(phy_setups == (e830 ? 1u : 0u) && !ice.ice_phy_fw_pending);
	assert(!ice.ice_phy_fw_fault && impacts == (e830 ? 1u : 0u));
	fw_faults = 0;
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

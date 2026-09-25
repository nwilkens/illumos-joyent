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

/* Run firmware recovery-mode detection and its FMA report. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef int boolean_t;

#define	B_TRUE			1
#define	B_FALSE			0
#define	BIT(n)			(1u << (n))
#define	MAKEMASK(m, s)		((m) << (s))
#define	DDI_FM_OK		0
#define	DDI_SERVICE_LOST	1
#define	DDI_NOSLEEP		1
#define	CE_WARN			2
#define	FM_MAX_CLASS		100
#define	FM_ENA_FMT1		1
#define	FM_VERSION		"version"
#define	FM_EREPORT_VERS0	0
#define	DATA_TYPE_UINT8		1
#define	DATA_TYPE_UINT32	2
#define	DATA_TYPE_STRING	3
#define	DDI_FM_DEVICE		"device"
#define	DDI_FM_DEVICE_FW_CORRUPT	"fw_corrupt"
#define	DDI_FM_EREPORT_CAP(c)	(((c) & 1) != 0)

enum ice_mac_type { ICE_MAC_UNKNOWN, ICE_MAC_E810, ICE_MAC_E830 };
enum ice_fw_modes {
	ICE_FW_MODE_NORMAL,
	ICE_FW_MODE_DBG,
	ICE_FW_MODE_REC,
	ICE_FW_MODE_ROLLBACK
};

struct ice_hw {
	enum ice_mac_type mac_type;
};

typedef struct ice {
	struct ice_hw ice_hw;
	int ice_fm_caps;
	void *ice_dip;
	struct {
		int ios_reg_handle;
	} ice_osdep;
} ice_t;

#include "fw_recovery_regs.h"

static u32 fwsm_value;
static int access_fault;
static unsigned ereports, impacts, warnings;
static char last_class[FM_MAX_CLASS];
static u32 last_fwsm;

static u32
rd32(struct ice_hw *hw, u32 reg)
{
	(void) hw;
	assert(reg == GL_MNG_FWSM);
	return (fwsm_value);
}

static int
ice_check_acc_handle(ice_t *ice, int handle)
{
	(void) ice;
	(void) handle;
	return (access_fault ? -1 : DDI_FM_OK);
}

static uint64_t
fm_ena_generate(uint64_t a, int b)
{
	(void) a;
	(void) b;
	return (1);
}

static void
ddi_fm_ereport_post(void *dip, const char *class, uint64_t ena, int flags,
    ...)
{
	va_list ap;
	const char *name;

	(void) dip;
	(void) ena;
	(void) flags;
	(void) snprintf(last_class, sizeof (last_class), "%s", class);
	va_start(ap, flags);
	while ((name = va_arg(ap, const char *)) != NULL) {
		int type = va_arg(ap, int);

		if (type == DATA_TYPE_UINT32) {
			u32 v = va_arg(ap, u32);

			if (strcmp(name, "mng_fwsm") == 0)
				last_fwsm = v;
		} else if (type == DATA_TYPE_STRING) {
			(void) va_arg(ap, const char *);
		} else {
			(void) va_arg(ap, int);
		}
	}
	va_end(ap);
	ereports++;
}

static void
ddi_fm_service_impact(void *dip, int impact)
{
	(void) dip;
	assert(impact == DDI_SERVICE_LOST);
	impacts++;
}

static void
dev_err(void *dip, int level, const char *fmt, ...)
{
	(void) dip;
	assert(level == CE_WARN);
	assert(strstr(fmt, "recovery mode") != NULL);
	assert(strstr(fmt, "Update the NVM") != NULL);
	warnings++;
}

#include "fw_recovery_body.h"

static ice_fw_state_t
state(enum ice_mac_type mac, u32 fwsm)
{
	ice_t ice;
	u32 seen = 0;
	ice_fw_state_t r;

	(void) memset(&ice, 0, sizeof (ice));
	ice.ice_hw.mac_type = mac;
	fwsm_value = fwsm;
	r = ice_fw_state(&ice, &seen);
	assert(seen == fwsm);
	return (r);
}

static boolean_t
detect(enum ice_mac_type mac, u32 fwsm)
{
	ice_fw_state_t r = state(mac, fwsm);

	assert(r != ICE_FW_UNREADABLE);
	return (r == ICE_FW_RECOVERY);
}

int
main(void)
{
	ice_t ice;

	assert(!detect(ICE_MAC_E810, 0));
	assert(detect(ICE_MAC_E810, 2));
	/* The core reports DBG here; the recovery bit still counts. */
	assert(detect(ICE_MAC_E810, 3));
	/* Rollback and debug alone are usable firmware. */
	assert(!detect(ICE_MAC_E810, 4));
	assert(!detect(ICE_MAC_E810, 1));
	/* E830 has a two-bit field; bit 2 is reserved there. */
	assert(detect(ICE_MAC_E830, 2 | 4));
	assert(!detect(ICE_MAC_E830, 4));
	/* Upper bits (for example FW_LOADING) do not select a mode. */
	assert(!detect(ICE_MAC_E810, 0x40000000));
	/* A faulted read (all ones) is neither recovery nor usable. */
	access_fault = 1;
	assert(state(ICE_MAC_E810, 0xffffffff) == ICE_FW_UNREADABLE);
	assert(state(ICE_MAC_E810, 0) == ICE_FW_UNREADABLE);
	access_fault = 0;

	(void) memset(&ice, 0, sizeof (ice));
	ice.ice_fm_caps = 1;
	ice_fw_recovery_report(&ice, 0x2);
	assert(ereports == 1 && impacts == 1 && warnings == 1);
	assert(strcmp(last_class, "device.fw_corrupt") == 0);
	assert(last_fwsm == 0x2);
	/* Without ereport capability the operator still hears about it. */
	ice.ice_fm_caps = 0;
	ice_fw_recovery_report(&ice, 0x2);
	assert(ereports == 1 && impacts == 2 && warnings == 2);

	(void) puts("PASS: firmware recovery mode detection and report");
	return (0);
}

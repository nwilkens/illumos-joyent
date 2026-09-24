/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016, Anish Gupta (anish@freebsd.org)
 * Copyright (c) 2021 The FreeBSD Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
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
/* This file is dual-licensed; see usr/src/contrib/bhyve/LICENSE */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/acpi/acpi.h>

#include "amdvi_priv.h"

/* IVHD types 0x11 and 0x40 append EFR and EFR2 register images. */
#define	IVHD_TYPE10_LEN		sizeof (ACPI_IVRS_HARDWARE)
#define	IVHD_TYPE11_LEN		(sizeof (ACPI_IVRS_HARDWARE) + 16)

/* Device entry types not defined by the illumos ACPICA headers. */
#define	IVHD_DEV_ACPI_HID	0xf0
#define	IVHD_DEV_HID_UIDLEN_OFF	21
#define	IVHD_DEV_HID_LEN	22

static uint_t
ivhd_hdr_len(uint8_t type)
{
	switch (type) {
	case AMDVI_IVHD_TYPE_10:
		return (IVHD_TYPE10_LEN);
	case AMDVI_IVHD_TYPE_11:
	case AMDVI_IVHD_TYPE_40:
		return (IVHD_TYPE11_LEN);
	default:
		return (0);
	}
}

static void
ivhd_set_range(amdvi_devcfg_t *devcfg, uint8_t unit, uint_t start,
    uint_t end, int alias, uint8_t data)
{
	for (uint_t rid = start; rid <= end; rid++) {
		devcfg[rid].adc_unit = unit;
		devcfg[rid].adc_alias = (alias < 0) ? rid : (uint16_t)alias;
		devcfg[rid].adc_data = data;
	}
}

/*
 * Record the requester IDs that an IVHD block places behind its unit.
 */
static int
ivhd_parse_devices(const ACPI_IVRS_HEADER *ivhd, uint8_t unit,
    amdvi_devcfg_t *devcfg)
{
	const uint8_t *p = (const uint8_t *)ivhd + ivhd_hdr_len(ivhd->Type);
	const uint8_t *end = (const uint8_t *)ivhd + ivhd->Length;
	int range_start = -1, range_alias = -1;
	uint8_t range_data = 0;

	while (p < end) {
		const ACPI_IVRS_DE_HEADER *de = (const ACPI_IVRS_DE_HEADER *)p;
		size_t remaining = end - p;
		size_t len;

		if (remaining < sizeof (ACPI_IVRS_DEVICE4))
			goto truncated;

		if (de->Type == IVHD_DEV_ACPI_HID) {
			if (remaining < IVHD_DEV_HID_LEN)
				goto truncated;
			len = IVHD_DEV_HID_LEN + p[IVHD_DEV_HID_UIDLEN_OFF];
		} else if (de->Type < 0x40) {
			len = sizeof (ACPI_IVRS_DEVICE4);
		} else if (de->Type < 0x80) {
			len = sizeof (ACPI_IVRS_DEVICE8A);
		} else {
			cmn_err(CE_WARN, "!amdvi: IVHD %x: unknown device "
			    "entry type 0x%x", ivhd->DeviceId, de->Type);
			return (EINVAL);
		}
		if (len > remaining)
			goto truncated;

		switch (de->Type) {
		case ACPI_IVRS_TYPE_ALL:
			for (uint_t rid = 0; rid < AMDVI_NUM_DEVID; rid++) {
				if (devcfg[rid].adc_unit != AMDVI_UNIT_NONE)
					continue;
				ivhd_set_range(devcfg, unit, rid, rid, -1,
				    de->DataSetting);
			}
			break;
		case ACPI_IVRS_TYPE_SELECT:
		case ACPI_IVRS_TYPE_EXT_SELECT:
			ivhd_set_range(devcfg, unit, de->Id, de->Id, -1,
			    de->DataSetting);
			break;
		case ACPI_IVRS_TYPE_ALIAS_SELECT:
			ivhd_set_range(devcfg, unit, de->Id, de->Id,
			    ((const ACPI_IVRS_DEVICE8A *)de)->UsedId,
			    de->DataSetting);
			break;
		case ACPI_IVRS_TYPE_START:
		case ACPI_IVRS_TYPE_EXT_START:
		case ACPI_IVRS_TYPE_ALIAS_START:
			if (range_start != -1)
				goto bad_range;
			range_start = de->Id;
			range_data = de->DataSetting;
			range_alias = (de->Type == ACPI_IVRS_TYPE_ALIAS_START) ?
			    ((const ACPI_IVRS_DEVICE8A *)de)->UsedId : -1;
			break;
		case ACPI_IVRS_TYPE_END:
			if (range_start == -1 || de->Id < range_start)
				goto bad_range;
			ivhd_set_range(devcfg, unit, range_start, de->Id,
			    range_alias, range_data);
			range_start = range_alias = -1;
			break;
		default:
			/* Padding, IOAPIC/HPET and ACPI HID entries */
			break;
		}
		p += len;
	}

	return (0);

truncated:
	cmn_err(CE_WARN, "!amdvi: IVHD %x: truncated device entry",
	    ivhd->DeviceId);
	return (EINVAL);

bad_range:
	cmn_err(CE_WARN, "!amdvi: IVHD %x: malformed device range",
	    ivhd->DeviceId);
	return (EINVAL);
}

/*
 * Find the IOMMUs described by the IVRS table and which requester IDs each
 * one translates.  Firmware may describe the same unit with IVHD types 0x10,
 * 0x11 and 0x40; the highest type supersedes the others.
 */
int
amdvi_ivrs_parse(amdvi_unit_t *units, uint_t *nunitsp, amdvi_devcfg_t *devcfg)
{
	const ACPI_IVRS_HEADER *ivhds[AMDVI_MAX_UNITS];
	ACPI_TABLE_IVRS *ivrs;
	const uint8_t *p, *end;
	uint_t nunits = 0;
	int err = 0;

	if (ACPI_FAILURE(AcpiGetTable(ACPI_SIG_IVRS, 1,
	    (ACPI_TABLE_HEADER **)&ivrs))) {
		return (ENXIO);
	}
	if (ivrs->Header.Length < sizeof (*ivrs)) {
		err = EINVAL;
		goto out;
	}

	p = (const uint8_t *)ivrs + sizeof (*ivrs);
	end = (const uint8_t *)ivrs + ivrs->Header.Length;
	while (p < end) {
		const ACPI_IVRS_HEADER *sub = (const ACPI_IVRS_HEADER *)p;
		const ACPI_IVRS_HARDWARE *hw = (const ACPI_IVRS_HARDWARE *)p;
		uint_t hdr_len, i;

		if ((size_t)(end - p) < sizeof (*sub) ||
		    sub->Length < sizeof (*sub) || sub->Length > end - p) {
			cmn_err(CE_WARN, "!amdvi: malformed IVRS subtable");
			err = EINVAL;
			goto out;
		}
		p += sub->Length;

		hdr_len = ivhd_hdr_len(sub->Type);
		if (hdr_len == 0)
			continue;
		if (sub->Length < hdr_len) {
			err = EINVAL;
			goto out;
		}
		if (hw->PciSegmentGroup != 0) {
			cmn_err(CE_WARN, "!amdvi: ignoring IOMMU %x on PCI "
			    "segment %u", sub->DeviceId, hw->PciSegmentGroup);
			continue;
		}

		for (i = 0; i < nunits; i++) {
			if (ivhds[i]->DeviceId == sub->DeviceId)
				break;
		}
		if (i == nunits) {
			if (nunits == AMDVI_MAX_UNITS) {
				cmn_err(CE_WARN, "!amdvi: more than %u IOMMUs",
				    AMDVI_MAX_UNITS);
				err = ENOTSUP;
				goto out;
			}
			ivhds[nunits++] = sub;
		} else if (sub->Type > ivhds[i]->Type) {
			ivhds[i] = sub;
		}
	}

	for (uint_t i = 0; i < nunits; i++) {
		const ACPI_IVRS_HARDWARE *hw =
		    (const ACPI_IVRS_HARDWARE *)ivhds[i];
		amdvi_unit_t *u = &units[i];

		if (hw->BaseAddress == 0 ||
		    (hw->BaseAddress & (AMDVI_MMIO_SIZE - 1)) != 0) {
			cmn_err(CE_WARN, "!amdvi: IOMMU %x has invalid MMIO "
			    "base 0x%lx", hw->Header.DeviceId, hw->BaseAddress);
			err = EINVAL;
			goto out;
		}
		u->au_devid = hw->Header.DeviceId;
		u->au_seg = hw->PciSegmentGroup;
		u->au_ivhd_type = hw->Header.Type;
		u->au_ivhd_flags = hw->Header.Flags;
		u->au_mmio_pa = hw->BaseAddress;

		err = ivhd_parse_devices(ivhds[i], i, devcfg);
		if (err != 0)
			goto out;
	}

	if (nunits == 0)
		err = ENXIO;
	*nunitsp = nunits;

out:
	AcpiPutTable((ACPI_TABLE_HEADER *)ivrs);
	return (err);
}

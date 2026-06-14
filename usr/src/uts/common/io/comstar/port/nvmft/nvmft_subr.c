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
 * Provenance: ported to illumos from FreeBSD
 * sys/dev/nvmf/controller/nvmft_subr.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Lower-level controller-specific routines.  These are pure protocol helpers
 * (NQN validation, CAP/CC computation, Identify Controller population) and port
 * with mechanical KPI translation only:
 *
 *   FreeBSD                       illumos
 *   -------                       -------
 *   bool                          boolean_t (B_TRUE/B_FALSE)
 *   htole16/32/64                 LE_16/LE_32/LE_64 (sys/byteorder.h)
 *   NVMEF()/NVMEV() field macros  bitx32/bitset32 not available here; the
 *                                 register fields are written with explicit
 *                                 shifts/masks against nvme_reg.h definitions.
 *   struct nvme_controller_data   nvme_identify_ctrl_t (sys/nvme.h)
 *   PAGE_SIZE / ffs               PAGESIZE / highbit
 *
 * The Identify Controller data structure differs in field naming between
 * FreeBSD's struct nvme_controller_data and illumos's nvme_identify_ctrl_t.
 * The field semantics are identical; see the per-field PORT-TODO notes where a
 * field name could not be matched 1:1 against <sys/nvme.h> in this scaffold.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#ifdef _KERNEL
#include <sys/ddi.h>
#include <sys/sunddi.h>
#else
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#endif

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

#include "nvmft_var.h"

/*
 * NVMe register field helpers.  FreeBSD uses NVMEF(field, v) to shift a value
 * into a register field and NVMEV(field, v) to extract it.  illumos's
 * <sys/nvme.h> exposes the register layouts but not these generic accessors,
 * so we reproduce the two we need locally.  These operate on the *_SHIFT and
 * *_MASK style fields; for the scaffold the relevant CAP/CC fields are encoded
 * with explicit constants matching the NVMe 1.4 base spec.
 */

boolean_t
nvmf_nqn_valid(const char *nqn)
{
	size_t len;

	len = strnlen(nqn, NVMF_NQN_FIELD_SIZE);
	if (len == 0 || len > NVMF_NQN_MAX_LEN)
		return (B_FALSE);
	return (B_TRUE);
}

/*
 * Compute the initial value of the Controller Capabilities (CAP) property.
 *
 * (FreeBSD: _nvmf_controller_cap)
 *
 * PORT-TODO (FreeBSD nvmft_subr.c:_nvmf_controller_cap): the FreeBSD source
 * builds CAP from the NVMEF(NVME_CAP_*) field macros.  This scaffold encodes
 * the same fields using the bit positions from the NVMe 1.4 base spec so it can
 * be reviewed independently of the eventual <sys/nvme.h> field-macro choices.
 * The only runtime-significant fields for a Fabrics controller are TO (timeout),
 * CQR, MQES, MPSMIN/MPSMAX and CSS; everything else is zero.
 */
uint64_t
_nvmf_controller_cap(uint32_t max_io_qsize, uint8_t enable_timeout)
{
	uint32_t caphi, caplo;
	uint_t mps;

	caphi = 0;
	if (max_io_qsize != 0) {
		/*
		 * Memory Page Size min/max are expressed as (log2(size) - 12).
		 * illumos PAGESIZE is 4K on supported platforms, so mps == 0.
		 */
		mps = highbit(PAGESIZE) - 1;
		if (mps < 12)
			mps = 0;
		else
			mps -= 12;
		/* MPSMAX bits 55:52, MPSMIN bits 51:48 of CAP (>>32). */
		caphi |= (mps & 0xf) << 20;	/* MPSMAX */
		caphi |= (mps & 0xf) << 16;	/* MPSMIN */
	}
	/* CSS bit 44 (NVM command set) set in the high dword (>>32). */
	caphi |= (1u << 5);			/* CSS: NVM command set */

	/* CQR bit 16, TO bits 31:24, AMS bits 18:17 of CAP low dword. */
	caplo = (1u << 16);			/* CQR */
	caplo |= ((uint32_t)enable_timeout) << 24;	/* TO */
	if (max_io_qsize != 0)
		caplo |= (max_io_qsize - 1) & 0xffff;	/* MQES */

	return ((uint64_t)caphi << 32 | caplo);
}

/*
 * Validate a proposed new Controller Configuration (CC) value.
 *
 * (FreeBSD: _nvmf_validate_cc)
 *
 * PORT-TODO (FreeBSD nvmft_subr.c:_nvmf_validate_cc): this scaffold checks the
 * IOCQES/IOSQES/SHN/AMS/MPS/CSS fields using explicit shifts.  The Linux
 * initiator writes non-zero IOCQES/IOSQES for discovery controllers, which the
 * FreeBSD source tolerates outside of STRICT_CHECKS; the same leniency is kept
 * here.  Finish wiring these against the canonical <sys/nvme.h> CC field macros
 * once selected.
 */
boolean_t
_nvmf_validate_cc(uint32_t max_io_qsize, uint64_t cap, uint32_t old_cc,
    uint32_t new_cc)
{
	uint32_t caphi, changes, field;

	_NOTE(ARGUNUSED(max_io_qsize));

	changes = old_cc ^ new_cc;

	field = (new_cc >> 20) & 0xf;		/* IOCQES */
	if (field != 0 && field != 4)
		return (B_FALSE);
	field = (new_cc >> 16) & 0xf;		/* IOSQES */
	if (field != 0 && field != 6)
		return (B_FALSE);
	field = (new_cc >> 14) & 0x3;		/* SHN */
	if (field == 3)
		return (B_FALSE);
	field = (new_cc >> 11) & 0x7;		/* AMS */
	if (field != 0)
		return (B_FALSE);

	caphi = cap >> 32;
	field = (new_cc >> 7) & 0xf;		/* MPS */
	if (field < ((caphi >> 20) & 0xf) || field > ((caphi >> 16) & 0xf))
		return (B_FALSE);

	field = (new_cc >> 4) & 0x7;		/* CSS */
	if (field != 0 && field != 0x7)
		return (B_FALSE);

	/* AMS, MPS, and CSS can only change while CC.EN is 0. */
	if ((old_cc & 0x1) != 0 &&
	    (((changes >> 11) & 0x7) != 0 ||
	    ((changes >> 7) & 0xf) != 0 ||
	    ((changes >> 4) & 0x7) != 0))
		return (B_FALSE);

	return (B_TRUE);
}

void
nvmf_controller_serial(char *buf, size_t len, ulong_t hostid)
{
	(void) snprintf(buf, len, "HI:%lu", hostid);
}

/*
 * Copy an ASCII string into dst, padding the tail with spaces and no NUL.
 * (FreeBSD: nvmf_strpad)
 */
void
nvmf_strpad(char *dst, const char *src, size_t len)
{
	while (len > 0 && *src != '\0') {
		*dst++ = *src++;
		len--;
	}
	while (len-- > 0)
		*dst++ = ' ';
}

/*
 * Populate an Identify Controller data structure for an I/O controller.
 *
 * (FreeBSD: _nvmf_init_io_controller_data)
 *
 * The FreeBSD source fills "struct nvme_controller_data"; illumos's
 * nvme_identify_ctrl_t uses different member names for the same fields.  The
 * capability fields (ctratt, frmw, lpa, kas, sqes/cqes, vwc, sgls, ...) are
 * mapped onto their nvme_identify_ctrl_t equivalents below.  oncs/fuses and the
 * firmware-slot page are set by the port-create path (nvmft_stmf.c), and CMIC/
 * ANACAP by nvmft_ana_init_identify().  The IEEE OUI is still the FreeBSD value
 * and must be replaced with an illumos/Oxide OUI.
 */
void
_nvmf_init_io_controller_data(uint16_t cntlid, uint32_t max_io_qsize,
    const char *serial, const char *model, const char *firmware_version,
    const char *subnqn, int nn, uint32_t ioccsz, uint32_t iorcsz,
    nvme_identify_ctrl_t *cdata)
{
	const char *cp;

	(void) bzero(cdata, sizeof (*cdata));

	nvmf_strpad(cdata->id_serial, serial, sizeof (cdata->id_serial));
	nvmf_strpad(cdata->id_model, model, sizeof (cdata->id_model));
	nvmf_strpad(cdata->id_fwrev, firmware_version,
	    sizeof (cdata->id_fwrev));

	/*
	 * FreeBSD truncates the firmware revision at the first '-' (a build
	 * string with a dash is space-padded from the dash to the end of the
	 * field) so the reported FR matches the reference Identify Controller
	 * output.  (FreeBSD: memchr(cdata->fr, '-', ...); memset(cp, ' ', ...).)
	 * illumos memchr() returns const, so locate the dash then space-pad the
	 * (non-const) field from that offset to the end.
	 */
	cp = memchr(cdata->id_fwrev, '-', sizeof (cdata->id_fwrev));
	if (cp != NULL) {
		size_t off = (size_t)(cp - cdata->id_fwrev);

		(void) memset(cdata->id_fwrev + off, ' ',
		    sizeof (cdata->id_fwrev) - off);
	}

	/* OUI: FreeBSD value; replace with assigned OUI. */
	cdata->id_oui[0] = 0xfc;
	cdata->id_oui[1] = 0x9c;
	cdata->id_oui[2] = 0x58;

	cdata->id_cntlid = LE_16(cntlid);
	cdata->id_ver = LE_32(NVMFT_VER_1_4);

	/*
	 * CTRATT: advertise the 128-bit Host Identifier and Traffic-Based Keep
	 * Alive support.  (FreeBSD: NVME_CTRLR_DATA_CTRATT_128BIT_HOSTID |
	 * _TBKAS.)
	 */
	cdata->id_ctratt.ctrat_hid = 1;
	cdata->id_ctratt.ctrat_tbkas = 1;

	cdata->id_cntrltype = NVME_CNTRLTYPE_IO;	/* I/O controller */
	cdata->id_acl = 3;				/* Abort Command Limit */
	cdata->id_aerl = 3;				/* AER Limit */

	/* 1 read-only firmware slot. */
	cdata->id_frmw.fw_readonly = 1;
	cdata->id_frmw.fw_nslot = 1;

	/* Extended Get Log Page data supported. */
	cdata->id_lpa.lp_extsup = 1;

	/* Single power state. */
	cdata->id_npss = 0;

	/*
	 * NVMe 1.2+ require non-zero warning/critical composite temperatures
	 * even though they make no sense for Fabrics.  (FreeBSD: 0x0157.)
	 */
	cdata->ap_wctemp = LE_16(0x0157);
	cdata->ap_cctemp = cdata->ap_wctemp;

	/* 1 second granularity for Keep Alive (10 * 100ms). */
	cdata->ap_kas = LE_16(10);

	/* SQ/CQ entry sizes: NVM command set fixes both at min == max. */
	cdata->id_sqes.qes_min = 6;
	cdata->id_sqes.qes_max = 6;
	cdata->id_cqes.qes_min = 4;
	cdata->id_cqes.qes_max = 4;

	cdata->id_nn = LE_32((uint32_t)nn);
	cdata->id_maxcmd = LE_16((uint16_t)max_io_qsize);

	/* Volatile write cache present. */
	cdata->id_vwc.vwc_present = 1;

	/*
	 * SGL support: transport data block, address-as-offset, NVM command
	 * set.  (FreeBSD: NVME_CTRLR_DATA_SGLS_TRANSPORT_DATA_BLOCK |
	 * _ADDRESS_AS_OFFSET | _NVM_COMMAND_SET.)
	 */
	cdata->id_sgls.sgl_sup = NVME_SGL_SUP_UNALIGN;	/* NVM command set */
	cdata->id_sgls.sgl_offset = 1;			/* address as offset */
	cdata->id_sgls.sgl_tport = 1;			/* transport data block */

	(void) strlcpy((char *)cdata->id_subnqn, subnqn,
	    sizeof (cdata->id_subnqn));

	/*
	 * IOCCSZ/IORCSZ are expressed in 16-byte units and live in the NVMe-oF
	 * transport-specific region (id_nvmof) of Identify Controller.  Per the
	 * spec layout that region begins at byte 1792:
	 *   ioccsz @ 1792 (uint32), iorcsz @ 1796 (uint32),
	 *   icdoff @ 1800 (uint16), fcatt @ 1802 (uint8), msdbd @ 1803 (uint8).
	 * <sys/nvme.h> exposes this as the opaque id_nvmof[] byte array, so the
	 * fields are written by offset.  (FreeBSD: cdata->ioccsz/iorcsz/icdoff/
	 * fcatt/msdbd.)
	 */
	{
		uint32_t le_ioccsz = LE_32(ioccsz / 16);
		uint32_t le_iorcsz = LE_32(iorcsz / 16);

		(void) bcopy(&le_ioccsz, &cdata->id_nvmof[0],
		    sizeof (le_ioccsz));
		(void) bcopy(&le_iorcsz, &cdata->id_nvmof[4],
		    sizeof (le_iorcsz));
		/* icdoff @ +8 = 0, fcatt @ +10 = 0 (already zeroed). */
		cdata->id_nvmof[11] = 1;		/* msdbd = 1 */
	}
}

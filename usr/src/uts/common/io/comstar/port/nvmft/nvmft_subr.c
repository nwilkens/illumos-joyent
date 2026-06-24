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
 *   NVMEF()/NVMEV() field macros  nvme_reg_cap_t / nvme_reg_cc_t bitfield
 *                                 unions (io/nvme/nvme_reg.h)
 *   struct nvme_controller_data   nvme_identify_ctrl_t (sys/nvme.h)
 *   PAGE_SIZE / ffs               PAGESIZE / highbit
 *
 * The Identify Controller data structure differs in field naming between
 * FreeBSD's struct nvme_controller_data and illumos's nvme_identify_ctrl_t, but
 * the field semantics are identical.
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
 * Compute the initial value of the Controller Capabilities (CAP) property.  The
 * only runtime-significant fields for a Fabrics controller are TO (timeout), CQR,
 * MQES, MPSMIN/MPSMAX and CSS; everything else is zero.
 *
 * (FreeBSD: _nvmf_controller_cap)
 */
uint64_t
_nvmf_controller_cap(uint32_t max_io_qsize, uint8_t enable_timeout)
{
	nvme_reg_cap_t cap;
	uint_t mps;

	cap.r = 0;
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
		cap.b.cap_mpsmax = mps & 0xf;
		cap.b.cap_mpsmin = mps & 0xf;
		cap.b.cap_mqes = (uint16_t)(max_io_qsize - 1);
	}
	cap.b.cap_css = NVME_CAP_CSS_NVM;
	cap.b.cap_cqr = 1;
	cap.b.cap_to = enable_timeout;

	return (cap.r);
}

/*
 * Validate a proposed new Controller Configuration (CC) value.
 *
 * The Linux initiator writes non-zero IOCQES/IOSQES for discovery controllers,
 * which FreeBSD tolerates outside of STRICT_CHECKS; the same leniency is kept
 * here (IOCQES/IOSQES of 0 are accepted alongside the spec values).
 *
 * (FreeBSD: _nvmf_validate_cc)
 */
boolean_t
_nvmf_validate_cc(uint32_t max_io_qsize, uint64_t cap, uint32_t old_cc,
    uint32_t new_cc)
{
	nvme_reg_cc_t oldcc, newcc, changes;
	nvme_reg_cap_t capr;

	_NOTE(ARGUNUSED(max_io_qsize));

	oldcc.r = old_cc;
	newcc.r = new_cc;
	changes.r = old_cc ^ new_cc;
	capr.r = cap;

	if (newcc.b.cc_iocqes != 0 && newcc.b.cc_iocqes != 4)
		return (B_FALSE);
	if (newcc.b.cc_iosqes != 0 && newcc.b.cc_iosqes != 6)
		return (B_FALSE);
	if (newcc.b.cc_shn == 3)
		return (B_FALSE);
	if (newcc.b.cc_ams != 0)
		return (B_FALSE);

	if (newcc.b.cc_mps < capr.b.cap_mpsmin ||
	    newcc.b.cc_mps > capr.b.cap_mpsmax)
		return (B_FALSE);

	if (newcc.b.cc_css != 0 && newcc.b.cc_css != 0x7)
		return (B_FALSE);

	/* AMS, MPS, and CSS can only change while CC.EN is 0. */
	if (oldcc.b.cc_en != 0 &&
	    (changes.b.cc_ams != 0 || changes.b.cc_mps != 0 ||
	    changes.b.cc_css != 0))
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

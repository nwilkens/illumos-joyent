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
 * Lower-level controller-specific protocol helpers (NQN validation, CAP/CC
 * computation, Identify Controller population) shared between the kernel and
 * userland.  This is the userland twin of the in-kernel port at
 * uts/common/io/comstar/port/nvmft/nvmft_subr.c; the logic is identical and the
 * Identify Controller layout is filled against illumos's nvme_identify_ctrl_t
 * (whose member names differ from FreeBSD's struct nvme_controller_data but
 * whose field semantics match).  Keeping a userland copy avoids dragging the
 * kernel-only nvmft_var.h dependencies (ddi, STMF) into the library.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

#include "internal.h"

/* (kernel nvmft_var.h: NVMFT_VER_1_4) Identify Controller version, NVMe 1.4. */
#define	NVMF_VER_1_4		0x00010400u

bool
nvmf_nqn_valid(const char *nqn)
{
	size_t len;

	len = strnlen(nqn, NVMF_NQN_FIELD_SIZE);
	if (len == 0 || len > NVMF_NQN_MAX_LEN)
		return (false);
	return (true);
}

/*
 * Compute the initial value of the Controller Capabilities (CAP) property.
 * (FreeBSD: _nvmf_controller_cap)
 *
 * The runtime-significant fields for a Fabrics controller are TO (timeout),
 * CQR, MQES, MPSMIN/MPSMAX and CSS; everything else is zero.  illumos PAGESIZE
 * is 4K on supported platforms, so the memory-page-size field is zero.
 */
/* Userland highbit(): position (1-indexed) of the most significant set bit. */
static uint_t
nvmf_highbit(uint64_t v)
{
	return (v == 0 ? 0 : 64 - (uint_t)__builtin_clzll(v));
}

uint64_t
_nvmf_controller_cap(uint32_t max_io_qsize, uint8_t enable_timeout)
{
	uint32_t caphi, caplo;
	uint_t mps;

	caphi = 0;
	if (max_io_qsize != 0) {
		/* MPS min/max are expressed as (log2(size) - 12). */
		mps = nvmf_highbit(PAGESIZE) - 1;
		if (mps < 12)
			mps = 0;
		else
			mps -= 12;
		caphi |= (mps & 0xf) << 20;	/* MPSMAX (CAP bits 55:52) */
		caphi |= (mps & 0xf) << 16;	/* MPSMIN (CAP bits 51:48) */
	}
	caphi |= (1u << 5);			/* CSS: NVM command set (bit 44) */

	caplo = (1u << 16);			/* CQR (bit 16) */
	caplo |= ((uint32_t)enable_timeout) << 24;	/* TO (bits 31:24) */
	if (max_io_qsize != 0)
		caplo |= (max_io_qsize - 1) & 0xffff;	/* MQES (bits 15:0) */

	return ((uint64_t)caphi << 32 | caplo);
}

/*
 * Validate a proposed new Controller Configuration (CC) value.
 * (FreeBSD: _nvmf_validate_cc)
 *
 * The Linux initiator writes non-zero IOCQES/IOSQES when connecting to a
 * discovery controller, which FreeBSD tolerates outside of STRICT_CHECKS; the
 * same leniency is kept here.
 */
bool
_nvmf_validate_cc(uint32_t max_io_qsize, uint64_t cap, uint32_t old_cc,
    uint32_t new_cc)
{
	uint32_t caphi, changes, field;

	(void) max_io_qsize;

	changes = old_cc ^ new_cc;

	field = (new_cc >> 20) & 0xf;		/* IOCQES */
	if (field != 0 && field != 4)
		return (false);
	field = (new_cc >> 16) & 0xf;		/* IOSQES */
	if (field != 0 && field != 6)
		return (false);
	field = (new_cc >> 14) & 0x3;		/* SHN */
	if (field == 3)
		return (false);
	field = (new_cc >> 11) & 0x7;		/* AMS */
	if (field != 0)
		return (false);

	caphi = cap >> 32;
	field = (new_cc >> 7) & 0xf;		/* MPS */
	if (field < ((caphi >> 20) & 0xf) || field > ((caphi >> 16) & 0xf))
		return (false);

	field = (new_cc >> 4) & 0x7;		/* CSS */
	if (field != 0 && field != 0x7)
		return (false);

	/* AMS, MPS, and CSS can only change while CC.EN is 0. */
	if ((old_cc & 0x1) != 0 &&
	    (((changes >> 11) & 0x7) != 0 ||
	    ((changes >> 7) & 0xf) != 0 ||
	    ((changes >> 4) & 0x7) != 0))
		return (false);

	return (true);
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
 * (FreeBSD: _nvmf_init_io_controller_data)
 *
 * The FreeBSD source fills "struct nvme_controller_data"; illumos's
 * nvme_identify_ctrl_t uses different member names for the same fields.  The
 * IOCCSZ/IORCSZ/ICDOFF/FCATT/MSDBD fields live in the NVMe-oF transport-specific
 * region of Identify Controller, which <sys/nvme.h> exposes as the opaque
 * id_nvmof[] byte array, so those are written by spec offset.
 */
void
_nvmf_init_io_controller_data(uint16_t cntlid, uint32_t max_io_qsize,
    const char *serial, const char *model, const char *firmware_version,
    const char *subnqn, int nn, uint32_t ioccsz, uint32_t iorcsz,
    nvme_identify_ctrl_t *cdata)
{
	char *cp;
	uint32_t le_ioccsz, le_iorcsz;

	(void) memset(cdata, 0, sizeof (*cdata));

	nvmf_strpad(cdata->id_serial, serial, sizeof (cdata->id_serial));
	nvmf_strpad(cdata->id_model, model, sizeof (cdata->id_model));
	nvmf_strpad(cdata->id_fwrev, firmware_version,
	    sizeof (cdata->id_fwrev));

	/*
	 * FreeBSD truncates the firmware revision at the first '-' so the
	 * reported FR matches the reference Identify Controller output.
	 */
	cp = memchr(cdata->id_fwrev, '-', sizeof (cdata->id_fwrev));
	if (cp != NULL) {
		size_t off = (size_t)(cp - cdata->id_fwrev);

		(void) memset(cdata->id_fwrev + off, ' ',
		    sizeof (cdata->id_fwrev) - off);
	}

	/* OUI: FreeBSD value; replace with an assigned OUI. */
	cdata->id_oui[0] = 0xfc;
	cdata->id_oui[1] = 0x9c;
	cdata->id_oui[2] = 0x58;

	cdata->id_cntlid = LE_16(cntlid);
	cdata->id_ver = LE_32(NVMF_VER_1_4);

	/* CTRATT: 128-bit Host Identifier and Traffic-Based Keep Alive. */
	cdata->id_ctratt.ctrat_hid = 1;
	cdata->id_ctratt.ctrat_tbkas = 1;

	cdata->id_cntrltype = NVME_CNTRLTYPE_IO;
	cdata->id_acl = 3;			/* Abort Command Limit */
	cdata->id_aerl = 3;			/* AER Limit */

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

	/* SGL: transport data block, address-as-offset, NVM command set. */
	cdata->id_sgls.sgl_sup = NVME_SGL_SUP_UNALIGN;
	cdata->id_sgls.sgl_offset = 1;
	cdata->id_sgls.sgl_tport = 1;

	(void) strlcpy((char *)cdata->id_subnqn, subnqn,
	    sizeof (cdata->id_subnqn));

	/*
	 * NVMe-oF transport-specific region (id_nvmof) begins at byte 1792:
	 *   ioccsz @ +0 (u32), iorcsz @ +4 (u32), icdoff @ +8 (u16),
	 *   fcatt @ +10 (u8), msdbd @ +11 (u8).  IOCCSZ/IORCSZ are 16-byte units.
	 */
	le_ioccsz = LE_32(ioccsz / 16);
	le_iorcsz = LE_32(iorcsz / 16);
	(void) memcpy(&cdata->id_nvmof[0], &le_ioccsz, sizeof (le_ioccsz));
	(void) memcpy(&cdata->id_nvmof[4], &le_iorcsz, sizeof (le_iorcsz));
	/* icdoff @ +8 = 0, fcatt @ +10 = 0 (already zeroed). */
	cdata->id_nvmof[11] = 1;		/* msdbd = 1 */
}

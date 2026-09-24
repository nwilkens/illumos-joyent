/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016 Anish Gupta (anish@freebsd.org)
 * Copyright (c) 2021 The FreeBSD Foundation
 *
 * Portions of this software were developed by Ka Ho Ng
 * under sponsorship from the FreeBSD Foundation.
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

#ifndef _AMDVI_PRIV_H_
#define	_AMDVI_PRIV_H_

#include <sys/types.h>
#include <sys/list.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Definitions from the "AMD I/O Virtualization Technology (IOMMU)
 * Specification", publication 48882.
 */

/* MMIO registers */
#define	AMDVI_MMIO_SIZE		0x4000
#define	AMDVI_REG_DEVTAB_BASE	0x0000
#define	AMDVI_REG_CMDBUF_BASE	0x0008
#define	AMDVI_REG_EVLOG_BASE	0x0010
#define	AMDVI_REG_CTRL		0x0018
#define	AMDVI_REG_EXCL_BASE	0x0020
#define	AMDVI_REG_EXCL_LIMIT	0x0028
#define	AMDVI_REG_EFR		0x0030
#define	AMDVI_REG_CMDBUF_HEAD	0x2000
#define	AMDVI_REG_CMDBUF_TAIL	0x2008
#define	AMDVI_REG_EVLOG_HEAD	0x2010
#define	AMDVI_REG_EVLOG_TAIL	0x2018
#define	AMDVI_REG_STATUS	0x2020

#define	AMDVI_CTRL_EN		(1ULL << 0)
#define	AMDVI_CTRL_HTTUN_EN	(1ULL << 1)
#define	AMDVI_CTRL_EVLOG_EN	(1ULL << 2)
#define	AMDVI_CTRL_EVINT_EN	(1ULL << 3)
#define	AMDVI_CTRL_COMWINT_EN	(1ULL << 4)
#define	AMDVI_CTRL_INVTO_MASK	(7ULL << 5)
#define	AMDVI_CTRL_INVTO_1S	(4ULL << 5)
#define	AMDVI_CTRL_PASSPW	(1ULL << 8)
#define	AMDVI_CTRL_RESPASSPW	(1ULL << 9)
#define	AMDVI_CTRL_COHERENT	(1ULL << 10)
#define	AMDVI_CTRL_ISOC		(1ULL << 11)
#define	AMDVI_CTRL_CMDBUF_EN	(1ULL << 12)
#define	AMDVI_CTRL_DEVTABSEG	(7ULL << 34)

#define	AMDVI_EFR_IASUP		(1ULL << 6)
#define	AMDVI_EFR_HATS(efr)	(((efr) >> 10) & 0x3)

#define	AMDVI_STATUS_EVOVRFLW	(1ULL << 0)
#define	AMDVI_STATUS_EVLOGRUN	(1ULL << 3)
#define	AMDVI_STATUS_CMDBUFRUN	(1ULL << 4)

/* The ring pointer registers hold byte offsets in bits 18:4. */
#define	AMDVI_RING_PTR_MASK	0x7fff0ULL
#define	AMDVI_RING_LEN_SHIFT	56

/*
 * The command buffer and the event log each use a single page, which holds
 * 256 entries of 16 bytes.  Their base registers encode that as log2(256).
 */
#define	AMDVI_PAGE_SIZE		4096
#define	AMDVI_RING_SIZE		4096
#define	AMDVI_RING_ENTRY_SIZE	16
#define	AMDVI_RING_LEN_256	8ULL

/* Device table */
#define	AMDVI_NUM_DEVID		0x10000
#define	AMDVI_DTE_SIZE		32
#define	AMDVI_DEVTAB_SIZE	(AMDVI_NUM_DEVID * AMDVI_DTE_SIZE)

#define	AMDVI_DTE0_V		(1ULL << 0)
#define	AMDVI_DTE0_TV		(1ULL << 1)
#define	AMDVI_DTE0_MODE_SHIFT	9
#define	AMDVI_DTE0_IR		(1ULL << 61)
#define	AMDVI_DTE0_IW		(1ULL << 62)
#define	AMDVI_DTE1_SE		(1ULL << 33)
#define	AMDVI_DTE1_SYSMGT_SHIFT	40
#define	AMDVI_DTE2_INITPASS	(1ULL << 56)
#define	AMDVI_DTE2_EINTPASS	(1ULL << 57)
#define	AMDVI_DTE2_NMIPASS	(1ULL << 58)
#define	AMDVI_DTE2_LINT0PASS	(1ULL << 62)
#define	AMDVI_DTE2_LINT1PASS	(1ULL << 63)

/* Page table entries */
#define	AMDVI_PTE_PR		(1ULL << 0)
#define	AMDVI_PTE_NL_SHIFT	9
#define	AMDVI_PTE_NL_MASK	(7ULL << AMDVI_PTE_NL_SHIFT)
#define	AMDVI_PTE_PA_MASK	0x000ffffffffff000ULL
#define	AMDVI_PA_MAX		0x000fffffffffffffULL
#define	AMDVI_PTE_FC		(1ULL << 60)
#define	AMDVI_PTE_IR		(1ULL << 61)
#define	AMDVI_PTE_IW		(1ULL << 62)

#define	AMDVI_PT_SHIFT		9
#define	AMDVI_PT_ENTRIES	(1 << AMDVI_PT_SHIFT)
#define	AMDVI_MAX_LEVELS	6

/* Commands */
#define	AMDVI_CMD_OP_SHIFT		60
#define	AMDVI_CMD_COMPLETION_WAIT	0x1ULL
#define	AMDVI_CMD_INV_DEVTAB_ENTRY	0x2ULL
#define	AMDVI_CMD_INV_IOMMU_PAGES	0x3ULL
#define	AMDVI_CMD_INV_ALL		0x8ULL

#define	AMDVI_CMD_CW_STORE		(1ULL << 0)
#define	AMDVI_CMD_CW_ADDR_MASK		0x000ffffffffffff8ULL
#define	AMDVI_CMD_INVP_S		(1ULL << 0)
#define	AMDVI_CMD_INVP_PDE		(1ULL << 1)
#define	AMDVI_CMD_INVP_ALL_ADDR		0x7ffffffffffff000ULL

/* Event log entries */
#define	AMDVI_EV_CODE(e)	((uint_t)((e) >> 60))
#define	AMDVI_EV_DEVID(e)	((uint16_t)(e))
#define	AMDVI_EV_DOMID(e)	((uint16_t)((e) >> 32))
#define	AMDVI_EV_FLAGS(e)	((uint_t)(((e) >> 48) & 0xfff))

#define	AMDVI_EV_ILL_DEV_TABLE_ENTRY	0x1
#define	AMDVI_EV_IO_PAGE_FAULT		0x2
#define	AMDVI_EV_DEV_TAB_HW_ERROR	0x3
#define	AMDVI_EV_PAGE_TAB_HW_ERROR	0x4
#define	AMDVI_EV_ILL_CMD_ERROR		0x5
#define	AMDVI_EV_CMD_HW_ERROR		0x6
#define	AMDVI_EV_IOTLB_INV_TIMEOUT	0x7
#define	AMDVI_EV_INVALID_DEV_REQ	0x8

/* IVRS */
#define	AMDVI_IVHD_TYPE_10	0x10
#define	AMDVI_IVHD_TYPE_11	0x11
#define	AMDVI_IVHD_TYPE_40	0x40

#define	AMDVI_IVHD_FLAG_HTTUN	(1U << 0)
#define	AMDVI_IVHD_FLAG_PASSPW	(1U << 1)
#define	AMDVI_IVHD_FLAG_RESPASSPW (1U << 2)
#define	AMDVI_IVHD_FLAG_ISOC	(1U << 3)

#define	AMDVI_MAX_UNITS		32
#define	AMDVI_UNIT_NONE		0xff

/*
 * What the IVRS table says about a single requester ID: which unit
 * translates it, the requester ID the unit will actually see (differs from
 * the device's own for devices behind a PCIe-to-PCI bridge), and the IVHD
 * DataSetting byte.
 */
typedef struct amdvi_devcfg {
	uint16_t	adc_alias;
	uint8_t		adc_unit;
	uint8_t		adc_data;
} amdvi_devcfg_t;

typedef struct amdvi_unit {
	uint16_t	au_devid;
	uint16_t	au_seg;
	uint8_t		au_ivhd_type;
	uint8_t		au_ivhd_flags;
	uint64_t	au_mmio_pa;

	caddr_t		au_regs;
	uint64_t	au_efr;
	uint64_t	au_ctrl;
	bool		au_enabled;

	uint64_t	*au_cmdbuf;
	uint64_t	au_cmdbuf_pa;
	uint32_t	au_cmd_tail;
	bool		au_cmd_err;

	uint64_t	*au_evlog;
	uint64_t	au_evlog_pa;

	volatile uint64_t au_cw_store;
	uint64_t	au_cw_seq;
} amdvi_unit_t;

typedef struct amdvi_domain {
	list_node_t	ad_node;
	uint64_t	*ad_root;
	uint64_t	ad_root_pa;
	uint_t		ad_levels;
	uint16_t	ad_id;
	bool		ad_host;
} amdvi_domain_t;

/* ivrs_drv.c */
int amdvi_ivrs_parse(amdvi_unit_t *, uint_t *, amdvi_devcfg_t *);

/* amdvi_hw.c */
int amdvi_hw_init(void);
void amdvi_hw_fini(void);
void amdvi_hw_enable(void);
void amdvi_hw_disable(void);
uint_t amdvi_hw_max_levels(void);
int amdvi_hw_attach(const amdvi_domain_t *, uint16_t);
void amdvi_hw_detach(const amdvi_domain_t *, uint16_t);
bool amdvi_hw_inv_domain(const amdvi_domain_t *);

#ifdef __cplusplus
}
#endif

#endif /* _AMDVI_PRIV_H_ */

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016, Anish Gupta (anish@freebsd.org)
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

/*
 * AMD-Vi hardware support: unit bring-up, the command buffer, the device
 * table and the event log.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/atomic.h>
#include <sys/mman.h>
#include <sys/acpi/acpi.h>

#include <dev/pci/pcireg.h>

#include <machine/vmparam.h>
#include <sys/vmm_vm.h>

#include "amdvi_priv.h"

/* Reach into i86pc/os for these */
extern void *contig_alloc(size_t, ddi_dma_attr_t *, uintptr_t, int);
extern void contig_free(void *, size_t);
extern caddr_t psm_map_phys_new(paddr_t, size_t, int);
extern void psm_unmap_phys(caddr_t, size_t);

#define	AMDVI_CMD_TIMEOUT	NANOSEC
#define	AMDVI_EV_REPORT_MAX	16

/*
 * amdvi_hw_lock covers the device table, the command buffers and the
 * enabled state of every unit.
 */
static kmutex_t		amdvi_hw_lock;
static amdvi_unit_t	amdvi_units[AMDVI_MAX_UNITS];
static uint_t		amdvi_nunits;
static amdvi_devcfg_t	*amdvi_devcfg;
static uint64_t		*amdvi_devtab;
static uint64_t		amdvi_devtab_pa;
static ddi_periodic_t	amdvi_evpoll;

static ddi_dma_attr_t amdvi_dma_attr = {
	.dma_attr_version	= DMA_ATTR_V0,
	.dma_attr_addr_lo	= 0,
	.dma_attr_addr_hi	= AMDVI_PA_MAX,
	.dma_attr_count_max	= 0xffffffffULL,
	.dma_attr_align		= AMDVI_PAGE_SIZE,
	.dma_attr_burstsizes	= 1,
	.dma_attr_minxfer	= 1,
	.dma_attr_maxxfer	= 0xffffffffULL,
	.dma_attr_seg		= 0xffffffffffffffffULL,
	.dma_attr_sgllen	= 1,
	.dma_attr_granular	= 1,
	.dma_attr_flags		= 0,
};

static inline uint64_t
amdvi_read(const amdvi_unit_t *u, uint_t off)
{
	return (*(volatile uint64_t *)(u->au_regs + off));
}

static inline void
amdvi_write(const amdvi_unit_t *u, uint_t off, uint64_t val)
{
	*(volatile uint64_t *)(u->au_regs + off) = val;
}

static void *
amdvi_contig_alloc(size_t len, uint64_t *pap)
{
	void *va = contig_alloc(len, &amdvi_dma_attr, MMU_PAGESIZE, 1);

	if (va != NULL) {
		bzero(va, len);
		*pap = vtophys(va);
	}
	return (va);
}

static bool
amdvi_cmd_submit(amdvi_unit_t *u, uint64_t c0, uint64_t c1)
{
	const hrtime_t deadline = gethrtime() + AMDVI_CMD_TIMEOUT;
	uint_t idx;

	ASSERT(MUTEX_HELD(&amdvi_hw_lock));
	ASSERT(u->au_enabled);

	for (;;) {
		uint64_t head = amdvi_read(u, AMDVI_REG_CMDBUF_HEAD) &
		    AMDVI_RING_PTR_MASK;
		uint64_t used = (u->au_cmd_tail - head) &
		    (AMDVI_RING_SIZE - 1);

		if (AMDVI_RING_SIZE - used > AMDVI_RING_ENTRY_SIZE)
			break;
		if (gethrtime() > deadline) {
			cmn_err(CE_WARN, "amdvi: IOMMU %x: command buffer "
			    "stalled", u->au_devid);
			u->au_cmd_err = true;
			return (false);
		}
		drv_usecwait(1);
	}

	idx = u->au_cmd_tail / sizeof (uint64_t);
	u->au_cmdbuf[idx] = c0;
	u->au_cmdbuf[idx + 1] = c1;
	u->au_cmd_tail = (u->au_cmd_tail + AMDVI_RING_ENTRY_SIZE) &
	    (AMDVI_RING_SIZE - 1);
	membar_producer();
	amdvi_write(u, AMDVI_REG_CMDBUF_TAIL, u->au_cmd_tail);

	return (true);
}

/*
 * Wait for the unit to finish every command queued since the last sync.
 * Fails if any of them could not be queued.
 */
static bool
amdvi_cmd_sync(amdvi_unit_t *u)
{
	const hrtime_t deadline = gethrtime() + AMDVI_CMD_TIMEOUT;
	const uint64_t seq = ++u->au_cw_seq;
	const uint64_t pa = vtophys((void *)&u->au_cw_store);
	const bool queued = !u->au_cmd_err;

	u->au_cmd_err = false;
	if (!amdvi_cmd_submit(u, (pa & AMDVI_CMD_CW_ADDR_MASK) |
	    AMDVI_CMD_CW_STORE |
	    (AMDVI_CMD_COMPLETION_WAIT << AMDVI_CMD_OP_SHIFT), seq)) {
		u->au_cmd_err = false;
		return (false);
	}

	while (u->au_cw_store != seq) {
		if (gethrtime() > deadline) {
			cmn_err(CE_WARN, "amdvi: IOMMU %x: command completion "
			    "timed out, status 0x%lx", u->au_devid,
			    amdvi_read(u, AMDVI_REG_STATUS));
			return (false);
		}
		drv_usecwait(1);
	}

	return (queued);
}

static void
amdvi_cmd_inv_dte(amdvi_unit_t *u, uint16_t rid)
{
	(void) amdvi_cmd_submit(u, rid |
	    (AMDVI_CMD_INV_DEVTAB_ENTRY << AMDVI_CMD_OP_SHIFT), 0);
}

static void
amdvi_cmd_inv_pages(amdvi_unit_t *u, uint16_t domid)
{
	(void) amdvi_cmd_submit(u, ((uint64_t)domid << 32) |
	    (AMDVI_CMD_INV_IOMMU_PAGES << AMDVI_CMD_OP_SHIFT),
	    AMDVI_CMD_INVP_ALL_ADDR | AMDVI_CMD_INVP_PDE | AMDVI_CMD_INVP_S);
}

/*
 * Wait for the unit to stop using a ring after its enable bit is cleared.
 */
static void
amdvi_ring_wait_stopped(const amdvi_unit_t *u, uint64_t run_bit)
{
	const hrtime_t deadline = gethrtime() + AMDVI_CMD_TIMEOUT;

	while ((amdvi_read(u, AMDVI_REG_STATUS) & run_bit) != 0) {
		if (gethrtime() > deadline) {
			cmn_err(CE_WARN, "amdvi: IOMMU %x: ring did not stop, "
			    "status 0x%lx", u->au_devid,
			    amdvi_read(u, AMDVI_REG_STATUS));
			return;
		}
		drv_usecwait(1);
	}
}

static const char *
amdvi_ev_name(uint_t code)
{
	switch (code) {
	case AMDVI_EV_ILL_DEV_TABLE_ENTRY:
		return ("illegal device table entry");
	case AMDVI_EV_IO_PAGE_FAULT:
		return ("I/O page fault");
	case AMDVI_EV_DEV_TAB_HW_ERROR:
		return ("device table hardware error");
	case AMDVI_EV_PAGE_TAB_HW_ERROR:
		return ("page table hardware error");
	case AMDVI_EV_ILL_CMD_ERROR:
		return ("illegal command");
	case AMDVI_EV_CMD_HW_ERROR:
		return ("command hardware error");
	case AMDVI_EV_IOTLB_INV_TIMEOUT:
		return ("IOTLB invalidation timeout");
	case AMDVI_EV_INVALID_DEV_REQ:
		return ("invalid device request");
	default:
		return ("unknown event");
	}
}

static void
amdvi_evlog_drain(amdvi_unit_t *u)
{
	uint64_t head, tail, status;
	uint_t reported = 0, dropped = 0;

	ASSERT(MUTEX_HELD(&amdvi_hw_lock));

	status = amdvi_read(u, AMDVI_REG_STATUS);
	head = amdvi_read(u, AMDVI_REG_EVLOG_HEAD) & AMDVI_RING_PTR_MASK;
	tail = amdvi_read(u, AMDVI_REG_EVLOG_TAIL) & AMDVI_RING_PTR_MASK;

	while (head != tail) {
		volatile uint64_t *ev = &u->au_evlog[head / sizeof (uint64_t)];

		/*
		 * The unit may advance the tail pointer before the entry
		 * itself is visible.
		 */
		for (uint_t i = 0; ev[0] == 0 && i < 100; i++)
			drv_usecwait(1);

		if (ev[0] != 0 && reported < AMDVI_EV_REPORT_MAX) {
			const uint16_t devid = AMDVI_EV_DEVID(ev[0]);

			cmn_err(CE_WARN, "!amdvi: IOMMU %x: %s, device "
			    "%02x:%02x.%x domain %u address 0x%lx flags 0x%x",
			    u->au_devid, amdvi_ev_name(AMDVI_EV_CODE(ev[0])),
			    PCI_RID2BUS(devid), PCI_RID2SLOT(devid),
			    PCI_RID2FUNC(devid), AMDVI_EV_DOMID(ev[0]), ev[1],
			    AMDVI_EV_FLAGS(ev[0]));
			reported++;
		} else {
			dropped++;
		}
		ev[0] = 0;
		ev[1] = 0;
		head = (head + AMDVI_RING_ENTRY_SIZE) & (AMDVI_RING_SIZE - 1);
	}
	amdvi_write(u, AMDVI_REG_EVLOG_HEAD, head);

	if (dropped != 0) {
		cmn_err(CE_WARN, "!amdvi: IOMMU %x: %u more events not "
		    "reported", u->au_devid, dropped);
	}

	/* Logging stops on overflow and must be restarted. */
	if ((status & AMDVI_STATUS_EVOVRFLW) != 0) {
		cmn_err(CE_WARN, "!amdvi: IOMMU %x: event log overflow",
		    u->au_devid);
		amdvi_write(u, AMDVI_REG_CTRL, u->au_ctrl &
		    ~AMDVI_CTRL_EVLOG_EN);
		amdvi_ring_wait_stopped(u, AMDVI_STATUS_EVLOGRUN);
		amdvi_write(u, AMDVI_REG_STATUS, AMDVI_STATUS_EVOVRFLW);
		bzero(u->au_evlog, AMDVI_RING_SIZE);
		amdvi_write(u, AMDVI_REG_EVLOG_HEAD, 0);
		amdvi_write(u, AMDVI_REG_EVLOG_TAIL, 0);
		amdvi_write(u, AMDVI_REG_CTRL, u->au_ctrl);
	}
}

static void
amdvi_evlog_poll(void *arg __unused)
{
	mutex_enter(&amdvi_hw_lock);
	for (uint_t i = 0; i < amdvi_nunits; i++) {
		if (amdvi_units[i].au_enabled)
			amdvi_evlog_drain(&amdvi_units[i]);
	}
	mutex_exit(&amdvi_hw_lock);
}

/*
 * Replace a device table entry and wait for the unit to drop its cached
 * copy.  The unit may fetch the entry at any time, so a translating entry is
 * first made to block DMA.  That keeps a fetch from pairing one domain's ID
 * with another's page table.
 */
static bool
amdvi_dte_write(amdvi_unit_t *u, uint16_t rid, const uint64_t dte[4])
{
	volatile uint64_t *cur = &amdvi_devtab[(uint_t)rid * 4];

	ASSERT(MUTEX_HELD(&amdvi_hw_lock));

	if ((cur[0] & AMDVI_DTE0_TV) != 0)
		cur[0] = AMDVI_DTE0_V;
	membar_producer();
	cur[3] = dte[3];
	cur[2] = dte[2];
	cur[1] = dte[1];
	membar_producer();
	cur[0] = dte[0];

	if (!u->au_enabled)
		return (true);
	amdvi_cmd_inv_dte(u, rid);
	return (amdvi_cmd_sync(u));
}

static void
amdvi_bdf_str(uint16_t rid, char *buf, size_t len)
{
	(void) snprintf(buf, len, "%02x:%02x.%x", PCI_RID2BUS(rid),
	    PCI_RID2SLOT(rid), PCI_RID2FUNC(rid));
}

int
amdvi_hw_attach(const amdvi_domain_t *dom, uint16_t rid)
{
	const amdvi_devcfg_t *cfg = &amdvi_devcfg[rid];
	const uint8_t data = cfg->adc_data;
	uint64_t dte[4];
	char bdf[16];
	bool ok;

	amdvi_bdf_str(rid, bdf, sizeof (bdf));
	if (cfg->adc_unit == AMDVI_UNIT_NONE) {
		if (dom->ad_host)
			return (0);
		cmn_err(CE_WARN, "amdvi: device %s is not behind an IOMMU",
		    bdf);
		return (ENXIO);
	}

	/*
	 * Devices behind a PCIe-to-PCI bridge share the bridge's requester
	 * ID, so none of them can be isolated from the others.
	 */
	if (!dom->ad_host && cfg->adc_alias != rid) {
		cmn_err(CE_WARN, "amdvi: device %s shares requester ID "
		    "%02x:%02x.%x with other devices", bdf,
		    PCI_RID2BUS(cfg->adc_alias), PCI_RID2SLOT(cfg->adc_alias),
		    PCI_RID2FUNC(cfg->adc_alias));
		return (ENOTSUP);
	}

	dte[0] = AMDVI_DTE0_V | AMDVI_DTE0_TV | AMDVI_DTE0_IR |
	    AMDVI_DTE0_IW | dom->ad_root_pa |
	    ((uint64_t)dom->ad_levels << AMDVI_DTE0_MODE_SHIFT);
	dte[1] = dom->ad_id | AMDVI_DTE1_SE |
	    ((uint64_t)((data & ACPI_IVHD_SYSTEM_MGMT) >> 4) <<
	    AMDVI_DTE1_SYSMGT_SHIFT);
	dte[2] = 0;
	if ((data & ACPI_IVHD_INIT_PASS) != 0)
		dte[2] |= AMDVI_DTE2_INITPASS;
	if ((data & ACPI_IVHD_EINT_PASS) != 0)
		dte[2] |= AMDVI_DTE2_EINTPASS;
	if ((data & ACPI_IVHD_NMI_PASS) != 0)
		dte[2] |= AMDVI_DTE2_NMIPASS;
	if ((data & ACPI_IVHD_LINT0_PASS) != 0)
		dte[2] |= AMDVI_DTE2_LINT0PASS;
	if ((data & ACPI_IVHD_LINT1_PASS) != 0)
		dte[2] |= AMDVI_DTE2_LINT1PASS;
	dte[3] = 0;

	mutex_enter(&amdvi_hw_lock);
	ok = amdvi_dte_write(&amdvi_units[cfg->adc_unit], rid, dte);
	if (ok && cfg->adc_alias != rid) {
		ok = amdvi_dte_write(&amdvi_units[cfg->adc_unit],
		    cfg->adc_alias, dte);
	}
	mutex_exit(&amdvi_hw_lock);

	return (ok ? 0 : EIO);
}

/*
 * Leave the device with a valid entry that blocks its DMA, since an invalid
 * entry would let it through untranslated, and drop whatever the unit has
 * cached for the domain it leaves.  A shared requester ID is left alone; it
 * only ever belongs to the host domain.
 */
void
amdvi_hw_detach(const amdvi_domain_t *dom, uint16_t rid)
{
	const amdvi_devcfg_t *cfg = &amdvi_devcfg[rid];
	const uint64_t dte[4] = { AMDVI_DTE0_V, 0, 0, 0 };
	amdvi_unit_t *u;

	if (cfg->adc_unit == AMDVI_UNIT_NONE)
		return;
	u = &amdvi_units[cfg->adc_unit];

	mutex_enter(&amdvi_hw_lock);
	(void) amdvi_dte_write(u, rid, dte);
	if (u->au_enabled) {
		amdvi_cmd_inv_pages(u, dom->ad_id);
		(void) amdvi_cmd_sync(u);
	}
	mutex_exit(&amdvi_hw_lock);
}

bool
amdvi_hw_inv_domain(const amdvi_domain_t *dom)
{
	bool ok = true;

	mutex_enter(&amdvi_hw_lock);
	for (uint_t i = 0; i < amdvi_nunits; i++) {
		amdvi_unit_t *u = &amdvi_units[i];

		if (!u->au_enabled)
			continue;
		amdvi_cmd_inv_pages(u, dom->ad_id);
		if (!amdvi_cmd_sync(u))
			ok = false;
	}
	mutex_exit(&amdvi_hw_lock);

	return (ok);
}

uint_t
amdvi_hw_max_levels(void)
{
	uint_t levels = AMDVI_MAX_LEVELS;

	for (uint_t i = 0; i < amdvi_nunits; i++) {
		uint_t hats = AMDVI_EFR_HATS(amdvi_units[i].au_efr);

		/* HATS 3 is reserved; assume only the minimum of 4 levels. */
		levels = MIN(levels, 4 + (hats == 3 ? 0 : hats));
	}
	return (levels);
}

/*
 * Discard everything the unit may have cached, including entries left over
 * from before this module programmed it.
 */
static void
amdvi_flush_all(amdvi_unit_t *u, uint_t idx)
{
	if ((u->au_efr & AMDVI_EFR_IASUP) != 0) {
		(void) amdvi_cmd_submit(u,
		    AMDVI_CMD_INV_ALL << AMDVI_CMD_OP_SHIFT, 0);
	} else {
		for (uint_t rid = 0; rid < AMDVI_NUM_DEVID; rid++) {
			const amdvi_devcfg_t *cfg = &amdvi_devcfg[rid];

			if (cfg->adc_unit != idx)
				continue;
			amdvi_cmd_inv_dte(u, rid);
			if (cfg->adc_alias != rid)
				amdvi_cmd_inv_dte(u, cfg->adc_alias);
		}
	}
	(void) amdvi_cmd_sync(u);
}

void
amdvi_hw_enable(void)
{
	mutex_enter(&amdvi_hw_lock);
	for (uint_t i = 0; i < amdvi_nunits; i++) {
		amdvi_unit_t *u = &amdvi_units[i];
		const uint8_t flags = u->au_ivhd_flags;

		VERIFY(!u->au_enabled);

		amdvi_write(u, AMDVI_REG_CMDBUF_HEAD, 0);
		amdvi_write(u, AMDVI_REG_CMDBUF_TAIL, 0);
		amdvi_write(u, AMDVI_REG_EVLOG_HEAD, 0);
		amdvi_write(u, AMDVI_REG_EVLOG_TAIL, 0);
		u->au_cmd_tail = 0;

		u->au_ctrl |= AMDVI_CTRL_EN | AMDVI_CTRL_EVLOG_EN |
		    AMDVI_CTRL_CMDBUF_EN | AMDVI_CTRL_COHERENT |
		    AMDVI_CTRL_INVTO_1S;
		if ((flags & AMDVI_IVHD_FLAG_HTTUN) != 0)
			u->au_ctrl |= AMDVI_CTRL_HTTUN_EN;
		if ((flags & AMDVI_IVHD_FLAG_PASSPW) != 0)
			u->au_ctrl |= AMDVI_CTRL_PASSPW;
		if ((flags & AMDVI_IVHD_FLAG_RESPASSPW) != 0)
			u->au_ctrl |= AMDVI_CTRL_RESPASSPW;
		if ((flags & AMDVI_IVHD_FLAG_ISOC) != 0)
			u->au_ctrl |= AMDVI_CTRL_ISOC;
		amdvi_write(u, AMDVI_REG_CTRL, u->au_ctrl);
		u->au_enabled = true;

		amdvi_flush_all(u, i);
	}
	mutex_exit(&amdvi_hw_lock);

	amdvi_evpoll = ddi_periodic_add(amdvi_evlog_poll, NULL, NANOSEC,
	    DDI_IPL_0);
}

void
amdvi_hw_disable(void)
{
	if (amdvi_evpoll != NULL) {
		ddi_periodic_delete(amdvi_evpoll);
		amdvi_evpoll = NULL;
	}

	mutex_enter(&amdvi_hw_lock);
	for (uint_t i = 0; i < amdvi_nunits; i++) {
		amdvi_unit_t *u = &amdvi_units[i];

		if (!u->au_enabled)
			continue;
		amdvi_evlog_drain(u);
		u->au_ctrl &= ~(AMDVI_CTRL_EN | AMDVI_CTRL_EVLOG_EN |
		    AMDVI_CTRL_CMDBUF_EN);
		amdvi_write(u, AMDVI_REG_CTRL, u->au_ctrl);
		amdvi_ring_wait_stopped(u,
		    AMDVI_STATUS_CMDBUFRUN | AMDVI_STATUS_EVLOGRUN);
		u->au_enabled = false;
	}
	mutex_exit(&amdvi_hw_lock);
}

static int
amdvi_unit_init(amdvi_unit_t *u)
{
	uint64_t ctrl;

	u->au_regs = psm_map_phys_new(u->au_mmio_pa, AMDVI_MMIO_SIZE,
	    PROT_READ | PROT_WRITE);
	if (u->au_regs == NULL)
		return (ENOMEM);

	ctrl = amdvi_read(u, AMDVI_REG_CTRL);
	if ((ctrl & AMDVI_CTRL_EN) != 0) {
		cmn_err(CE_WARN, "amdvi: IOMMU %x is already enabled",
		    u->au_devid);
		return (EBUSY);
	}
	u->au_ctrl = ctrl & ~(AMDVI_CTRL_EN | AMDVI_CTRL_EVLOG_EN |
	    AMDVI_CTRL_EVINT_EN | AMDVI_CTRL_COMWINT_EN |
	    AMDVI_CTRL_CMDBUF_EN | AMDVI_CTRL_INVTO_MASK |
	    AMDVI_CTRL_DEVTABSEG);
	u->au_efr = amdvi_read(u, AMDVI_REG_EFR);

	/* Firmware may leave an exclusion range that bypasses translation. */
	amdvi_write(u, AMDVI_REG_EXCL_BASE, 0);
	amdvi_write(u, AMDVI_REG_EXCL_LIMIT, 0);

	u->au_cmdbuf = amdvi_contig_alloc(AMDVI_RING_SIZE, &u->au_cmdbuf_pa);
	u->au_evlog = amdvi_contig_alloc(AMDVI_RING_SIZE, &u->au_evlog_pa);
	if (u->au_cmdbuf == NULL || u->au_evlog == NULL)
		return (ENOMEM);

	amdvi_write(u, AMDVI_REG_DEVTAB_BASE, amdvi_devtab_pa |
	    (AMDVI_DEVTAB_SIZE / MMU_PAGESIZE - 1));
	amdvi_write(u, AMDVI_REG_CMDBUF_BASE, u->au_cmdbuf_pa |
	    (AMDVI_RING_LEN_256 << AMDVI_RING_LEN_SHIFT));
	amdvi_write(u, AMDVI_REG_EVLOG_BASE, u->au_evlog_pa |
	    (AMDVI_RING_LEN_256 << AMDVI_RING_LEN_SHIFT));

	return (0);
}

static void
amdvi_unit_fini(amdvi_unit_t *u)
{
	ASSERT(!u->au_enabled);

	if (u->au_regs != NULL && u->au_cmdbuf != NULL) {
		amdvi_write(u, AMDVI_REG_DEVTAB_BASE, 0);
		amdvi_write(u, AMDVI_REG_CMDBUF_BASE, 0);
		amdvi_write(u, AMDVI_REG_EVLOG_BASE, 0);
	}
	if (u->au_cmdbuf != NULL)
		contig_free(u->au_cmdbuf, AMDVI_RING_SIZE);
	if (u->au_evlog != NULL)
		contig_free(u->au_evlog, AMDVI_RING_SIZE);
	if (u->au_regs != NULL)
		psm_unmap_phys(u->au_regs, AMDVI_MMIO_SIZE);
	bzero(u, sizeof (*u));
}

int
amdvi_hw_init(void)
{
	int err;

	VERIFY3U(amdvi_nunits, ==, 0);

	amdvi_devcfg = kmem_alloc(AMDVI_NUM_DEVID * sizeof (amdvi_devcfg_t),
	    KM_SLEEP);
	for (uint_t rid = 0; rid < AMDVI_NUM_DEVID; rid++) {
		amdvi_devcfg[rid].adc_alias = rid;
		amdvi_devcfg[rid].adc_unit = AMDVI_UNIT_NONE;
		amdvi_devcfg[rid].adc_data = 0;
	}

	err = amdvi_ivrs_parse(amdvi_units, &amdvi_nunits, amdvi_devcfg);
	if (err != 0)
		goto fail;

	amdvi_devtab = amdvi_contig_alloc(AMDVI_DEVTAB_SIZE, &amdvi_devtab_pa);
	if (amdvi_devtab == NULL) {
		err = ENOMEM;
		goto fail;
	}

	for (uint_t i = 0; i < amdvi_nunits; i++) {
		err = amdvi_unit_init(&amdvi_units[i]);
		if (err != 0)
			goto fail;
	}

	return (0);

fail:
	amdvi_hw_fini();
	return (err);
}

void
amdvi_hw_fini(void)
{
	VERIFY3P(amdvi_evpoll, ==, NULL);

	for (uint_t i = 0; i < AMDVI_MAX_UNITS; i++)
		amdvi_unit_fini(&amdvi_units[i]);
	amdvi_nunits = 0;

	if (amdvi_devtab != NULL) {
		contig_free(amdvi_devtab, AMDVI_DEVTAB_SIZE);
		amdvi_devtab = NULL;
	}
	if (amdvi_devcfg != NULL) {
		kmem_free(amdvi_devcfg,
		    AMDVI_NUM_DEVID * sizeof (amdvi_devcfg_t));
		amdvi_devcfg = NULL;
	}
}

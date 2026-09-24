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
 * AMD-Vi backend for bhyve PCI passthrough, loaded by io/iommu.c as
 * misc/vmm_amdvi.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/errno.h>
#include <sys/id_space.h>
#include <sys/list.h>
#include <sys/modctl.h>
#include <sys/sysmacros.h>

#include <machine/vmparam.h>
#include <sys/vmm_vm.h>

#include "io/iommu.h"
#include "amdvi_priv.h"

/* Domain ID 0 is left unused, as Linux does. */
#define	AMDVI_DOMID_MIN		1
#define	AMDVI_DOMID_LIMIT	0x10000

/* Leaf entries map at most 1 GiB. */
#define	AMDVI_MAX_LEAF_LEVEL	3

#define	AMDVI_LVL_SHIFT(l)	(MMU_PAGESHIFT + AMDVI_PT_SHIFT * ((l) - 1))
#define	AMDVI_LVL_SIZE(l)	(1ULL << AMDVI_LVL_SHIFT(l))
#define	AMDVI_PTE_IDX(gpa, l)	\
	(((gpa) >> AMDVI_LVL_SHIFT(l)) & (AMDVI_PT_ENTRIES - 1))

/*
 * amdvi_lock protects the domain lists and amdvi_expect_host.  Domains whose
 * invalidation failed are kept on amdvi_quarantine until the unit is off.
 */
static kmutex_t		amdvi_lock;
static list_t		amdvi_domains;
static list_t		amdvi_quarantine;
static id_space_t	*amdvi_domids;
static uint_t		amdvi_levels_max;
static bool		amdvi_expect_host;

static uint64_t
amdvi_domain_limit(uint_t levels)
{
	const uint_t shift = AMDVI_LVL_SHIFT(levels + 1);

	return (shift >= 64 ? UINT64_MAX : (1ULL << shift));
}

static uint64_t *
amdvi_pte_next(uint64_t pte)
{
	return ((uint64_t *)PHYS_TO_DMAP(pte & AMDVI_PTE_PA_MASK));
}

static bool
amdvi_pte_is_leaf(uint64_t pte)
{
	return ((pte & AMDVI_PTE_NL_MASK) == 0);
}

static bool
amdvi_pte_is_table(uint64_t pte)
{
	return ((pte & AMDVI_PTE_PR) != 0 && !amdvi_pte_is_leaf(pte));
}

static void
amdvi_free_pt(uint64_t *pt, uint_t lvl)
{
	if (lvl > 1) {
		for (uint_t i = 0; i < AMDVI_PT_ENTRIES; i++) {
			const uint64_t pte = pt[i];

			if (amdvi_pte_is_table(pte))
				amdvi_free_pt(amdvi_pte_next(pte), lvl - 1);
		}
	}
	vmm_ptp_free(pt);
}

static void
amdvi_domain_free(amdvi_domain_t *dom)
{
	amdvi_free_pt(dom->ad_root, dom->ad_levels);
	kmem_free(dom, sizeof (*dom));
}

static int
amdvi_init(void)
{
	int err;

	err = amdvi_hw_init();
	if (err != 0)
		return (err);

	amdvi_levels_max = amdvi_hw_max_levels();
	amdvi_domids = id_space_create("amdvi_domid", AMDVI_DOMID_MIN,
	    AMDVI_DOMID_LIMIT);
	list_create(&amdvi_domains, sizeof (amdvi_domain_t),
	    offsetof(amdvi_domain_t, ad_node));
	list_create(&amdvi_quarantine, sizeof (amdvi_domain_t),
	    offsetof(amdvi_domain_t, ad_node));

	/* io/iommu.c creates the host domain right after init. */
	amdvi_expect_host = true;

	return (0);
}

static void
amdvi_cleanup(void)
{
	amdvi_domain_t *dom;

	amdvi_hw_fini();

	if (amdvi_domids != NULL) {
		VERIFY(list_is_empty(&amdvi_domains));
		while ((dom = list_remove_head(&amdvi_quarantine)) != NULL) {
			id_free(amdvi_domids, dom->ad_id);
			amdvi_domain_free(dom);
		}
		list_destroy(&amdvi_domains);
		list_destroy(&amdvi_quarantine);
		id_space_destroy(amdvi_domids);
		amdvi_domids = NULL;
	}
}

static void
amdvi_enable(void)
{
	amdvi_domain_t *dom;

	amdvi_hw_enable();

	mutex_enter(&amdvi_lock);
	for (dom = list_head(&amdvi_domains); dom != NULL;
	    dom = list_next(&amdvi_domains, dom)) {
		(void) amdvi_hw_inv_domain(dom);
	}
	mutex_exit(&amdvi_lock);
}

static void
amdvi_disable(void)
{
	amdvi_hw_disable();
}

static void *
amdvi_create_domain(vm_paddr_t maxaddr)
{
	amdvi_domain_t *dom;
	uint_t levels;
	id_t id;

	for (levels = 1; levels <= amdvi_levels_max; levels++) {
		if (maxaddr <= amdvi_domain_limit(levels))
			break;
	}
	if (levels > amdvi_levels_max) {
		cmn_err(CE_WARN, "amdvi: cannot map 0x%lx bytes with %u "
		    "page table levels", maxaddr, amdvi_levels_max);
		return (NULL);
	}

	id = id_alloc_nosleep(amdvi_domids);
	if (id == -1)
		return (NULL);

	dom = kmem_zalloc(sizeof (*dom), KM_SLEEP);
	dom->ad_id = (uint16_t)id;
	dom->ad_levels = levels;
	dom->ad_root = vmm_ptp_alloc();
	dom->ad_root_pa = vtophys(dom->ad_root);

	/*
	 * The ID may have been used before; drop anything cached for it.  On
	 * failure the ID stays allocated so it is not handed out again.
	 */
	if (!amdvi_hw_inv_domain(dom)) {
		amdvi_domain_free(dom);
		return (NULL);
	}

	mutex_enter(&amdvi_lock);
	dom->ad_host = amdvi_expect_host;
	amdvi_expect_host = false;
	list_insert_tail(&amdvi_domains, dom);
	mutex_exit(&amdvi_lock);

	return (dom);
}

static void
amdvi_destroy_domain(void *arg)
{
	amdvi_domain_t *dom = arg;

	const bool flushed = amdvi_hw_inv_domain(dom);

	mutex_enter(&amdvi_lock);
	list_remove(&amdvi_domains, dom);
	if (!flushed) {
		/* The unit may still walk these page tables. */
		cmn_err(CE_WARN, "amdvi: quarantining domain %u after failed "
		    "invalidation", dom->ad_id);
		list_insert_tail(&amdvi_quarantine, dom);
		mutex_exit(&amdvi_lock);
		return;
	}
	mutex_exit(&amdvi_lock);

	id_free(amdvi_domids, dom->ad_id);
	amdvi_domain_free(dom);
}

static void
amdvi_check_range(const amdvi_domain_t *dom, vm_paddr_t gpa, uint64_t len)
{
	VERIFY0(gpa & MMU_PAGEOFFSET);
	VERIFY0(len & MMU_PAGEOFFSET);
	VERIFY3U(len, >, 0);
	VERIFY3U(gpa + len, >, gpa);
	VERIFY3U(gpa + len, <=, amdvi_domain_limit(dom->ad_levels));
}

/*
 * Map the largest naturally aligned block at gpa that fits in len and
 * return its size.
 */
static uint64_t
amdvi_create_mapping(void *arg, vm_paddr_t gpa, vm_paddr_t hpa, uint64_t len)
{
	amdvi_domain_t *dom = arg;
	uint64_t *pt = dom->ad_root;
	uint_t leaf = 1, lvl;
	uint64_t *pte;

	amdvi_check_range(dom, gpa, len);
	VERIFY0(hpa & MMU_PAGEOFFSET);

	for (lvl = MIN(AMDVI_MAX_LEAF_LEVEL, dom->ad_levels - 1); lvl > 1;
	    lvl--) {
		const uint64_t sz = AMDVI_LVL_SIZE(lvl);

		if (((gpa | hpa) & (sz - 1)) == 0 && len >= sz) {
			leaf = lvl;
			break;
		}
	}

	for (lvl = dom->ad_levels; lvl > leaf; lvl--) {
		pte = &pt[AMDVI_PTE_IDX(gpa, lvl)];

		if ((*pte & AMDVI_PTE_PR) == 0) {
			uint64_t *next = vmm_ptp_alloc();

			*pte = vtophys(next) |
			    ((uint64_t)(lvl - 1) << AMDVI_PTE_NL_SHIFT) |
			    AMDVI_PTE_IR | AMDVI_PTE_IW | AMDVI_PTE_PR;
		} else if (amdvi_pte_is_leaf(*pte)) {
			panic("amdvi: mapping 0x%lx overlaps a large page",
			    gpa);
		}
		pt = amdvi_pte_next(*pte);
	}

	pte = &pt[AMDVI_PTE_IDX(gpa, leaf)];
	if (amdvi_pte_is_table(*pte))
		panic("amdvi: large page 0x%lx overlaps a page table", gpa);
	*pte = hpa | AMDVI_PTE_FC | AMDVI_PTE_IR | AMDVI_PTE_IW |
	    AMDVI_PTE_PR;

	return (AMDVI_LVL_SIZE(leaf));
}

/*
 * Remove the mapping at gpa and return how much of the range it covered,
 * or the distance to the next possible mapping if nothing is mapped there.
 */
static uint64_t
amdvi_remove_mapping(void *arg, vm_paddr_t gpa, uint64_t len)
{
	amdvi_domain_t *dom = arg;
	uint64_t *pt = dom->ad_root;

	amdvi_check_range(dom, gpa, len);

	for (uint_t lvl = dom->ad_levels; ; lvl--) {
		const uint64_t sz = AMDVI_LVL_SIZE(lvl);
		uint64_t *pte = &pt[AMDVI_PTE_IDX(gpa, lvl)];

		if ((*pte & AMDVI_PTE_PR) == 0)
			return (MIN(sz - (gpa & (sz - 1)), len));

		if (lvl == 1 || amdvi_pte_is_leaf(*pte)) {
			if ((gpa & (sz - 1)) != 0 || len < sz) {
				panic("amdvi: partial unmap of large page "
				    "0x%lx", gpa);
			}
			*pte = 0;
			return (sz);
		}
		pt = amdvi_pte_next(*pte);
	}
}

static int
amdvi_add_device(void *arg, uint16_t rid)
{
	return (amdvi_hw_attach(arg, rid));
}

static void
amdvi_remove_device(void *arg, uint16_t rid)
{
	amdvi_hw_detach(arg, rid);
}

static void
amdvi_invalidate_tlb(void *arg)
{
	(void) amdvi_hw_inv_domain(arg);
}

const struct iommu_ops vmm_iommu_ops = {
	.init = amdvi_init,
	.cleanup = amdvi_cleanup,
	.enable = amdvi_enable,
	.disable = amdvi_disable,
	.create_domain = amdvi_create_domain,
	.destroy_domain = amdvi_destroy_domain,
	.create_mapping = amdvi_create_mapping,
	.remove_mapping = amdvi_remove_mapping,
	.add_device = amdvi_add_device,
	.remove_device = amdvi_remove_device,
	.invalidate_tlb = amdvi_invalidate_tlb,
};

static struct modlmisc modlmisc = {
	&mod_miscops,
	"bhyve vmm AMD-Vi",
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modlmisc,
	NULL
};

int
_init(void)
{
	return (mod_install(&modlinkage));
}

int
_fini(void)
{
	return (mod_remove(&modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

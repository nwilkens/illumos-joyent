/*
 * Copyright (c) 2009-2010 Chelsio, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Protection domains, fast registration MRs, the TPT and PBL memory of the
 * adapter, and DMA buffers.  The TPT entry layout and the MR life cycle
 * follow Linux cxgb4/mem.c and resource.c under the OpenIB license.
 *
 * Kernel QPs run with STAG0 enabled, so the local DMA lkey is STag 0 and no
 * TPT entry exists for it.  Every other STag is a fast registration MR: its
 * TPT entry is written invalid at allocation and made valid by a REG_MR
 * work request on a QP of the same PD.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/bitmap.h>
#include <sys/vmem.h>
#include <sys/atomic.h>

#include "iwc.h"

#define	IWC_TPT_ENTRY	32

/* An arena over [start, start + size); vmem cannot hand out address 0. */
static vmem_t *
iwc_arena(const char *name, uint32_t start, uint32_t size, uint_t shift)
{
	const size_t q = (size_t)1 << shift;
	const uint32_t first = P2ROUNDUP(start, (uint32_t)q);
	size_t len;

	if (first - start >= size)
		return (NULL);
	len = P2ALIGN((size_t)(size - (first - start)), q);
	if (len == 0)
		return (NULL);
	return (vmem_create(name, (void *)(uintptr_t)q, len, q, NULL, NULL,
	    NULL, 0, VM_SLEEP));
}

int
iwc_mem_init(iwc_t *iwc)
{
	const t4_rdma_vres_t *vr = &iwc->iwc_info.tri_vres;
	char name[32];
	int inst = ddi_get_instance(iwc->iwc_dip);

	iwc->iwc_nstag = MIN(vr->trv_stag.trr_size / IWC_TPT_ENTRY,
	    1U << 24);
	iwc->iwc_stag_map = kmem_zalloc(BT_SIZEOFMAP(iwc->iwc_nstag),
	    KM_SLEEP);
	BT_SET(iwc->iwc_stag_map, 0);
	iwc->iwc_pdid_map = kmem_zalloc(BT_SIZEOFMAP(IWC_MAX_PD), KM_SLEEP);
	BT_SET(iwc->iwc_pdid_map, 0);

	(void) snprintf(name, sizeof (name), "iwc%d_pbl", inst);
	iwc->iwc_pbl_arena = iwc_arena(name, vr->trv_pbl.trr_start,
	    vr->trv_pbl.trr_size, IWC_MIN_PBL_SHIFT);
	(void) snprintf(name, sizeof (name), "iwc%d_rqt", inst);
	iwc->iwc_rqt_arena = iwc_arena(name, vr->trv_rq.trr_start,
	    vr->trv_rq.trr_size, IWC_MIN_RQT_SHIFT);
	if (iwc->iwc_pbl_arena == NULL || iwc->iwc_rqt_arena == NULL)
		return (ENOMEM);
	return (0);
}

void
iwc_mem_fini(iwc_t *iwc)
{
	if (iwc->iwc_pbl_arena != NULL) {
		vmem_destroy(iwc->iwc_pbl_arena);
		iwc->iwc_pbl_arena = NULL;
	}
	if (iwc->iwc_rqt_arena != NULL) {
		vmem_destroy(iwc->iwc_rqt_arena);
		iwc->iwc_rqt_arena = NULL;
	}
	if (iwc->iwc_stag_map != NULL) {
		kmem_free(iwc->iwc_stag_map, BT_SIZEOFMAP(iwc->iwc_nstag));
		iwc->iwc_stag_map = NULL;
	}
	if (iwc->iwc_pdid_map != NULL) {
		kmem_free(iwc->iwc_pdid_map, BT_SIZEOFMAP(IWC_MAX_PD));
		iwc->iwc_pdid_map = NULL;
	}
}

/* Adapter memory from an arena, as an absolute address; 0 on failure. */
static uint32_t
iwc_arena_alloc(vmem_t *vm, uint32_t start, uint_t shift, size_t len)
{
	const size_t q = (size_t)1 << shift;
	void *a;

	if ((a = vmem_alloc(vm, P2ROUNDUP(len, q), VM_NOSLEEP)) == NULL)
		return (0);
	return (P2ROUNDUP(start, (uint32_t)q) + (uint32_t)((uintptr_t)a - q));
}

static void
iwc_arena_free(vmem_t *vm, uint32_t start, uint_t shift, uint32_t addr,
    size_t len)
{
	const size_t q = (size_t)1 << shift;

	vmem_free(vm, (void *)(uintptr_t)(addr - P2ROUNDUP(start,
	    (uint32_t)q) + q), P2ROUNDUP(len, q));
}

uint32_t
iwc_rqt_alloc(iwc_t *iwc, uint32_t entries)
{
	return (iwc_arena_alloc(iwc->iwc_rqt_arena,
	    iwc->iwc_info.tri_vres.trv_rq.trr_start, IWC_MIN_RQT_SHIFT,
	    (size_t)entries << T4_RQT_ENTRY_SHIFT));
}

void
iwc_rqt_free(iwc_t *iwc, uint32_t addr, uint32_t entries)
{
	iwc_arena_free(iwc->iwc_rqt_arena,
	    iwc->iwc_info.tri_vres.trv_rq.trr_start, IWC_MIN_RQT_SHIFT, addr,
	    (size_t)entries << T4_RQT_ENTRY_SHIFT);
}

static int
iwc_bit_alloc(iwc_t *iwc, ulong_t *map, uint32_t n, uint32_t *rotor,
    uint32_t *idxp)
{
	uint32_t i, idx;

	mutex_enter(&iwc->iwc_res_lock);
	for (i = 0; i < n; i++) {
		idx = (*rotor + i) % n;
		if (!BT_TEST(map, idx))
			break;
	}
	if (i == n) {
		mutex_exit(&iwc->iwc_res_lock);
		return (ENOSPC);
	}
	BT_SET(map, idx);
	*rotor = (idx + 1) % n;
	mutex_exit(&iwc->iwc_res_lock);
	*idxp = idx;
	return (0);
}

static void
iwc_bit_free(iwc_t *iwc, ulong_t *map, uint32_t idx)
{
	mutex_enter(&iwc->iwc_res_lock);
	VERIFY(BT_TEST(map, idx));
	BT_CLEAR(map, idx);
	mutex_exit(&iwc->iwc_res_lock);
}

int
iwc_alloc_pd(struct rdk_pd *rpd)
{
	iwc_t *iwc = iwc_of(rpd->device);
	iwc_pd_t *pd = (iwc_pd_t *)rpd;
	uint32_t rotor = 1;

	if (iwc->iwc_fatal)
		return (EIO);
	return (iwc_bit_alloc(iwc, iwc->iwc_pdid_map, IWC_MAX_PD, &rotor,
	    &pd->pd_pdid));
}

void
iwc_dealloc_pd(struct rdk_pd *rpd)
{
	iwc_pd_t *pd = (iwc_pd_t *)rpd;

	iwc_bit_free(iwc_of(rpd->device), iwc_of(rpd->device)->iwc_pdid_map,
	    pd->pd_pdid);
}

int
iwc_tpt_perms(int acc)
{
	return (((acc & RDK_ACCESS_REMOTE_WRITE) != 0 ?
	    FW_RI_MEM_ACCESS_REM_WRITE : 0) |
	    ((acc & RDK_ACCESS_REMOTE_READ) != 0 ?
	    FW_RI_MEM_ACCESS_REM_READ : 0) |
	    ((acc & RDK_ACCESS_LOCAL_WRITE) != 0 ?
	    FW_RI_MEM_ACCESS_LOCAL_WRITE : 0) | FW_RI_MEM_ACCESS_LOCAL_READ);
}

static int
iwc_tpt_write(iwc_t *iwc, uint32_t stag, const struct fw_ri_tpte *tpt)
{
	const uint32_t addr = iwc->iwc_info.tri_vres.trv_stag.trr_start +
	    (stag >> 8) * IWC_TPT_ENTRY;

	return (iwc->iwc_ops->tro_tpt_write(iwc->iwc_peer, addr, tpt,
	    sizeof (*tpt)));
}

int
iwc_alloc_mr(struct rdk_pd *rpd, enum rdk_mr_type type, uint32_t max,
    struct rdk_mr **mrp)
{
	iwc_t *iwc = iwc_of(rpd->device);
	const t4_rdma_vres_t *vr = &iwc->iwc_info.tri_vres;
	iwc_pd_t *pd = (iwc_pd_t *)rpd;
	struct fw_ri_tpte tpt;
	iwc_mr_t *mr;
	uint32_t idx, pblbytes;
	boolean_t leak = B_FALSE;
	int ret;

	if (type != RDK_MR_TYPE_MEM_REG || max == 0 ||
	    max > T4_MAX_FR_IMMD_DEPTH)
		return (EINVAL);
	if (iwc->iwc_fatal)
		return (EIO);
	mr = kmem_zalloc(sizeof (*mr), KM_SLEEP);
	mr->mr_iwc = iwc;
	mr->mr_pdid = pd->pd_pdid;
	mr->mr_max = max;
	mr->mr_pages = kmem_zalloc(max * sizeof (uint64_t), KM_SLEEP);
	pblbytes = roundup(max * sizeof (uint64_t), IWC_TPT_ENTRY);
	mr->mr_pbl_addr = iwc_arena_alloc(iwc->iwc_pbl_arena,
	    vr->trv_pbl.trr_start, IWC_MIN_PBL_SHIFT, pblbytes);
	if (mr->mr_pbl_addr == 0) {
		ret = ENOMEM;
		goto fail;
	}
	if ((ret = iwc_bit_alloc(iwc, iwc->iwc_stag_map, iwc->iwc_nstag,
	    &iwc->iwc_stag_rotor, &idx)) != 0)
		goto fail;
	mr->mr_stag = (idx << 8) | (atomic_inc_8_nv(&iwc->iwc_stag_key) &
	    0xff);

	bzero(&tpt, sizeof (tpt));
	tpt.valid_to_pdid = BE_32(F_FW_RI_TPTE_VALID |
	    V_FW_RI_TPTE_STAGKEY(mr->mr_stag & M_FW_RI_TPTE_STAGKEY) |
	    V_FW_RI_TPTE_STAGSTATE(0) | V_FW_RI_TPTE_STAGTYPE(FW_RI_STAG_NSMR) |
	    V_FW_RI_TPTE_PDID(pd->pd_pdid));
	tpt.nosnoop_pbladdr = BE_32(V_FW_RI_TPTE_PBLADDR(
	    (mr->mr_pbl_addr - vr->trv_pbl.trr_start) >> 3));
	if ((ret = iwc_tpt_write(iwc, mr->mr_stag, &tpt)) != 0) {
		/* The entry may be half written; keep the STag out of use. */
		iwc_warn(iwc, "TPT write for STag 0x%x failed: %d",
		    mr->mr_stag, ret);
		leak = B_TRUE;
		goto fail;
	}
	mr->mr_rdk.lkey = mr->mr_rdk.rkey = mr->mr_stag;
	*mrp = &mr->mr_rdk;
	return (0);
fail:
	if (mr->mr_pbl_addr != 0 && !leak) {
		iwc_arena_free(iwc->iwc_pbl_arena, vr->trv_pbl.trr_start,
		    IWC_MIN_PBL_SHIFT, mr->mr_pbl_addr, pblbytes);
	}
	kmem_free(mr->mr_pages, max * sizeof (uint64_t));
	kmem_free(mr, sizeof (*mr));
	return (ret);
}

static int
iwc_set_page(struct rdk_mr *rmr, uint64_t addr)
{
	iwc_mr_t *mr = (iwc_mr_t *)rmr;

	if (mr->mr_npages == mr->mr_max)
		return (-ENOMEM);
	mr->mr_pages[mr->mr_npages++] = addr;
	return (0);
}

int
iwc_map_mr_sg(struct rdk_mr *rmr, const ddi_dma_cookie_t *cookies, uint_t n,
    uint64_t *offset)
{
	iwc_mr_t *mr = (iwc_mr_t *)rmr;

	if (rmr->page_size < 4096 || rmr->page_size > (128U << 20))
		return (-EINVAL);
	mr->mr_npages = 0;
	return (rdk_sg_to_pages(rmr, cookies, n, offset, iwc_set_page));
}

/*
 * Invalidate the TPT entry before the STag and PBL go back.  When the
 * adapter does not confirm the write, both stay out of use.
 */
int
iwc_dereg_mr(struct rdk_mr *rmr)
{
	iwc_mr_t *mr = (iwc_mr_t *)rmr;
	iwc_t *iwc = mr->mr_iwc;
	const t4_rdma_vres_t *vr = &iwc->iwc_info.tri_vres;
	struct fw_ri_tpte tpt;
	int ret;

	bzero(&tpt, sizeof (tpt));
	ret = iwc_tpt_write(iwc, mr->mr_stag, &tpt);
	if (ret == 0) {
		iwc_bit_free(iwc, iwc->iwc_stag_map, mr->mr_stag >> 8);
		iwc_arena_free(iwc->iwc_pbl_arena, vr->trv_pbl.trr_start,
		    IWC_MIN_PBL_SHIFT, mr->mr_pbl_addr,
		    roundup(mr->mr_max * sizeof (uint64_t), IWC_TPT_ENTRY));
	} else {
		iwc_warn(iwc, "STag 0x%x not invalidated: %d; leaking it",
		    mr->mr_stag, ret);
		ret = EIO;
	}
	kmem_free(mr->mr_pages, mr->mr_max * sizeof (uint64_t));
	kmem_free(mr, sizeof (*mr));
	return (ret);
}

/* A consumer DMA buffer, naturally aligned up to 2 MB. */
int
iwc_dma_alloc(struct rdk_device *rdev, size_t len, rdk_dma_buf_t *buf)
{
	iwc_t *iwc = iwc_of(rdev);
	t4_rdma_dma_t *d;
	size_t align = 4096;
	int ret;

	while (align < len && align < 2 * 1024 * 1024)
		align <<= 1;
	if ((ret = iwc->iwc_ops->tro_dma_alloc(iwc->iwc_peer, len, align,
	    &d)) != 0)
		return (ret);
	buf->rdb_va = d->trd_va;
	buf->rdb_pa = d->trd_pa;
	buf->rdb_len = len;
	buf->rdb_priv = d;
	return (0);
}

void
iwc_dma_free(struct rdk_device *rdev, rdk_dma_buf_t *buf)
{
	iwc_t *iwc = iwc_of(rdev);

	iwc->iwc_ops->tro_dma_free(iwc->iwc_peer, buf->rdb_priv,
	    !iwc->iwc_fatal);
}

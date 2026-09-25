/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2015 - 2021 Intel Corporation */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Fast registration memory regions, after irdma_alloc_mr(),
 * irdma_map_mr_sg(), irdma_hw_alloc_stag() and irdma_dereg_mr() of Linux
 * irdma (see README.illumos).  An MR reserves a stag and a level 1 page
 * list at allocation; a REG_MR work request binds the list, and LOCAL_INV
 * or a send with invalidate unbinds it.  The page list is one contiguous
 * run of PBLEs, which the fast register WQE requires.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/random.h>

#include "irdma_verbs.h"

static void
irdma_free_stag(irdma_t *irdma, uint32_t stag)
{
	uint32_t idx = (stag & irdma->irdma_mr_stagmask) >>
	    IRDMA_CQPSQ_STAG_IDX_S;

	irdma_free_rsrc(irdma, irdma->irdma_mr_map, idx);
}

/* A stag with a random index start, driver key and consumer key. */
static uint32_t
irdma_create_stag(irdma_t *irdma)
{
	uint32_t rnd, idx, next, stag;

	(void) random_get_pseudo_bytes((uint8_t *)&rnd, sizeof (rnd));
	next = ((rnd & irdma->irdma_mr_stagmask) >> 8) % irdma->irdma_max_mr;
	if (irdma_alloc_rsrc(irdma, irdma->irdma_mr_map, irdma->irdma_max_mr,
	    &idx, &next) != 0)
		return (0);
	stag = idx << IRDMA_CQPSQ_STAG_IDX_S;
	stag |= rnd & ~irdma->irdma_mr_stagmask;
	stag += (uint8_t)rnd;
	/* Index 0 with key 0 would be the local DMA lkey. */
	if (stag == 0) {
		irdma_free_rsrc(irdma, irdma->irdma_mr_map, idx);
		return (0);
	}
	return (stag);
}

static int
irdma_hw_alloc_stag(irdma_t *irdma, irdma_mr_t *mr, uint32_t pd_id)
{
	struct irdma_allocate_stag_info *info;
	irdma_cqp_req_t *req;

	if ((req = irdma_vreq(irdma, IRDMA_OP_ALLOC_STAG)) == NULL)
		return (EIO);
	info = &req->icr_cmd.in.u.alloc_stag.info;
	info->page_size = PAGESIZE;
	info->stag_idx = mr->imr_stag >> IRDMA_CQPSQ_STAG_IDX_S;
	info->pd_id = pd_id;
	info->total_len = (u64)mr->imr_page_cnt * PAGESIZE;
	info->all_memory = false;
	info->remote_access = true;
	req->icr_cmd.in.u.alloc_stag.dev = &irdma->irdma_sc;
	req->icr_cmd.in.u.alloc_stag.scratch = irdma_req_scratch(irdma, req);
	return (irdma_cqp_exec(irdma, req, NULL));
}

int
irdma_alloc_mr(struct rdk_pd *rpd, enum rdk_mr_type type, uint32_t max_sg,
    struct rdk_mr **mrp)
{
	irdma_t *irdma = IRDMA_DEV(rpd->device);
	irdma_mr_t *mr;
	int ret;

	if (type != RDK_MR_TYPE_MEM_REG || max_sg == 0 ||
	    max_sg > irdma->irdma_rdk.rd_attr.max_fast_reg_page_list_len)
		return (EINVAL);

	mr = kmem_zalloc(sizeof (*mr), KM_SLEEP);
	mr->imr_irdma = irdma;
	if ((mr->imr_stag = irdma_create_stag(irdma)) == 0) {
		kmem_free(mr, sizeof (*mr));
		return (ENOSPC);
	}
	mr->imr_rdk.lkey = mr->imr_rdk.rkey = mr->imr_stag;
	mr->imr_page_cnt = max_sg;
	if (irdma_get_pble(irdma->irdma_pble, &mr->imr_pble, max_sg,
	    PBLE_LEVEL_1) != 0 || mr->imr_pble.level != PBLE_LEVEL_1) {
		if (mr->imr_pble.level != PBLE_LEVEL_0)
			irdma_free_pble(irdma->irdma_pble, &mr->imr_pble);
		irdma_free_stag(irdma, mr->imr_stag);
		kmem_free(mr, sizeof (*mr));
		return (ENOMEM);
	}
	mr->imr_pble_live = B_TRUE;

	ret = irdma_hw_alloc_stag(irdma, mr, IRDMA_PD(rpd)->ipd_sc.pd_id);
	if (ret != 0) {
		/* The device may hold the stag; keep its index and PBLEs. */
		irdma_taint(irdma);
		kmem_free(mr, sizeof (*mr));
		return (ret);
	}
	mr->imr_hwreg = B_TRUE;
	atomic_inc_32(&irdma->irdma_nmrs);
	*mrp = &mr->imr_rdk;
	return (0);
}

static int
irdma_set_page(struct rdk_mr *rmr, uint64_t addr)
{
	irdma_mr_t *mr = IRDMA_MR(rmr);

	if (mr->imr_npages >= mr->imr_page_cnt)
		return (-ENOMEM);
	mr->imr_pble.level1.addr[mr->imr_npages++] = addr;
	return (0);
}

int
irdma_map_mr_sg(struct rdk_mr *rmr, const ddi_dma_cookie_t *cookies,
    uint_t n, uint64_t *offset)
{
	irdma_mr_t *mr = IRDMA_MR(rmr);

	if (rmr->page_size != PAGESIZE)
		return (-EINVAL);
	mr->imr_npages = 0;
	return (rdk_sg_to_pages(rmr, cookies, n, offset, irdma_set_page));
}

/*
 * The stag index and the PBLEs are released only if the device confirmed
 * it let go of the stag.
 */
int
irdma_dereg_mr(struct rdk_mr *rmr)
{
	irdma_mr_t *mr = IRDMA_MR(rmr);
	irdma_t *irdma = mr->imr_irdma;
	struct irdma_dealloc_stag_info *info;
	irdma_cqp_req_t *req;
	int ret = EIO;

	if ((req = irdma_vreq(irdma, IRDMA_OP_DEALLOC_STAG)) != NULL) {
		info = &req->icr_cmd.in.u.dealloc_stag.info;
		info->pd_id = IRDMA_PD(rmr->pd)->ipd_sc.pd_id;
		info->stag_idx = mr->imr_stag >> IRDMA_CQPSQ_STAG_IDX_S;
		info->mr = true;
		info->dealloc_pbl = true;
		req->icr_cmd.in.u.dealloc_stag.dev = &irdma->irdma_sc;
		req->icr_cmd.in.u.dealloc_stag.scratch =
		    irdma_req_scratch(irdma, req);
		ret = irdma_cqp_exec(irdma, req, NULL);
	}
	if (ret == 0) {
		irdma_free_pble(irdma->irdma_pble, &mr->imr_pble);
		irdma_free_stag(irdma, mr->imr_stag);
	} else {
		irdma_taint(irdma);
		ret = EIO;
	}
	atomic_dec_32(&irdma->irdma_nmrs);
	kmem_free(mr, sizeof (*mr));
	return (ret);
}

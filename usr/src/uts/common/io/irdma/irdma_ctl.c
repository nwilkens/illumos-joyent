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
 * The RDMA control plane: bring-up and teardown of the CQP, the HMC, the
 * CCQ, CEQ 0, the AEQ, the PBLE pool and the work scheduler, the CQP request
 * layer, and the event queue processing.  The step order and the core calls
 * follow the Linux irdma driver (hw.c); the code is written for illumos.
 *
 * CQP requests: a command lives in a request slot, not on the caller's
 * stack, because the core may queue it.  Its scratch value names the slot
 * and a generation.  A caller that gives up marks the slot abandoned; a late
 * completion then only frees it.  A timeout marks the CQP dead, taints the
 * device and asks ice for a PF reset.
 *
 * Completions: CEQ 0 carries the CCQ.  Its interrupt only queues work on
 * irdma_taskq, which consumes the CCQ and re-enables the vector.  A waiter
 * also polls the CCQ every IRDMA_CQP_POLL_MS, so a lost interrupt delays a
 * completion but does not lose it.
 *
 * Locks, outermost first: irdma_cfg_lock, irdma_ccq_lock, irdma_req_lock,
 * then the core's own locks.  irdma_intr_lock is at interrupt priority and
 * guards only the owed-work flags.
 */

#include "irdma_impl.h"
#include "puda.h"
#include "virtchnl.h"

#include <sys/bitmap.h>
#include <sys/ddi_intr.h>

#define	IRDMA_CCQ_SIZE		(IRDMA_CQP_SW_SQSIZE_2048 + 2)
/* The feature version the core assumes until the query answers. */
#define	IRDMA_FW_VER_DEFAULT	2
#define	IRDMA_OBJ_MEM_SIZE	8192
#define	IRDMA_CQP_POLL_MS	10
/* Slots a waiter tries before it reports no request free. */
#define	IRDMA_REQ_WAIT_TRIES	100

static const char *irdma_step_names[IRDMA_STEP_MAX] = {
	"open", "dev", "intr", "cqp", "fpm", "hmc", "ccq", "ceq0", "aeq",
	"pble", "ws", "pefltr"
};

static boolean_t
irdma_hw_ok(irdma_t *irdma)
{
	return ((irdma->irdma_flags & IRDMA_F_CQP_DEAD) == 0 &&
	    !irdma->irdma_ops->iro_resetting(irdma->irdma_peer));
}

/* The PF-relative vector number of entry i of the RDMA block. */
static uint32_t
irdma_hw_vec(irdma_t *irdma, uint_t i)
{
	return (irdma->irdma_intr.irin_first + i);
}

/*
 * Carve a small aligned piece out of the object memory.
 */
static int
irdma_obj_mem(irdma_t *irdma, struct irdma_dma_mem *m, u32 size, u32 mask)
{
	struct irdma_dma_mem *next = &irdma->irdma_obj_next;
	uintptr_t va = (uintptr_t)next->va;
	uintptr_t aligned = P2ROUNDUP(va, (uintptr_t)mask + 1);
	uintptr_t end = (uintptr_t)irdma->irdma_obj_mem.va +
	    irdma->irdma_obj_mem.size;

	if (aligned > end || size > end - aligned)
		return (-ENOMEM);
	m->va = (void *)aligned;
	m->pa = next->pa + (aligned - va);
	m->size = size;
	next->va = (u8 *)m->va + size;
	next->pa = m->pa + size;
	return (0);
}

/*
 * CQP requests.
 */

static u64
irdma_req_scratch(irdma_t *irdma, irdma_cqp_req_t *req)
{
	uint_t idx = (uint_t)(req - irdma->irdma_reqs);

	return (((u64)req->icr_gen << 32) | (idx + 1));
}

/* The caller holds irdma_req_lock. */
static irdma_cqp_req_t *
irdma_req_lookup(irdma_t *irdma, u64 scratch)
{
	u64 idx = (scratch & UINT32_MAX);
	irdma_cqp_req_t *req;

	ASSERT(MUTEX_HELD(&irdma->irdma_req_lock));

	if (idx == 0 || idx > IRDMA_CQP_NREQS)
		return (NULL);
	req = &irdma->irdma_reqs[idx - 1];
	if ((scratch >> 32) != req->icr_gen ||
	    (req->icr_state != IRDMA_REQ_BUSY &&
	    req->icr_state != IRDMA_REQ_ABANDONED))
		return (NULL);
	return (req);
}

static irdma_cqp_req_t *
irdma_req_alloc(irdma_t *irdma)
{
	irdma_cqp_req_t *req = NULL;
	uint_t i, tries;

	mutex_enter(&irdma->irdma_req_lock);
	for (tries = 0; tries < IRDMA_REQ_WAIT_TRIES; tries++) {
		if ((irdma->irdma_flags & IRDMA_F_CQP_DEAD) != 0)
			break;
		for (i = 0; i < IRDMA_CQP_NREQS; i++) {
			if (irdma->irdma_reqs[i].icr_state == IRDMA_REQ_FREE) {
				req = &irdma->irdma_reqs[i];
				break;
			}
		}
		if (req != NULL)
			break;
		(void) cv_reltimedwait(&irdma->irdma_req_cv,
		    &irdma->irdma_req_lock, drv_usectohz(10 * MILLISEC),
		    TR_CLOCK_TICK);
	}
	if (req != NULL) {
		req->icr_state = IRDMA_REQ_BUSY;
		req->icr_gen++;
		bzero(&req->icr_cqe, sizeof (req->icr_cqe));
		bzero(&req->icr_cmd, sizeof (req->icr_cmd));
	}
	mutex_exit(&irdma->irdma_req_lock);
	return (req);
}

/* The caller holds irdma_req_lock. */
static void
irdma_req_free(irdma_t *irdma, irdma_cqp_req_t *req)
{
	ASSERT(MUTEX_HELD(&irdma->irdma_req_lock));
	req->icr_state = IRDMA_REQ_FREE;
	cv_broadcast(&irdma->irdma_req_cv);
}

/* The caller holds irdma_req_lock. */
static void
irdma_req_complete(irdma_t *irdma, const struct irdma_ccq_cqe_info *info)
{
	irdma_cqp_req_t *req;

	ASSERT(MUTEX_HELD(&irdma->irdma_req_lock));

	irdma->irdma_cqp_completed++;
	if (info->error)
		irdma->irdma_cqp_errors++;
	/* Commands that the core polls for carry no request. */
	if (info->scratch == 0)
		return;
	if ((req = irdma_req_lookup(irdma, info->scratch)) == NULL) {
		irdma->irdma_bad_entries++;
		return;
	}
	if (req->icr_state == IRDMA_REQ_ABANDONED) {
		irdma_req_free(irdma, req);
		return;
	}
	req->icr_cqe = *info;
	req->icr_state = IRDMA_REQ_DONE;
	cv_broadcast(&req->icr_cv);
}

/*
 * Stop accepting CQP commands and wake every waiter.  The waiters leave
 * their requests abandoned.
 */
void
irdma_cqp_fail_all(irdma_t *irdma)
{
	uint_t i;

	mutex_enter(&irdma->irdma_req_lock);
	atomic_or_32(&irdma->irdma_flags, IRDMA_F_CQP_DEAD);
	for (i = 0; i < IRDMA_CQP_NREQS; i++)
		cv_broadcast(&irdma->irdma_reqs[i].icr_cv);
	cv_broadcast(&irdma->irdma_req_cv);
	while (irdma->irdma_req_waiters != 0)
		cv_wait(&irdma->irdma_req_cv, &irdma->irdma_req_lock);
	mutex_exit(&irdma->irdma_req_lock);
}

/*
 * Consume the CCQ.  A malformed entry means the device cannot be trusted;
 * the CCQ is left as it is and the function goes down.
 */
static void
irdma_ccq_poll(irdma_t *irdma)
{
	struct irdma_ccq_cqe_info info;
	struct irdma_sc_cq *ccq = &irdma->irdma_ccq;
	uint_t n = 0;
	int ret;

	mutex_enter(&irdma->irdma_ccq_lock);
	if ((irdma->irdma_progress & BIT(IRDMA_STEP_CCQ)) == 0 ||
	    irdma->irdma_hold_cqes) {
		mutex_exit(&irdma->irdma_ccq_lock);
		return;
	}

	for (;;) {
		bzero(&info, sizeof (info));
		ret = irdma_sc_ccq_get_cqe_info(ccq, &info);
		if (ret == -ENOENT)
			break;
		if (ret != 0) {
			irdma->irdma_bad_entries++;
			mutex_exit(&irdma->irdma_ccq_lock);
			irdma_fatal(irdma, "malformed CCQ entry");
			return;
		}
		mutex_enter(&irdma->irdma_req_lock);
		irdma_req_complete(irdma, &info);
		mutex_exit(&irdma->irdma_req_lock);
		if (++n >= IRDMA_CCQ_SIZE)
			break;
	}

	if (n != 0) {
		(void) irdma_process_bh(&irdma->irdma_sc);
		irdma_sc_ccq_arm(ccq);
	}
	mutex_exit(&irdma->irdma_ccq_lock);
}

/*
 * Submit the command in req and, with wait, wait for its completion.  The
 * request is freed or abandoned on return.
 */
static int
irdma_cqp_exec(irdma_t *irdma, irdma_cqp_req_t *req,
    struct irdma_ccq_cqe_info *out)
{
	hrtime_t deadline;
	int status, ret = 0;

	mutex_enter(&irdma->irdma_req_lock);
	if ((irdma->irdma_flags & IRDMA_F_CQP_DEAD) != 0) {
		irdma_req_free(irdma, req);
		mutex_exit(&irdma->irdma_req_lock);
		return (EIO);
	}
	req->icr_cmd.post_sq = 1;
	status = irdma_process_cqp_cmd(&irdma->irdma_sc, &req->icr_cmd);
	if (status != 0) {
		irdma->irdma_cqp_errors++;
		irdma_req_free(irdma, req);
		mutex_exit(&irdma->irdma_req_lock);
		return (EIO);
	}
	irdma->irdma_cqp_submitted++;
	irdma->irdma_req_waiters++;

	deadline = gethrtime() + MSEC2NSEC(irdma->irdma_cqp_timeout_ms);
	while (req->icr_state == IRDMA_REQ_BUSY) {
		if ((irdma->irdma_flags & IRDMA_F_CQP_DEAD) != 0) {
			req->icr_state = IRDMA_REQ_ABANDONED;
			ret = EIO;
			break;
		}
		if (gethrtime() >= deadline) {
			req->icr_state = IRDMA_REQ_ABANDONED;
			irdma->irdma_cqp_timeouts++;
			ret = ETIMEDOUT;
			break;
		}
		if (cv_reltimedwait(&req->icr_cv, &irdma->irdma_req_lock,
		    drv_usectohz(IRDMA_CQP_POLL_MS * MILLISEC),
		    TR_CLOCK_TICK) == -1) {
			mutex_exit(&irdma->irdma_req_lock);
			irdma_ccq_poll(irdma);
			mutex_enter(&irdma->irdma_req_lock);
		}
	}

	if (ret == 0) {
		if (out != NULL)
			*out = req->icr_cqe;
		if (req->icr_cqe.error)
			ret = EIO;
		irdma_req_free(irdma, req);
	}
	if (--irdma->irdma_req_waiters == 0)
		cv_broadcast(&irdma->irdma_req_cv);
	mutex_exit(&irdma->irdma_req_lock);

	if (ret == ETIMEDOUT)
		irdma_fatal(irdma, "CQP command timed out");
	return (ret);
}

/*
 * The CQP commands the core asks the OS layer to issue.
 */
int
irdma_cqp_sds_cmd(struct irdma_sc_dev *dev, struct irdma_update_sds_info *sd)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);
	irdma_cqp_req_t *req;
	int ret;

	if ((req = irdma_req_alloc(irdma)) == NULL) {
		irdma_taint(irdma);
		return (-ENOMEM);
	}
	req->icr_cmd.cqp_cmd = IRDMA_OP_UPDATE_PE_SDS;
	req->icr_cmd.in.u.update_pe_sds.info = *sd;
	req->icr_cmd.in.u.update_pe_sds.dev = dev;
	req->icr_cmd.in.u.update_pe_sds.scratch = irdma_req_scratch(irdma, req);
	ret = irdma_cqp_exec(irdma, req, NULL);
	/* A failed SD update leaves the device's view of host memory open. */
	if (ret != 0)
		irdma_taint(irdma);
	return (ret == 0 ? 0 : -EIO);
}

/* The SD update used before the CCQ exists and after it is gone. */
static int
irdma_cqp_sds_poll(struct irdma_sc_dev *dev, struct irdma_update_sds_info *sd)
{
	int ret = irdma_update_sds_noccq(dev, sd);

	if (ret != 0)
		irdma_taint(IRDMA_FROM_DEV(dev));
	return (ret);
}

int
irdma_cqp_ceq_cmd(struct irdma_sc_dev *dev, struct irdma_sc_ceq *ceq, u8 op)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);
	irdma_cqp_req_t *req;

	if ((req = irdma_req_alloc(irdma)) == NULL)
		return (-ENOMEM);
	req->icr_cmd.cqp_cmd = op;
	req->icr_cmd.in.u.ceq_create.ceq = ceq;
	req->icr_cmd.in.u.ceq_create.scratch = irdma_req_scratch(irdma, req);
	return (irdma_cqp_exec(irdma, req, NULL) == 0 ? 0 : -EIO);
}

int
irdma_cqp_aeq_cmd(struct irdma_sc_dev *dev, struct irdma_sc_aeq *aeq, u8 op)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);
	irdma_cqp_req_t *req;

	if ((req = irdma_req_alloc(irdma)) == NULL)
		return (-ENOMEM);
	req->icr_cmd.cqp_cmd = op;
	req->icr_cmd.in.u.aeq_create.aeq = aeq;
	req->icr_cmd.in.u.aeq_create.scratch = irdma_req_scratch(irdma, req);
	return (irdma_cqp_exec(irdma, req, NULL) == 0 ? 0 : -EIO);
}

int
irdma_cqp_ws_node_cmd(struct irdma_sc_dev *dev, u8 cmd,
    struct irdma_ws_node_info *node)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);
	struct irdma_ccq_cqe_info cqe;
	irdma_cqp_req_t *req;

	if ((req = irdma_req_alloc(irdma)) == NULL)
		return (-ENOMEM);
	req->icr_cmd.cqp_cmd = cmd;
	req->icr_cmd.in.u.ws_node.info = *node;
	req->icr_cmd.in.u.ws_node.cqp = dev->cqp;
	req->icr_cmd.in.u.ws_node.scratch = irdma_req_scratch(irdma, req);
	if (irdma_cqp_exec(irdma, req, &cqe) != 0)
		return (-EIO);
	node->qs_handle = (u16)cqe.op_ret_val;
	return (0);
}

/*
 * A command that changes nothing but writes to host memory: the FPM query
 * into its usual buffer.  The test hooks use it.
 */
int
irdma_cqp_probe(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	irdma_cqp_req_t *req;

	if ((irdma->irdma_progress & BIT(IRDMA_STEP_CEQ0)) == 0)
		return (ENXIO);
	if ((req = irdma_req_alloc(irdma)) == NULL)
		return (ENOMEM);
	req->icr_cmd.cqp_cmd = IRDMA_OP_QUERY_FPM_VAL;
	req->icr_cmd.in.u.query_fpm_val.cqp = dev->cqp;
	req->icr_cmd.in.u.query_fpm_val.fpm_val_va = dev->fpm_query_buf;
	req->icr_cmd.in.u.query_fpm_val.fpm_val_pa = dev->fpm_query_buf_pa;
	req->icr_cmd.in.u.query_fpm_val.hmc_fn_id = (u8)dev->hmc_fn_id;
	req->icr_cmd.in.u.query_fpm_val.scratch = irdma_req_scratch(irdma, req);
	return (irdma_cqp_exec(irdma, req, NULL));
}

void *
irdma_remove_cqp_head(struct irdma_sc_dev *dev)
{
	struct list_head *e;

	if (list_empty(&dev->cqp_cmd_head))
		return (NULL);
	e = dev->cqp_cmd_head.next;
	list_del(e);
	return (e);
}

/* VF functions and the statistics instances are not used. */
int
irdma_cqp_manage_hmc_fcn_cmd(struct irdma_sc_dev *dev,
    struct irdma_hmc_fcn_info *info, u16 *pmf_idx)
{
	_NOTE(ARGUNUSED(dev, info, pmf_idx));
	return (-EOPNOTSUPP);
}

int
irdma_alloc_query_fpm_buf(struct irdma_sc_dev *dev, struct irdma_dma_mem *mem)
{
	_NOTE(ARGUNUSED(dev, mem));
	return (-EOPNOTSUPP);
}

int
irdma_cqp_stats_inst_cmd(struct irdma_sc_vsi *vsi, u8 cmd,
    struct irdma_stats_inst_info *info)
{
	_NOTE(ARGUNUSED(vsi, cmd, info));
	return (-EOPNOTSUPP);
}

int
irdma_cqp_gather_stats_cmd(struct irdma_sc_dev *dev,
    struct irdma_vsi_pestat *pestat, bool wait)
{
	_NOTE(ARGUNUSED(dev, pestat, wait));
	return (-EOPNOTSUPP);
}

void
irdma_hw_stats_start_timer(struct irdma_sc_vsi *vsi)
{
	_NOTE(ARGUNUSED(vsi));
}

void
irdma_hw_stats_stop_timer(struct irdma_sc_vsi *vsi)
{
	_NOTE(ARGUNUSED(vsi));
}

void
irdma_add_dev_ref(struct irdma_sc_dev *dev)
{
	_NOTE(ARGUNUSED(dev));
}

void
irdma_put_dev_ref(struct irdma_sc_dev *dev)
{
	_NOTE(ARGUNUSED(dev));
}

/*
 * Work scheduler node IDs.  Node 0 is the root.
 */
u16
irdma_alloc_ws_node_id(struct irdma_sc_dev *dev)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);
	u16 id = IRDMA_WS_NODE_INVALID;
	uint_t i;

	mutex_enter(&irdma->irdma_ws_lock);
	for (i = 1; i < irdma->irdma_ws_max; i++) {
		if (!BT_TEST(irdma->irdma_ws_ids, i)) {
			BT_SET(irdma->irdma_ws_ids, i);
			id = (u16)i;
			break;
		}
	}
	mutex_exit(&irdma->irdma_ws_lock);
	return (id);
}

void
irdma_free_ws_node_id(struct irdma_sc_dev *dev, u16 id)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);

	mutex_enter(&irdma->irdma_ws_lock);
	if (id != 0 && id < irdma->irdma_ws_max)
		BT_CLEAR(irdma->irdma_ws_ids, id);
	mutex_exit(&irdma->irdma_ws_lock);
}

/*
 * The PBLE resource manager.  Each chunk has a bitmap of pble_shift sized
 * units.
 */
int
irdma_prm_add_pble_mem(struct irdma_pble_prm *prm, struct irdma_chunk *chunk)
{
	u64 bits;

	if ((chunk->size & 0xfff) != 0)
		return (-EINVAL);
	bits = chunk->size >> prm->pble_shift;
	chunk->bitmapbuf = bitmap_zalloc(bits, GFP_KERNEL);
	if (chunk->bitmapbuf == NULL)
		return (-ENOMEM);
	chunk->sizeofbitmap = bits;
	prm->total_pble_alloc += chunk->size >> 3;
	prm->free_pble_cnt += chunk->size >> 3;
	return (0);
}

/* The first run of n clear bits in the first max bits, or max. */
static u64
irdma_bitmap_find_run(unsigned long *map, u64 max, u64 n)
{
	u64 start = 0, i;

	while (start + n <= max) {
		for (i = 0; i < n; i++) {
			if (BT_TEST(map, start + i))
				break;
		}
		if (i == n)
			return (start);
		start += i + 1;
	}
	return (max);
}

int
irdma_prm_get_pbles(struct irdma_pble_prm *prm,
    struct irdma_pble_chunkinfo *info, u64 mem_size, u64 **vaddr, u64 *fpm)
{
	struct list_head *e;
	struct irdma_chunk *chunk = NULL;
	u64 need, idx = 0, i;
	unsigned long flags;

	*vaddr = NULL;
	*fpm = 0;
	need = howmany(mem_size, 1ULL << prm->pble_shift);
	if (need == 0)
		return (-EINVAL);

	spin_lock_irqsave(&prm->prm_lock, flags);
	for (e = prm->clist.next; e != &prm->clist; e = e->next) {
		chunk = container_of(e, struct irdma_chunk, list);
		idx = irdma_bitmap_find_run(chunk->bitmapbuf,
		    chunk->sizeofbitmap, need);
		if (idx < chunk->sizeofbitmap)
			break;
		chunk = NULL;
	}
	if (chunk == NULL) {
		spin_unlock_irqrestore(&prm->prm_lock, flags);
		return (-ENOMEM);
	}
	for (i = 0; i < need; i++)
		BT_SET(chunk->bitmapbuf, idx + i);
	*vaddr = (u64 *)(void *)((u8 *)chunk->vaddr +
	    (idx << prm->pble_shift));
	*fpm = chunk->fpm_addr + (idx << prm->pble_shift);
	info->pchunk = chunk;
	info->bit_idx = idx;
	info->bits_used = need;
	prm->free_pble_cnt -= need << (prm->pble_shift - 3);
	spin_unlock_irqrestore(&prm->prm_lock, flags);
	return (0);
}

void
irdma_prm_return_pbles(struct irdma_pble_prm *prm,
    struct irdma_pble_chunkinfo *info)
{
	unsigned long flags;
	u64 i;

	spin_lock_irqsave(&prm->prm_lock, flags);
	prm->free_pble_cnt += info->bits_used << (prm->pble_shift - 3);
	for (i = 0; i < info->bits_used; i++)
		BT_CLEAR(info->pchunk->bitmapbuf, info->bit_idx + i);
	spin_unlock_irqrestore(&prm->prm_lock, flags);
}

/*
 * A paged PBLE chunk: one contiguous DMA buffer, described page by page.
 */
int
irdma_pble_get_paged_mem(struct irdma_chunk *chunk, u32 pg_cnt)
{
	struct irdma_hw *hw = chunk->dev->hw;
	dma_addr_t pa;
	void *va;
	u32 i;

	if (pg_cnt == 0 || pg_cnt > IRDMA_HMC_PD_CNT_IN_SD)
		return (-EINVAL);
	chunk->dmainfo.dmaaddrs = kcalloc(pg_cnt, sizeof (dma_addr_t),
	    GFP_KERNEL);
	if (chunk->dmainfo.dmaaddrs == NULL)
		return (-ENOMEM);
	va = dma_alloc_coherent(hw->device, (size_t)pg_cnt * PAGESIZE, &pa,
	    GFP_KERNEL);
	if (va == NULL) {
		kfree(chunk->dmainfo.dmaaddrs);
		chunk->dmainfo.dmaaddrs = NULL;
		return (-ENOMEM);
	}
	for (i = 0; i < pg_cnt; i++)
		chunk->dmainfo.dmaaddrs[i] = pa + (u64)i * PAGESIZE;
	chunk->vaddr = va;
	chunk->size = (u64)pg_cnt * PAGESIZE;
	chunk->pg_cnt = pg_cnt;
	chunk->type = PBLE_SD_PAGED;
	return (0);
}

void
irdma_pble_free_paged_mem(struct irdma_chunk *chunk)
{
	struct irdma_hw *hw = chunk->dev->hw;

	if (chunk->vaddr != NULL && chunk->dmainfo.dmaaddrs != NULL) {
		dma_free_coherent(hw->device, (size_t)chunk->pg_cnt * PAGESIZE,
		    chunk->vaddr, chunk->dmainfo.dmaaddrs[0]);
	}
	kfree(chunk->dmainfo.dmaaddrs);
	chunk->dmainfo.dmaaddrs = NULL;
	chunk->vaddr = NULL;
	chunk->type = 0;
}

/*
 * Interrupts.  The handler records which queue is owed and queues the
 * work; the vector stays masked until the task re-enables it.
 */
uint_t
irdma_intr(caddr_t arg1, caddr_t arg2)
{
	irdma_t *irdma = (irdma_t *)(void *)arg1;
	uint_t vec = (uint_t)(uintptr_t)arg2;
	boolean_t dispatch = B_FALSE;

	mutex_enter(&irdma->irdma_intr_lock);
	if (!irdma->irdma_intr_off) {
		if (vec == irdma->irdma_ceq_vec) {
			irdma->irdma_ceq_owed = B_TRUE;
			irdma->irdma_ceq_intrs++;
		}
		if (vec == irdma->irdma_aeq_vec) {
			irdma->irdma_aeq_owed = B_TRUE;
			irdma->irdma_aeq_intrs++;
		}
		if (!irdma->irdma_task_queued) {
			irdma->irdma_task_queued = B_TRUE;
			dispatch = B_TRUE;
		}
	}
	mutex_exit(&irdma->irdma_intr_lock);

	if (dispatch && ddi_taskq_dispatch(irdma->irdma_taskq,
	    irdma_intr_task, irdma, DDI_NOSLEEP) != DDI_SUCCESS) {
		mutex_enter(&irdma->irdma_intr_lock);
		irdma->irdma_task_queued = B_FALSE;
		mutex_exit(&irdma->irdma_intr_lock);
	}
	return (DDI_INTR_CLAIMED);
}

static void
irdma_vec_enable(irdma_t *irdma, uint_t vec)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;

	dev->irq_ops->irdma_en_irq(dev, irdma_hw_vec(irdma, vec));
}

static void
irdma_vec_disable(irdma_t *irdma, uint_t vec)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;

	dev->irq_ops->irdma_dis_irq(dev, irdma_hw_vec(irdma, vec));
}

static void
irdma_ceq0_process(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_sc_cq *cq;
	uint32_t n = 0;

	/* Only CQs registered on CEQ 0 come back from the core. */
	while (n++ < irdma->irdma_ceq0.elem_cnt &&
	    (cq = irdma_sc_process_ceq(dev, &irdma->irdma_ceq0)) != NULL) {
		if (cq == &irdma->irdma_ccq)
			irdma_ccq_poll(irdma);
	}
}

static void
irdma_aeq_process(irdma_t *irdma)
{
	struct irdma_aeqe_info info;
	uint32_t n = 0;

	while (n < irdma->irdma_aeq.elem_cnt) {
		bzero(&info, sizeof (info));
		if (irdma_sc_get_next_aeqe(&irdma->irdma_aeq, &info) != 0)
			break;
		n++;
		irdma->irdma_aeqes++;
		/* No QP or CQ exists yet, so every event is unexpected. */
		irdma_error(irdma, "async event 0x%x source 0x%x id %u",
		    info.ae_id, info.ae_src, info.qp_cq_id);
	}
	if (n != 0)
		irdma_sc_repost_aeq_entries(&irdma->irdma_sc, n);
}

void
irdma_intr_task(void *arg)
{
	irdma_t *irdma = arg;
	boolean_t ceq, aeq;

	for (;;) {
		mutex_enter(&irdma->irdma_intr_lock);
		ceq = irdma->irdma_ceq_owed;
		aeq = irdma->irdma_aeq_owed;
		irdma->irdma_ceq_owed = irdma->irdma_aeq_owed = B_FALSE;
		if ((!ceq && !aeq) || irdma->irdma_intr_off) {
			irdma->irdma_task_queued = B_FALSE;
			mutex_exit(&irdma->irdma_intr_lock);
			return;
		}
		mutex_exit(&irdma->irdma_intr_lock);

		if (ceq && (irdma->irdma_progress & BIT(IRDMA_STEP_CEQ0))) {
			irdma_ceq0_process(irdma);
			irdma_vec_enable(irdma, irdma->irdma_ceq_vec);
		}
		if (aeq && (irdma->irdma_progress & BIT(IRDMA_STEP_AEQ))) {
			irdma_aeq_process(irdma);
			if (irdma->irdma_aeq_vec != irdma->irdma_ceq_vec ||
			    !ceq)
				irdma_vec_enable(irdma, irdma->irdma_aeq_vec);
		}
	}
}

/*
 * Stop queueing interrupt work and wait for the task.  Both handlers stay
 * registered; nothing enables the vectors again.
 */
static void
irdma_intr_quiesce(irdma_t *irdma)
{
	mutex_enter(&irdma->irdma_intr_lock);
	irdma->irdma_intr_off = B_TRUE;
	mutex_exit(&irdma->irdma_intr_lock);
	ddi_taskq_wait(irdma->irdma_taskq);
}

/*
 * The bring-up steps.  Each up function undoes its own partial work on
 * failure; each down function undoes a completed step.
 */

static int
irdma_step_dev(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_device_init_info info;
	struct irdma_dma_mem m;
	size_t size;
	int ret;

	irdma->irdma_obj_mem.size = IRDMA_OBJ_MEM_SIZE;
	irdma->irdma_obj_mem.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    irdma->irdma_obj_mem.size, &irdma->irdma_obj_mem.pa, GFP_KERNEL);
	if (irdma->irdma_obj_mem.va == NULL)
		return (ENOMEM);
	irdma->irdma_obj_next = irdma->irdma_obj_mem;

	size = sizeof (struct irdma_hmc_pble_rsrc) +
	    sizeof (struct irdma_hmc_obj_info) * IRDMA_HMC_IW_MAX;
	irdma->irdma_hmc_mem = kmem_zalloc(size, KM_SLEEP);
	irdma->irdma_hmc_mem_size = size;
	irdma->irdma_pble = irdma->irdma_hmc_mem;
	dev->hmc_info = &irdma->irdma_hw.hmc;
	dev->hmc_info->hmc_obj =
	    (struct irdma_hmc_obj_info *)(void *)(irdma->irdma_pble + 1);

	bzero(&info, sizeof (info));
	if (irdma_obj_mem(irdma, &m, IRDMA_QUERY_FPM_BUF_SIZE,
	    IRDMA_FPM_QUERY_BUF_ALIGNMENT_M) != 0)
		goto nomem;
	info.fpm_query_buf_pa = m.pa;
	info.fpm_query_buf = m.va;
	if (irdma_obj_mem(irdma, &m, IRDMA_COMMIT_FPM_BUF_SIZE,
	    IRDMA_FPM_COMMIT_BUF_ALIGNMENT_M) != 0)
		goto nomem;
	info.fpm_commit_buf_pa = m.pa;
	info.fpm_commit_buf = m.va;

	info.bar0 = irdma->irdma_info.iri_bar0;
	info.hmc_fn_id = irdma->irdma_info.iri_pf_id;
	info.protocol_used = IRDMA_ROCE_PROTOCOL_ONLY;
	info.hw = &irdma->irdma_hw;
	dev->hw_attrs.uk_attrs.hw_rev = IRDMA_GEN_2;
	dev->is_pf = true;
	dev->privileged = true;

	ret = irdma_sc_dev_init(IRDMA_GEN_2, dev, &info);
	if (ret != 0) {
		irdma_error(irdma, "device init failed: %d", ret);
		goto fail;
	}
	return (0);

nomem:
	ret = -ENOMEM;
fail:
	kmem_free(irdma->irdma_hmc_mem, irdma->irdma_hmc_mem_size);
	irdma->irdma_hmc_mem = NULL;
	dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_obj_mem.size,
	    irdma->irdma_obj_mem.va, irdma->irdma_obj_mem.pa);
	irdma->irdma_obj_mem.va = NULL;
	return (ret == -ENOMEM ? ENOMEM : EIO);
}

static void
irdma_unstep_dev(irdma_t *irdma)
{
	if (irdma->irdma_hmc_mem != NULL) {
		kmem_free(irdma->irdma_hmc_mem, irdma->irdma_hmc_mem_size);
		irdma->irdma_hmc_mem = NULL;
	}
	dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_obj_mem.size,
	    irdma->irdma_obj_mem.va, irdma->irdma_obj_mem.pa);
	irdma->irdma_obj_mem.va = NULL;
}

static int
irdma_step_intr(irdma_t *irdma)
{
	ice_rdma_intr_t *in = &irdma->irdma_intr;
	uint_t i;
	int rc;

	for (i = 0; i < in->irin_count; i++) {
		rc = ddi_intr_add_handler(in->irin_handles[i], irdma_intr,
		    (caddr_t)irdma, (caddr_t)(uintptr_t)i);
		if (rc != DDI_SUCCESS)
			goto fail;
		irdma->irdma_intr_added = i + 1;
		rc = ddi_intr_enable(in->irin_handles[i]);
		if (rc != DDI_SUCCESS) {
			(void) ddi_intr_remove_handler(in->irin_handles[i]);
			irdma->irdma_intr_added = i;
			goto fail;
		}
	}
	return (0);

fail:
	irdma_error(irdma, "failed to set up RDMA vector %u: %d", i, rc);
	while (i-- > 0) {
		(void) ddi_intr_disable(in->irin_handles[i]);
		(void) ddi_intr_remove_handler(in->irin_handles[i]);
	}
	irdma->irdma_intr_added = 0;
	return (EIO);
}

static void
irdma_unstep_intr(irdma_t *irdma)
{
	ice_rdma_intr_t *in = &irdma->irdma_intr;
	uint_t i;

	irdma_intr_quiesce(irdma);
	for (i = 0; i < irdma->irdma_intr_added; i++) {
		if (ddi_intr_disable(in->irin_handles[i]) != DDI_SUCCESS ||
		    ddi_intr_remove_handler(in->irin_handles[i]) !=
		    DDI_SUCCESS)
			irdma_error(irdma, "failed to release RDMA vector %u",
			    i);
	}
	irdma->irdma_intr_added = 0;
}

static int
irdma_step_cqp(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_sc_cqp *cqp = &irdma->irdma_cqp;
	struct irdma_cqp_init_info info;
	struct irdma_dma_mem ctx;
	u32 sqsize = IRDMA_CQP_SW_SQSIZE_2048;
	u16 maj = 0, min = 0;
	int ret;

	irdma->irdma_cqp_scratch = kcalloc(sqsize, sizeof (u64), GFP_KERNEL);
	irdma->irdma_cqp_ooo = kcalloc(sqsize,
	    sizeof (struct irdma_ooo_cqp_op), GFP_KERNEL);
	if (irdma->irdma_cqp_scratch == NULL || irdma->irdma_cqp_ooo == NULL) {
		ret = ENOMEM;
		goto fail;
	}
	irdma->irdma_cqp_sq.size = ALIGN(sizeof (struct irdma_cqp_sq_wqe) *
	    sqsize, IRDMA_CQP_ALIGNMENT);
	irdma->irdma_cqp_sq.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    irdma->irdma_cqp_sq.size, &irdma->irdma_cqp_sq.pa, GFP_KERNEL);
	if (irdma->irdma_cqp_sq.va == NULL) {
		ret = ENOMEM;
		goto fail;
	}
	if (irdma_obj_mem(irdma, &ctx, sizeof (struct irdma_cqp_ctx),
	    IRDMA_HOST_CTX_ALIGNMENT_M) != 0) {
		ret = ENOMEM;
		goto fail;
	}

	dev->cqp = cqp;
	cqp->dev = dev;
	bzero(&info, sizeof (info));
	info.dev = dev;
	info.sq_size = sqsize;
	info.sq = irdma->irdma_cqp_sq.va;
	info.sq_pa = irdma->irdma_cqp_sq.pa;
	info.host_ctx_pa = ctx.pa;
	info.host_ctx = ctx.va;
	info.hmc_profile = IRDMA_HMC_PROFILE_DEFAULT;
	info.scratch_array = irdma->irdma_cqp_scratch;
	info.ooo_op_array = irdma->irdma_cqp_ooo;
	info.protocol_used = IRDMA_ROCE_PROTOCOL_ONLY;
	info.hw_maj_ver = IRDMA_CQPHC_HW_MAJVER_GEN_2;
	cqp->host_ctx_pa = ctx.pa;
	cqp->host_ctx = ctx.va;

	ret = irdma_sc_cqp_init(cqp, &info);
	if (ret != 0) {
		irdma_error(irdma, "CQP init failed: %d", ret);
		ret = EIO;
		goto fail;
	}
	ret = irdma_sc_cqp_create(cqp, &maj, &min);
	if (ret != 0) {
		irdma_error(irdma, "CQP create failed: %d (0x%x/0x%x)", ret,
		    maj, min);
		/* The device may hold the SQ; see irdma_osdep_fini(). */
		irdma_taint(irdma);
		ret = EIO;
		goto fail;
	}
	cqp->process_cqp_sds = irdma_cqp_sds_poll;
	atomic_and_32(&irdma->irdma_flags, ~IRDMA_F_CQP_DEAD);
	atomic_or_32(&irdma->irdma_flags, IRDMA_F_CQP_LIVE);
	return (0);

fail:
	if (irdma->irdma_cqp_sq.va != NULL) {
		dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_cqp_sq.size,
		    irdma->irdma_cqp_sq.va, irdma->irdma_cqp_sq.pa);
		irdma->irdma_cqp_sq.va = NULL;
	}
	kfree(irdma->irdma_cqp_ooo);
	kfree(irdma->irdma_cqp_scratch);
	irdma->irdma_cqp_ooo = NULL;
	irdma->irdma_cqp_scratch = NULL;
	dev->cqp = NULL;
	return (ret);
}

static void
irdma_unstep_cqp(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	boolean_t ok = irdma_hw_ok(irdma);

	irdma_cqp_fail_all(irdma);
	if (ok) {
		irdma_osdep_defer(irdma);
		ok = irdma_sc_cqp_destroy(&irdma->irdma_cqp) == 0;
		if (ok)
			atomic_and_32(&irdma->irdma_flags, ~IRDMA_F_CQP_LIVE);
		irdma_osdep_release(irdma, ok);
	}
	if (!ok)
		irdma_taint(irdma);

	dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_cqp_sq.size,
	    irdma->irdma_cqp_sq.va, irdma->irdma_cqp_sq.pa);
	irdma->irdma_cqp_sq.va = NULL;
	kfree(irdma->irdma_cqp_ooo);
	kfree(irdma->irdma_cqp_scratch);
	irdma->irdma_cqp_ooo = NULL;
	irdma->irdma_cqp_scratch = NULL;
	dev->cqp = NULL;
}

static int
irdma_step_fpm(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	int ret;

	dev->feature_info[IRDMA_FEATURE_FW_INFO] = IRDMA_FW_VER_DEFAULT;
	ret = irdma_get_rdma_features(dev);
	if (ret != 0) {
		irdma_error(irdma, "feature query failed: %d", ret);
		irdma_taint(irdma);
		return (EIO);
	}
	ret = irdma_cfg_fpm_val(dev, irdma->irdma_qp_limit);
	if (ret != 0) {
		irdma_error(irdma, "FPM configuration failed: %d", ret);
		irdma_taint(irdma);
		return (EIO);
	}
	return (0);
}

static void
irdma_unstep_fpm(irdma_t *irdma)
{
	struct irdma_hmc_info *hmc = irdma->irdma_sc.hmc_info;

	kfree(hmc->sd_table.sd_entry);
	hmc->sd_table.sd_entry = NULL;
}

static void
irdma_hmc_del(irdma_t *irdma, uint_t upto)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_hmc_info *hmc = dev->hmc_info;
	struct irdma_hmc_del_obj_info del;
	boolean_t reset = !irdma_hw_ok(irdma);
	uint_t i;

	if (reset)
		irdma_taint(irdma);
	for (i = 0; i < upto; i++) {
		if (hmc->hmc_obj[i].cnt == 0)
			continue;
		bzero(&del, sizeof (del));
		del.hmc_info = hmc;
		del.rsrc_type = i;
		del.count = hmc->hmc_obj[i].cnt;
		del.privileged = true;
		if (irdma_sc_del_hmc_obj(dev, &del, reset) != 0)
			irdma_taint(irdma);
	}
}

static int
irdma_step_hmc(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_hmc_info *hmc = dev->hmc_info;
	struct irdma_hmc_create_obj_info info;
	uint_t i;
	int ret = 0;

	for (i = 0; i < IRDMA_HMC_IW_MAX; i++) {
		if (i == IRDMA_HMC_IW_PBLE || hmc->hmc_obj[i].cnt == 0)
			continue;
		bzero(&info, sizeof (info));
		info.hmc_info = hmc;
		info.privileged = true;
		info.entry_type = IRDMA_SD_TYPE_DIRECT;
		info.rsrc_type = i;
		info.count = hmc->hmc_obj[i].cnt;
		ret = irdma_sc_create_hmc_obj(dev, &info);
		if (ret != 0) {
			irdma_error(irdma, "HMC object %u failed: %d", i, ret);
			break;
		}
	}
	if (ret == 0) {
		ret = irdma_sc_static_hmc_pages_allocated(dev->cqp, 0,
		    dev->hmc_fn_id, true, true);
		if (ret != 0)
			irdma_error(irdma, "HMC pages not accepted: %d", ret);
	}
	/* A type that failed has undone its own SDs. */
	if (ret != 0) {
		irdma_hmc_del(irdma, i);
		return (EIO);
	}
	return (0);
}

/*
 * The PBLE pool is released only here, after the SDs that map its pages
 * are cleared.
 */
static void
irdma_unstep_hmc(irdma_t *irdma)
{
	irdma_hmc_del(irdma, IRDMA_HMC_IW_MAX);
	if (irdma->irdma_pble_live) {
		irdma_destroy_pble_prm(irdma->irdma_pble);
		irdma->irdma_pble_live = B_FALSE;
	}
}

static int
irdma_step_ccq(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_ccq_init_info info;
	int ret;

	irdma->irdma_ccq_mem.size = ALIGN(sizeof (struct irdma_cqe) *
	    IRDMA_CCQ_SIZE, IRDMA_CQ0_ALIGNMENT);
	irdma->irdma_ccq_mem.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    irdma->irdma_ccq_mem.size, &irdma->irdma_ccq_mem.pa, GFP_KERNEL);
	if (irdma->irdma_ccq_mem.va == NULL)
		return (ENOMEM);
	if (irdma_obj_mem(irdma, &irdma->irdma_ccq_shadow,
	    sizeof (struct irdma_cq_shadow_area), IRDMA_SHADOWAREA_M) != 0) {
		ret = ENOMEM;
		goto fail;
	}

	dev->ccq = &irdma->irdma_ccq;
	dev->ccq->dev = dev;
	bzero(&info, sizeof (info));
	info.dev = dev;
	info.cq_base = irdma->irdma_ccq_mem.va;
	info.cq_pa = irdma->irdma_ccq_mem.pa;
	info.num_elem = IRDMA_CCQ_SIZE;
	info.shadow_area = irdma->irdma_ccq_shadow.va;
	info.shadow_area_pa = irdma->irdma_ccq_shadow.pa;
	info.ceqe_mask = false;
	info.ceq_id_valid = true;
	info.shadow_read_threshold = 16;
	/* The CQ create command names the PF's VSI. */
	irdma->irdma_vsi.vsi_idx = irdma->irdma_info.iri_vsi_num;
	info.vsi = &irdma->irdma_vsi;
	ret = irdma_sc_ccq_init(dev->ccq, &info);
	if (ret == 0) {
		mutex_enter(&irdma->irdma_ccq_lock);
		ret = irdma_sc_ccq_create(dev->ccq, 0, true, true);
		mutex_exit(&irdma->irdma_ccq_lock);
	}
	if (ret != 0) {
		irdma_error(irdma, "CCQ create failed: %d", ret);
		irdma_taint(irdma);
		ret = EIO;
		goto fail;
	}
	return (0);

fail:
	dev->ccq = NULL;
	dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_ccq_mem.size,
	    irdma->irdma_ccq_mem.va, irdma->irdma_ccq_mem.pa);
	irdma->irdma_ccq_mem.va = NULL;
	return (ret);
}

static void
irdma_unstep_ccq(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	int ret = -EIO;

	mutex_enter(&irdma->irdma_ccq_lock);
	if (irdma_hw_ok(irdma))
		ret = irdma_sc_ccq_destroy(dev->ccq, 0, true);
	if (ret != 0)
		irdma_taint(irdma);
	mutex_exit(&irdma->irdma_ccq_lock);
	/* The core put back its own polled SD update. */
	irdma->irdma_cqp.process_cqp_sds = irdma_cqp_sds_poll;
	dev->ccq = NULL;

	dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_ccq_mem.size,
	    irdma->irdma_ccq_mem.va, irdma->irdma_ccq_mem.pa);
	irdma->irdma_ccq_mem.va = NULL;
}

static int
irdma_step_ceq0(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_ceq_init_info info;
	u32 size;
	int ret;

	size = MIN(dev->hmc_info->hmc_obj[IRDMA_HMC_IW_CQ].cnt,
	    dev->hw_attrs.max_hw_ceq_size);
	size = MAX(size, dev->hw_attrs.min_hw_ceq_size);
	irdma->irdma_ceq0_mem.size = ALIGN(sizeof (struct irdma_ceqe) * size,
	    IRDMA_CEQ_ALIGNMENT);
	irdma->irdma_ceq0_mem.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    irdma->irdma_ceq0_mem.size, &irdma->irdma_ceq0_mem.pa, GFP_KERNEL);
	irdma->irdma_ceq0_reg = kcalloc(size, sizeof (struct irdma_sc_cq *),
	    GFP_KERNEL);
	irdma->irdma_ceq0_nreg = size;
	if (irdma->irdma_ceq0_mem.va == NULL || irdma->irdma_ceq0_reg == NULL) {
		ret = ENOMEM;
		goto fail;
	}

	bzero(&info, sizeof (info));
	info.ceq_id = 0;
	info.ceqe_base = irdma->irdma_ceq0_mem.va;
	info.ceqe_pa = irdma->irdma_ceq0_mem.pa;
	info.elem_cnt = size;
	info.dev = dev;
	info.vsi_idx = irdma->irdma_info.iri_vsi_num;
	info.reg_cq = irdma->irdma_ceq0_reg;
	ret = irdma_sc_ceq_init(&irdma->irdma_ceq0, &info);
	if (ret == 0) {
		ret = irdma_sc_add_cq_ctx(&irdma->irdma_ceq0,
		    &irdma->irdma_ccq);
	}
	if (ret == 0) {
		mutex_enter(&irdma->irdma_ccq_lock);
		ret = irdma_sc_cceq_create(&irdma->irdma_ceq0, 0);
		mutex_exit(&irdma->irdma_ccq_lock);
		if (ret != 0)
			irdma_taint(irdma);
	}
	if (ret != 0) {
		irdma_error(irdma, "CEQ 0 create failed: %d", ret);
		ret = EIO;
		goto fail;
	}

	dev->irq_ops->irdma_cfg_ceq(dev, 0,
	    irdma_hw_vec(irdma, irdma->irdma_ceq_vec), true);
	dev->ceq_valid = true;
	mutex_enter(&irdma->irdma_ccq_lock);
	irdma_sc_ccq_arm(&irdma->irdma_ccq);
	mutex_exit(&irdma->irdma_ccq_lock);
	irdma_vec_enable(irdma, irdma->irdma_ceq_vec);
	return (0);

fail:
	if (irdma->irdma_ceq0_mem.va != NULL) {
		dma_free_coherent(&irdma->irdma_osdev,
		    irdma->irdma_ceq0_mem.size, irdma->irdma_ceq0_mem.va,
		    irdma->irdma_ceq0_mem.pa);
		irdma->irdma_ceq0_mem.va = NULL;
	}
	kfree(irdma->irdma_ceq0_reg);
	irdma->irdma_ceq0_reg = NULL;
	return (ret);
}

static void
irdma_unstep_ceq0(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_sc_ceq *ceq = &irdma->irdma_ceq0;
	int ret = -EIO;

	dev->irq_ops->irdma_cfg_ceq(dev, 0,
	    irdma_hw_vec(irdma, irdma->irdma_ceq_vec), false);
	irdma_vec_disable(irdma, irdma->irdma_ceq_vec);
	irdma_intr_quiesce(irdma);

	mutex_enter(&irdma->irdma_ccq_lock);
	if (irdma_hw_ok(irdma)) {
		ret = irdma_sc_ceq_destroy(ceq, 0, true);
		if (ret == 0)
			ret = irdma_sc_cceq_destroy_done(ceq);
	}
	if (ret != 0)
		irdma_taint(irdma);
	mutex_exit(&irdma->irdma_ccq_lock);
	dev->ceq_valid = false;

	dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_ceq0_mem.size,
	    irdma->irdma_ceq0_mem.va, irdma->irdma_ceq0_mem.pa);
	irdma->irdma_ceq0_mem.va = NULL;
	kfree(irdma->irdma_ceq0_reg);
	irdma->irdma_ceq0_reg = NULL;
}

static int
irdma_step_aeq(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_hmc_info *hmc = dev->hmc_info;
	struct irdma_aeq_init_info info;
	u32 size;
	int ret;

	size = hmc->hmc_obj[IRDMA_HMC_IW_QP].cnt +
	    hmc->hmc_obj[IRDMA_HMC_IW_CQ].cnt;
	size = MIN(size, dev->hw_attrs.max_hw_aeq_size);
	size = MAX(size, dev->hw_attrs.min_hw_aeq_size);
	irdma->irdma_aeq_mem.size = ALIGN(sizeof (struct irdma_sc_aeqe) *
	    size, IRDMA_AEQ_ALIGNMENT);
	irdma->irdma_aeq_mem.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    irdma->irdma_aeq_mem.size, &irdma->irdma_aeq_mem.pa, GFP_KERNEL);
	if (irdma->irdma_aeq_mem.va == NULL)
		return (ENOMEM);

	bzero(&info, sizeof (info));
	info.aeqe_base = irdma->irdma_aeq_mem.va;
	info.aeq_elem_pa = irdma->irdma_aeq_mem.pa;
	info.elem_cnt = size;
	info.dev = dev;
	info.msix_idx = irdma_hw_vec(irdma, irdma->irdma_aeq_vec);
	ret = irdma_sc_aeq_init(&irdma->irdma_aeq, &info);
	if (ret == 0) {
		ret = irdma_cqp_aeq_cmd(dev, &irdma->irdma_aeq,
		    IRDMA_OP_AEQ_CREATE);
		if (ret != 0)
			irdma_taint(irdma);
	}
	if (ret != 0) {
		irdma_error(irdma, "AEQ create failed: %d", ret);
		dma_free_coherent(&irdma->irdma_osdev,
		    irdma->irdma_aeq_mem.size, irdma->irdma_aeq_mem.va,
		    irdma->irdma_aeq_mem.pa);
		irdma->irdma_aeq_mem.va = NULL;
		return (EIO);
	}

	dev->irq_ops->irdma_cfg_aeq(dev, info.msix_idx, true);
	if (irdma->irdma_aeq_vec != irdma->irdma_ceq_vec)
		irdma_vec_enable(irdma, irdma->irdma_aeq_vec);
	return (0);
}

static void
irdma_unstep_aeq(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	int ret = -EIO;

	dev->irq_ops->irdma_cfg_aeq(dev,
	    irdma_hw_vec(irdma, irdma->irdma_aeq_vec), false);
	if (irdma->irdma_aeq_vec != irdma->irdma_ceq_vec)
		irdma_vec_disable(irdma, irdma->irdma_aeq_vec);

	if (irdma_hw_ok(irdma)) {
		irdma->irdma_aeq.size = 0;
		ret = irdma_cqp_aeq_cmd(dev, &irdma->irdma_aeq,
		    IRDMA_OP_AEQ_DESTROY);
	}
	if (ret != 0)
		irdma_taint(irdma);

	dma_free_coherent(&irdma->irdma_osdev, irdma->irdma_aeq_mem.size,
	    irdma->irdma_aeq_mem.va, irdma->irdma_aeq_mem.pa);
	irdma->irdma_aeq_mem.va = NULL;
}

static int
irdma_step_pble(irdma_t *irdma)
{
	int ret;

	ret = irdma_hmc_init_pble(&irdma->irdma_sc, irdma->irdma_pble);
	if (ret != 0) {
		irdma_error(irdma, "PBLE init failed: %d", ret);
		return (EIO);
	}
	irdma->irdma_pble_live = B_TRUE;
	return (0);
}

/* irdma_unstep_hmc() releases the pool. */
static void
irdma_unstep_pble(irdma_t *irdma)
{
	_NOTE(ARGUNUSED(irdma));
}

static int
irdma_register_qset(struct irdma_sc_vsi *vsi, struct irdma_ws_node *node)
{
	irdma_t *irdma = vsi->back_vsi;
	ice_rdma_qset_t qs;
	int ret;

	bzero(&qs, sizeof (qs));
	qs.irqs_handle = node->qs_handle;
	qs.irqs_vsi_num = irdma->irdma_info.iri_vsi_num;
	qs.irqs_tc = node->traffic_class;
	ret = irdma->irdma_ops->iro_qset_add(irdma->irdma_peer, &qs, 1);
	if (ret != 0) {
		irdma_error(irdma, "qset add failed: %d", ret);
		return (-EIO);
	}
	node->l2_sched_node_id = qs.irqs_teid;
	vsi->qos[node->user_pri].l2_sched_node_id = qs.irqs_teid;
	return (0);
}

static void
irdma_unregister_qset(struct irdma_sc_vsi *vsi, struct irdma_ws_node *node)
{
	irdma_t *irdma = vsi->back_vsi;
	ice_rdma_qset_t qs;
	int ret;

	bzero(&qs, sizeof (qs));
	qs.irqs_handle = node->qs_handle;
	qs.irqs_vsi_num = irdma->irdma_info.iri_vsi_num;
	qs.irqs_tc = node->traffic_class;
	qs.irqs_teid = node->l2_sched_node_id;
	ret = irdma->irdma_ops->iro_qset_del(irdma->irdma_peer, &qs, 1);
	if (ret != 0 && irdma_hw_ok(irdma))
		irdma_error(irdma, "qset delete failed: %d", ret);
}

static int
irdma_step_ws(irdma_t *irdma)
{
	struct irdma_l2params *l2 = &irdma->irdma_l2;
	struct irdma_vsi_init_info info;
	const ice_rdma_qos_t *qos = &irdma->irdma_info.iri_qos;
	uint_t i;
	int ret;

	bzero(l2, sizeof (*l2));
	l2->mtu = (u16)MIN(irdma->irdma_info.iri_mtu, UINT16_MAX);
	l2->num_tc = MAX(qos->irq_num_tc, 1);
	for (i = 0; i < IRDMA_MAX_USER_PRIORITY; i++) {
		/* ice programs TC0 only; anything else is not ours to use. */
		l2->up2tc[i] = 0;
		l2->tc_info[i].rel_bw = qos->irq_tc[i].irt_rel_bw;
		l2->tc_info[i].prio_type = qos->irq_tc[i].irt_prio_type;
	}

	irdma->irdma_ws_max = IRDMA_MAX_WS_NODES;
	irdma->irdma_ws_ids = kmem_zalloc(BT_SIZEOFMAP(irdma->irdma_ws_max),
	    KM_SLEEP);
	BT_SET(irdma->irdma_ws_ids, 0);

	bzero(&info, sizeof (info));
	info.dev = &irdma->irdma_sc;
	info.back_vsi = irdma;
	info.params = l2;
	info.pf_data_vsi_num = irdma->irdma_info.iri_vsi_num;
	info.exception_lan_q = 2;
	info.register_qset = irdma_register_qset;
	info.unregister_qset = irdma_unregister_qset;
	irdma_sc_vsi_init(&irdma->irdma_vsi, &info);

	ret = irdma_ws_add(&irdma->irdma_vsi, 0);
	if (ret != 0) {
		irdma_error(irdma, "work scheduler setup failed: %d", ret);
		kmem_free(irdma->irdma_ws_ids,
		    BT_SIZEOFMAP(irdma->irdma_ws_max));
		irdma->irdma_ws_ids = NULL;
		return (EIO);
	}
	return (0);
}

static void
irdma_unstep_ws(irdma_t *irdma)
{
	irdma_ws_remove(&irdma->irdma_vsi, 0);
	kmem_free(irdma->irdma_ws_ids, BT_SIZEOFMAP(irdma->irdma_ws_max));
	irdma->irdma_ws_ids = NULL;
}

static int
irdma_step_pefltr(irdma_t *irdma)
{
	int ret;

	ret = irdma->irdma_ops->iro_pe_filter(irdma->irdma_peer, B_TRUE);
	if (ret != 0) {
		irdma_error(irdma, "PE filter enable failed: %d", ret);
		return (EIO);
	}
	return (0);
}

static void
irdma_unstep_pefltr(irdma_t *irdma)
{
	if (irdma_hw_ok(irdma))
		(void) irdma->irdma_ops->iro_pe_filter(irdma->irdma_peer,
		    B_FALSE);
}

static const struct {
	int	(*is_up)(irdma_t *);
	void	(*is_down)(irdma_t *);
} irdma_steps[IRDMA_STEP_MAX] = {
	[IRDMA_STEP_OPEN] = { NULL, NULL },
	[IRDMA_STEP_DEV] = { irdma_step_dev, irdma_unstep_dev },
	[IRDMA_STEP_INTR] = { irdma_step_intr, irdma_unstep_intr },
	[IRDMA_STEP_CQP] = { irdma_step_cqp, irdma_unstep_cqp },
	[IRDMA_STEP_FPM] = { irdma_step_fpm, irdma_unstep_fpm },
	[IRDMA_STEP_HMC] = { irdma_step_hmc, irdma_unstep_hmc },
	[IRDMA_STEP_CCQ] = { irdma_step_ccq, irdma_unstep_ccq },
	[IRDMA_STEP_CEQ0] = { irdma_step_ceq0, irdma_unstep_ceq0 },
	[IRDMA_STEP_AEQ] = { irdma_step_aeq, irdma_unstep_aeq },
	[IRDMA_STEP_PBLE] = { irdma_step_pble, irdma_unstep_pble },
	[IRDMA_STEP_WS] = { irdma_step_ws, irdma_unstep_ws },
	[IRDMA_STEP_PEFLTR] = { irdma_step_pefltr, irdma_unstep_pefltr }
};

/*
 * Run the steps after OPEN.  A DEBUG build fails the step named by the
 * fail_step property, counting OPEN as 1, before running it.
 */
int
irdma_ctl_start(irdma_t *irdma)
{
	uint_t s;
	int ret;

	ASSERT(MUTEX_HELD(&irdma->irdma_cfg_lock));

	/* One vector serves both queues; with more, CEQ 0 has its own. */
	irdma->irdma_aeq_vec = 0;
	irdma->irdma_ceq_vec = irdma->irdma_intr.irin_count > 1 ? 1 : 0;

	for (s = IRDMA_STEP_DEV; s < IRDMA_STEP_MAX; s++) {
#ifdef DEBUG
		if (irdma->irdma_fail_step == s + 1) {
			irdma_error(irdma, "injected failure at step %s",
			    irdma_step_names[s]);
			ret = EIO;
			goto fail;
		}
#endif
		ret = irdma_steps[s].is_up(irdma);
		if (ret != 0) {
			irdma_error(irdma, "bring-up failed at step %s: %d",
			    irdma_step_names[s], ret);
			goto fail;
		}
		irdma->irdma_progress |= BIT(s);
	}
	return (0);

fail:
	irdma_ctl_stop(irdma);
	return (ret);
}

void
irdma_ctl_stop(irdma_t *irdma)
{
	uint_t s;

	ASSERT(MUTEX_HELD(&irdma->irdma_cfg_lock));

	for (s = IRDMA_STEP_MAX; s-- > IRDMA_STEP_DEV; ) {
		if ((irdma->irdma_progress & BIT(s)) == 0)
			continue;
		irdma_steps[s].is_down(irdma);
		irdma->irdma_progress &= ~BIT(s);
	}
}

void
irdma_ctl_hold_release(irdma_t *irdma)
{
	mutex_enter(&irdma->irdma_ccq_lock);
	irdma->irdma_hold_cqes = B_FALSE;
	mutex_exit(&irdma->irdma_ccq_lock);
	irdma_ccq_poll(irdma);
}

/*
 * The iWARP and VF interfaces of the core code.  This driver runs RoCEv2 on
 * a PF, so none of them is reached; each fails or does nothing.
 */
void
irdma_ieq_mpa_crc_ae(struct irdma_sc_dev *dev, struct irdma_sc_qp *qp)
{
	_NOTE(ARGUNUSED(dev, qp));
}

int
irdma_ieq_check_mpacrc(const void *addr, u32 len, u32 val)
{
	_NOTE(ARGUNUSED(addr, len, val));
	return (-EINVAL);
}

struct irdma_sc_qp *
irdma_ieq_get_qp(struct irdma_sc_dev *dev, struct irdma_puda_buf *buf)
{
	_NOTE(ARGUNUSED(dev, buf));
	return (NULL);
}

void
irdma_send_ieq_ack(struct irdma_sc_qp *qp)
{
	_NOTE(ARGUNUSED(qp));
}

void
irdma_ieq_update_tcpip_info(struct irdma_puda_buf *buf, u16 len, u32 seq)
{
	_NOTE(ARGUNUSED(buf, len, seq));
}

int
irdma_puda_get_tcpip_info(struct irdma_puda_cmpl_info *info,
    struct irdma_puda_buf *buf)
{
	_NOTE(ARGUNUSED(info, buf));
	return (-EINVAL);
}

void
irdma_term_modify_qp(struct irdma_sc_qp *qp, u8 next, u8 term, u8 term_len)
{
	_NOTE(ARGUNUSED(qp, next, term, term_len));
}

void
irdma_terminate_done(struct irdma_sc_qp *qp, int timeout)
{
	_NOTE(ARGUNUSED(qp, timeout));
}

void
irdma_terminate_start_timer(struct irdma_sc_qp *qp)
{
	_NOTE(ARGUNUSED(qp));
}

void
irdma_terminate_del_timer(struct irdma_sc_qp *qp)
{
	_NOTE(ARGUNUSED(qp));
}

int
irdma_cqp_qp_create_cmd(struct irdma_sc_dev *dev, struct irdma_sc_qp *qp)
{
	_NOTE(ARGUNUSED(dev, qp));
	return (-EOPNOTSUPP);
}

int
irdma_cqp_cq_create_cmd(struct irdma_sc_dev *dev, struct irdma_sc_cq *cq)
{
	_NOTE(ARGUNUSED(dev, cq));
	return (-EOPNOTSUPP);
}

int
irdma_cqp_qp_destroy_cmd(struct irdma_sc_dev *dev, struct irdma_sc_qp *qp)
{
	_NOTE(ARGUNUSED(dev, qp));
	return (-EOPNOTSUPP);
}

void
irdma_cqp_cq_destroy_cmd(struct irdma_sc_dev *dev, struct irdma_sc_cq *cq)
{
	_NOTE(ARGUNUSED(dev, cq));
}

int
irdma_cqp_qp_suspend_resume(struct irdma_sc_qp *qp, u8 cmd)
{
	_NOTE(ARGUNUSED(qp, cmd));
	return (-EOPNOTSUPP);
}

void
irdma_modify_qp_to_err(struct irdma_sc_qp *qp)
{
	_NOTE(ARGUNUSED(qp));
}

void
irdma_reinitialize_ieq(struct irdma_sc_vsi *vsi)
{
	_NOTE(ARGUNUSED(vsi));
}

void
irdma_puda_ieq_get_ah_info(struct irdma_sc_qp *qp, struct irdma_ah_info *ah)
{
	_NOTE(ARGUNUSED(qp, ah));
}

int
irdma_puda_create_ah(struct irdma_sc_dev *dev, struct irdma_ah_info *info,
    bool wait, enum puda_rsrc_type type, void *cb_param,
    struct irdma_sc_ah **ah)
{
	_NOTE(ARGUNUSED(dev, info, wait, type, cb_param));
	*ah = NULL;
	return (-EOPNOTSUPP);
}

void
irdma_puda_free_ah(struct irdma_sc_dev *dev, struct irdma_sc_ah *ah)
{
	_NOTE(ARGUNUSED(dev, ah));
}

int
irdma_vchnl_req_get_hmc_fcn(struct irdma_sc_dev *dev)
{
	_NOTE(ARGUNUSED(dev));
	return (-EOPNOTSUPP);
}

int
irdma_vchnl_req_get_reg_layout(struct irdma_sc_dev *dev)
{
	_NOTE(ARGUNUSED(dev));
	return (-EOPNOTSUPP);
}

void
i40iw_init_hw(struct irdma_sc_dev *dev)
{
	_NOTE(ARGUNUSED(dev));
}

void
ig3rdma_init_hw(struct irdma_sc_dev *dev)
{
	_NOTE(ARGUNUSED(dev));
}

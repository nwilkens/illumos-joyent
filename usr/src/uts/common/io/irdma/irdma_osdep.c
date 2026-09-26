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
 * The services osdep.h promises the core code: memory, DMA through ice,
 * register access, messages, and the checks on firmware FPM data.
 */

#include <sys/varargs.h>
#include <sys/cpuvar.h>

#include "irdma_impl.h"

/*
 * Memory from kzalloc() carries its size in a header so that kfree() can
 * return it.  The header keeps the 16 byte alignment of kmem_alloc().
 */
#define	IRDMA_KM_HDR		16
#define	IRDMA_KM_MAX		(64 * 1024 * 1024)

void *
kzalloc(size_t size, gfp_t flags)
{
	size_t *p;

	_NOTE(ARGUNUSED(flags));
	if (size == 0 || size > IRDMA_KM_MAX)
		return (NULL);
	p = kmem_zalloc(size + IRDMA_KM_HDR, KM_NOSLEEP);
	if (p == NULL)
		return (NULL);
	*p = size;
	return ((char *)p + IRDMA_KM_HDR);
}

void *
kcalloc(size_t n, size_t size, gfp_t flags)
{
	if (size != 0 && n > IRDMA_KM_MAX / size)
		return (NULL);
	return (kzalloc(n * size, flags));
}

void
kfree(const void *va)
{
	size_t *p;

	if (va == NULL)
		return;
	p = (size_t *)(void *)((char *)va - IRDMA_KM_HDR);
	kmem_free(p, *p + IRDMA_KM_HDR);
}

void
irdma_osdep_mutex_init(kmutex_t *m)
{
	(mutex_init)(m, NULL, MUTEX_DRIVER, NULL);
}

/*
 * DMA.  Each buffer comes from ice, which frees it only once the device can
 * no longer reach it.
 */
typedef struct irdma_osbuf {
	list_node_t	iob_node;
	void		*iob_va;
	dma_addr_t	iob_pa;
	size_t		iob_len;
	ice_rdma_dma_t	*iob_dma;
} irdma_osbuf_t;

void
irdma_osdep_init(irdma_t *irdma)
{
	struct device *od = &irdma->irdma_osdev;

	od->od_irdma = irdma;
	(mutex_init)(&od->od_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&od->od_bufs, sizeof (irdma_osbuf_t),
	    offsetof(irdma_osbuf_t, iob_node));
	list_create(&od->od_deferred, sizeof (irdma_osbuf_t),
	    offsetof(irdma_osbuf_t, iob_node));
	irdma->irdma_ibdev.ib_dip = irdma->irdma_dip;
	irdma->irdma_hw.device = od;
}

/*
 * Hand every buffer still held back to ice.  The caller has closed the
 * device, so none of them is freed while the device may use it unless ice
 * knows that it cannot.
 */
void
irdma_osdep_fini(irdma_t *irdma)
{
	struct device *od = &irdma->irdma_osdev;
	boolean_t quiesced = irdma_quiesced(irdma);
	irdma_osbuf_t *b;

	mutex_enter(&od->od_lock);
	list_move_tail(&od->od_bufs, &od->od_deferred);
	while ((b = list_remove_head(&od->od_bufs)) != NULL) {
		od->od_nbufs--;
		mutex_exit(&od->od_lock);
		irdma->irdma_ops->iro_dma_free(irdma->irdma_peer, b->iob_dma,
		    quiesced);
		kmem_free(b, sizeof (*b));
		mutex_enter(&od->od_lock);
	}
	mutex_exit(&od->od_lock);
	list_destroy(&od->od_deferred);
	list_destroy(&od->od_bufs);
	mutex_destroy(&od->od_lock);
}

/*
 * Whether a buffer freed now is unreachable by the device: nothing went
 * wrong, and the CQP is gone or has no command outstanding.
 */
boolean_t
irdma_quiesced(irdma_t *irdma)
{
	struct irdma_sc_cqp *cqp = &irdma->irdma_cqp;

	if ((irdma->irdma_flags & IRDMA_F_TAINTED) != 0)
		return (B_FALSE);
	if (irdma->irdma_ops->iro_resetting(irdma->irdma_peer))
		return (B_FALSE);
	if ((irdma->irdma_flags & IRDMA_F_CQP_LIVE) == 0)
		return (B_TRUE);
	return (cqp->requested_ops ==
	    (u64)atomic64_read(&cqp->completed_ops));
}

void
irdma_taint(irdma_t *irdma)
{
	atomic_or_32(&irdma->irdma_flags, IRDMA_F_TAINTED);
}

/*
 * Some core calls free DMA memory before they return whether the device let
 * go of it (irdma_sc_cqp_destroy()).  Between irdma_osdep_defer() and
 * irdma_osdep_release() such frees are held, and the caller releases them
 * once it knows.
 */
void
irdma_osdep_defer(irdma_t *irdma)
{
	atomic_or_32(&irdma->irdma_flags, IRDMA_F_DEFER);
}

void
irdma_osdep_release(irdma_t *irdma, boolean_t ok)
{
	struct device *od = &irdma->irdma_osdev;
	boolean_t quiesced;
	irdma_osbuf_t *b;

	atomic_and_32(&irdma->irdma_flags, ~IRDMA_F_DEFER);
	if (!ok)
		irdma_taint(irdma);
	quiesced = irdma_quiesced(irdma);

	mutex_enter(&od->od_lock);
	while ((b = list_remove_head(&od->od_deferred)) != NULL) {
		mutex_exit(&od->od_lock);
		irdma->irdma_ops->iro_dma_free(irdma->irdma_peer, b->iob_dma,
		    quiesced);
		kmem_free(b, sizeof (*b));
		mutex_enter(&od->od_lock);
	}
	mutex_exit(&od->od_lock);
}

/* Linux gives natural alignment up to the page order of the size. */
static size_t
irdma_dma_align(size_t size)
{
	size_t align = (size_t)roundup_pow_of_two(size);

	return (MAX(MIN(align, SZ_2M), PAGESIZE));
}

void *
dma_alloc_coherent(struct device *od, size_t size, dma_addr_t *pa, gfp_t flags)
{
	irdma_t *irdma = od->od_irdma;
	ice_rdma_dma_t *dma;
	irdma_osbuf_t *b;

	_NOTE(ARGUNUSED(flags));
	*pa = 0;
	if (size == 0)
		return (NULL);

	if (irdma->irdma_ops->iro_dma_alloc(irdma->irdma_peer, size,
	    irdma_dma_align(size), &dma) != 0)
		return (NULL);
	b = kmem_zalloc(sizeof (*b), KM_SLEEP);
	b->iob_va = dma->ird_va;
	b->iob_pa = dma->ird_pa;
	b->iob_len = size;
	b->iob_dma = dma;

	mutex_enter(&od->od_lock);
	list_insert_tail(&od->od_bufs, b);
	od->od_nbufs++;
	mutex_exit(&od->od_lock);

	*pa = dma->ird_pa;
	return (dma->ird_va);
}

static void
irdma_dma_free(struct device *od, size_t size, void *va, dma_addr_t pa,
    boolean_t consumer)
{
	irdma_t *irdma = od->od_irdma;
	irdma_osbuf_t *b;
	boolean_t quiesced;

	if (va == NULL)
		return;

	mutex_enter(&od->od_lock);
	for (b = list_head(&od->od_bufs); b != NULL;
	    b = list_next(&od->od_bufs, b)) {
		if (b->iob_va == va)
			break;
	}
	if (b == NULL || b->iob_pa != pa || b->iob_len != size) {
		mutex_exit(&od->od_lock);
		irdma_error(irdma, "free of an unknown DMA buffer %p", va);
		return;
	}
	list_remove(&od->od_bufs, b);
	od->od_nbufs--;
	if (!consumer && (irdma->irdma_flags & IRDMA_F_DEFER) != 0) {
		list_insert_tail(&od->od_deferred, b);
		mutex_exit(&od->od_lock);
		return;
	}
	mutex_exit(&od->od_lock);

	/*
	 * The device reaches a consumer buffer only through an MR or QP that
	 * the consumer destroyed first, so a pending control command does not
	 * matter for it, only a device that failed to confirm the destroy.
	 */
	quiesced = consumer ? irdma_healthy(irdma) : irdma_quiesced(irdma);
	irdma->irdma_ops->iro_dma_free(irdma->irdma_peer, b->iob_dma, quiesced);
	kmem_free(b, sizeof (*b));
}

void
dma_free_coherent(struct device *od, size_t size, void *va, dma_addr_t pa)
{
	irdma_dma_free(od, size, va, pa, B_FALSE);
}

void
irdma_osdep_free_consumer(irdma_t *irdma, void *va, uint64_t pa, size_t size)
{
	irdma_dma_free(&irdma->irdma_osdev, size, va, pa, B_TRUE);
}

/* Streaming DMA serves only the iWARP exception queues, not supported. */
dma_addr_t
dma_map_single(struct device *od, void *va, size_t size,
    enum dma_data_direction dir)
{
	_NOTE(ARGUNUSED(od, va, size, dir));
	return (0);
}

void
dma_unmap_single(struct device *od, dma_addr_t pa, size_t size,
    enum dma_data_direction dir)
{
	_NOTE(ARGUNUSED(od, pa, size, dir));
}

int
dma_mapping_error(struct device *od, dma_addr_t pa)
{
	_NOTE(ARGUNUSED(od));
	return (pa == 0);
}

void
dma_sync_single_for_cpu(struct device *od, dma_addr_t pa, size_t size,
    enum dma_data_direction dir)
{
	_NOTE(ARGUNUSED(od, pa, size, dir));
}

void
dma_sync_single_for_device(struct device *od, dma_addr_t pa, size_t size,
    enum dma_data_direction dir)
{
	_NOTE(ARGUNUSED(od, pa, size, dir));
}

int
irdma_map_vm_page_list(struct irdma_hw *hw, void *va, dma_addr_t *pg_dma,
    u32 pg_cnt)
{
	_NOTE(ARGUNUSED(hw, va, pg_dma, pg_cnt));
	return (-ENOMEM);
}

void
irdma_unmap_vm_page_list(struct irdma_hw *hw, dma_addr_t *pg_dma, u32 pg_cnt)
{
	_NOTE(ARGUNUSED(hw, pg_dma, pg_cnt));
}

/*
 * Registers.  The core code keeps plain addresses into BAR0, so readl() and
 * writel() find the mapping of the RDMA function that holds the address,
 * and with it the access handle; an address outside every mapping is
 * refused.  A function's slot is filled at attach before its first register
 * access and emptied at detach after its control plane is stopped, so the
 * handle a lookup finds stays valid while the function uses it.  Lookups
 * take no lock: slots change only between two increments of irdma_regs_gen,
 * and a lookup that sees it odd or changed looks again.
 */
#define	IRDMA_REGS_MAX		16

typedef struct irdma_regmap {
	caddr_t			irm_base;
	size_t			irm_len;
	ddi_acc_handle_t	irm_handle;
	caddr_t			irm_sqdb;
	caddr_t			irm_cqarm;
	irdma_dbstat_t		*irm_stats;
} irdma_regmap_t;

static kmutex_t irdma_regs_lock;	/* writers */
static volatile uint64_t irdma_regs_gen;
static irdma_regmap_t irdma_regs[IRDMA_REGS_MAX];

void
irdma_osdep_regs_init(void)
{
	mutex_init(&irdma_regs_lock, NULL, MUTEX_DRIVER, NULL);
}

void
irdma_osdep_regs_fini(void)
{
	mutex_destroy(&irdma_regs_lock);
}

static void
irdma_regs_change(void)
{
	ASSERT(MUTEX_HELD(&irdma_regs_lock));
	membar_producer();
	irdma_regs_gen++;
	membar_producer();
}

boolean_t
irdma_osdep_regs_add(caddr_t base, size_t len, ddi_acc_handle_t h)
{
	uint_t i;

	mutex_enter(&irdma_regs_lock);
	for (i = 0; i < IRDMA_REGS_MAX; i++) {
		if (irdma_regs[i].irm_base == NULL) {
			irdma_regs_change();
			irdma_regs[i].irm_base = base;
			irdma_regs[i].irm_len = len;
			irdma_regs[i].irm_handle = h;
			irdma_regs_change();
			break;
		}
	}
	mutex_exit(&irdma_regs_lock);
	return (i < IRDMA_REGS_MAX);
}

/* Count writes to the send queue doorbell and the CQ arm register. */
void
irdma_osdep_regs_dbs(caddr_t base, caddr_t sqdb, caddr_t cqarm,
    irdma_dbstat_t *stats)
{
	uint_t i;

	mutex_enter(&irdma_regs_lock);
	for (i = 0; i < IRDMA_REGS_MAX; i++) {
		if (irdma_regs[i].irm_base == base) {
			irdma_regs_change();
			irdma_regs[i].irm_sqdb = sqdb;
			irdma_regs[i].irm_cqarm = cqarm;
			irdma_regs[i].irm_stats = stats;
			irdma_regs_change();
		}
	}
	mutex_exit(&irdma_regs_lock);
}

void
irdma_osdep_regs_remove(caddr_t base)
{
	uint_t i;

	mutex_enter(&irdma_regs_lock);
	for (i = 0; i < IRDMA_REGS_MAX; i++) {
		if (irdma_regs[i].irm_base == base) {
			irdma_regs_change();
			bzero(&irdma_regs[i], sizeof (irdma_regs[i]));
			irdma_regs_change();
		}
	}
	mutex_exit(&irdma_regs_lock);
}

/* Copy the slot that maps addr into rm. */
static boolean_t
irdma_regs_find(const volatile void *addr, irdma_regmap_t *rm)
{
	uintptr_t a = (uintptr_t)addr, base;
	boolean_t found;
	uint64_t gen;
	uint_t i;

	if ((a & 3) != 0)
		return (B_FALSE);
	do {
		while (((gen = irdma_regs_gen) & 1) != 0)
			;
		membar_consumer();
		found = B_FALSE;
		for (i = 0; i < IRDMA_REGS_MAX; i++) {
			base = (uintptr_t)irdma_regs[i].irm_base;
			if (base != 0 && a >= base && a - base <=
			    irdma_regs[i].irm_len - sizeof (uint32_t)) {
				*rm = irdma_regs[i];
				found = B_TRUE;
				break;
			}
		}
		membar_consumer();
	} while (gen != irdma_regs_gen);
	return (found);
}

static void
irdma_regs_count(const irdma_regmap_t *rm, const volatile void *addr)
{
	irdma_dbstat_t *st;

	if (rm->irm_stats == NULL ||
	    (addr != rm->irm_sqdb && addr != rm->irm_cqarm))
		return;
	kpreempt_disable();
	st = &rm->irm_stats[CPU->cpu_seqid];
	if (addr == rm->irm_sqdb)
		st->ids_sq_doorbells++;
	else
		st->ids_cq_arms++;
	kpreempt_enable();
}

u32
readl(const volatile void *addr)
{
	irdma_regmap_t rm;

	if (!irdma_regs_find(addr, &rm)) {
		cmn_err(CE_WARN, "!irdma: read of an unmapped address %p",
		    (void *)addr);
		return (UINT32_MAX);
	}
	return (ddi_get32(rm.irm_handle, (uint32_t *)(uintptr_t)addr));
}

void
writel(u32 v, volatile void *addr)
{
	irdma_regmap_t rm;

	if (!irdma_regs_find(addr, &rm)) {
		cmn_err(CE_WARN, "!irdma: write of an unmapped address %p",
		    (void *)addr);
		return;
	}
	ddi_put32(rm.irm_handle, (uint32_t *)(uintptr_t)addr, v);
	irdma_regs_count(&rm, addr);
}

void
wr32(struct irdma_hw *hw, u32 reg, u32 val)
{
	writel(val, hw->hw_addr + reg);
}

u32
rd32(struct irdma_hw *hw, u32 reg)
{
	return (readl(hw->hw_addr + reg));
}

u64
rd64(struct irdma_hw *hw, u32 reg)
{
	return ((u64)readl(hw->hw_addr + reg) |
	    ((u64)readl(hw->hw_addr + reg + 4) << 32));
}

/*
 * Messages from the core code go to the system log only when irdma_debug is
 * set.
 */
int irdma_debug = 0;

struct ib_device *
to_ibdev(struct irdma_sc_dev *dev)
{
	return (&IRDMA_FROM_DEV(dev)->irdma_ibdev);
}

void
irdma_osdep_debug(struct ib_device *ibdev, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	if (irdma_debug == 0)
		return;
	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);
	if (ibdev != NULL && ibdev->ib_dip != NULL)
		dev_err(ibdev->ib_dip, CE_NOTE, "!%s", buf);
	else
		cmn_err(CE_NOTE, "!irdma: %s", buf);
}

/*
 * Firmware FPM data.  The core code sizes host memory, divides and loops on
 * these values, so each is bounded first.  The bounds are wide of any E810
 * firmware and small enough that the core arithmetic cannot overflow for
 * the QP counts this driver asks for.
 */
#define	IRDMA_FPM_MAX_OBJ_SIZE	(1U << 20)
#define	IRDMA_FPM_MAX_CNT	(1U << 28)
#define	IRDMA_FPM_MAX_BLOCK	(1U << 12)
#define	IRDMA_FPM_MAX_HT_MULT	16
#define	IRDMA_FPM_MAX_TIMER	(1U << 16)
/* irdma_q1_cnt() of one QP; below it the core would loop forever. */
#define	IRDMA_FPM_MIN_Q1	1024
#define	IRDMA_FPM_MIN_PBLE	1024

int
irdma_osdep_fpm_query_check(struct irdma_sc_dev *dev,
    struct irdma_hmc_info *hmc, struct irdma_hmc_fpm_misc *misc)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);
	struct irdma_hmc_obj_info *o = hmc->hmc_obj;
	uint_t i;

	if (misc->max_sds == 0 || misc->max_sds > IRDMA_HMC_MAX_SD_COUNT ||
	    misc->max_ceqs == 0 || misc->max_ceqs > IRDMA_CEQ_MAX_COUNT ||
	    misc->xf_block_size == 0 ||
	    misc->xf_block_size > IRDMA_FPM_MAX_BLOCK ||
	    misc->q1_block_size == 0 ||
	    misc->q1_block_size > IRDMA_FPM_MAX_BLOCK ||
	    misc->rrf_block_size > IRDMA_FPM_MAX_BLOCK ||
	    misc->ooiscf_block_size > IRDMA_FPM_MAX_BLOCK ||
	    misc->ht_multiplier == 0 ||
	    misc->ht_multiplier > IRDMA_FPM_MAX_HT_MULT ||
	    misc->timer_bucket > IRDMA_FPM_MAX_TIMER) {
		irdma_error(irdma, "firmware FPM limits out of range: sds %u "
		    "ceqs %u blocks %u/%u/%u ht %u timer %u", misc->max_sds,
		    misc->max_ceqs, misc->xf_block_size, misc->q1_block_size,
		    misc->rrf_block_size, misc->ht_multiplier,
		    misc->timer_bucket);
		return (-EINVAL);
	}

	for (i = 0; i < IRDMA_HMC_IW_MAX; i++) {
		if (o[i].max_cnt > IRDMA_FPM_MAX_CNT ||
		    (o[i].max_cnt != 0 && (o[i].size == 0 ||
		    !ISP2(o[i].size) || o[i].size > IRDMA_FPM_MAX_OBJ_SIZE))) {
			irdma_error(irdma, "firmware FPM object %u out of "
			    "range: count %u size %llu", i, o[i].max_cnt,
			    o[i].size);
			return (-EINVAL);
		}
	}

	if (o[IRDMA_HMC_IW_QP].max_cnt == 0 ||
	    o[IRDMA_HMC_IW_CQ].max_cnt == 0 ||
	    o[IRDMA_HMC_IW_Q1].max_cnt < IRDMA_FPM_MIN_Q1 ||
	    o[IRDMA_HMC_IW_PBLE].max_cnt < IRDMA_FPM_MIN_PBLE) {
		irdma_error(irdma, "firmware FPM counts too small: qp %u cq %u "
		    "q1 %u pble %u", o[IRDMA_HMC_IW_QP].max_cnt,
		    o[IRDMA_HMC_IW_CQ].max_cnt, o[IRDMA_HMC_IW_Q1].max_cnt,
		    o[IRDMA_HMC_IW_PBLE].max_cnt);
		return (-EINVAL);
	}

	return (0);
}

/*
 * The committed layout indexes the SD table, which the core allocates from
 * sd_cnt next: every object must lie inside it, and no count may exceed
 * what the driver asked for in req.
 */
int
irdma_osdep_fpm_commit_check(struct irdma_sc_dev *dev,
    struct irdma_hmc_info *hmc, const u32 *req)
{
	irdma_t *irdma = IRDMA_FROM_DEV(dev);
	struct irdma_hmc_obj_info *o = hmc->hmc_obj;
	u32 sds = hmc->sd_table.sd_cnt;
	u64 limit;
	uint_t i;

	if (sds == 0 || sds > dev->hmc_fpm_misc.max_sds) {
		irdma_error(irdma, "firmware committed %u SDs of %u", sds,
		    dev->hmc_fpm_misc.max_sds);
		return (-EINVAL);
	}
	limit = (u64)sds * IRDMA_HMC_DIRECT_BP_SIZE;

	for (i = 0; i < IRDMA_HMC_IW_MAX; i++) {
		if (o[i].cnt == 0)
			continue;
		if (o[i].cnt > o[i].max_cnt || o[i].cnt > req[i] ||
		    o[i].base >= limit ||
		    (u64)o[i].cnt * o[i].size > limit - o[i].base) {
			irdma_error(irdma, "firmware committed object %u "
			    "outside its limits: count %u of %u (asked %u), "
			    "base 0x%llx", i, o[i].cnt, o[i].max_cnt, req[i],
			    o[i].base);
			return (-EINVAL);
		}
	}

	if (o[IRDMA_HMC_IW_QP].cnt == 0 || o[IRDMA_HMC_IW_CQ].cnt == 0 ||
	    o[IRDMA_HMC_IW_PBLE].cnt < IRDMA_FPM_MIN_PBLE) {
		irdma_error(irdma, "firmware committed too few objects");
		return (-EINVAL);
	}

	return (0);
}

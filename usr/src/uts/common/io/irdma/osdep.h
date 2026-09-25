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

#ifndef _IRDMA_OSDEP_H
#define	_IRDMA_OSDEP_H

/*
 * The illumos environment of the imported irdma code in core/.  Those files
 * include "osdep.h", which resolves here.  Everything is written for illumos;
 * none of it comes from the Linux headers.
 *
 * Locks: the core code runs only in thread context (attach, detach and the
 * taskqs), never in an interrupt handler, so its spinlocks are adaptive
 * mutexes.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/stdbool.h>
#include <sys/atomic.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ksynch.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/ethernet.h>
#include <sys/note.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The core prints 64-bit values with %ll, so u64 is unsigned long long. */
typedef uint8_t			u8;
typedef int8_t			s8;
typedef uint16_t		u16;
typedef int16_t			s16;
typedef uint32_t		u32;
typedef int32_t			s32;
typedef unsigned long long	u64;
typedef long long		s64;
typedef uint16_t		__le16;
typedef uint32_t		__le32;
typedef u64			__le64;
typedef uint16_t		__be16;
typedef uint32_t		__be32;
typedef u64			dma_addr_t;
typedef u64			resource_size_t;
typedef int		gfp_t;

#define	__iomem
#define	__packed		__attribute__((__packed__))
#ifdef __CHECKER__
#define	fallthrough		do { } while (0)
#else
#define	fallthrough		__attribute__((__fallthrough__))
#endif
#define	likely(x)		__builtin_expect(!!(x), 1)
#define	unlikely(x)		__builtin_expect(!!(x), 0)

#define	GFP_KERNEL		0
#define	GFP_ATOMIC		1
#define	__GFP_NOWARN		0

#define	SZ_4K			0x1000UL
#define	SZ_2M			0x200000UL
#define	SZ_1G			0x40000000UL

#define	ETH_ALEN		ETHERADDRL
#define	DSCP_MAX		64

/*
 * The core decodes object sizes from firmware with BIT_ULL(); a shift of 64
 * or more yields 0, which irdma_osdep_fpm_query_check() rejects.
 */
#define	BIT(n)			((n) < 64 ? 1UL << (n) : 0UL)
#define	BIT_ULL(n)		((n) < 64 ? 1ULL << (n) : 0ULL)
#define	GENMASK(h, l)		\
	((~0UL >> (63 - (h))) & ~((1UL << (l)) - 1UL))
#define	GENMASK_ULL(h, l)	\
	((~0ULL >> (63 - (h))) & ~((1ULL << (l)) - 1ULL))

/* The masks are constants with at least one bit set. */
#define	FIELD_PREP(mask, val)	\
	((((u64)(val)) << __builtin_ctzll(mask)) & (u64)(mask))
#define	FIELD_GET(mask, reg)	\
	((((u64)(reg)) & (u64)(mask)) >> __builtin_ctzll(mask))

#ifndef ARRAY_SIZE
#define	ARRAY_SIZE(a)		(sizeof (a) / sizeof ((a)[0]))
#endif
#define	ALIGN(x, a)		P2ROUNDUP((x), (a))
#define	round_up(x, y)		P2ROUNDUP((x), (y))
#define	DIV_ROUND_UP(n, d)	howmany((n), (d))
#define	BITS_PER_LONG		64
#define	BITS_TO_LONGS(n)	howmany((n), BITS_PER_LONG)

/* <sys/ddi.h> defines min() and max() as macros. */
#define	min_t(t, a, b)		MIN((t)(a), (t)(b))
#define	max_t(t, a, b)		MAX((t)(a), (t)(b))
#define	swap(a, b)		\
	do { __typeof__(a) __t = (a); (a) = (b); (b) = __t; } while (0)

#define	container_of(p, t, m)	((t *)(void *)((char *)(p) - offsetof(t, m)))

#define	cpu_to_le16(x)		LE_16(x)
#define	cpu_to_le32(x)		LE_32(x)
#define	cpu_to_le64(x)		LE_64(x)
#define	le16_to_cpu(x)		LE_16(x)
#define	le32_to_cpu(x)		LE_32(x)
#define	le64_to_cpu(x)		LE_64(x)

#define	READ_ONCE(x)		(*(volatile __typeof__(x) *)&(x))
#define	WRITE_ONCE(x, v)	(*(volatile __typeof__(x) *)&(x) = (v))

/* The device reads and writes coherent memory in order on this platform. */
#define	dma_wmb()		membar_producer()
#define	dma_rmb()		membar_consumer()
#define	wmb()			membar_producer()
#define	rmb()			membar_consumer()
#define	mb()			do { membar_enter(); membar_exit(); } while (0)

static inline u64
roundup_pow_of_two(u64 n)
{
	return (n <= 1 ? 1 : 1ULL << highbit64(n - 1));
}

static inline int
ilog2(u64 n)
{
	return (n == 0 ? 0 : highbit64(n) - 1);
}

static inline u64
ether_addr_to_u64(const u8 *addr)
{
	u64 v = 0;
	int i;

	for (i = 0; i < ETHERADDRL; i++)
		v = (v << 8) | addr[i];
	return (v);
}

static inline void
ether_addr_copy(u8 *dst, const u8 *src)
{
	bcopy(src, dst, ETHERADDRL);
}

/*
 * Counters.
 */
typedef struct {
	volatile uint32_t	counter;
} atomic_t;

typedef struct {
	volatile uint64_t	counter;
} atomic64_t;

typedef atomic_t refcount_t;

#define	atomic_set(a, v)	((a)->counter = (uint32_t)(v))
#define	atomic_read(a)		((int)(a)->counter)
#define	atomic_inc(a)		atomic_inc_32(&(a)->counter)
#define	atomic_dec(a)		atomic_dec_32(&(a)->counter)
#define	atomic64_set(a, v)	((a)->counter = (uint64_t)(v))
#define	atomic64_read(a)	((s64)(a)->counter)
#define	atomic64_inc(a)		atomic_inc_64(&(a)->counter)

/*
 * Locks.
 */
typedef struct {
	kmutex_t	sl_lock;
} spinlock_t;

/* The core's struct mutex is the illumos kmutex_t. */

#define	spin_lock_init(l)	\
	mutex_init(&(l)->sl_lock, NULL, MUTEX_DRIVER, NULL)
#define	spin_lock_irqsave(l, f)	\
	do { (f) = 0; mutex_enter(&(l)->sl_lock); } while (0)
#define	spin_unlock_irqrestore(l, f)	\
	do { (void) (f); mutex_exit(&(l)->sl_lock); } while (0)
#define	spin_lock(l)		mutex_enter(&(l)->sl_lock)
#define	spin_unlock(l)		mutex_exit(&(l)->sl_lock)
#define	mutex_lock(l)		mutex_enter(l)
#define	mutex_unlock(l)		mutex_exit(l)

/*
 * The core code calls mutex_init() with the mutex only; four arguments still
 * reach the kernel function.
 */
extern void irdma_osdep_mutex_init(kmutex_t *);
#define	IRDMA_MUTEX_INIT(_1, _2, _3, _4, f, ...)	f
#define	mutex_init(...)							\
	IRDMA_MUTEX_INIT(__VA_ARGS__, (mutex_init), _3, _2,		\
	    irdma_osdep_mutex_init)(__VA_ARGS__)

/*
 * Doubly linked lists with the calling convention the core code expects.
 */
struct list_head {
	struct list_head	*next;
	struct list_head	*prev;
};

static inline void
INIT_LIST_HEAD(struct list_head *h)
{
	h->next = h;
	h->prev = h;
}

static inline void
irdma_list_link(struct list_head *e, struct list_head *before,
    struct list_head *after)
{
	e->prev = before;
	e->next = after;
	before->next = e;
	after->prev = e;
}

static inline void
list_add(struct list_head *e, struct list_head *h)
{
	irdma_list_link(e, h, h->next);
}

static inline void
list_add_tail(struct list_head *e, struct list_head *h)
{
	irdma_list_link(e, h->prev, h);
}

static inline void
list_del(struct list_head *e)
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
	e->next = NULL;
	e->prev = NULL;
}

static inline void
list_move(struct list_head *e, struct list_head *h)
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
	list_add(e, h);
}

static inline int
list_empty(const struct list_head *h)
{
	return (h->next == h);
}

#define	list_entry(p, t, m)		container_of(p, t, m)
#define	list_first_entry(h, t, m)	list_entry((h)->next, t, m)
#define	list_last_entry(h, t, m)	list_entry((h)->prev, t, m)
#define	list_for_each_entry(p, h, m)					\
	for ((p) = list_entry((h)->next, __typeof__(*(p)), m);		\
	    &(p)->m != (h);						\
	    (p) = list_entry((p)->m.next, __typeof__(*(p)), m))
#define	list_for_each_entry_safe(p, n, h, m)				\
	for ((p) = list_entry((h)->next, __typeof__(*(p)), m),		\
	    (n) = list_entry((p)->m.next, __typeof__(*(p)), m);		\
	    &(p)->m != (h);						\
	    (p) = (n), (n) = list_entry((n)->m.next, __typeof__(*(n)), m))

/* Present in the core structures; the illumos glue drives its own timers. */
struct timer_list {
	timeout_id_t	tl_id;
};

/*
 * Memory.  kzalloc() never sleeps, so a size taken from the device fails
 * instead of blocking.
 */
extern void *kzalloc(size_t, gfp_t);
extern void *kcalloc(size_t, size_t, gfp_t);
extern void kfree(const void *);
#define	kmalloc(s, f)		kzalloc((s), (f))
#define	vzalloc(s)		kzalloc((s), GFP_KERNEL)
#define	vfree(p)		kfree(p)
#define	bitmap_zalloc(n, f)	\
	((unsigned long *)kcalloc(BITS_TO_LONGS(n), sizeof (long), (f)))
#define	bitmap_free(p)		kfree(p)

/*
 * DMA.  struct device is the illumos RDMA function; irdma_osdep.c allocates
 * through ice and tracks each buffer so that it can be freed by address.
 */
struct device;

enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2
};

extern void *dma_alloc_coherent(struct device *, size_t, dma_addr_t *, gfp_t);
extern void dma_free_coherent(struct device *, size_t, void *, dma_addr_t);
extern dma_addr_t dma_map_single(struct device *, void *, size_t,
    enum dma_data_direction);
extern void dma_unmap_single(struct device *, dma_addr_t, size_t,
    enum dma_data_direction);
extern int dma_mapping_error(struct device *, dma_addr_t);
extern void dma_sync_single_for_cpu(struct device *, dma_addr_t, size_t,
    enum dma_data_direction);
extern void dma_sync_single_for_device(struct device *, dma_addr_t, size_t,
    enum dma_data_direction);

/*
 * Registers.  readl() and writel() take a mapped BAR0 address.
 */
extern u32 readl(const volatile void *);
extern void writel(u32, volatile void *);

#define	udelay(us)		drv_usecwait(us)
#define	mdelay(ms)		delay(drv_usectohz((clock_t)(ms) * 1000))

/*
 * Messages.
 */
struct ib_device;
extern void irdma_osdep_debug(struct ib_device *, const char *, ...)
    __KPRINTFLIKE(2);
#define	ibdev_dbg(d, ...)	irdma_osdep_debug((d), __VA_ARGS__)
#define	ibdev_err(d, ...)	irdma_osdep_debug((d), __VA_ARGS__)
#define	ibdev_warn(d, ...)	irdma_osdep_debug((d), __VA_ARGS__)
#define	print_hex_dump_debug(p, t, r, g, b, l, a)	((void)0)
#define	DUMP_PREFIX_OFFSET	0

/* The one verbs structure the core code uses, with the OFED layout. */
struct ib_sge {
	u64	addr;
	u32	length;
	u32	lkey;
};

/*
 * The interface the core code has with the OS layer: irdma_osdep.c and
 * irdma_ctl.c provide these.
 */
struct irdma_dma_info {
	dma_addr_t	*dmaaddrs;
};

/* Host bookkeeping only; Linux packs these, which nothing depends on. */
struct irdma_dma_mem {
	void		*va;
	dma_addr_t	pa;
	u32		size;
};

struct irdma_virt_mem {
	void		*va;
	u32		size;
};

struct irdma_sc_vsi;
struct irdma_sc_dev;
struct irdma_sc_qp;
struct irdma_puda_buf;
struct irdma_puda_cmpl_info;
struct irdma_update_sds_info;
struct irdma_hmc_fcn_info;
struct irdma_manage_vf_pble_info;
struct irdma_hw;
struct irdma_pci_f;
struct irdma_hmc_info;
struct irdma_hmc_fpm_misc;

extern struct ib_device *to_ibdev(struct irdma_sc_dev *);
extern void irdma_ieq_mpa_crc_ae(struct irdma_sc_dev *, struct irdma_sc_qp *);
extern void irdma_add_dev_ref(struct irdma_sc_dev *);
extern void irdma_put_dev_ref(struct irdma_sc_dev *);
extern int irdma_ieq_check_mpacrc(const void *, u32, u32);
extern struct irdma_sc_qp *irdma_ieq_get_qp(struct irdma_sc_dev *,
    struct irdma_puda_buf *);
extern void irdma_send_ieq_ack(struct irdma_sc_qp *);
extern void irdma_ieq_update_tcpip_info(struct irdma_puda_buf *, u16, u32);
extern int irdma_puda_get_tcpip_info(struct irdma_puda_cmpl_info *,
    struct irdma_puda_buf *);
extern int irdma_cqp_sds_cmd(struct irdma_sc_dev *,
    struct irdma_update_sds_info *);
extern int irdma_cqp_manage_hmc_fcn_cmd(struct irdma_sc_dev *,
    struct irdma_hmc_fcn_info *, u16 *);
extern int irdma_alloc_query_fpm_buf(struct irdma_sc_dev *,
    struct irdma_dma_mem *);
extern void *irdma_remove_cqp_head(struct irdma_sc_dev *);
extern void irdma_term_modify_qp(struct irdma_sc_qp *, u8, u8, u8);
extern void irdma_terminate_done(struct irdma_sc_qp *, int);
extern void irdma_terminate_start_timer(struct irdma_sc_qp *);
extern void irdma_terminate_del_timer(struct irdma_sc_qp *);
extern void irdma_hw_stats_start_timer(struct irdma_sc_vsi *);
extern void irdma_hw_stats_stop_timer(struct irdma_sc_vsi *);
extern void wr32(struct irdma_hw *, u32, u32);
extern u32 rd32(struct irdma_hw *, u32);
extern u64 rd64(struct irdma_hw *, u32);
extern int irdma_map_vm_page_list(struct irdma_hw *, void *, dma_addr_t *,
    u32);
extern void irdma_unmap_vm_page_list(struct irdma_hw *, dma_addr_t *, u32);

/*
 * illumos: checks on firmware data that the core code calls (listed in
 * core/README.illumos).
 */
extern int irdma_osdep_fpm_query_check(struct irdma_sc_dev *,
    struct irdma_hmc_info *, struct irdma_hmc_fpm_misc *);
extern int irdma_osdep_fpm_commit_check(struct irdma_sc_dev *,
    struct irdma_hmc_info *);

/*
 * illumos: the QP a CQE names, if it is live on the CQ (irdma_cq.c), and
 * whether index i of a ring lies between its tail and head.
 */
struct irdma_cq_uk;
struct irdma_qp_uk;
extern struct irdma_qp_uk *irdma_osdep_cqe_qp(struct irdma_cq_uk *, u64,
    u32);
#define	IRDMA_OSDEP_RING_HOLDS(r, i)					\
	((i) < (r).size && (((i) + (r).size - (r).tail) % (r).size) <	\
	(((r).head + (r).size - (r).tail) % (r).size))

#ifdef __cplusplus
}
#endif

#endif /* _IRDMA_OSDEP_H */

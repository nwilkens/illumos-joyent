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
 * Copyright 2026 MNX Cloud, Inc.
 */

/*
 * Single-threaded host stand-ins for the kernel interfaces that the mlxcx
 * command, event and page code uses. Sleeping primitives call
 * stub_sleep_hook(), which a test uses to run scripted device events and to
 * advance a fake lbolt clock. A sleep that no event can end is a test failure.
 */

#ifndef _MLXCX_STUB_H
#define	_MLXCX_STUB_H

#include <sys/param.h>
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <inttypes.h>

#if !defined(__illumos__) && !defined(__sun)
typedef enum { B_FALSE = 0, B_TRUE = 1 } boolean_t;
typedef unsigned int uint_t;
typedef unsigned char uchar_t;
#endif

#ifndef NBBY
#define	NBBY	8
#endif

#define	CTASSERT(x)	_Static_assert((x), #x)

/* Failure reporting. */
static int stub_verbose;
static uint64_t stub_warnings;
static uint64_t stub_ereports;

static void
stub_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) fprintf(stderr, "FAIL: ");
	(void) vfprintf(stderr, fmt, ap);
	(void) fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

#define	STUB_CHECK(cond)	do {					\
	if (!(cond))							\
		stub_fail("%s:%d: %s", __FILE__, __LINE__, #cond);	\
} while (0)

#define	VERIFY(x)	do {						\
	if (!(x))							\
		stub_fail("kernel panic: VERIFY(%s) at %s:%d", #x,	\
		    __FILE__, __LINE__);				\
} while (0)
#define	VERIFY0(x)		VERIFY((x) == 0)
#define	STUB_U64(x)		((uint64_t)(x))
#define	STUB_S64(x)		((int64_t)(x))
#define	STUB_PTR(x)		((uintptr_t)(x))
#define	VERIFY3U(a, op, b)	VERIFY(STUB_U64(a) op STUB_U64(b))
#define	VERIFY3S(a, op, b)	VERIFY(STUB_S64(a) op STUB_S64(b))
#define	VERIFY3P(a, op, b)	VERIFY(STUB_PTR(a) op STUB_PTR(b))
#define	ASSERT(x)		VERIFY(x)
#define	ASSERT0(x)		VERIFY0(x)
#define	ASSERT3U(a, op, b)	VERIFY3U(a, op, b)
#define	ASSERT3S(a, op, b)	VERIFY3S(a, op, b)
#define	ASSERT3P(a, op, b)	VERIFY3P(a, op, b)

#define	DTRACE_PROBE(a)
#define	DTRACE_PROBE1(a, b, c)
#define	DTRACE_PROBE2(a, b, c, d, e)
#define	DTRACE_PROBE3(a, b, c, d, e, f, g)

#define	membar_consumer()	__sync_synchronize()
#define	membar_producer()	__sync_synchronize()

#ifndef MIN
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define	MAX(a, b)	((a) < (b) ? (b) : (a))
#endif

/* Fake clock, in ticks of 10ms. Sleeping for a day of it is a hang. */
#define	STUB_FOREVER	(100 * 60 * 60 * 24)

static clock_t stub_now;
static boolean_t stub_sleep_hook(clock_t deadline);

static void
stub_hang_check(void)
{
	if (stub_now > STUB_FOREVER)
		stub_fail("slept for a day of fake time (hang)");
}

static clock_t
ddi_get_lbolt(void)
{
	return (stub_now);
}

static clock_t
drv_usectohz(clock_t us)
{
	return ((us + 9999) / 10000);
}

static void
delay(clock_t ticks)
{
	clock_t end = stub_now + ticks;

	while (stub_sleep_hook(end))
		;
	if (stub_now < end)
		stub_now = end;
	stub_hang_check();
}

/* Memory. */
#define	KM_SLEEP	0
#define	KM_NOSLEEP	1

typedef struct {
	uint64_t	sh_magic;
	size_t		sh_size;
} stub_hdr_t;

#define	STUB_MAGIC	0x6d6c7863786b6d61ULL

static int64_t stub_kmem_live;
static size_t stub_kmem_max_request = (size_t)64 * 1024 * 1024;

static void *
kmem_alloc(size_t size, int flag)
{
	stub_hdr_t *h;

	(void) flag;
	if (size > stub_kmem_max_request)
		stub_fail("kmem_alloc(%zu) would sleep forever", size);
	if ((h = malloc(sizeof (*h) + size)) == NULL)
		stub_fail("host out of memory");
	h->sh_magic = STUB_MAGIC;
	h->sh_size = size;
	memset(h + 1, 0xa5, size);
	stub_kmem_live++;
	return (h + 1);
}

static void *
kmem_zalloc(size_t size, int flag)
{
	void *p = kmem_alloc(size, flag);

	memset(p, 0, size);
	return (p);
}

static void
kmem_free(void *p, size_t size)
{
	stub_hdr_t *h = (stub_hdr_t *)p - 1;

	if (h->sh_magic != STUB_MAGIC)
		stub_fail("kmem_free of a bad or freed buffer");
	if (h->sh_size != size)
		stub_fail("kmem_free size %zu, allocated %zu", size,
		    h->sh_size);
	h->sh_magic = 0;
	stub_kmem_live--;
	free(h);
}

/*
 * Object caches. Freeing to a destroyed cache is a use after free in the
 * kernel, so it fails the test.
 */
typedef struct kmem_cache {
	struct kmem_cache *kc_next;
	char		kc_name[32];
	size_t		kc_size;
	int		(*kc_constr)(void *, void *, int);
	void		(*kc_destr)(void *, void *);
	void		*kc_arg;
	int64_t		kc_live;
	int		kc_dead;
} kmem_cache_t;

static kmem_cache_t *stub_caches;

/* A live cache owns its kstat name; a second one of that name has none. */
static kmem_cache_t *
kmem_cache_create(char *name, size_t size, size_t align,
    int (*constr)(void *, void *, int), void (*destr)(void *, void *),
    void (*reclaim)(void *), void *arg, void *vmp, int flags)
{
	kmem_cache_t *cp = calloc(1, sizeof (*cp));

	(void) align; (void) reclaim; (void) vmp; (void) flags;
	for (kmem_cache_t *o = stub_caches; o != NULL; o = o->kc_next) {
		if (!o->kc_dead && strcmp(o->kc_name, name) == 0)
			stub_fail("kmem cache name %s is already in use", name);
	}
	(void) snprintf(cp->kc_name, sizeof (cp->kc_name), "%s", name);
	cp->kc_next = stub_caches;
	stub_caches = cp;
	cp->kc_size = size;
	cp->kc_constr = constr;
	cp->kc_destr = destr;
	cp->kc_arg = arg;
	return (cp);
}

static void *
kmem_cache_alloc(kmem_cache_t *cp, int flag)
{
	void *p;

	if (cp->kc_dead)
		stub_fail("kmem_cache_alloc from a destroyed cache");
	p = calloc(1, cp->kc_size);
	if (cp->kc_constr != NULL)
		(void) cp->kc_constr(p, cp->kc_arg, flag);
	cp->kc_live++;
	return (p);
}

static void
kmem_cache_free(kmem_cache_t *cp, void *p)
{
	if (cp == NULL || cp->kc_dead)
		stub_fail("kmem_cache_free to a destroyed cache");
	if (cp->kc_destr != NULL)
		cp->kc_destr(p, cp->kc_arg);
	cp->kc_live--;
	free(p);
}

static void
kmem_cache_destroy(kmem_cache_t *cp)
{
	cp->kc_dead = 1;
}

/* Locks. */
typedef struct {
	int	km_init;
	int	km_held;
} kmutex_t;

typedef struct {
	int	kc_init;
} kcondvar_t;

#define	MUTEX_DRIVER	0
#define	CV_DRIVER	0
#define	DDI_INTR_PRI(x)	((void *)(uintptr_t)(x))

static void
mutex_init(kmutex_t *m, const char *name, int type, void *arg)
{
	(void) name; (void) type; (void) arg;
	m->km_init = 1;
	m->km_held = 0;
}

static void
mutex_destroy(kmutex_t *m)
{
	if (!m->km_init)
		stub_fail("mutex_destroy of an uninitialized mutex");
	if (m->km_held)
		stub_fail("mutex_destroy of a held mutex");
	m->km_init = 0;
}

static void
mutex_enter(kmutex_t *m)
{
	if (!m->km_init)
		stub_fail("mutex_enter of an uninitialized mutex");
	if (m->km_held)
		stub_fail("mutex_enter of a held mutex (deadlock)");
	m->km_held = 1;
}

static void
mutex_exit(kmutex_t *m)
{
	if (!m->km_init)
		stub_fail("mutex_exit of an uninitialized mutex");
	if (!m->km_held)
		stub_fail("mutex_exit of a mutex that is not held");
	m->km_held = 0;
}

#define	mutex_owned(m)	((m)->km_held)
#define	MUTEX_HELD(m)	((m)->km_held)

static uint64_t stub_cv_wakeups;

static void
cv_init(kcondvar_t *cv, const char *name, int type, void *arg)
{
	(void) name; (void) type; (void) arg;
	cv->kc_init = 1;
}

static void
cv_destroy(kcondvar_t *cv)
{
	cv->kc_init = 0;
}

static void
cv_broadcast(kcondvar_t *cv)
{
	(void) cv;
	stub_cv_wakeups++;
}

#define	cv_signal(cv)	cv_broadcast(cv)

static void
cv_wait(kcondvar_t *cv, kmutex_t *m)
{
	uint64_t w = stub_cv_wakeups;

	(void) cv;
	mutex_exit(m);
	while (stub_cv_wakeups == w) {
		if (!stub_sleep_hook(-1))
			stub_fail("cv_wait would sleep forever");
	}
	mutex_enter(m);
}

static clock_t
cv_timedwait(kcondvar_t *cv, kmutex_t *m, clock_t deadline)
{
	uint64_t w = stub_cv_wakeups;
	clock_t ret = 1;

	(void) cv;
	mutex_exit(m);
	while (stub_cv_wakeups == w) {
		if (!stub_sleep_hook(deadline)) {
			if (stub_now < deadline)
				stub_now = deadline;
			stub_hang_check();
			ret = -1;
			break;
		}
	}
	mutex_enter(m);
	return (ret);
}

/* Lists. */
typedef struct list_node {
	struct list_node	*list_next;
	struct list_node	*list_prev;
} list_node_t;

typedef struct {
	size_t		list_size;
	size_t		list_offset;
	list_node_t	list_head;
} list_t;

#define	STUB_L2N(l, o)	((list_node_t *)((char *)(o) + (l)->list_offset))
#define	STUB_N2L(l, n)	((void *)((char *)(n) - (l)->list_offset))

static void
list_create(list_t *l, size_t size, size_t offset)
{
	l->list_size = size;
	l->list_offset = offset;
	l->list_head.list_next = l->list_head.list_prev = &l->list_head;
}

static int
list_is_empty(list_t *l)
{
	return (l->list_head.list_next == &l->list_head);
}

static void
list_destroy(list_t *l)
{
	if (l->list_head.list_next == NULL)
		stub_fail("list_destroy of a destroyed list");
	if (!list_is_empty(l))
		stub_fail("list_destroy of a non-empty list");
	l->list_head.list_next = l->list_head.list_prev = NULL;
}

static void
list_insert_tail(list_t *l, void *o)
{
	list_node_t *n = STUB_L2N(l, o);

	n->list_prev = l->list_head.list_prev;
	n->list_next = &l->list_head;
	n->list_prev->list_next = n;
	l->list_head.list_prev = n;
}

static void
list_remove(list_t *l, void *o)
{
	list_node_t *n = STUB_L2N(l, o);

	n->list_prev->list_next = n->list_next;
	n->list_next->list_prev = n->list_prev;
	n->list_next = n->list_prev = NULL;
}

static void *
list_head(list_t *l)
{
	if (list_is_empty(l))
		return (NULL);
	return (STUB_N2L(l, l->list_head.list_next));
}

static void *
list_next(list_t *l, void *o)
{
	list_node_t *n = STUB_L2N(l, o);

	if (n->list_next == &l->list_head)
		return (NULL);
	return (STUB_N2L(l, n->list_next));
}

static void *
list_remove_head(list_t *l)
{
	void *o = list_head(l);

	if (o != NULL)
		list_remove(l, o);
	return (o);
}

static void
list_move_tail(list_t *dst, list_t *src)
{
	void *o;

	while ((o = list_remove_head(src)) != NULL)
		list_insert_tail(dst, o);
}

/* AVL trees: a sorted-enough linked list is all the page code needs. */
typedef struct avl_node {
	struct avl_node	*avl_next;
} avl_node_t;

typedef struct {
	int		(*avl_cmp)(const void *, const void *);
	size_t		avl_size;
	size_t		avl_offset;
	avl_node_t	*avl_first;
	uint64_t	avl_n;
	int		avl_init;
} avl_tree_t;

typedef uintptr_t avl_index_t;

static void
avl_create(avl_tree_t *t, int (*cmp)(const void *, const void *),
    size_t size, size_t offset)
{
	t->avl_cmp = cmp;
	t->avl_size = size;
	t->avl_offset = offset;
	t->avl_first = NULL;
	t->avl_n = 0;
	t->avl_init = 1;
}

static void *
avl_find(avl_tree_t *t, const void *probe, avl_index_t *where)
{
	avl_node_t *n;

	(void) where;
	for (n = t->avl_first; n != NULL; n = n->avl_next) {
		void *o = (char *)n - t->avl_offset;
		if (t->avl_cmp(probe, o) == 0)
			return (o);
	}
	return (NULL);
}

static void
avl_add(avl_tree_t *t, void *o)
{
	avl_node_t *n = (avl_node_t *)((char *)o + t->avl_offset);

	if (avl_find(t, o, NULL) != NULL)
		stub_fail("kernel panic: avl_add of a duplicate node");
	n->avl_next = t->avl_first;
	t->avl_first = n;
	t->avl_n++;
}

static void
avl_remove(avl_tree_t *t, void *o)
{
	avl_node_t *n = (avl_node_t *)((char *)o + t->avl_offset);
	avl_node_t **pp;

	for (pp = &t->avl_first; *pp != NULL; pp = &(*pp)->avl_next) {
		if (*pp == n) {
			*pp = n->avl_next;
			t->avl_n--;
			return;
		}
	}
	stub_fail("avl_remove of a node not in the tree");
}

static int
avl_is_empty(avl_tree_t *t)
{
	return (t->avl_n == 0);
}

static uint64_t
avl_numnodes(avl_tree_t *t)
{
	return (t->avl_n);
}

static void
avl_destroy(avl_tree_t *t)
{
	if (t->avl_n != 0)
		stub_fail("avl_destroy of a non-empty tree");
	t->avl_init = 0;
}

/* Identifier spaces. */
typedef struct {
	uint_t	is_lo;
	uint_t	is_hi;
	uint_t	is_next;
	uint8_t	is_used[256];
} id_space_t;

static id_space_t *
id_space_create(const char *name, uint_t lo, uint_t hi)
{
	id_space_t *is = calloc(1, sizeof (*is));

	(void) name;
	is->is_lo = is->is_next = lo;
	is->is_hi = hi;
	return (is);
}

static void
id_space_destroy(id_space_t *is)
{
	free(is);
}

static uint_t
id_alloc(id_space_t *is)
{
	uint_t i, id;

	for (i = 0; i < is->is_hi - is->is_lo; i++) {
		id = is->is_lo + (is->is_next - is->is_lo + i) %
		    (is->is_hi - is->is_lo);
		if (!is->is_used[id]) {
			is->is_used[id] = 1;
			is->is_next = id + 1;
			if (is->is_next >= is->is_hi)
				is->is_next = is->is_lo;
			return (id);
		}
	}
	stub_fail("id_alloc would sleep forever");
	return (0);
}

static void
id_free(id_space_t *is, uint_t id)
{
	if (id < is->is_lo || id >= is->is_hi || !is->is_used[id])
		stub_fail("id_free of an unallocated id %u", id);
	is->is_used[id] = 0;
}

static int
stub_ids_used(id_space_t *is)
{
	int n = 0;

	for (uint_t i = 0; i < 256; i++)
		n += is->is_used[i];
	return (n);
}

static void
atomic_inc_uint(volatile uint_t *p)
{
	(*p)++;
}

static void
atomic_inc_64(volatile uint64_t *p)
{
	(*p)++;
}

static void
atomic_or_uint(volatile uint_t *p, uint_t v)
{
	*p |= v;
}

static void
atomic_and_uint(volatile uint_t *p, uint_t v)
{
	*p &= v;
}

static int
ddi_ffs(long mask)
{
	return (__builtin_ffsl(mask));
}

/* DDI odds and ends. */
#define	DDI_SUCCESS		0
#define	DDI_INTR_CLAIMED	1
#define	DDI_FAILURE		(-1)
#define	DDI_SLEEP		0
#define	DDI_NOSLEEP		1
#define	TASKQ_NAMELEN		31
#define	TASKQ_DEFAULTPRI	0
#define	TASKQ_PREPOPULATE	1
#define	DDI_DMA_SYNC_FORDEV	0
#define	DDI_DMA_SYNC_FORKERNEL	1
#define	DDI_DMA_SYNC_FORCPU	1
#define	DDI_FM_DEVICE_NO_RESPONSE	"no_response"
#define	DDI_SERVICE_LOST	1
#define	minclsyspri		60
#define	PRIx64_			PRIx64

typedef void *dev_info_t;
typedef void *ddi_acc_handle_t;
typedef void *ddi_dma_handle_t;
typedef struct {
	int	devacc_attr_version;
	int	devacc_attr_endian_flags;
	int	devacc_attr_dataorder;
	int	devacc_attr_access;
} ddi_device_acc_attr_t;

#define	DDI_DEVICE_ATTR_V0	1
#define	DDI_STRUCTURE_BE_ACC	2
#define	DDI_STRICTORDER_ACC	0
#define	DDI_FLAGERR_ACC		2
#define	DDI_DEFAULT_ACC		1
#define	DDI_FM_ACC_ERR_CAP(c)	(((c) & 0x2) != 0)
#define	longlong_t		long long
#define	KSTAT_STRLEN		31

typedef unsigned long long u_longlong_t;
static int stub_instance;

static int
ddi_get_instance(dev_info_t *dip)
{
	(void) dip;
	return (stub_instance);
}

static off_t stub_regsize = 32 * 1024 * 1024;

static int
ddi_dev_regsize(dev_info_t *dip, int rnumber, off_t *result)
{
	(void) dip; (void) rnumber;
	*result = stub_regsize;
	return (DDI_SUCCESS);
}

static int
ddi_regs_map_setup(dev_info_t *dip, int rnumber, caddr_t *addrp, off_t off,
    off_t len, ddi_device_acc_attr_t *acc, ddi_acc_handle_t *handlep)
{
	(void) dip; (void) rnumber; (void) off; (void) len; (void) acc;
	*addrp = (caddr_t)0x1000000;
	*handlep = (ddi_acc_handle_t)addrp;
	return (DDI_SUCCESS);
}
typedef struct { int unused; } ddi_dma_attr_t;
typedef struct { void *tqe_func; } taskq_ent_t;

typedef struct {
	uint64_t	dmac_laddress;
	size_t		dmac_size;
} ddi_dma_cookie_t;

static int
ddi_dma_sync(ddi_dma_handle_t h, off_t off, size_t len, uint_t flags)
{
	(void) h; (void) off; (void) len; (void) flags;
	return (DDI_SUCCESS);
}

static uint64_t stub_service_impacts;

static void
ddi_fm_service_impact(dev_info_t *dip, int impact)
{
	(void) dip; (void) impact;
	stub_service_impacts++;
}

/* Task queues run nothing by themselves; tests run queued work. */
typedef struct stub_taskq {
	char		tq_name[32];
	int		tq_live;
	int		tq_pending;
	void		(*tq_func[64])(void *);
	void		*tq_arg[64];
} stub_taskq_t;

typedef stub_taskq_t taskq_t;
typedef stub_taskq_t ddi_taskq_t;

static taskq_t *
taskq_create(const char *name, int n, int pri, int min, int max, uint_t f)
{
	taskq_t *tq = calloc(1, sizeof (*tq));

	(void) n; (void) pri; (void) min; (void) max; (void) f;
	(void) snprintf(tq->tq_name, sizeof (tq->tq_name), "%s", name);
	tq->tq_live = 1;
	return (tq);
}

static void
stub_taskq_run(taskq_t *tq)
{
	while (tq->tq_pending > 0) {
		void (*func)(void *) = tq->tq_func[0];
		void *arg = tq->tq_arg[0];

		tq->tq_pending--;
		memmove(&tq->tq_func[0], &tq->tq_func[1],
		    sizeof (tq->tq_func[0]) * tq->tq_pending);
		memmove(&tq->tq_arg[0], &tq->tq_arg[1],
		    sizeof (tq->tq_arg[0]) * tq->tq_pending);
		func(arg);
	}
}

static void
taskq_wait(taskq_t *tq)
{
	stub_taskq_run(tq);
}

static void
taskq_destroy(taskq_t *tq)
{
	stub_taskq_run(tq);
	tq->tq_live = 0;
}

static void
taskq_dispatch_ent(taskq_t *tq, void (*func)(void *), void *arg, uint_t f,
    taskq_ent_t *ent)
{
	(void) f; (void) ent;
	if (!tq->tq_live)
		stub_fail("dispatch to a destroyed taskq");
	if (tq->tq_pending == 64)
		stub_fail("taskq overflow");
	tq->tq_func[tq->tq_pending] = func;
	tq->tq_arg[tq->tq_pending] = arg;
	tq->tq_pending++;
}

static ddi_taskq_t *
ddi_taskq_create(dev_info_t *dip, const char *name, int n, int pri, uint_t f)
{
	(void) dip;
	return (taskq_create(name, n, pri, 0, 0, f));
}

static void
ddi_taskq_destroy(ddi_taskq_t *tq)
{
	taskq_destroy(tq);
}

/* The old command path runs its taskq function inline. */
static int
ddi_taskq_dispatch(ddi_taskq_t *tq, void (*func)(void *), void *arg, uint_t f)
{
	(void) tq; (void) f;
	func(arg);
	return (DDI_SUCCESS);
}

/*
 * STREAMS messages. A freed mblk is kept and marked, so that a later device
 * read of it can be caught.
 */
typedef struct frtn {
	void		(*free_func)(caddr_t);
	caddr_t		free_arg;
} frtn_t;

typedef struct msgb {
	struct msgb	*b_cont;
	unsigned char	*b_rptr;
	unsigned char	*b_wptr;
	frtn_t		*b_frtn;
	int		b_freed;
} mblk_t;

#define	MBLKL(mp)	((mp)->b_wptr - (mp)->b_rptr)

static mblk_t *
desballoc(unsigned char *base, size_t size, uint_t pri, frtn_t *frtn)
{
	mblk_t *mp = calloc(1, sizeof (*mp));

	(void) pri;
	mp->b_rptr = base;
	mp->b_wptr = base + size;
	mp->b_frtn = frtn;
	return (mp);
}

static void
freeb(mblk_t *mp)
{
	if (mp->b_freed)
		stub_fail("freeb of a freed mblk");
	mp->b_freed = 1;
	if (mp->b_frtn != NULL)
		mp->b_frtn->free_func(mp->b_frtn->free_arg);
}

static void
freemsg(mblk_t *mp)
{
	while (mp != NULL) {
		mblk_t *next = mp->b_cont;

		freeb(mp);
		mp = next;
	}
}

#endif /* _MLXCX_STUB_H */

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
 * Just enough of the illumos kernel, on POSIX threads, for whole rdmak
 * source files to run on the build host: mutexes and condition variables,
 * kmem, thread-specific data, taskqs with real threads, and a panic() that
 * a test may catch.
 */

#ifndef _RDK_KENV_H
#define	_RDK_KENV_H

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef int boolean_t;
typedef unsigned int uint_t;
typedef unsigned char uchar_t;
typedef unsigned long ulong_t;
typedef unsigned long long u_longlong_t;
typedef long long longlong_t;
typedef int64_t hrtime_t;
typedef long clock_t_k;
typedef uint32_t ipaddr_t;
typedef int processorid_t;
typedef int lgrp_id_t;
typedef struct dev_info dev_info_t;
typedef struct cred cred_t;
typedef struct kthread kthread_t;
typedef uintptr_t callout_id_t;

#define	B_FALSE		0
#define	B_TRUE		1
#define	ETHERADDRL	6
#define	PAGESIZE	4096
#define	KM_SLEEP	0
#define	KM_NOSLEEP	1
#define	MUTEX_DRIVER	0
#define	CV_DRIVER	0
#define	RW_DRIVER	0
#define	CE_NOTE		1
#define	CE_WARN		2
#define	MILLISEC	1000
#define	MICROSEC	1000000
#define	NANOSEC		1000000000LL
#define	LGRP_NONE	(-1)
#define	TASKQ_PREPOPULATE	1
#define	CALLOUT_NORMAL	1
#define	minclsyspri	60
#define	USEC2NSEC(u)	((hrtime_t)(u) * 1000)
#define	MSEC2NSEC(m)	((hrtime_t)(m) * 1000000)
#define	SEC2NSEC(s)	((hrtime_t)(s) * NANOSEC)
#define	_NOTE(x)
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))
#define	ISP2(x)		(((x) & ((x) - 1)) == 0)
#define	P2ROUNDUP(x, a)	(-(-(x) & -(a)))
#define	P2PHASE(x, a)	((x) & ((a) - 1))
#ifndef MIN
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define	MAX(a, b)	((a) > (b) ? (a) : (b))
#endif
#ifndef howmany
#define	howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif
#define	CTASSERT(x)	_Static_assert((x), #x)

/* A panic() a test may catch by setting kenv_panic_jmp. */
static __thread jmp_buf *kenv_panic_jmp;
static __thread char kenv_panic_msg[256];

static void
panic(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(kenv_panic_msg, sizeof (kenv_panic_msg), fmt, ap);
	va_end(ap);
	if (kenv_panic_jmp != NULL)
		longjmp(*kenv_panic_jmp, 1);
	(void) fprintf(stderr, "panic: %s\n", kenv_panic_msg);
	abort();
}

#define	VERIFY(x)	do {						\
	if (!(x))							\
		panic("%s:%d: VERIFY(%s)", __FILE__, __LINE__, #x);	\
} while (0)
#define	VERIFY0(x)		VERIFY((x) == 0)
#define	VERIFY3U(a, o, b)	VERIFY((a)o(b))
#define	VERIFY3S(a, o, b)	VERIFY((a)o(b))
#define	VERIFY3P(a, o, b)	VERIFY((a)o(b))
#define	ASSERT(x)		VERIFY(x)
#define	ASSERT0(x)		VERIFY0(x)
#define	ASSERT3U(a, op, b)	VERIFY3U(a, op, b)
#define	ASSERT3S(a, op, b)	VERIFY3S(a, op, b)
#define	ASSERT3P(a, op, b)	VERIFY3P(a, op, b)

static int kenv_warnings;

static void
dev_err(dev_info_t *dip, int ce, const char *fmt, ...)
{
	(void) dip;
	if (ce == CE_WARN)
		__atomic_add_fetch(&kenv_warnings, 1, __ATOMIC_SEQ_CST);
	(void) fmt;
}

/* Threads: curthread is a per-thread tag. */
struct kthread {
	int	kt_unused;
};
static __thread struct kthread kenv_self;
#define	curthread	(&kenv_self)

/* Mutexes and condition variables. */
typedef struct {
	pthread_mutex_t	km_mutex;
	kthread_t	*km_owner;
} kmutex_t;
typedef pthread_cond_t kcondvar_t;

static inline void
mutex_init(kmutex_t *m, void *n, int t, void *a)
{
	(void) n; (void) t; (void) a;
	VERIFY0(pthread_mutex_init(&m->km_mutex, NULL));
	m->km_owner = NULL;
}

static inline void
mutex_destroy(kmutex_t *m)
{
	VERIFY(m->km_owner == NULL);
	VERIFY0(pthread_mutex_destroy(&m->km_mutex));
}

static inline void
mutex_enter(kmutex_t *m)
{
	VERIFY(m->km_owner != curthread);
	VERIFY0(pthread_mutex_lock(&m->km_mutex));
	m->km_owner = curthread;
}

static inline void
mutex_exit(kmutex_t *m)
{
	VERIFY(m->km_owner == curthread);
	m->km_owner = NULL;
	VERIFY0(pthread_mutex_unlock(&m->km_mutex));
}

#define	MUTEX_HELD(m)	((m)->km_owner == curthread)

static inline void
cv_init(kcondvar_t *c, void *n, int t, void *a)
{
	(void) n; (void) t; (void) a;
	VERIFY0(pthread_cond_init(c, NULL));
}

static inline void
cv_destroy(kcondvar_t *c)
{
	VERIFY0(pthread_cond_destroy(c));
}

static inline void
cv_wait(kcondvar_t *c, kmutex_t *m)
{
	m->km_owner = NULL;
	VERIFY0(pthread_cond_wait(c, &m->km_mutex));
	m->km_owner = curthread;
}

static inline void
cv_broadcast(kcondvar_t *c)
{
	VERIFY0(pthread_cond_broadcast(c));
}

static inline void
cv_signal(kcondvar_t *c)
{
	VERIFY0(pthread_cond_signal(c));
}

static inline hrtime_t
gethrtime(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((hrtime_t)ts.tv_sec * NANOSEC + ts.tv_nsec);
}

/* Ticks are milliseconds here. */
static inline long
ddi_get_lbolt(void)
{
	return ((long)(gethrtime() / 1000000));
}

static inline long
drv_usectohz(long us)
{
	return (MAX(us / 1000, 1));
}

/* Returns -1 once the absolute tick deadline passes. */
static inline int
cv_timedwait(kcondvar_t *c, kmutex_t *m, long deadline)
{
	struct timespec ts;
	hrtime_t now = gethrtime(), until;
	int r;

	until = now + (hrtime_t)(deadline - ddi_get_lbolt()) * 1000000;
	if (until <= now)
		return (-1);
	(void) clock_gettime(CLOCK_REALTIME, &ts);
	until = (hrtime_t)ts.tv_sec * NANOSEC + ts.tv_nsec + (until - now);
	ts.tv_sec = until / NANOSEC;
	ts.tv_nsec = until % NANOSEC;
	m->km_owner = NULL;
	r = pthread_cond_timedwait(c, &m->km_mutex, &ts);
	m->km_owner = curthread;
	return (r == ETIMEDOUT ? -1 : 1);
}

/* kmem: the size must match, and ASan catches the rest. */
static inline void *
kmem_zalloc(size_t n, int flag)
{
	size_t *p;

	(void) flag;
	p = calloc(1, n + 16);
	VERIFY(p != NULL);
	*p = n;
	return ((char *)p + 16);
}

static inline void *
kmem_alloc(size_t n, int flag)
{
	return (kmem_zalloc(n, flag));
}

static inline void
kmem_free(void *p, size_t n)
{
	size_t *h = (size_t *)(void *)((char *)p - 16);

	VERIFY(*h == n);
	free(h);
}

/* Atomics and barriers. */
#define	atomic_inc_32(p)	((void) __atomic_add_fetch((p), 1, \
	__ATOMIC_SEQ_CST))
#define	atomic_dec_32(p)	((void) __atomic_sub_fetch((p), 1, \
	__ATOMIC_SEQ_CST))
#define	atomic_inc_64(p)	((void) __atomic_add_fetch((p), 1, \
	__ATOMIC_SEQ_CST))
#define	atomic_or_32(p, v)	((void) __atomic_or_fetch((p), (v), \
	__ATOMIC_SEQ_CST))
#define	atomic_and_32(p, v)	((void) __atomic_and_fetch((p), (v), \
	__ATOMIC_SEQ_CST))
#define	atomic_swap_ptr(p, v)	__atomic_exchange_n((void **)(p), (v), \
	__ATOMIC_SEQ_CST)
static inline void *
atomic_cas_ptr(volatile void *p, void *old, void *new)
{
	void *o = old;

	(void) __atomic_compare_exchange_n((void **)(uintptr_t)p, &o, new,
	    0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return (o);
}
#define	membar_producer()	__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define	membar_consumer()	__atomic_thread_fence(__ATOMIC_SEQ_CST)

/* Thread-specific data. */
#define	KENV_TSD_KEYS	8
static uint_t kenv_tsd_next = 1;
static __thread void *kenv_tsd[KENV_TSD_KEYS];

static inline void
tsd_create(uint_t *key, void (*dtor)(void *))
{
	(void) dtor;
	VERIFY(kenv_tsd_next < KENV_TSD_KEYS);
	*key = kenv_tsd_next++;
}

static inline void
tsd_destroy(uint_t *key)
{
	*key = 0;
}

static inline void *
tsd_get(uint_t key)
{
	return (key == 0 ? NULL : kenv_tsd[key]);
}

static inline int
tsd_set(uint_t key, void *v)
{
	if (key == 0)
		return (EINVAL);
	kenv_tsd[key] = v;
	return (0);
}

/* Taskqs: a FIFO served by real threads. */
typedef struct taskq_ent {
	struct taskq_ent	*tqent_next;
	void			(*tqent_func)(void *);
	void			*tqent_arg;
} taskq_ent_t;

typedef struct taskq {
	pthread_mutex_t	tq_lock;
	pthread_cond_t	tq_cv;
	taskq_ent_t	*tq_head;
	taskq_ent_t	*tq_tail;
	int		tq_nthreads;
	int		tq_active;
	int		tq_exit;
	pthread_t	tq_threads[16];
} taskq_t;

static int ncpus = 4;

static void *
kenv_taskq_thread(void *arg)
{
	taskq_t *tq = arg;
	taskq_ent_t *e;

	(void) pthread_mutex_lock(&tq->tq_lock);
	for (;;) {
		while (tq->tq_head == NULL && !tq->tq_exit)
			(void) pthread_cond_wait(&tq->tq_cv, &tq->tq_lock);
		if ((e = tq->tq_head) == NULL)
			break;
		if ((tq->tq_head = e->tqent_next) == NULL)
			tq->tq_tail = NULL;
		tq->tq_active++;
		(void) pthread_mutex_unlock(&tq->tq_lock);
		e->tqent_func(e->tqent_arg);
		(void) pthread_mutex_lock(&tq->tq_lock);
		tq->tq_active--;
		(void) pthread_cond_broadcast(&tq->tq_cv);
	}
	(void) pthread_mutex_unlock(&tq->tq_lock);
	return (NULL);
}

static inline taskq_t *
taskq_create(const char *name, int n, int pri, int min, int max, uint_t fl)
{
	taskq_t *tq = calloc(1, sizeof (*tq));
	int i;

	(void) name; (void) pri; (void) min; (void) max; (void) fl;
	(void) pthread_mutex_init(&tq->tq_lock, NULL);
	(void) pthread_cond_init(&tq->tq_cv, NULL);
	tq->tq_nthreads = MIN(n, 16);
	for (i = 0; i < tq->tq_nthreads; i++) {
		VERIFY0(pthread_create(&tq->tq_threads[i], NULL,
		    kenv_taskq_thread, tq));
	}
	return (tq);
}

static inline void
taskq_dispatch_ent(taskq_t *tq, void (*func)(void *), void *arg, uint_t fl,
    taskq_ent_t *e)
{
	(void) fl;
	e->tqent_func = func;
	e->tqent_arg = arg;
	e->tqent_next = NULL;
	(void) pthread_mutex_lock(&tq->tq_lock);
	if (tq->tq_tail != NULL)
		tq->tq_tail->tqent_next = e;
	else
		tq->tq_head = e;
	tq->tq_tail = e;
	(void) pthread_cond_broadcast(&tq->tq_cv);
	(void) pthread_mutex_unlock(&tq->tq_lock);
}

static inline void
taskq_wait(taskq_t *tq)
{
	(void) pthread_mutex_lock(&tq->tq_lock);
	while (tq->tq_head != NULL || tq->tq_active != 0)
		(void) pthread_cond_wait(&tq->tq_cv, &tq->tq_lock);
	(void) pthread_mutex_unlock(&tq->tq_lock);
}

static inline void
taskq_destroy(taskq_t *tq)
{
	int i;

	taskq_wait(tq);
	(void) pthread_mutex_lock(&tq->tq_lock);
	tq->tq_exit = 1;
	(void) pthread_cond_broadcast(&tq->tq_cv);
	(void) pthread_mutex_unlock(&tq->tq_lock);
	for (i = 0; i < tq->tq_nthreads; i++)
		(void) pthread_join(tq->tq_threads[i], NULL);
	(void) pthread_cond_destroy(&tq->tq_cv);
	(void) pthread_mutex_destroy(&tq->tq_lock);
	free(tq);
}

/* Moderation timeouts are never set by these tests. */
static inline callout_id_t
timeout_generic(int type, void (*func)(void *), void *arg, hrtime_t exp,
    hrtime_t res, int flags)
{
	(void) type; (void) func; (void) arg; (void) exp; (void) res;
	(void) flags;
	panic("timeout_generic: not in this environment");
	return (0);
}

static inline hrtime_t
untimeout_generic(callout_id_t id, int nowait)
{
	(void) id; (void) nowait;
	return (-1);
}

/* The DDI types rdk.h names. */
typedef struct {
	uint64_t	dmac_laddress;
	uint64_t	dmac_size;
	uint_t		dmac_type;
} ddi_dma_cookie_t;

typedef struct list_node {
	struct list_node	*list_next;
	struct list_node	*list_prev;
} list_node_t;

typedef struct list {
	size_t		list_size;
	size_t		list_offset;
	list_node_t	list_head;
} list_t;

typedef struct {
	pthread_rwlock_t	krw;
} krwlock_t;

#endif /* _RDK_KENV_H */

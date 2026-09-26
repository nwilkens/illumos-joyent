/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/* Just enough of the kernel for the extracted t4nex sources to run. */

#ifndef _KSTUB_H
#define	_KSTUB_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef unsigned int uint_t;
typedef int boolean_t;
typedef int kmutex_t;
typedef int kcondvar_t;
typedef struct kthread kthread_t;

#define	B_FALSE	0
#define	B_TRUE	1
#define	KM_SLEEP	0
#define	KM_NOSLEEP	1
#define	MUTEX_DRIVER	0
#define	CV_DRIVER	0
#define	DDI_INTR_PRI(p)	((void *)(uintptr_t)(p))
#define	MUTEX_HELD(m)	(*(m) != 0)
#ifndef howmany
#define	howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif
#ifndef MIN
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define	MAX(a, b)	((a) > (b) ? (a) : (b))
#endif

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		exit(1);						\
	}								\
} while (0)
#define	ASSERT(x)		CHECK(x)
#define	VERIFY(x)		CHECK(x)
#define	ASSERT3U(a, op, b)	CHECK(a op b)
#define	VERIFY3U(a, op, b)	CHECK(a op b)
#define	ASSERT3P(a, op, b)	CHECK(a op b)
#define	VERIFY3P(a, op, b)	CHECK(a op b)

static int stub_in_intr;
static int stub_nosleep_fail;
static int stub_cv_waits;

static inline int
servicing_interrupt(void)
{
	return (stub_in_intr);
}

static inline void
mutex_init(kmutex_t *m, void *n, int t, void *p)
{
	(void) n; (void) t; (void) p;
	*m = 0;
}

static inline void
mutex_destroy(kmutex_t *m)
{
	CHECK(*m == 0);
}

static inline void
mutex_enter(kmutex_t *m)
{
	CHECK(*m == 0);
	*m = 1;
}

static inline void
mutex_exit(kmutex_t *m)
{
	CHECK(*m == 1);
	*m = 0;
}

static inline void
cv_init(kcondvar_t *c, void *n, int t, void *a)
{
	(void) n; (void) t; (void) a;
	*c = 0;
}

static inline void
cv_destroy(kcondvar_t *c)
{
	(void) c;
}

static inline void
cv_broadcast(kcondvar_t *c)
{
	(*c)++;
}

/* A waiter in a single threaded test would sleep forever: fail instead. */
static inline void
cv_wait(kcondvar_t *c, kmutex_t *m)
{
	(void) c;
	CHECK(*m == 1);
	stub_cv_waits++;
	CHECK(!"cv_wait with nobody to wake it");
}

static inline void *
kmem_zalloc(size_t n, int flag)
{
	if (flag == KM_NOSLEEP && stub_nosleep_fail > 0) {
		stub_nosleep_fail--;
		return (NULL);
	}
	return (calloc(1, n));
}

static inline void
kmem_free(void *p, size_t n)
{
	(void) n;
	free(p);
}

#endif /* _KSTUB_H */

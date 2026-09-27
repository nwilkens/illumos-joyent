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
 * Run the rdmak DMA quarantine from rdk_device.c and rdk_verbs.c with the
 * irdma consumer buffer free from irdma_osdep.c: memory is given back only
 * while the device is not tainted, a failed deregistration taints it, an
 * irdma taint reaches rdmak, and a tainted device leaks what it is given.
 */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef int boolean_t;
typedef unsigned long long u_longlong_t;
typedef int kmutex_t;
typedef uint64_t dma_addr_t;
typedef void dev_info_t;
#define	B_TRUE		1
#define	B_FALSE		0
#define	CE_WARN		1
#define	ASSERT(x)	assert(x)

static inline void mutex_enter(kmutex_t *m) { assert(*m == 0); *m = 1; }
static inline void mutex_exit(kmutex_t *m) { assert(*m == 1); *m = 0; }
static inline void atomic_or_32(volatile uint32_t *p, uint32_t v) { *p |= v; }
static inline void atomic_dec_32(volatile uint32_t *p) { (*p)--; }
static inline void kmem_free(void *p, size_t n) { (void) n; free(p); }

static int warnings;

static void
dev_err(dev_info_t *dip, int ce, const char *fmt, ...)
{
	(void) dip; (void) ce; (void) fmt;
	warnings++;
}

/* A doubly linked list with just the calls the bodies make. */
typedef struct list_node {
	struct list_node *next, *prev;
} list_node_t;

typedef struct {
	list_node_t	head;
	size_t		off;
} list_t;

static void
list_init(list_t *l, size_t off)
{
	l->head.next = l->head.prev = &l->head;
	l->off = off;
}

static void *
list_obj(list_t *l, list_node_t *n)
{
	return (n == &l->head ? NULL : (char *)n - l->off);
}

static void *list_head(list_t *l) { return (list_obj(l, l->head.next)); }

static void *
list_next(list_t *l, void *o)
{
	return (list_obj(l, ((list_node_t *)((char *)o + l->off))->next));
}

static void
list_insert_tail(list_t *l, void *o)
{
	list_node_t *n = (list_node_t *)((char *)o + l->off);

	n->prev = l->head.prev;
	n->next = &l->head;
	l->head.prev->next = n;
	l->head.prev = n;
}

static void
list_remove(list_t *l, void *o)
{
	list_node_t *n = (list_node_t *)((char *)o + l->off);

	(void) l;
	n->prev->next = n->next;
	n->next->prev = n->prev;
}

/* rdmak */
struct rdk_device_priv {
	kmutex_t	rdp_lock;
	uint64_t	rdp_leaked;
	uint64_t	rdp_leaked_bytes;
	uint64_t	rdp_nobjs;
};

struct rdk_device;
struct rdk_mr;
typedef struct { void *rdb_va; } rdk_dma_buf_t;

struct rdk_device_ops {
	int	(*dereg_mr)(struct rdk_mr *);
	void	(*dma_free)(struct rdk_device *, rdk_dma_buf_t *);
};

struct rdk_device {
	char				rd_name[32];
	dev_info_t			*rd_dip;
	const struct rdk_device_ops	*rd_ops;
	struct rdk_device_priv		*rd_priv;
	volatile uint32_t		rd_tainted;
};

struct rdk_pd {
	volatile uint32_t	usecnt;
};

struct rdk_mr {
	struct rdk_device	*device;
	struct rdk_pd		*pd;
};

static void
rdk_obj_rele(struct rdk_device *dev)
{
	dev->rd_priv->rdp_nobjs--;
}

/* irdma */
#include "quar_flags.h"

typedef struct ice_rdma_dma { int unused; } ice_rdma_dma_t;

typedef struct {
	boolean_t	(*iro_resetting)(void *);
	void		(*iro_dma_free)(void *, ice_rdma_dma_t *, boolean_t);
} ice_rdma_ops_t;

typedef struct irdma irdma_t;

struct device {
	irdma_t		*od_irdma;
	kmutex_t	od_lock;
	list_t		od_bufs;
	list_t		od_deferred;
	uint32_t	od_nbufs;
};

typedef struct irdma_osbuf {
	list_node_t	iob_node;
	void		*iob_va;
	dma_addr_t	iob_pa;
	size_t		iob_len;
	ice_rdma_dma_t	*iob_dma;
} irdma_osbuf_t;

struct irdma {
	volatile uint32_t	irdma_flags;
	const ice_rdma_ops_t	*irdma_ops;
	void			*irdma_peer;
	struct device		irdma_osdev;
	struct rdk_device	irdma_rdk;
};

static void
irdma_error(irdma_t *irdma, const char *fmt, ...)
{
	(void) irdma; (void) fmt;
	warnings++;
}

/* Frees made while a CQP command is outstanding are core frees only. */
static boolean_t
irdma_quiesced(irdma_t *irdma)
{
	return ((irdma->irdma_flags & IRDMA_F_TAINTED) == 0);
}

#include "quar_bodies.h"

static boolean_t resetting;
static int frees, last_quiesced = -1;

static boolean_t
fake_resetting(void *peer)
{
	(void) peer;
	return (resetting);
}

static void
fake_ice_free(void *peer, ice_rdma_dma_t *dma, boolean_t quiesced)
{
	(void) peer; (void) dma;
	frees++;
	last_quiesced = quiesced;
}

static const ice_rdma_ops_t ice_ops = {
	.iro_resetting = fake_resetting,
	.iro_dma_free = fake_ice_free
};

static int dereg_ret, provider_frees;

static int
fake_dereg(struct rdk_mr *mr)
{
	(void) mr;
	return (dereg_ret);
}

static void
fake_dma_free(struct rdk_device *dev, rdk_dma_buf_t *buf)
{
	(void) dev; (void) buf;
	provider_frees++;
}

static const struct rdk_device_ops fake_ops = {
	.dereg_mr = fake_dereg,
	.dma_free = fake_dma_free
};

static int released;

static void
release(void *arg)
{
	released += *(int *)arg;
}

static void
setup(irdma_t *irdma, struct rdk_device_priv *priv)
{
	memset(irdma, 0, sizeof (*irdma));
	memset(priv, 0, sizeof (*priv));
	irdma->irdma_ops = &ice_ops;
	irdma->irdma_osdev.od_irdma = irdma;
	list_init(&irdma->irdma_osdev.od_bufs,
	    offsetof(irdma_osbuf_t, iob_node));
	list_init(&irdma->irdma_osdev.od_deferred,
	    offsetof(irdma_osbuf_t, iob_node));
	(void) strcpy(irdma->irdma_rdk.rd_name, "irdma0");
	irdma->irdma_rdk.rd_ops = &fake_ops;
	irdma->irdma_rdk.rd_priv = priv;
	resetting = B_FALSE;
}

/* A consumer buffer on the osdep list, as dma_alloc_coherent() makes it. */
static irdma_osbuf_t *
consumer_buf(irdma_t *irdma, uintptr_t va)
{
	irdma_osbuf_t *b = calloc(1, sizeof (*b));
	static ice_rdma_dma_t dma;

	b->iob_va = (void *)va;
	b->iob_pa = va + 0x1000;
	b->iob_len = 4096;
	b->iob_dma = &dma;
	list_insert_tail(&irdma->irdma_osdev.od_bufs, b);
	irdma->irdma_osdev.od_nbufs++;
	return (b);
}

static void
consumer_free(irdma_t *irdma, uintptr_t va)
{
	irdma_osdep_free_consumer(irdma, (void *)va, va + 0x1000, 4096);
}

int
main(void)
{
	struct rdk_device_priv priv;
	struct rdk_pd pd;
	struct rdk_mr mr;
	irdma_t irdma;
	rdk_dma_buf_t buf;
	int one = 1;

	/* A healthy device returns memory at once. */
	setup(&irdma, &priv);
	(void) consumer_buf(&irdma, 0x10000);
	consumer_free(&irdma, 0x10000);
	assert(frees == 1 && last_quiesced == B_TRUE);
	assert(rdk_dma_release(&irdma.irdma_rdk, release, &one, 64));
	assert(released == 1 && priv.rdp_leaked == 0);

	/* A reset in progress holds provider buffers but taints nothing. */
	resetting = B_TRUE;
	(void) consumer_buf(&irdma, 0x20000);
	consumer_free(&irdma, 0x20000);
	assert(frees == 2 && last_quiesced == B_FALSE);
	assert(!rdk_device_tainted(&irdma.irdma_rdk));
	assert(rdk_dma_release(&irdma.irdma_rdk, release, &one, 64));
	assert(released == 2);

	/* A deregistration the device did not confirm taints rdmak. */
	setup(&irdma, &priv);
	memset(&pd, 0, sizeof (pd));
	pd.usecnt = 1;
	mr.device = &irdma.irdma_rdk;
	mr.pd = &pd;
	priv.rdp_nobjs = 1;
	dereg_ret = 0;
	assert(rdk_dereg_mr(&mr) == 0 && !rdk_device_tainted(&irdma.irdma_rdk));
	pd.usecnt = 1;
	priv.rdp_nobjs = 1;
	dereg_ret = EIO;
	assert(rdk_dereg_mr(&mr) == EIO);
	assert(pd.usecnt == 0 && priv.rdp_nobjs == 0);
	assert(rdk_device_tainted(&irdma.irdma_rdk));
	/* irdma honors the rdmak taint for consumer memory... */
	(void) consumer_buf(&irdma, 0x30000);
	consumer_free(&irdma, 0x30000);
	assert(frees == 3 && last_quiesced == B_FALSE);
	/* ...and rdk_dma_release() leaks, warning once. */
	warnings = 0;
	assert(!rdk_dma_release(&irdma.irdma_rdk, release, &one, 4096));
	assert(!rdk_dma_release(&irdma.irdma_rdk, release, &one, 8192));
	assert(released == 2 && priv.rdp_leaked == 2 &&
	    priv.rdp_leaked_bytes == 12288 && warnings == 1);
	/* Provider buffers still go to the provider, which holds them. */
	priv.rdp_nobjs = 1;
	buf.rdb_va = (void *)0x40000;
	rdk_dma_buf_free(&irdma.irdma_rdk, &buf);
	assert(provider_frees == 1 && buf.rdb_va == NULL);

	/* An irdma taint reaches rdmak, and stays. */
	setup(&irdma, &priv);
	assert(!rdk_device_tainted(&irdma.irdma_rdk));
	irdma_taint(&irdma);
	assert(rdk_device_tainted(&irdma.irdma_rdk));
	(void) consumer_buf(&irdma, 0x50000);
	consumer_free(&irdma, 0x50000);
	assert(last_quiesced == B_FALSE);
	assert(!rdk_dma_release(&irdma.irdma_rdk, release, &one, 1));
	assert(released == 2);

	/* An unknown buffer is refused, never handed to ice. */
	warnings = 0;
	consumer_free(&irdma, 0x60000);
	assert(frees == 4 && warnings == 1);

	(void) printf("PASS: DMA quarantine lifecycle\n");
	return (0);
}

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
 * The registered data buffer pool of a device.  STMF gets its data buffers
 * here (sbd copies between them and the LU), and every RDMA transfer's
 * local memory is one of them, so a transfer names a single cookie with
 * the device's local DMA key and no pool memory ever has a remote key.
 *
 * The pool grows by chunks in a taskq, since allocation happens where it
 * must not sleep, up to nvmf_rdma_pool_max.  A buffer freed while its
 * device is tainted is never reused.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/vmem.h>
#include <sys/errno.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "nvmf_rdma_impl.h"

#define	NR_POOL_CHUNK	(1024 * 1024)

uint32_t nvmf_rdma_dbuf_max = 128 * 1024;
size_t nvmf_rdma_pool_init = 32 * 1024 * 1024;
size_t nvmf_rdma_pool_max = 512 * 1024 * 1024;

static void nr_pool_grow_task(void *);

/* Chunk i spans [2i + 1, 2i + 2) chunk sizes of the arena. */
static uintptr_t
nr_pool_base(uint_t i)
{
	return ((uintptr_t)(2 * i + 1) * NR_POOL_CHUNK);
}

int
nr_pool_init(nr_dev_t *nd)
{
	nr_pool_t *p = &nd->nd_pool;

	mutex_init(&p->np_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&p->np_cv, NULL, CV_DRIVER, NULL);
	p->np_maxchunks = (uint_t)MAX(1, nvmf_rdma_pool_max / NR_POOL_CHUNK);
	p->np_chunks = kmem_zalloc(sizeof (rdk_dma_buf_t) * p->np_maxchunks,
	    KM_SLEEP);
	p->np_arena = vmem_create("nvmf_rdma_pool", NULL, 0, PAGESIZE, NULL,
	    NULL, NULL, 0, VM_SLEEP);
	return (0);
}

/* Add one chunk; thread context. */
static int
nr_pool_add(nr_dev_t *nd)
{
	nr_pool_t *p = &nd->nd_pool;
	rdk_dma_buf_t b;
	uint_t i;
	int ret;

	mutex_enter(&p->np_lock);
	if (p->np_dying || p->np_nchunks == p->np_maxchunks) {
		mutex_exit(&p->np_lock);
		return (ENOSPC);
	}
	mutex_exit(&p->np_lock);

	if ((ret = rdk_dma_buf_alloc(nd->nd_dev, NR_POOL_CHUNK, &b)) != 0)
		return (ret);

	mutex_enter(&p->np_lock);
	if (p->np_dying || p->np_nchunks == p->np_maxchunks) {
		mutex_exit(&p->np_lock);
		rdk_dma_buf_free(nd->nd_dev, &b);
		return (ENOSPC);
	}
	i = p->np_nchunks;
	if (vmem_add(p->np_arena, (void *)nr_pool_base(i), NR_POOL_CHUNK,
	    VM_NOSLEEP) == NULL) {
		mutex_exit(&p->np_lock);
		rdk_dma_buf_free(nd->nd_dev, &b);
		return (ENOMEM);
	}
	p->np_chunks[i] = b;
	p->np_nchunks++;
	p->np_free += NR_POOL_CHUNK;
	mutex_exit(&p->np_lock);
	return (0);
}

/* Fill the pool to its initial size for a new listener. */
int
nr_pool_prime(nr_dev_t *nd)
{
	nr_pool_t *p = &nd->nd_pool;
	int ret = 0;

	for (;;) {
		mutex_enter(&p->np_lock);
		if ((size_t)p->np_nchunks * NR_POOL_CHUNK >=
		    nvmf_rdma_pool_init) {
			mutex_exit(&p->np_lock);
			return (0);
		}
		mutex_exit(&p->np_lock);
		if ((ret = nr_pool_add(nd)) != 0)
			return (ret == ENOSPC ? 0 : ret);
	}
}

static void
nr_pool_grow_task(void *arg)
{
	nr_dev_t *nd = arg;
	nr_pool_t *p = &nd->nd_pool;

	(void) nr_pool_add(nd);
	mutex_enter(&p->np_lock);
	p->np_growing = B_FALSE;
	cv_broadcast(&p->np_cv);
	mutex_exit(&p->np_lock);
}

/* Grow ahead of need once a quarter or less is free. */
static void
nr_pool_kick_locked(nr_dev_t *nd)
{
	nr_pool_t *p = &nd->nd_pool;

	ASSERT(MUTEX_HELD(&p->np_lock));
	if (p->np_growing || p->np_dying || p->np_nchunks == p->np_maxchunks ||
	    p->np_free > (size_t)p->np_nchunks * NR_POOL_CHUNK / 4)
		return;
	p->np_growing = B_TRUE;
	taskq_dispatch_ent(nvmf_rdma_taskq, nr_pool_grow_task, nd, 0,
	    &p->np_grow_ent);
}

/* A buffer of min_len to len bytes, or NULL; never sleeps. */
nr_buf_t *
nr_buf_alloc(nr_dev_t *nd, size_t len, size_t min_len)
{
	nr_pool_t *p = &nd->nd_pool;
	uintptr_t addr = 0;
	size_t want, off;
	nr_buf_t *b;
	uint_t i;

	if (len == 0 || min_len > len)
		return (NULL);
	len = MIN(len, nvmf_rdma_dbuf_max);
	if (min_len > len)
		return (NULL);
	if ((b = kmem_zalloc(sizeof (*b), KM_NOSLEEP)) == NULL)
		return (NULL);

	mutex_enter(&p->np_lock);
	for (want = len; ; want = min_len) {
		if (!p->np_dying) {
			addr = (uintptr_t)vmem_alloc(p->np_arena,
			    P2ROUNDUP(want, PAGESIZE), VM_NOSLEEP | VM_BESTFIT);
		}
		if (addr != 0 || want == min_len)
			break;
	}
	if (addr == 0) {
		nr_pool_kick_locked(nd);
		mutex_exit(&p->np_lock);
		kmem_free(b, sizeof (*b));
		return (NULL);
	}
	want = P2ROUNDUP(want, PAGESIZE);
	p->np_free -= want;
	p->np_bufs++;
	nr_pool_kick_locked(nd);
	i = (uint_t)(addr / NR_POOL_CHUNK - 1) / 2;
	off = addr % NR_POOL_CHUNK;
	ASSERT3U(i, <, p->np_nchunks);
	ASSERT3U(off + want, <=, NR_POOL_CHUNK);
	b->nb_dev = nd;
	b->nb_key = addr;
	b->nb_len = MIN(want, len);
	b->nb_va = p->np_chunks[i].rdb_va + off;
	b->nb_ck.dmac_laddress = p->np_chunks[i].rdb_pa + off;
	b->nb_ck.dmac_size = b->nb_len;
	mutex_exit(&p->np_lock);
	return (b);
}

void
nr_buf_free(nr_buf_t *b)
{
	nr_dev_t *nd = b->nb_dev;
	nr_pool_t *p = &nd->nd_pool;
	size_t len = P2ROUNDUP(b->nb_len, PAGESIZE);

	mutex_enter(&p->np_lock);
	if (rdk_device_tainted(nd->nd_dev)) {
		p->np_leaked++;
	} else {
		vmem_free(p->np_arena, (void *)b->nb_key, len);
		p->np_free += len;
	}
	ASSERT3U(p->np_bufs, >, 0);
	if (--p->np_bufs == 0)
		cv_broadcast(&p->np_cv);
	mutex_exit(&p->np_lock);
	kmem_free(b, sizeof (*b));
}

/*
 * Wait for STMF to give back every buffer, then free the chunks.  A tainted
 * device's provider keeps the chunks until it is reset.
 */
void
nr_pool_fini(nr_dev_t *nd)
{
	nr_pool_t *p = &nd->nd_pool;
	uint_t i;

	mutex_enter(&p->np_lock);
	p->np_dying = B_TRUE;
	while (p->np_bufs != 0 || p->np_growing) {
		if (cv_reltimedwait(&p->np_cv, &p->np_lock, SEC_TO_TICK(10),
		    TR_SEC) == -1 && p->np_bufs != 0) {
			dev_err(nd->nd_dev->rd_dip, CE_WARN, "!nvmf_rdma: "
			    "waiting for %u data buffers", p->np_bufs);
		}
	}
	mutex_exit(&p->np_lock);

	/* A leaked buffer keeps its span, so the arena stays too. */
	if (p->np_leaked == 0)
		vmem_destroy(p->np_arena);
	for (i = 0; i < p->np_nchunks; i++)
		rdk_dma_buf_free(nd->nd_dev, &p->np_chunks[i]);
	kmem_free(p->np_chunks, sizeof (rdk_dma_buf_t) * p->np_maxchunks);
	cv_destroy(&p->np_cv);
	mutex_destroy(&p->np_lock);
}

int
nr_alloc_data_buf(struct nvmf_qpair *nq, size_t len, size_t min_len,
    nvmf_databuf_t *db)
{
	nr_queue_t *q = NR_Q(nq);
	nr_buf_t *b;

	len = MIN(len, q->nq_xfer_len);
	if ((b = nr_buf_alloc(q->nq_dev, len, min_len)) == NULL)
		return (ENOMEM);
	db->ndb_addr = b->nb_va;
	db->ndb_len = b->nb_len;
	db->ndb_cookies = &b->nb_ck;
	db->ndb_ncookies = 1;
	db->ndb_priv = b;
	return (0);
}

void
nr_free_data_buf(nvmf_databuf_t *db)
{
	nr_buf_free(db->ndb_priv);
}

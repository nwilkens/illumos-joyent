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
 * A work queue that firmware would not stop may still write its posted RX
 * buffers and read its TX buffers. Detach must keep them, and the mblks and
 * loaned buffers that TX buffers refer to, until TEARDOWN_HCA succeeds, and
 * leak them if it does not. Loaned buffers that the stack returns after such
 * a leak must not touch the freed mlxcx_t.
 */

#define	DEBUG	1

#include "mlxcx_stub.h"
#include <mlxcx_reg.h>
#include "mlxcx_types.h"
#include "mlxcx_bufs_types.h"
#include "mlxcx_min.h"

static boolean_t
stub_sleep_hook(clock_t deadline)
{
	(void) deadline;
	return (B_FALSE);
}

static void
mlxcx_dma_buf_attr(mlxcx_t *mlxp, ddi_dma_attr_t *attr)
{
	(void) mlxp;
	memset(attr, 0, sizeof (*attr));
}

static boolean_t
mlxcx_dma_alloc_offset(mlxcx_t *mlxp, mlxcx_dma_buffer_t *dma,
    ddi_dma_attr_t *attr, ddi_device_acc_attr_t *acc, boolean_t zero,
    size_t size, size_t offset, boolean_t wait)
{
	(void) offset;
	return (mlxcx_dma_alloc(mlxp, dma, attr, acc, zero, size, wait));
}

static boolean_t
mlxcx_dma_init(mlxcx_t *mlxp, mlxcx_dma_buffer_t *dma, ddi_dma_attr_t *attr,
    boolean_t wait)
{
	(void) mlxp; (void) attr; (void) wait;
	memset(dma, 0, sizeof (*dma));
	return (B_TRUE);
}

static void
mlxcx_dma_unbind(mlxcx_t *mlxp, mlxcx_dma_buffer_t *dma)
{
	(void) mlxp;
	dma->mxdb_flags &= ~MLXCX_DMABUF_BOUND;
}

/* Firmware that will not stop a work queue. */
static boolean_t
stop_fails(mlxcx_t *mlxp, mlxcx_work_queue_t *wq)
{
	(void) mlxp; (void) wq;
	return (B_FALSE);
}

static boolean_t
destroy_cmd(mlxcx_t *mlxp, mlxcx_work_queue_t *wq)
{
	(void) mlxp; (void) wq;
	stub_fail("destroy of a work queue that did not stop");
	return (B_FALSE);
}

#define	mlxcx_cmd_stop_rq	stop_fails
#define	mlxcx_cmd_stop_sq	stop_fails
#define	mlxcx_cmd_destroy_rq	destroy_cmd
#define	mlxcx_cmd_destroy_sq	destroy_cmd

/* Module linkage for _fini(). */
#include <errno.h>
static int mod_removed;
static int mlxcx_modlinkage, mlxcx_dev_ops;
static void *mlxcx_softstate;

static int
mod_remove(int *ml)
{
	(void) ml;
	mod_removed = 1;
	return (DDI_SUCCESS);
}

static void
mac_fini_ops(int *ops)
{
	(void) ops;
}

static void
ddi_soft_state_fini(void **ss)
{
	(void) ss;
}

void mlxcx_buf_return(mlxcx_t *, mlxcx_buffer_t *);
void mlxcx_buf_destroy(mlxcx_t *, mlxcx_buffer_t *);
void mlxcx_buf_return_chain(mlxcx_t *, mlxcx_buffer_t *, boolean_t);
void mlxcx_buf_quarantine_free(mlxcx_t *, mlxcx_buffer_t *);
void mlxcx_dma_quarantine(mlxcx_t *, mlxcx_dma_buffer_t *);
mlxcx_buf_shard_t *mlxcx_mlbs_create(mlxcx_t *);

#include "mlxcx_bufs_body.h"

static mlxcx_t mlx;
static mlxcx_port_t port;

/* What hardware still holds: RX buffer memory and TX mblks. */
static uint64_t hw_pa[32];
static uint_t hw_npa;
static mblk_t *hw_mp[8];
static uint_t hw_nmp;
static boolean_t hw_stopped;

static void
hw_access(void)
{
	if (hw_stopped)
		return;
	for (uint_t i = 0; i < hw_npa; i++)
		*(volatile uint8_t *)stub_dma_va(hw_pa[i], 1, B_TRUE) = 1;
	for (uint_t i = 0; i < hw_nmp; i++) {
		if (hw_mp[i]->b_freed)
			stub_fail("hardware reads a freed TX mblk");
	}
}

static void
setup(void)
{
	memset(&mlx, 0, sizeof (mlx));
	mlx.mlx_ports = &port;
	port.mlp_mtu = 1500;
	mutex_init(&mlx.mlx_quarantine_mtx, NULL, MUTEX_DRIVER, NULL);
	list_create(&mlx.mlx_quarantine, sizeof (mlxcx_dma_quarantine_t),
	    offsetof(mlxcx_dma_quarantine_t, mdq_node));
#ifdef HAVE_BUF_QUARANTINE
	list_create(&mlx.mlx_quarantine_bufs, sizeof (mlxcx_buffer_t),
	    offsetof(mlxcx_buffer_t, mlb_entry));
#endif
	list_create(&mlx.mlx_wqs, sizeof (mlxcx_work_queue_t),
	    offsetof(mlxcx_work_queue_t, mlwq_entry));
	(void) mlxcx_setup_bufs(&mlx);
}

static mlxcx_buf_shard_t *
shard(uint_t n, boolean_t foreign)
{
	mlxcx_buf_shard_t *s = mlxcx_mlbs_create(&mlx);
	mlxcx_buffer_t *b;

	for (uint_t i = 0; i < n; i++) {
		if (foreign)
			(void) mlxcx_buf_create_foreign(&mlx, s, &b);
		else
			(void) mlxcx_buf_create(&mlx, s, &b);
		mlxcx_buf_return(&mlx, b);
	}
	return (s);
}

static mlxcx_completion_queue_t *
new_cq(void)
{
	mlxcx_completion_queue_t *cq = calloc(1, sizeof (*cq));

	mutex_init(&cq->mlcq_mtx, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&cq->mlcq_bufbmtx, NULL, MUTEX_DRIVER, NULL);
	list_create(&cq->mlcq_buffers, sizeof (mlxcx_buffer_t),
	    offsetof(mlxcx_buffer_t, mlb_cq_entry));
	list_create(&cq->mlcq_buffers_b, sizeof (mlxcx_buffer_t),
	    offsetof(mlxcx_buffer_t, mlb_cq_entry));
	return (cq);
}

static mlxcx_work_queue_t *
new_wq(uint_t type, mlxcx_completion_queue_t *cq)
{
	mlxcx_work_queue_t *wq = calloc(1, sizeof (*wq));
	ddi_device_acc_attr_t acc;
	ddi_dma_attr_t attr;

	mutex_init(&wq->mlwq_mtx, NULL, MUTEX_DRIVER, NULL);
	list_insert_tail(&mlx.mlx_wqs, wq);
	wq->mlwq_type = type;
	wq->mlwq_cq = cq;
	cq->mlcq_wq = wq;
	(void) mlxcx_dma_alloc(&mlx, &wq->mlwq_dma, &attr, &acc, B_TRUE, 4096,
	    B_TRUE);
	(void) mlxcx_dma_alloc(&mlx, &wq->mlwq_doorbell_dma, &attr, &acc,
	    B_TRUE, 64, B_TRUE);
	wq->mlwq_state = MLXCX_WQ_ALLOC | MLXCX_WQ_CREATED | MLXCX_WQ_STARTED;
	return (wq);
}

/* The rest of mlxcx_cq_teardown() for buffers: give back what is left. */
static void
cq_teardown(mlxcx_completion_queue_t *cq)
{
	mlxcx_buffer_t *b;

	while ((b = list_remove_head(&cq->mlcq_buffers)) != NULL)
		mlxcx_buf_return_chain(&mlx, b, B_FALSE);
	while ((b = list_remove_head(&cq->mlcq_buffers_b)) != NULL)
		mlxcx_buf_return_chain(&mlx, b, B_FALSE);
}

/*
 * The buffer steps of mlxcx_teardown() in the revision under test, with
 * hardware still running until TEARDOWN_HCA succeeds.
 */
static void
detach(boolean_t hca_ok)
{
	mlxcx_buf_shard_t *s;

	for (s = list_head(&mlx.mlx_buf_shards); s != NULL;
	    s = list_next(&mlx.mlx_buf_shards, s))
		mlxcx_shard_draining(s);
#ifdef HAVE_BUF_QUARANTINE
	boolean_t bufs_done = B_FALSE;

	if (list_is_empty(&mlx.mlx_quarantine_bufs)) {
		mlxcx_teardown_bufs(&mlx);
		bufs_done = B_TRUE;
	}
	hw_access();
	if (hca_ok) {
		hw_stopped = B_TRUE;
		mlxcx_dma_quarantine_free(&mlx);
	}
	if (!bufs_done) {
		if (list_is_empty(&mlx.mlx_quarantine_bufs))
			mlxcx_teardown_bufs(&mlx);
		else
			mlxcx_orphan_bufs(&mlx);
	}
#else
	mlxcx_teardown_bufs(&mlx);
	hw_access();
	hw_stopped = hca_ok;
#endif
	hw_access();
}

static mlxcx_buffer_t *
post_rx(mlxcx_work_queue_t *wq)
{
	mlxcx_buffer_t *b = mlxcx_buf_take(&mlx, wq);

	list_insert_tail(&wq->mlwq_cq->mlcq_buffers, b);
	return (b);
}

static void
rx_posted(mlxcx_work_queue_t *wq, uint_t n)
{
	for (uint_t i = 0; i < n; i++) {
		mlxcx_buffer_t *b = post_rx(wq);

		hw_pa[hw_npa++] = mlxcx_dma_cookie_one(&b->mlb_dma)->
		    dmac_laddress;
	}
}

static void
rx_stuck(void)
{
	mlxcx_completion_queue_t *cq;
	mlxcx_work_queue_t *rq;

	setup();
	cq = new_cq();
	rq = new_wq(MLXCX_WQ_TYPE_RECVQ, cq);
	rq->mlwq_bufs = shard(8, B_FALSE);
	rx_posted(rq, 3);

	mlxcx_wq_teardown(&mlx, rq);
	cq_teardown(cq);
	detach(B_TRUE);

	if (stub_dma_live != 0)
		stub_fail("%" PRId64 " DMA buffers left after TEARDOWN_HCA",
		    stub_dma_live);
	if (mlx.mlx_bufs_cache->kc_live != 0)
		stub_fail("%" PRId64 " packet buffers left",
		    mlx.mlx_bufs_cache->kc_live);
}

static void
rx_stuck_leak(void)
{
	mlxcx_completion_queue_t *cq;
	mlxcx_work_queue_t *rq;
	mlxcx_buffer_t *loaned;

	setup();
	cq = new_cq();
	rq = new_wq(MLXCX_WQ_TYPE_RECVQ, cq);
	rq->mlwq_bufs = shard(8, B_FALSE);
	loaned = mlxcx_buf_take(&mlx, rq);
	if (!mlxcx_buf_loan(&mlx, loaned))
		stub_fail("loan failed");
	rx_posted(rq, 3);

	mlxcx_wq_teardown(&mlx, rq);
	cq_teardown(cq);
	detach(B_FALSE);

	/* Detach has freed mlxp; the stack now frees the loaned mblk. */
	mlx.mlx_bufs_cache = NULL;
	freeb(loaned->mlb_mp);
	hw_access();
}

static void
tx_stuck(void)
{
	mlxcx_completion_queue_t *cq, *rcq;
	mlxcx_work_queue_t *sq, *rq;
	mlxcx_buffer_t *loaned, *head, *member;
	mblk_t *m1;

	setup();
	/* A healthy RQ lends a received packet, which is sent back out. */
	rcq = new_cq();
	rq = new_wq(MLXCX_WQ_TYPE_RECVQ, rcq);
	rq->mlwq_bufs = shard(8, B_FALSE);
	loaned = mlxcx_buf_take(&mlx, rq);
	if (!mlxcx_buf_loan(&mlx, loaned))
		stub_fail("loan failed");
	rq->mlwq_state &= ~MLXCX_WQ_STARTED;
	rq->mlwq_state |= MLXCX_WQ_DESTROYED;

	cq = new_cq();
	sq = new_wq(MLXCX_WQ_TYPE_SENDQ, cq);
	sq->mlwq_bufs = shard(4, B_FALSE);
	sq->mlwq_foreign_bufs = shard(4, B_TRUE);

	/* The head copies the headers; a foreign buffer binds the payload. */
	m1 = desballoc(calloc(1, 64), 64, 0, NULL);
	m1->b_cont = loaned->mlb_mp;
	head = mlxcx_buf_take(&mlx, sq);
	head->mlb_tx_head = head;
	head->mlb_tx_mp = m1;
	member = mlxcx_buf_take_foreign(&mlx, sq);
	member->mlb_dma.mxdb_flags |= MLXCX_DMABUF_BOUND;
	member->mlb_state = MLXCX_BUFFER_ON_CHAIN;
	member->mlb_tx_head = head;
	member->mlb_tx_mp = loaned->mlb_mp;
	list_insert_tail(&head->mlb_tx_chain, member);
	list_insert_tail(&cq->mlcq_buffers_b, head);
	hw_mp[hw_nmp++] = m1;
	hw_mp[hw_nmp++] = loaned->mlb_mp;
	hw_pa[hw_npa++] = mlxcx_dma_cookie_one(&loaned->mlb_dma)->dmac_laddress;

	mlxcx_wq_teardown(&mlx, rq);
	cq_teardown(rcq);
	mlxcx_wq_teardown(&mlx, sq);
	cq_teardown(cq);
	detach(B_TRUE);

	if (stub_dma_live != 0)
		stub_fail("%" PRId64 " DMA buffers left after TEARDOWN_HCA",
		    stub_dma_live);
	if (mlx.mlx_bufs_cache->kc_live != 0)
		stub_fail("%" PRId64 " packet buffers left",
		    mlx.mlx_bufs_cache->kc_live);
}

/* Buffers leaked by detach still call into the module; it must stay. */
static void
orphan_unload(void)
{
	mlxcx_completion_queue_t *cq;
	mlxcx_work_queue_t *rq;

	setup();
	cq = new_cq();
	rq = new_wq(MLXCX_WQ_TYPE_RECVQ, cq);
	rq->mlwq_bufs = shard(8, B_FALSE);
	rx_posted(rq, 3);

	mlxcx_wq_teardown(&mlx, rq);
	cq_teardown(cq);
	detach(B_FALSE);

	if (_fini() == 0 || mod_removed)
		stub_fail("module unloads while leaked buffers point into it");
}

/* The same instance attaches again after a detach that leaked its cache. */
static void
orphan_reattach(void)
{
	orphan_unload();
	hw_stopped = B_TRUE;
	setup();
	mlxcx_teardown_bufs(&mlx);
}

/*
 * With a long instance number the orphan count no longer fits the cache
 * name; a truncated name could match a leaked cache.
 */
static void
long_name(void)
{
	stub_instance = 2147483647;
	mlxcx_orphans = 100;
	if (!mlxcx_setup_bufs(&mlx))
		stub_fail("cache with a name that fits refused");
	mlxcx_orphans = 1000;
	(void) mlxcx_setup_bufs(&mlx);
}

static const char *const names[] = {
	"rx-stuck", "rx-stuck-leak", "tx-stuck", "orphan-unload",
	"orphan-reattach", "long-name", NULL
};
static void (*const funcs[])(void) = {
	rx_stuck, rx_stuck_leak, tx_stuck, orphan_unload, orphan_reattach,
	long_name
};

int
main(int argc, char **argv)
{
	if (argc != 2)
		stub_fail("usage: %s scenario", argv[0]);
	stub_verbose = getenv("MLXCX_TEST_VERBOSE") != NULL;
	for (uint_t i = 0; names[i] != NULL; i++) {
		if (strcmp(argv[1], names[i]) == 0) {
			funcs[i]();
			(void) printf("ok %s\n", names[i]);
			return (0);
		}
	}
	stub_fail("unknown scenario %s", argv[1]);
	return (1);
}

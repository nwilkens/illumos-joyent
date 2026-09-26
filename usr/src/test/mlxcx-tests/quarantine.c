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
 * When firmware rejects or never answers DESTROY_RQ, DESTROY_SQ, DESTROY_CQ
 * or DESTROY_EQ, the queue may still be live in hardware. Detach must not
 * panic, and must not free queue memory that hardware can still write,
 * until TEARDOWN_HCA succeeds.
 */

#define	DEBUG	1

#include "mlxcx_stub.h"
#include <mlxcx_reg.h>
#include "mlxcx_types.h"
#include "mlxcx_queue_types.h"
#include "mlxcx_min.h"

static boolean_t
stub_sleep_hook(clock_t deadline)
{
	(void) deadline;
	return (B_FALSE);
}

void mlxcx_dma_quarantine(mlxcx_t *, mlxcx_dma_buffer_t *);

#include "mlxcx_quarantine_body.h"

static mlxcx_t mlx;

static void
queue_dma(mlxcx_dma_buffer_t *dma)
{
	ddi_device_acc_attr_t acc;
	ddi_dma_attr_t attr;

	if (!mlxcx_dma_alloc(&mlx, dma, &attr, &acc, B_TRUE, 4096, B_TRUE))
		stub_fail("DMA allocation failed");
}

/* Hardware writes to every buffer it may still own. */
static uint64_t hw_pa[8];
static uint_t hw_n;

static void
hw_owns(mlxcx_dma_buffer_t *dma)
{
	hw_pa[hw_n++] = mlxcx_dma_cookie_one(dma)->dmac_laddress;
}

static void
hw_writes(void)
{
	for (uint_t i = 0; i < hw_n; i++)
		*(volatile uint8_t *)stub_dma_va(hw_pa[i], 1, B_TRUE) = 1;
}

static void
setup(void)
{
	mutex_init(&mlx.mlx_quarantine_mtx, NULL, MUTEX_DRIVER, NULL);
	list_create(&mlx.mlx_quarantine, sizeof (mlxcx_dma_quarantine_t),
	    offsetof(mlxcx_dma_quarantine_t, mdq_node));
}

static void
after_teardown_hca(void)
{
#ifdef HAVE_QUARANTINE
	mlxcx_dma_quarantine_free(&mlx);
	mlxcx_dma_quarantine_fini(&mlx);
#endif
	if (stub_dma_live != 0)
		stub_fail("%" PRId64 " buffers left after TEARDOWN_HCA",
		    stub_dma_live);
}

static void
failed_wq(void)
{
	mlxcx_work_queue_t wq = { 0 };

	setup();
	queue_dma(&wq.mlwq_dma);
	queue_dma(&wq.mlwq_doorbell_dma);
	hw_owns(&wq.mlwq_dma);
	hw_owns(&wq.mlwq_doorbell_dma);
	wq.mlwq_state = MLXCX_WQ_ALLOC | MLXCX_WQ_CREATED;
	mlxcx_wq_rele_dma(&mlx, &wq);
	hw_writes();
	after_teardown_hca();
}

static void
failed_cq(void)
{
	mlxcx_completion_queue_t cq = { 0 };

	setup();
	queue_dma(&cq.mlcq_dma);
	queue_dma(&cq.mlcq_doorbell_dma);
	hw_owns(&cq.mlcq_dma);
	hw_owns(&cq.mlcq_doorbell_dma);
	cq.mlcq_state = MLXCX_CQ_ALLOC | MLXCX_CQ_CREATED;
	mlxcx_cq_rele_dma(&mlx, &cq);
	hw_writes();
	after_teardown_hca();
}

static void
failed_eq(void)
{
	mlxcx_event_queue_t eq;

	memset(&eq, 0, sizeof (eq));
	setup();
	queue_dma(&eq.mleq_dma);
	hw_owns(&eq.mleq_dma);
	eq.mleq_state = MLXCX_EQ_ALLOC | MLXCX_EQ_CREATED;
	mlxcx_eq_rele_dma(&mlx, &eq);
	hw_writes();
	after_teardown_hca();
}

/* A queue that firmware did destroy is freed at once. */
static void
destroyed(void)
{
	mlxcx_completion_queue_t cq = { 0 };

	setup();
	queue_dma(&cq.mlcq_dma);
	queue_dma(&cq.mlcq_doorbell_dma);
	cq.mlcq_state = MLXCX_CQ_ALLOC | MLXCX_CQ_CREATED | MLXCX_CQ_DESTROYED;
	mlxcx_cq_rele_dma(&mlx, &cq);
	if (stub_dma_live != 0)
		stub_fail("destroyed queue memory not freed");
}

/* TEARDOWN_HCA failed too: the memory is leaked, never freed. */
static void
leak(void)
{
	mlxcx_work_queue_t wq = { 0 };

	setup();
	queue_dma(&wq.mlwq_dma);
	queue_dma(&wq.mlwq_doorbell_dma);
	hw_owns(&wq.mlwq_dma);
	wq.mlwq_state = MLXCX_WQ_ALLOC | MLXCX_WQ_CREATED;
	mlxcx_wq_rele_dma(&mlx, &wq);
#ifdef HAVE_QUARANTINE
	mlxcx_dma_quarantine_fini(&mlx);
#endif
	hw_writes();
}

static const char *const names[] = {
	"failed-wq", "failed-cq", "failed-eq", "destroyed", "leak", NULL
};
static void (*const funcs[])(void) = {
	failed_wq, failed_cq, failed_eq, destroyed, leak
};

int
main(int argc, char **argv)
{
	if (argc != 2)
		stub_fail("usage: %s scenario", argv[0]);
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

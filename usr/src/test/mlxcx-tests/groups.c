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
 * TX group setup can fail part way, when firmware rejects or never answers
 * CREATE_CQ or CREATE_SQ. Attach then tears the group down, and that must
 * work on every ring state setup can leave behind.
 */

#define	DEBUG	1

#include "mlxcx_stub.h"
#include <mlxcx_reg.h>
#include "mlxcx_types.h"
#include "mlxcx_group_types.h"
#include "mlxcx_min.h"

typedef struct mlxcx_ring_group {
	kmutex_t		mlg_mtx;
	mlxcx_t			*mlg_mlx;
	uint_t			mlg_state;
	uint_t			mlg_type;
	mlxcx_tis_t		mlg_tis;
	mlxcx_port_t		*mlg_port;
	size_t			mlg_nwqs;
	size_t			mlg_wqs_size;
	mlxcx_work_queue_t	*mlg_wqs;
} mlxcx_ring_group_t;

static boolean_t
stub_sleep_hook(clock_t deadline)
{
	(void) deadline;
	return (B_FALSE);
}

/* Firmware behaviour, per ring. */
static int fail_cq_at = -1;
static int fail_sq_at = -1;
static boolean_t sq_timeout;
static boolean_t fail_tis;
static int ncq;

static boolean_t
mlxcx_cmd_create_tis(mlxcx_t *mlxp, mlxcx_tis_t *tis)
{
	(void) mlxp;
	if (fail_tis)
		return (B_FALSE);
	tis->mltis_state |= MLXCX_TIS_CREATED;
	return (B_TRUE);
}

static boolean_t
mlxcx_cmd_destroy_tis(mlxcx_t *mlxp, mlxcx_tis_t *tis)
{
	(void) mlxp;
	tis->mltis_state |= MLXCX_TIS_DESTROYED;
	return (B_TRUE);
}

static mlxcx_completion_queue_t *live_cqs[16];

static boolean_t
mlxcx_cq_setup(mlxcx_t *mlxp, mlxcx_event_queue_t *eq,
    mlxcx_completion_queue_t **cqp, uint_t shift)
{
	mlxcx_completion_queue_t *cq = calloc(1, sizeof (*cq));

	(void) mlxp; (void) eq; (void) shift;
	mutex_init(&cq->mlcq_mtx, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&cq->mlcq_bufbmtx, NULL, MUTEX_DRIVER, NULL);
	live_cqs[ncq] = cq;
	if (ncq++ == fail_cq_at)
		return (B_FALSE);
	*cqp = cq;
	return (B_TRUE);
}

static void
mlxcx_cq_teardown(mlxcx_t *mlxp, mlxcx_completion_queue_t *cq)
{
	(void) mlxp;
	if (cq->mlcq_wq != NULL)
		stub_fail("CQ torn down while a work queue still uses it");
	mutex_destroy(&cq->mlcq_mtx);
	for (int i = 0; i < ncq; i++) {
		if (live_cqs[i] == cq)
			live_cqs[i] = NULL;
	}
	free(cq);
}

static void *
mlxcx_mlbs_create(mlxcx_t *mlxp)
{
	(void) mlxp;
	return (NULL);
}

static boolean_t
mlxcx_wq_alloc_dma(mlxcx_t *mlxp, mlxcx_work_queue_t *wq)
{
	ddi_device_acc_attr_t acc;
	ddi_dma_attr_t attr;

	(void) mlxcx_dma_alloc(mlxp, &wq->mlwq_dma, &attr, &acc, B_TRUE, 4096,
	    B_TRUE);
	(void) mlxcx_dma_alloc(mlxp, &wq->mlwq_doorbell_dma, &attr, &acc,
	    B_TRUE, 64, B_TRUE);
	wq->mlwq_nents = 1024;
	wq->mlwq_state |= MLXCX_WQ_ALLOC;
	return (B_TRUE);
}

static int nsq;

static boolean_t
mlxcx_cmd_create_sq(mlxcx_t *mlxp, mlxcx_work_queue_t *wq)
{
	(void) mlxp;
	if (nsq++ == fail_sq_at) {
		if (sq_timeout)
			wq->mlwq_state |= MLXCX_WQ_CREATE_UNSURE;
		return (B_FALSE);
	}
	wq->mlwq_state |= MLXCX_WQ_CREATED;
	return (B_TRUE);
}

static boolean_t
wq_cmd_ok(mlxcx_t *mlxp, mlxcx_work_queue_t *wq)
{
	(void) mlxp; (void) wq;
	return (B_TRUE);
}

static boolean_t
wq_destroy(mlxcx_t *mlxp, mlxcx_work_queue_t *wq)
{
	(void) mlxp;
	if (!(wq->mlwq_state & MLXCX_WQ_CREATED))
		stub_fail("destroy of a work queue that was never created");
	wq->mlwq_state |= MLXCX_WQ_DESTROYED;
	return (B_TRUE);
}

#define	mlxcx_cmd_stop_rq	wq_cmd_ok
#define	mlxcx_cmd_stop_sq	wq_cmd_ok
#define	mlxcx_cmd_destroy_rq	wq_destroy
#define	mlxcx_cmd_destroy_sq	wq_destroy

/* Later driver revisions quarantine buffers; nothing is posted here. */
static void
mlxcx_cq_quarantine_bufs(mlxcx_t *mlxp, mlxcx_completion_queue_t *cq)
{
	(void) mlxp; (void) cq;
}

void mlxcx_dma_quarantine(mlxcx_t *, mlxcx_dma_buffer_t *);
void mlxcx_wq_rele_dma(mlxcx_t *, mlxcx_work_queue_t *);
void mlxcx_wq_teardown(mlxcx_t *, mlxcx_work_queue_t *);

#include "mlxcx_groups_body.h"

static mlxcx_t mlx;
static mlxcx_port_t port;
static mlxcx_event_queue_t eqs[2];
static mlxcx_ring_group_t group;

static void
setup(uint_t nrings)
{
	mlx.mlx_eqs = eqs;
	mlx.mlx_intr_count = 2;
	mlx.mlx_intr_cq0 = 1;
	mlx.mlx_next_eq = 1;
	mlx.mlx_ports = &port;
	mlx.mlx_props.mldp_tx_nrings_per_group = nrings;
	mutex_init(&mlx.mlx_quarantine_mtx, NULL, MUTEX_DRIVER, NULL);
	list_create(&mlx.mlx_quarantine, sizeof (mlxcx_dma_quarantine_t),
	    offsetof(mlxcx_dma_quarantine_t, mdq_node));
	list_create(&mlx.mlx_wqs, sizeof (mlxcx_work_queue_t),
	    offsetof(mlxcx_work_queue_t, mlwq_entry));
}

/* Attach fails, then tears the group down as mlxcx_teardown() does. */
static void
fail_and_teardown(void)
{
	if (mlxcx_tx_group_setup(&mlx, &group))
		stub_fail("group setup succeeded");
	if (group.mlg_state & MLXCX_GROUP_INIT)
		mlxcx_teardown_tx_group(&mlx, &group);
	if (!list_is_empty(&mlx.mlx_wqs))
		stub_fail("work queues left on mlx_wqs after teardown");
}

static void
sq_fails(void)
{
	setup(1);
	fail_sq_at = 0;
	fail_and_teardown();
}

static void
sq_times_out(void)
{
	setup(1);
	fail_sq_at = 0;
	sq_timeout = B_TRUE;
	fail_and_teardown();
	if (stub_dma_live == 0)
		stub_fail("memory of a queue whose CREATE timed out was freed");
}

static void
cq_fails_mid(void)
{
	setup(3);
	fail_cq_at = 1;
	fail_and_teardown();
}

static void
sq_fails_mid(void)
{
	setup(3);
	fail_sq_at = 1;
	fail_and_teardown();
	for (int i = 0; i < ncq; i++) {
		if (live_cqs[i] != NULL && i < 2)
			stub_fail("CQ %d of a set up ring was not torn down",
			    i);
	}
}

static void
tis_fails(void)
{
	setup(3);
	fail_tis = B_TRUE;
	fail_and_teardown();
}

static const char *const names[] = {
	"sq-fails", "sq-times-out", "cq-fails-mid", "sq-fails-mid",
	"tis-fails", NULL
};
static void (*const funcs[])(void) = {
	sq_fails, sq_times_out, cq_fails_mid, sq_fails_mid, tis_fails
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

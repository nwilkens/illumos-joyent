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
 * Runs the real page request, page give, page take and page teardown code
 * against a model of the MANAGE_PAGES and QUERY_PAGES commands. The model
 * tracks which pages the device holds, and fails the test if the driver
 * frees one of them.
 */

#ifndef _PAGES_TEST_H
#define	_PAGES_TEST_H

#define	DEBUG	1

#include "mlxcx_stub.h"
#include <mlxcx_reg.h>
#include "mlxcx_types.h"
#include "mlxcx_eq_types.h"
#include "mlxcx_min.h"

clock_t mlxcx_reclaim_delay = 1000 * 50;
uint_t mlxcx_reclaim_tries = 100;

boolean_t mlxcx_give_pages(mlxcx_t *, int32_t, int32_t *);
int32_t mlxcx_pages_returned(mlxcx_t *, const uint64_t *, int32_t);
int mlxcx_page_compare(const void *, const void *);

/* The page command model. */
typedef enum {
	GIVE_ACCEPT,
	GIVE_REJECT,
	GIVE_TIMEOUT
} give_policy_t;

#define	DEV_MAX_PAGES	(1 << 16)

static give_policy_t give_policy;
static uint64_t dev_pages[DEV_MAX_PAGES];
static uint_t dev_npages;
static uint64_t give_calls, alloc_fail_calls, take_calls;
static int32_t query_reply;

/* Pages the device hands back, in order; a script may add unknown ones. */
static uint64_t take_script[64];
static uint_t take_script_n;
static boolean_t take_script_repeat;

static void
dev_own(uint64_t pa)
{
	if (dev_npages == DEV_MAX_PAGES)
		stub_fail("device page model full");
	dev_pages[dev_npages++] = pa;
}

static boolean_t
dev_release(uint64_t pa)
{
	for (uint_t i = 0; i < dev_npages; i++) {
		if (dev_pages[i] == pa) {
			dev_pages[i] = dev_pages[--dev_npages];
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

/* The device writes to every page it holds; a freed one is a UAF. */
static void
dev_touch(void)
{
	for (uint_t i = 0; i < dev_npages; i++) {
		if (dev_pages[i] >= STUB_DMA_BASE)
			*(volatile uint8_t *)stub_dma_va(dev_pages[i], 1,
			    B_TRUE) = 0x42;
	}
}

static boolean_t
stub_sleep_hook(clock_t deadline)
{
	(void) deadline;
	dev_touch();
	return (B_FALSE);
}

boolean_t
mlxcx_cmd_query_pages(mlxcx_t *mlxp, uint_t type, int32_t *npages)
{
	(void) mlxp; (void) type;
	*npages = query_reply;
	return (B_TRUE);
}

/*
 * The driver passes a timed out flag only once it can act on one, so the
 * stand-in takes it as an optional fifth argument.
 */
#define	mlxcx_cmd_give_pages(m, t, n, p, ...)	\
	stub_give_pages(m, t, n, p, (boolean_t *)(__VA_ARGS__ + 0))

static boolean_t
stub_give_pages(mlxcx_t *mlxp, uint_t type, int32_t npages,
    mlxcx_dev_page_t **pages, boolean_t *timedoutp)
{
	(void) mlxp;
	dev_touch();
	if (timedoutp != NULL)
		*timedoutp = (give_policy == GIVE_TIMEOUT);
	if (type == MLXCX_MANAGE_PAGES_OPMOD_ALLOC_FAIL) {
		alloc_fail_calls++;
		return (B_TRUE);
	}
	give_calls++;
	if (npages <= 0 || npages > MLXCX_MANAGE_PAGES_MAX_PAGES)
		stub_fail("give pages with npages %d", npages);
	if (give_policy == GIVE_REJECT)
		return (B_FALSE);
	for (int32_t i = 0; i < npages; i++)
		dev_own(pages[i]->mxdp_pa);
	return (give_policy == GIVE_ACCEPT);
}

boolean_t
mlxcx_cmd_return_pages(mlxcx_t *mlxp, int32_t nreq, uint64_t *pas,
    int32_t *nret)
{
	int32_t n = 0;

	(void) mlxp;
	dev_touch();
	take_calls++;
	if (nreq <= 0 || nreq > MLXCX_MANAGE_PAGES_MAX_PAGES)
		stub_fail("return pages with nreq %d", nreq);
	while (n < nreq && take_script_n > 0) {
		pas[n++] = take_script[0];
		if (!take_script_repeat) {
			memmove(&take_script[0], &take_script[1],
			    sizeof (take_script[0]) * --take_script_n);
		}
		if (take_script_repeat)
			break;
	}
	while (!take_script_repeat && n < nreq && dev_npages > 0)
		pas[n++] = dev_pages[dev_npages - 1], dev_npages--;
	for (int32_t i = 0; i < n; i++)
		(void) dev_release(pas[i]);
	*nret = n;
	return (B_TRUE);
}

/* Stand-ins for the parts of the async interrupt path we do not test. */
static mlxcx_eventq_ent_t eq_ents[16];
static uint_t eq_head, eq_tail;
static uint64_t link_tasks;

static boolean_t
mlxcx_intr_ini(mlxcx_t *mlxp, mlxcx_event_queue_t *mleq)
{
	(void) mlxp; (void) mleq;
	return (B_TRUE);
}

static void
mlxcx_intr_fini(mlxcx_event_queue_t *mleq)
{
	(void) mleq;
}

static mlxcx_eventq_ent_t *
mlxcx_eq_next(mlxcx_event_queue_t *mleq)
{
	(void) mleq;
	if (eq_head == eq_tail)
		return (NULL);
	return (&eq_ents[eq_head++]);
}

static void
mlxcx_arm_eq(mlxcx_t *mlxp, mlxcx_event_queue_t *mleq)
{
	(void) mlxp;
	mleq->mleq_state |= MLXCX_EQ_ARMED;
}

void
mlxcx_cmd_completion(mlxcx_t *mlxp, mlxcx_eventq_ent_t *ent)
{
	(void) mlxp; (void) ent;
}

static void
mlxcx_link_state_task(void *arg)
{
	(void) arg;
	link_tasks++;
}

static void
mlxcx_report_module_error(mlxcx_t *mlxp, mlxcx_evdata_port_mod_t *evd)
{
	(void) mlxp; (void) evd;
}

static void mlxcx_pages_task(void *);

#include "mlxcx_pages_body.h"

/* Test setup. */
static mlxcx_t mlx;
static mlxcx_event_queue_t mleq;

static void
pages_attach(void)
{
	mlxcx_t *mlxp = &mlx;

	mutex_init(&mlxp->mlx_pagemtx, NULL, MUTEX_DRIVER, NULL);
	avl_create(&mlxp->mlx_pages, mlxcx_page_compare,
	    sizeof (mlxcx_dev_page_t), offsetof(mlxcx_dev_page_t, mxdp_tree));
	mlxp->mlx_npages_max = 1024 * 1024;
	mlxp->mlx_async_tq = taskq_create("async", 1, 0, 1, 1, 0);
	mlxp->mlx_pages_tq = taskq_create("pages", 1, 0, 1, 1, 0);
	for (uint_t i = 0; i <= MLXCX_FUNC_ID_MAX; i++) {
		mlxp->mlx_npages_req[i].mla_mlx = mlxp;
		mutex_init(&mlxp->mlx_npages_req[i].mla_mtx, NULL,
		    MUTEX_DRIVER, NULL);
	}
	mutex_init(&mleq.mleq_mtx, NULL, MUTEX_DRIVER, NULL);
	mleq.mleq_state = MLXCX_EQ_ARMED;
}

/* Deliver one page request event through the real interrupt handler. */
static void
page_request(uint16_t func, uint32_t npages)
{
	mlxcx_eventq_ent_t *ent = &eq_ents[eq_tail++];

	memset(ent, 0, sizeof (*ent));
	ent->mleqe_event_type = MLXCX_EVENT_PAGE_REQUEST;
	ent->mleqe_page_request.mled_page_request_function_id = to_be16(func);
	ent->mleqe_page_request.mled_page_request_num_pages = to_be32(npages);
	(void) mlxcx_intr_async((caddr_t)&mlx, (caddr_t)&mleq);
	stub_taskq_run(mlx.mlx_pages_tq);
	stub_taskq_run(mlx.mlx_async_tq);
}

static uint64_t
known_pa(uint_t n)
{
	avl_node_t *node = mlx.mlx_pages.avl_first;

	while (n-- > 0 && node != NULL)
		node = node->avl_next;
	if (node == NULL)
		stub_fail("no known page %u", n);
	return (((mlxcx_dev_page_t *)((char *)node -
	    mlx.mlx_pages.avl_offset))->mxdp_pa);
}

static void
check_device_pages_live(void)
{
	dev_touch();
}

static int
test_main(int argc, char **argv, const char *const *names,
    void (*const *funcs)(void))
{
	if (argc != 2)
		stub_fail("usage: %s scenario", argv[0]);
	stub_verbose = getenv("MLXCX_TEST_VERBOSE") != NULL;
	for (uint_t i = 0; names[i] != NULL; i++) {
		if (strcmp(argv[1], names[i]) == 0) {
			pages_attach();
			funcs[i]();
			check_device_pages_live();
			(void) printf("ok %s\n", names[i]);
			return (0);
		}
	}
	stub_fail("unknown scenario %s", argv[1]);
	return (1);
}

#endif /* _PAGES_TEST_H */

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
 * Completion processing for CQs from rdk_alloc_cq().  Each work request
 * carries a struct rdk_cqe, and processing a completion calls its done()
 * function.
 *
 * RDK_POLL_TASKQ: the provider's completion event queues the CQ's task on
 * rdk_cq_taskq.  The task handles up to RDK_CQ_BUDGET completions, then
 * queues itself again if more may be waiting; otherwise it arms the CQ,
 * asking about completions that arrived before the arm, and stops.  An
 * event that arrives while the task runs makes it go around once more, so
 * no event is lost between the last poll and the arm.  A CQ's task never
 * runs twice at once.
 *
 * RDK_POLL_DIRECT: the consumer calls rdk_process_cq_direct() and the CQ
 * is never armed.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/disp.h>
#include <sys/cpuvar.h>
#include <sys/taskq_impl.h>

#include "rdk_impl.h"

/* Completions polled at once, and handled per task run. */
#define	RDK_CQ_BATCH	16
#define	RDK_CQ_BUDGET	256

taskq_t *rdk_cq_taskq;

struct rdk_cq_poller {
	kmutex_t	rcp_lock;
	kcondvar_t	rcp_cv;
	taskq_ent_t	rcp_ent;
	boolean_t	rcp_queued;	/* the task is queued or running */
	boolean_t	rcp_rerun;	/* an event came while it ran */
	boolean_t	rcp_dying;
	uint64_t	rcp_no_cqe;	/* completions without an rdk_cqe */
	struct rdk_wc	rcp_wc[RDK_CQ_BATCH];
};

int
rdk_cq_init(void)
{
	int n = MAX(MIN(ncpus, 32), 2);

	rdk_cq_taskq = taskq_create("rdk_cq", n, minclsyspri, n, INT_MAX,
	    TASKQ_PREPOPULATE);
	return (rdk_cq_taskq == NULL ? ENOMEM : 0);
}

void
rdk_cq_fini(void)
{
	taskq_destroy(rdk_cq_taskq);
	rdk_cq_taskq = NULL;
}

/*
 * Poll and dispatch up to budget completions, a negative budget meaning
 * until the CQ is empty.  The caller serializes calls for one CQ.
 */
static int
rdk_cq_process(struct rdk_cq *cq, struct rdk_wc *wcs, int budget)
{
	struct rdk_cq_poller *cp = cq->poller;
	int done = 0, want, n, i;

	for (;;) {
		want = RDK_CQ_BATCH;
		if (budget >= 0)
			want = MIN(want, budget - done);
		if (want <= 0)
			break;
		n = rdk_poll_cq(cq, want, wcs);
		for (i = 0; i < n; i++) {
			if (wcs[i].wr_cqe != NULL)
				wcs[i].wr_cqe->done(cq, &wcs[i]);
			else
				cp->rcp_no_cqe++;
		}
		if (n > 0)
			done += n;
		if (n < want)
			break;
	}
	return (done);
}

static void
rdk_cq_task(void *arg)
{
	struct rdk_cq *cq = arg;
	struct rdk_cq_poller *cp = cq->poller;
	int n;

	for (;;) {
		mutex_enter(&cp->rcp_lock);
		cp->rcp_rerun = B_FALSE;
		if (cp->rcp_dying) {
			cp->rcp_queued = B_FALSE;
			cv_broadcast(&cp->rcp_cv);
			mutex_exit(&cp->rcp_lock);
			return;
		}
		mutex_exit(&cp->rcp_lock);

		n = rdk_cq_process(cq, cp->rcp_wc, RDK_CQ_BUDGET);
		if (n >= RDK_CQ_BUDGET) {
			/* Let other CQs run; the task owns rcp_ent now. */
			taskq_dispatch_ent(rdk_cq_taskq, rdk_cq_task, cq, 0,
			    &cp->rcp_ent);
			return;
		}
		if (rdk_req_notify_cq(cq, RDK_CQ_NEXT_COMP |
		    RDK_CQ_REPORT_MISSED_EVENTS) > 0)
			continue;

		mutex_enter(&cp->rcp_lock);
		if (cp->rcp_rerun && !cp->rcp_dying) {
			mutex_exit(&cp->rcp_lock);
			continue;
		}
		cp->rcp_queued = B_FALSE;
		cv_broadcast(&cp->rcp_cv);
		mutex_exit(&cp->rcp_lock);
		return;
	}
}

/* The provider's completion event, in its thread context. */
static void
rdk_cq_event(struct rdk_cq *cq, void *ctx)
{
	struct rdk_cq_poller *cp = cq->poller;

	_NOTE(ARGUNUSED(ctx));
	mutex_enter(&cp->rcp_lock);
	if (cp->rcp_dying) {
		mutex_exit(&cp->rcp_lock);
		return;
	}
	if (cp->rcp_queued) {
		cp->rcp_rerun = B_TRUE;
	} else {
		cp->rcp_queued = B_TRUE;
		taskq_dispatch_ent(rdk_cq_taskq, rdk_cq_task, cq, 0,
		    &cp->rcp_ent);
	}
	mutex_exit(&cp->rcp_lock);
}

/*
 * Allocate a CQ of at least nr_cqe entries whose completions go to the
 * done() functions of the work requests.  private is the CQ's cq_context.
 */
int
rdk_alloc_cq(struct rdk_device *dev, void *private, int nr_cqe,
    int comp_vector, enum rdk_poll_context ctx, struct rdk_cq **cqp)
{
	struct rdk_cq_init_attr attr;
	struct rdk_cq_poller *cp;
	struct rdk_cq *cq;
	int ret;

	*cqp = NULL;
	if (nr_cqe <= 0 || comp_vector < 0 ||
	    (ctx != RDK_POLL_TASKQ && ctx != RDK_POLL_DIRECT))
		return (EINVAL);

	cp = kmem_zalloc(sizeof (*cp), KM_SLEEP);
	mutex_init(&cp->rcp_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&cp->rcp_cv, NULL, CV_DRIVER, NULL);

	bzero(&attr, sizeof (attr));
	attr.cqe = (uint32_t)nr_cqe;
	attr.comp_vector = (uint32_t)comp_vector;
	ret = rdk_create_cq(dev, ctx == RDK_POLL_TASKQ ? rdk_cq_event : NULL,
	    NULL, private, &attr, &cq);
	if (ret != 0) {
		cv_destroy(&cp->rcp_cv);
		mutex_destroy(&cp->rcp_lock);
		kmem_free(cp, sizeof (*cp));
		return (ret);
	}
	cq->poll_ctx = ctx;
	cq->poller = cp;
	if (ctx == RDK_POLL_TASKQ)
		(void) rdk_req_notify_cq(cq, RDK_CQ_NEXT_COMP);
	*cqp = cq;
	return (0);
}

void
rdk_free_cq(struct rdk_cq *cq)
{
	struct rdk_cq_poller *cp = cq->poller;

	if (cq->usecnt != 0) {
		/* rdk_destroy_cq() leaks it too; keep the poller with it. */
		rdk_destroy_cq(cq);
		return;
	}
	mutex_enter(&cp->rcp_lock);
	cp->rcp_dying = B_TRUE;
	while (cp->rcp_queued)
		cv_wait(&cp->rcp_cv, &cp->rcp_lock);
	mutex_exit(&cp->rcp_lock);

	rdk_destroy_cq(cq);
	cv_destroy(&cp->rcp_cv);
	mutex_destroy(&cp->rcp_lock);
	kmem_free(cp, sizeof (*cp));
}

/*
 * Process completions of a RDK_POLL_DIRECT CQ in the caller's context; a
 * negative budget processes until the CQ is empty.  Returns the number
 * processed.
 */
int
rdk_process_cq_direct(struct rdk_cq *cq, int budget)
{
	struct rdk_wc wcs[RDK_CQ_BATCH];

	VERIFY3U(cq->poll_ctx, ==, RDK_POLL_DIRECT);
	return (rdk_cq_process(cq, wcs, budget));
}

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
 * RDK_POLL_TASKQ: the provider's completion event runs the CQ's poller in
 * the provider's thread for the CQ's vector.  The poller handles up to
 * RDK_CQ_BUDGET completions, then arms the CQ, asking about completions
 * that arrived before the arm.  A CQ with more waiting than one budget is
 * handed back to its vector with the provider's cq_resched(), or to
 * rdk_cq_taskq when the provider has none, so that one CQ cannot keep the
 * others on its vector waiting.  An event that arrives while the poller
 * runs makes it go around once more, so no event is lost between the last
 * poll and the arm.  A CQ's poller never runs twice at once.
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

/*
 * Completions polled at once, and handled per poller run; the passes a run
 * makes on arms that report missed completions.
 */
#define	RDK_CQ_BATCH	16
#define	RDK_CQ_BUDGET	256
#define	RDK_CQ_PASSES	8

taskq_t *rdk_cq_taskq;

/*
 * rcp_queued is set while the poller runs or is handed back; rcp_deferred
 * while it is handed back, and rcp_in_tq while that is to rdk_cq_taskq,
 * which then owns rcp_ent.
 */
struct rdk_cq_poller {
	kmutex_t	rcp_lock;
	kcondvar_t	rcp_cv;
	taskq_ent_t	rcp_ent;
	boolean_t	rcp_queued;
	boolean_t	rcp_deferred;
	boolean_t	rcp_in_tq;
	boolean_t	rcp_rerun;	/* an event came while it ran */
	boolean_t	rcp_dying;
	kthread_t	*rcp_runner;	/* the thread polling and dispatching */
	uint64_t	rcp_batches;
	uint64_t	rcp_no_cqe;	/* completions without an rdk_cqe */
	uint64_t	rcp_resched;
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
		mutex_enter(&cp->rcp_lock);
		cp->rcp_runner = curthread;
		mutex_exit(&cp->rcp_lock);
		n = rdk_poll_cq(cq, want, wcs);
		for (i = 0; i < n; i++) {
			if (wcs[i].wr_cqe != NULL)
				wcs[i].wr_cqe->done(cq, &wcs[i]);
			else
				cp->rcp_no_cqe++;
		}
		mutex_enter(&cp->rcp_lock);
		cp->rcp_runner = NULL;
		cp->rcp_batches++;
		cv_broadcast(&cp->rcp_cv);
		mutex_exit(&cp->rcp_lock);
		if (n > 0)
			done += n;
		if (n < want)
			break;
	}
	return (done);
}

static void rdk_cq_task(void *);

/* The poller has more than a budget waiting; let the vector move on. */
static void
rdk_cq_defer(struct rdk_cq *cq)
{
	struct rdk_cq_poller *cp = cq->poller;
	void (*resched)(struct rdk_cq *) = cq->device->rd_ops->cq_resched;

	mutex_enter(&cp->rcp_lock);
	cp->rcp_deferred = B_TRUE;
	cp->rcp_in_tq = resched == NULL;
	cp->rcp_resched++;
	cv_broadcast(&cp->rcp_cv);
	mutex_exit(&cp->rcp_lock);
	if (resched != NULL)
		resched(cq);
	else
		taskq_dispatch_ent(rdk_cq_taskq, rdk_cq_task, cq, 0,
		    &cp->rcp_ent);
}

/* The caller has set rcp_queued. */
static void
rdk_cq_run(struct rdk_cq *cq)
{
	struct rdk_cq_poller *cp = cq->poller;
	int total = 0, passes = 0;

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

		total += rdk_cq_process(cq, cp->rcp_wc, RDK_CQ_BUDGET - total);
		if (total >= RDK_CQ_BUDGET || ++passes > RDK_CQ_PASSES) {
			rdk_cq_defer(cq);
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

static void
rdk_cq_task(void *arg)
{
	struct rdk_cq *cq = arg;
	struct rdk_cq_poller *cp = cq->poller;

	mutex_enter(&cp->rcp_lock);
	cp->rcp_deferred = cp->rcp_in_tq = B_FALSE;
	mutex_exit(&cp->rcp_lock);
	rdk_cq_run(cq);
}

/*
 * The provider's completion event, in the thread of the CQ's vector.  It
 * takes over a poller handed back with cq_resched(), but not one that
 * rdk_cq_taskq owns.
 */
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
	if (cp->rcp_queued && (!cp->rcp_deferred || cp->rcp_in_tq)) {
		cp->rcp_rerun = B_TRUE;
		mutex_exit(&cp->rcp_lock);
		return;
	}
	cp->rcp_queued = B_TRUE;
	cp->rcp_deferred = B_FALSE;
	mutex_exit(&cp->rcp_lock);
	rdk_cq_run(cq);
}

/*
 * Wait until no poller run is in progress or owed to rdk_cq_taskq.  A
 * poller handed back to the provider is not waited for: the provider
 * drops it when the CQ is destroyed, and a late call sees rcp_dying.
 */
static void
rdk_cq_wait_idle(struct rdk_cq_poller *cp)
{
	ASSERT(MUTEX_HELD(&cp->rcp_lock));
	while (cp->rcp_queued && (!cp->rcp_deferred || cp->rcp_in_tq))
		cv_wait(&cp->rcp_cv, &cp->rcp_lock);
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
	ret = rdk_create_cq_poll(dev, ctx == RDK_POLL_TASKQ ? rdk_cq_event :
	    NULL, NULL, private, &attr, ctx, cp, &cq);
	if (ret != 0) {
		/* The provider may have sent an event before it failed. */
		mutex_enter(&cp->rcp_lock);
		cp->rcp_dying = B_TRUE;
		rdk_cq_wait_idle(cp);
		mutex_exit(&cp->rcp_lock);
		cv_destroy(&cp->rcp_cv);
		mutex_destroy(&cp->rcp_lock);
		kmem_free(cp, sizeof (*cp));
		return (ret);
	}
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
	rdk_cq_wait_idle(cp);
	mutex_exit(&cp->rcp_lock);

	rdk_destroy_cq(cq);
	cv_destroy(&cp->rcp_cv);
	mutex_destroy(&cp->rcp_lock);
	kmem_free(cp, sizeof (*cp));
}

/*
 * Wait until completions polled before the call are dispatched.  After the
 * provider has destroyed a QP, no later poll returns its completions.
 */
void
rdk_cq_barrier(struct rdk_cq *cq)
{
	struct rdk_cq_poller *cp = cq->poller;
	uint64_t gen;

	if (cp == NULL)
		return;
	mutex_enter(&cp->rcp_lock);
	ASSERT3P(cp->rcp_runner, !=, curthread);
	gen = cp->rcp_batches;
	while (cp->rcp_runner != NULL && cp->rcp_runner != curthread &&
	    cp->rcp_batches == gen)
		cv_wait(&cp->rcp_cv, &cp->rcp_lock);
	mutex_exit(&cp->rcp_lock);
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

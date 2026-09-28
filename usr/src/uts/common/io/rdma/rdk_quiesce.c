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
 * Teardown away from callbacks.  The framework marks the thread while it
 * runs a consumer's completion or event callback, and the verbs that wait
 * for callbacks check the mark: see rdk.h.  A teardown runs its function
 * once on rdk_td_taskq, whose threads never run callbacks.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/disp.h>
#include <sys/cpuvar.h>
#include <sys/proc.h>
#include <sys/taskq_impl.h>

#include "rdk_impl.h"

#define	RDK_TD_IDLE	0
#define	RDK_TD_QUEUED	1
#define	RDK_TD_RUNNING	2
#define	RDK_TD_DONE	3

struct rdk_teardown {
	void		(*rtd_func)(void *);
	void		*rtd_arg;
	kmutex_t	rtd_lock;
	kcondvar_t	rtd_cv;
	uint_t		rtd_state;
	boolean_t	rtd_free;	/* free it once the function returns */
	kthread_t	*rtd_runner;
	taskq_ent_t	rtd_ent;
};

taskq_t *rdk_td_taskq;
static uint_t rdk_cb_key;

int
rdk_quiesce_init(void)
{
	int n = MAX(MIN(ncpus, 16), 4);

	tsd_create(&rdk_cb_key, NULL);
	rdk_td_taskq = taskq_create("rdk_teardown", n, minclsyspri, n,
	    INT_MAX, TASKQ_PREPOPULATE);
	if (rdk_td_taskq == NULL) {
		tsd_destroy(&rdk_cb_key);
		return (ENOMEM);
	}
	return (0);
}

void
rdk_quiesce_fini(void)
{
	taskq_destroy(rdk_td_taskq);
	rdk_td_taskq = NULL;
	tsd_destroy(&rdk_cb_key);
}

/* Mark the thread as running a callback; returns the mark to restore. */
void *
rdk_cb_enter(void *what)
{
	void *old = tsd_get(rdk_cb_key);

	(void) tsd_set(rdk_cb_key, what);
	return (old);
}

void
rdk_cb_exit(void *old)
{
	(void) tsd_set(rdk_cb_key, old);
}

boolean_t
rdk_in_callback(void)
{
	return (tsd_get(rdk_cb_key) != NULL);
}

/* A verb that waits for callbacks was called from one. */
void
rdk_cb_forbid(const char *verb)
{
	if (rdk_in_callback()) {
		panic("%s() called from an rdmak callback; use "
		    "rdk_teardown_start()", verb);
	}
}

void
rdk_event_upcall(void (*handler)(struct rdk_event *, void *),
    struct rdk_event *ev, void *arg)
{
	void *old;

	if (handler == NULL)
		return;
	old = rdk_cb_enter(ev);
	handler(ev, arg);
	rdk_cb_exit(old);
}

void
rdk_comp_upcall(struct rdk_cq *cq)
{
	void *old;

	if (cq->comp_handler == NULL)
		return;
	old = rdk_cb_enter(cq);
	cq->comp_handler(cq, cq->cq_context);
	rdk_cb_exit(old);
}

rdk_teardown_t *
rdk_teardown_alloc(void (*func)(void *), void *arg)
{
	rdk_teardown_t *td;

	VERIFY(func != NULL);
	td = kmem_zalloc(sizeof (*td), KM_SLEEP);
	td->rtd_func = func;
	td->rtd_arg = arg;
	mutex_init(&td->rtd_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&td->rtd_cv, NULL, CV_DRIVER, NULL);
	return (td);
}

static void
rdk_teardown_destroy(rdk_teardown_t *td)
{
	cv_destroy(&td->rtd_cv);
	mutex_destroy(&td->rtd_lock);
	kmem_free(td, sizeof (*td));
}

static void
rdk_teardown_task(void *arg)
{
	rdk_teardown_t *td = arg;
	boolean_t free;

	mutex_enter(&td->rtd_lock);
	td->rtd_state = RDK_TD_RUNNING;
	td->rtd_runner = curthread;
	mutex_exit(&td->rtd_lock);

	td->rtd_func(td->rtd_arg);

	mutex_enter(&td->rtd_lock);
	td->rtd_runner = NULL;
	td->rtd_state = RDK_TD_DONE;
	free = td->rtd_free;
	cv_broadcast(&td->rtd_cv);
	mutex_exit(&td->rtd_lock);
	if (free)
		rdk_teardown_destroy(td);
}

boolean_t
rdk_teardown_start(rdk_teardown_t *td)
{
	mutex_enter(&td->rtd_lock);
	if (td->rtd_state != RDK_TD_IDLE) {
		mutex_exit(&td->rtd_lock);
		return (B_FALSE);
	}
	td->rtd_state = RDK_TD_QUEUED;
	mutex_exit(&td->rtd_lock);
	taskq_dispatch_ent(rdk_td_taskq, rdk_teardown_task, td, 0,
	    &td->rtd_ent);
	return (B_TRUE);
}

boolean_t
rdk_teardown_dying(rdk_teardown_t *td)
{
	boolean_t dying;

	mutex_enter(&td->rtd_lock);
	dying = td->rtd_state != RDK_TD_IDLE;
	mutex_exit(&td->rtd_lock);
	return (dying);
}

void
rdk_teardown_wait(rdk_teardown_t *td)
{
	mutex_enter(&td->rtd_lock);
	while (td->rtd_state == RDK_TD_QUEUED ||
	    td->rtd_state == RDK_TD_RUNNING) {
		if (td->rtd_runner == curthread || rdk_in_callback()) {
			mutex_exit(&td->rtd_lock);
			rdk_cb_forbid("rdk_teardown_wait");
			panic("rdk_teardown_wait() from its own function");
		}
		cv_wait(&td->rtd_cv, &td->rtd_lock);
	}
	mutex_exit(&td->rtd_lock);
}

/*
 * From its own function or a callback, a teardown that has not finished is
 * freed by rdk_teardown_task() once the function returns.
 */
void
rdk_teardown_free(rdk_teardown_t *td)
{
	mutex_enter(&td->rtd_lock);
	if ((td->rtd_state == RDK_TD_QUEUED ||
	    td->rtd_state == RDK_TD_RUNNING) &&
	    (td->rtd_runner == curthread || rdk_in_callback())) {
		td->rtd_free = B_TRUE;
		mutex_exit(&td->rtd_lock);
		return;
	}
	while (td->rtd_state == RDK_TD_QUEUED ||
	    td->rtd_state == RDK_TD_RUNNING)
		cv_wait(&td->rtd_cv, &td->rtd_lock);
	mutex_exit(&td->rtd_lock);
	rdk_teardown_destroy(td);
}

/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * A controller is freed once, by a terminate, and never while a shutdown is
 * still queued for it.  The taskq here runs its entries in order on request,
 * as the one thread of ns_taskq does, and the controller is really freed, so
 * a task that runs after the free is caught.
 */
#include "teardown.h"

static nvmft_softc_t softc;
nvmft_softc_t *nvmft_global = &softc;

static taskq_ent_t *tq_head, **tq_tail = &tq_head;
static int frees, timers_armed, timers_cancelled, offline_completes;
static int admin_destroys, port_reles;
static void (*timer_func)(void *);
static void *timer_arg;
static struct nvmft_qpair *admin_qp = (struct nvmft_qpair *)0x10;

void
taskq_dispatch_ent(taskq_t *tq, void (*func)(void *), void *arg,
    unsigned flags, taskq_ent_t *ent)
{
	(void) tq;
	(void) flags;
	/* An entry that is queued twice corrupts the queue. */
	assert(!ent->queued);
	ent->queued = 1;
	ent->func = func;
	ent->arg = arg;
	ent->next = NULL;
	*tq_tail = ent;
	tq_tail = &ent->next;
}

static int
run_one(void)
{
	taskq_ent_t *ent = tq_head;
	void (*func)(void *);
	void *arg;

	if (ent == NULL)
		return (0);
	tq_head = ent->next;
	if (tq_head == NULL)
		tq_tail = &tq_head;
	ent->queued = 0;
	func = ent->func;
	arg = ent->arg;
	func(arg);
	return (1);
}

static void
run_all(void)
{
	while (run_one())
		;
}

timeout_id_t
timeout(void (*func)(void *), void *arg, long ticks)
{
	(void) ticks;
	/* One callout at a time: a second could never be cancelled. */
	assert(timer_func == NULL);
	timer_func = func;
	timer_arg = arg;
	timers_armed++;
	return ((timeout_id_t)&timer_func);
}

long
untimeout(timeout_id_t id)
{
	assert(id == (timeout_id_t)&timer_func);
	if (timer_func != NULL)
		timers_cancelled++;
	timer_func = NULL;
	return (0);
}

static void
fire_timer(void)
{
	void (*func)(void *) = timer_func;

	assert(func != NULL);
	timer_func = NULL;
	func(timer_arg);
}

void
nvmft_qpair_shutdown(struct nvmft_qpair *qp)
{
	(void) qp;
}

void
nvmft_qpair_destroy(struct nvmft_qpair *qp)
{
	if (qp == admin_qp)
		admin_destroys++;
}

void
nvmft_deferred_drain(nvmft_controller_t *ctrlr)
{
	(void) ctrlr;
}

void
nvmft_io_drain(nvmft_controller_t *ctrlr)
{
	(void) ctrlr;
}

void
nvmft_internal_drain(nvmft_controller_t *ctrlr)
{
	(void) ctrlr;
}

void
nvmft_terminate_commands(nvmft_controller_t *ctrlr)
{
	(void) ctrlr;
}

void
nvmft_admin_stop(nvmft_controller_t *ctrlr)
{
	assert(ctrlr->ctrlr_admin_stopped);
}

void
nvmft_session_deregister(nvmft_controller_t *ctrlr)
{
	(void) ctrlr;
}

void
nvmft_port_offline_complete(nvmft_port_t *np)
{
	/* STMF hears of it after the last session is gone. */
	assert(list_is_empty(&np->np_controllers));
	offline_completes++;
}

void
nvmft_port_rele(nvmft_port_t *np)
{
	(void) np;
	port_reles++;
}

void
id_free(void *ids, int id)
{
	(void) ids;
	(void) id;
}

int
nvmft_printf(nvmft_controller_t *ctrlr, const char *fmt, ...)
{
	(void) ctrlr;
	(void) fmt;
	return (0);
}

static void
nvmft_controller_free(nvmft_controller_t *ctrlr)
{
	assert(!ctrlr->ctrlr_shutdown_task.queued);
	assert(!ctrlr->ctrlr_terminate_task.queued);
	assert(timer_func == NULL);
	frees++;
	free(ctrlr);
}

static void
nvmft_keep_alive_timer(void *arg)
{
	(void) arg;
}

static nvmft_port_t port;

static nvmft_controller_t *
new_controller(boolean_t enabled)
{
	nvmft_controller_t *ctrlr = calloc(1, sizeof (*ctrlr));
	nvme_reg_cc_t cc = { .r = 0 };

	mutex_init(&ctrlr->ctrlr_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ctrlr->ctrlr_accept_cv, NULL, CV_DRIVER, NULL);
	ctrlr->ctrlr_np = &port;
	ctrlr->ctrlr_admin = admin_qp;
	cc.b.cc_en = enabled;
	ctrlr->ctrlr_cc = cc.r;
	list_insert_tail(&port.np_controllers, ctrlr);
	frees = timers_armed = timers_cancelled = offline_completes = 0;
	admin_destroys = port_reles = 0;
	return (ctrlr);
}

/* What update_cc() does for CC.EN = 0, then the shutdown it asks for. */
static void
host_disable(nvmft_controller_t *ctrlr)
{
	nvme_reg_cc_t cc;

	mutex_enter(&ctrlr->ctrlr_lock);
	assert(!ctrlr->ctrlr_shutdown);
	cc.r = ctrlr->ctrlr_cc;
	cc.b.cc_en = 0;
	ctrlr->ctrlr_cc = cc.r;
	ctrlr->ctrlr_shutdown = B_TRUE;
	ctrlr->ctrlr_shutdown_pending = B_TRUE;
	mutex_exit(&ctrlr->ctrlr_lock);
	nvmft_controller_start_shutdown(ctrlr);
}

static void
host_enable(nvmft_controller_t *ctrlr)
{
	nvme_reg_cc_t cc;

	mutex_enter(&ctrlr->ctrlr_lock);
	assert(!ctrlr->ctrlr_shutdown);
	cc.r = ctrlr->ctrlr_cc;
	cc.b.cc_en = 1;
	ctrlr->ctrlr_cc = cc.r;
	mutex_exit(&ctrlr->ctrlr_lock);
}

static void
gone(void)
{
	assert(frees == 1 && admin_destroys == 1 && port_reles == 1);
	assert(tq_head == NULL && timer_func == NULL);
	assert(list_is_empty(&port.np_controllers));
}

/*
 * The 2 minutes after a shutdown run out, and before the terminate runs an
 * error asks for another shutdown.  The terminate gives way to it.
 */
static void
stale_terminate(void)
{
	nvmft_controller_t *ctrlr = new_controller(B_TRUE);

	host_disable(ctrlr);
	run_all();
	assert(frees == 0 && timers_armed == 1);

	fire_timer();
	assert(ctrlr->ctrlr_terminate_task.queued);
	nvmft_controller_error(ctrlr, NULL, EIO);
	assert(ctrlr->ctrlr_shutdown_task.queued);

	assert(run_one());
	assert(frees == 0 && !ctrlr->ctrlr_terminate_queued);
	run_all();
	gone();
}

/* The same, with the host's own second shutdown after it enabled again. */
static void
stale_terminate_reenabled(void)
{
	nvmft_controller_t *ctrlr = new_controller(B_TRUE);

	host_disable(ctrlr);
	run_all();
	fire_timer();
	host_enable(ctrlr);
	host_disable(ctrlr);
	assert(run_one());
	assert(frees == 0);
	run_all();
	assert(frees == 0 && timers_armed == 2 && timer_func != NULL);

	/* The second 2 minutes run out with the controller disabled. */
	fire_timer();
	run_all();
	gone();
}

/* A controller that the host enabled again is not terminated. */
static void
reenabled(void)
{
	nvmft_controller_t *ctrlr = new_controller(B_TRUE);

	host_disable(ctrlr);
	run_all();
	fire_timer();
	host_enable(ctrlr);
	run_all();
	assert(frees == 0 && !ctrlr->ctrlr_terminate_queued);

	nvmft_controller_error(ctrlr, NULL, EIO);
	run_all();
	gone();
}

/* A port going offline does not wait out a shutdown's 2 minutes. */
static void
offline(void)
{
	nvmft_controller_t *ctrlr = new_controller(B_TRUE);

	/* During the shutdown. */
	port.np_offlining = B_TRUE;
	host_disable(ctrlr);
	nvmft_controller_offline(ctrlr);
	run_all();
	assert(timers_armed == 0 && offline_completes == 1);
	assert(!port.np_offlining);
	gone();

	/* After it, with the timer running. */
	ctrlr = new_controller(B_TRUE);
	port.np_offlining = B_TRUE;
	host_disable(ctrlr);
	run_all();
	assert(timers_armed == 1 && frees == 0);
	nvmft_controller_offline(ctrlr);
	assert(timers_cancelled == 1);
	run_all();
	assert(offline_completes == 1);
	gone();

	/* With the terminate already queued. */
	ctrlr = new_controller(B_TRUE);
	host_disable(ctrlr);
	run_all();
	fire_timer();
	nvmft_controller_offline(ctrlr);
	run_all();
	assert(offline_completes == 0);
	gone();

	/* A second controller keeps the port from being offline yet. */
	ctrlr = new_controller(B_TRUE);
	(void) new_controller(B_TRUE);
	port.np_offlining = B_TRUE;
	nvmft_controller_offline(ctrlr);
	run_all();
	assert(frees == 1 && offline_completes == 0 && port.np_offlining);
	nvmft_controller_offline(list_head(&port.np_controllers));
	run_all();
	assert(frees == 2 && offline_completes == 1);
	assert(list_is_empty(&port.np_controllers));
}

/*
 * The admin queue of an idle controller closes, which its receive and its
 * transmit thread can both report.
 */
static void
idle_close(void)
{
	nvmft_controller_t *ctrlr = new_controller(B_FALSE);

	nvmft_controller_error(ctrlr, admin_qp, 0);
	nvmft_controller_error(ctrlr, admin_qp, 0);
	nvmft_controller_error(ctrlr, admin_qp, EPIPE);
	run_all();
	gone();

	/* The host enabled the controller just before its queue closed. */
	ctrlr = new_controller(B_FALSE);
	nvmft_controller_error(ctrlr, admin_qp, 0);
	host_enable(ctrlr);
	run_all();
	gone();
}

/* The admin queue closes during a shutdown: no 2 minutes follow. */
static void
close_in_shutdown(void)
{
	nvmft_controller_t *ctrlr = new_controller(B_TRUE);

	host_disable(ctrlr);
	nvmft_controller_error(ctrlr, admin_qp, 0);
	/* Errors during a shutdown are not a second shutdown. */
	nvmft_controller_error(ctrlr, NULL, EPIPE);
	run_all();
	assert(timers_armed == 0);
	gone();
}

int
main(void)
{
	mutex_init(&port.np_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&port.np_controllers, sizeof (nvmft_controller_t),
	    offsetof(nvmft_controller_t, ctrlr_link));

	stale_terminate();
	stale_terminate_reenabled();
	reenabled();
	offline();
	idle_close();
	close_in_shutdown();
	(void) printf("controller teardown passed\n");
	return (0);
}

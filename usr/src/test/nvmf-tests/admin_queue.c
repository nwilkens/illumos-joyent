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
 * Drive nvmft's admin queue from several receive threads: each command runs
 * once, in arrival order, one at a time per controller.  Stopping the queue
 * waits for the running command, frees the queued ones, and nothing runs
 * after it returns.
 */
#include "admin.h"

#include <unistd.h>

static nvmft_softc_t softc;
nvmft_softc_t *nvmft_global = &softc;

static nvmft_controller_t ctrlr;
static int running, handled, freed, slow;
static int last_seen[4];
static volatile int after_stop;

void
taskq_dispatch_ent(taskq_t *tq, task_func_t func, void *arg, unsigned f,
    taskq_ent_t *ent)
{
	(void) tq;
	(void) f;
	assert(pthread_create(&ent->t, NULL, (void *(*)(void *))(void *)func,
	    arg) == 0);
	assert(pthread_detach(ent->t) == 0);
}

void
nvmft_handle_admin_command(nvmft_controller_t *c, struct nvmf_capsule *nc)
{
	int who = nc->nc_id >> 16, seq = nc->nc_id & 0xffff;

	assert(c == &ctrlr);
	assert(!after_stop);
	assert(__atomic_add_fetch(&running, 1, __ATOMIC_SEQ_CST) == 1);
	assert(seq == last_seen[who] + 1);
	last_seen[who] = seq;
	if (slow)
		usleep(20000);
	__atomic_sub_fetch(&running, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&handled, 1, __ATOMIC_SEQ_CST);
	free(nc);
}

void
nvmf_free_capsule(struct nvmf_capsule *nc)
{
	__atomic_add_fetch(&freed, 1, __ATOMIC_SEQ_CST);
	free(nc);
}

static struct nvmf_capsule *
capsule(int who, int seq)
{
	struct nvmf_capsule *nc = calloc(1, sizeof (*nc));

	nc->nc_id = who << 16 | seq;
	return (nc);
}

static void *
receiver(void *arg)
{
	int who = (int)(intptr_t)arg, seq;

	for (seq = 1; seq <= 2000; seq++)
		nvmft_queue_admin_command(&ctrlr, capsule(who, seq));
	return (NULL);
}

int
main(void)
{
	pthread_t t[4];
	int i;

	mutex_init(&ctrlr.ctrlr_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&ctrlr.ctrlr_admin_cv, NULL, CV_DRIVER, NULL);
	list_create(&ctrlr.ctrlr_admin_cmds, sizeof (nvmft_admin_cmd_t),
	    offsetof(nvmft_admin_cmd_t, nac_link));

	for (i = 0; i < 4; i++)
		assert(pthread_create(&t[i], NULL, receiver,
		    (void *)(intptr_t)i) == 0);
	for (i = 0; i < 4; i++)
		assert(pthread_join(t[i], NULL) == 0);
	while (__atomic_load_n(&handled, __ATOMIC_SEQ_CST) != 8000)
		usleep(1000);
	for (i = 0; i < 4; i++)
		assert(last_seen[i] == 2000);

	/* Stop with one command running and more queued behind it. */
	slow = 1;
	memset(last_seen, 0, sizeof (last_seen));
	for (i = 1; i <= 5; i++)
		nvmft_queue_admin_command(&ctrlr, capsule(0, i));
	usleep(5000);
	nvmft_admin_stop(&ctrlr);
	after_stop = 1;
	assert(running == 0 && !ctrlr.ctrlr_admin_running);
	assert(handled + freed == 8005 && handled >= 8001);
	nvmft_queue_admin_command(&ctrlr, capsule(0, 99));
	assert(handled + freed == 8006);
	usleep(50000);
	printf("admin queue passed: %d handled, %d dropped at stop\n",
	    handled, freed);
	return (0);
}

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
 * IDs whose client left, or whose request a full queue refused.  t4nex
 * finishes them itself: servers are closed, connections aborted and TIDs
 * released, with a timed retry for any request a queue could not take.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "t4_ofld.h"

#define	T4_OFLD_RETRY_USEC	100000

static void t4_ofld_retry_fire(void *);

/* Close an orphaned server.  Its CLOSE_LISTSRV_RPL frees it. */
void
t4_ofld_orphan_unlisten_locked(t4_ofld_t *of, t4_tid_ent_t *e, uint32_t stid)
{
	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));

	if ((e->te_flags & (TEF_LISTEN | TEF_UNLISTEN)) != TEF_LISTEN)
		return;
	if (t4_ofld_send_unlisten(of, e->te_port, stid,
	    (e->te_flags & TEF_V6) != 0) == 0)
		e->te_flags |= TEF_UNLISTEN;
	else
		t4_ofld_retry_arm_locked(of);
}

/*
 * Abort a connection nobody owns.  The entry stays until the chip confirms
 * with ABORT_RPL_RSS.  td_lock is held.
 */
void
t4_ofld_orphan_abort_locked(t4_ofld_t *of, t4_tid_ent_t *e, uint32_t tid)
{
	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));

	e->te_state = TTS_ORPHAN;
	if ((e->te_flags & TEF_ABORT) != 0)
		return;
	if ((e->te_flags & TEF_FLOWC) == 0 &&
	    t4_ofld_send_flowc(of, e->te_port, tid, NULL) == 0)
		e->te_flags |= TEF_FLOWC;
	if ((e->te_flags & TEF_FLOWC) != 0 &&
	    t4_ofld_send_abort(of, e->te_port, tid, B_TRUE) == 0) {
		e->te_flags |= TEF_ABORT;
		T4_OFLD_STAT(of, os_orphan_abort);
	} else {
		t4_ofld_retry_arm_locked(of);
	}
}

/*
 * Give an unowned TID back to the chip.  When the control queue is full the
 * entry stays as an orphan until the retry task sends the release, so that
 * the chip does not keep a TID the host has forgotten.
 */
void
t4_ofld_orphan_release_locked(t4_ofld_t *of, uint8_t port, uint32_t tid)
{
	t4_tids_t *td = &of->of_tids;
	t4_tid_ent_t *e;

	ASSERT(MUTEX_HELD(&td->td_lock));

	if (t4_ofld_send_tid_release(of, port, tid) == 0) {
		T4_OFLD_STAT(of, os_orphan_release);
		t4_tid_free_locked(of, T4_TID_HW, tid);
		return;
	}
	e = t4_tid_ent(of, T4_TID_HW, tid);
	if (e == NULL || e->te_state == TTS_FREE)
		return;
	if ((e->te_flags & TEF_EMBRYO) != 0)
		td->td_embryos--;
	e->te_state = TTS_ORPHAN;
	e->te_flags = TEF_RELPEND;
	e->te_owner = 0;
	e->te_ctx = NULL;
	t4_ofld_retry_arm_locked(of);
}

/*
 * Tear down, or mark for teardown, the IDs a client generation left behind.
 * Thread context, no lock held.
 */
void
t4_ofld_orphan_sweep(t4_ofld_t *of, uint32_t gen)
{
	t4_tids_t *td = &of->of_tids;
	t4_tid_ent_t *e;
	uint32_t i, id;

	mutex_enter(&td->td_lock);
	for (i = 0; i < td->td_stid.tt_n; i++) {
		e = &td->td_stid.tt_ent[i];
		id = td->td_stid.tt_base + i;
		if (e->te_state != TTS_OWNED || e->te_owner != gen)
			continue;
		if ((e->te_flags & TEF_V6) != 0 && (i & 1) != 0)
			continue;
		if ((e->te_flags & TEF_STID_BUSY) == 0) {
			t4_tid_free_locked(of, T4_TID_STID, id);
			continue;
		}
		e->te_state = TTS_ORPHAN;
		t4_ofld_orphan_unlisten_locked(of, e, id);
	}
	for (i = 0; i < td->td_atid.tt_n; i++) {
		e = &td->td_atid.tt_ent[i];
		if (e->te_state != TTS_OWNED || e->te_owner != gen)
			continue;
		if ((e->te_flags & TEF_OPEN) != 0)
			e->te_state = TTS_ORPHAN;
		else
			t4_tid_free_locked(of, T4_TID_ATID, i);
	}
	for (id = td->td_hw.tt_base; (e = t4_hwtid_next(of, &id)) != NULL;
	    id++) {
		if (e->te_state != TTS_OWNED || e->te_owner != gen)
			continue;
		if ((e->te_flags & TEF_RELEASING) != 0)
			continue;
		if ((e->te_flags & TEF_EMBRYO) != 0)
			t4_ofld_orphan_release_locked(of, e->te_port, id);
		else
			t4_ofld_orphan_abort_locked(of, e, id);
	}
	mutex_exit(&td->td_lock);
}

void
t4_ofld_retry_arm_locked(t4_ofld_t *of)
{
	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));

	T4_OFLD_STAT(of, os_orphan_retry);
	if (of->of_retry_stop || of->of_retry_tid != 0 || of->of_fatal)
		return;
	of->of_retry_tid = timeout(t4_ofld_retry_fire, of,
	    drv_usectohz(T4_OFLD_RETRY_USEC));
}

/* Resend the unlisten, abort and release requests the queues refused. */
static void
t4_ofld_retry_task(void *arg)
{
	t4_ofld_t *of = arg;
	t4_tids_t *td = &of->of_tids;
	t4_tid_ent_t *e;
	uint32_t i, id;

	mutex_enter(&td->td_lock);
	for (i = 0; i < td->td_stid.tt_n; i++) {
		e = &td->td_stid.tt_ent[i];
		if (e->te_state != TTS_ORPHAN ||
		    ((e->te_flags & TEF_V6) != 0 && (i & 1) != 0))
			continue;
		t4_ofld_orphan_unlisten_locked(of, e, td->td_stid.tt_base + i);
	}
	for (id = td->td_hw.tt_base; (e = t4_hwtid_next(of, &id)) != NULL;
	    id++) {
		if (e->te_state != TTS_ORPHAN)
			continue;
		if ((e->te_flags & TEF_RELPEND) != 0)
			t4_ofld_orphan_release_locked(of, e->te_port, id);
		else if ((e->te_flags & TEF_ABORT) == 0)
			t4_ofld_orphan_abort_locked(of, e, id);
	}
	mutex_exit(&td->td_lock);
}

static void
t4_ofld_retry_fire(void *arg)
{
	t4_ofld_t *of = arg;

	mutex_enter(&of->of_tids.td_lock);
	of->of_retry_tid = 0;
	if (!of->of_retry_stop && (of->of_tq == NULL ||
	    ddi_taskq_dispatch(of->of_tq, t4_ofld_retry_task, of,
	    DDI_NOSLEEP) != DDI_SUCCESS))
		t4_ofld_retry_arm_locked(of);
	mutex_exit(&of->of_tids.td_lock);
}

/* The caller then destroys the taskq, which waits for a running retry. */
void
t4_ofld_retry_stop(t4_ofld_t *of)
{
	timeout_id_t tid;

	mutex_enter(&of->of_tids.td_lock);
	of->of_retry_stop = B_TRUE;
	tid = of->of_retry_tid;
	of->of_retry_tid = 0;
	mutex_exit(&of->of_tids.td_lock);
	if (tid != 0)
		(void) untimeout(tid);
}

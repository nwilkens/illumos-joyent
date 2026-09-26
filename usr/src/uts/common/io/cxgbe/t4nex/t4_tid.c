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
 * The active open (atid), server (stid) and connection (hwtid) tables of the
 * offload core.  The client allocates atids and stids; the chip assigns
 * hwtids, and t4nex claims each one for the client when the CPL that carries
 * it arrives.
 *
 * Every entry records the client generation that owns it.  A CPL is handed to
 * the client only for an entry that generation still owns, and the lookup
 * holds the entry until the handler returns.  A freed atid goes to the tail of
 * its free list and a freed stid is skipped by the allocation rotor, so a
 * late CPL for a freed ID finds it free rather than reused.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>

#include "t4_ofld.h"

/*
 * hwtid entries live in chunks, all allocated at start so that a CPL never
 * finds a TID the table cannot track.
 */
#define	T4_TID_CHUNK_SHIFT	8
#define	T4_TID_CHUNK		(1U << T4_TID_CHUNK_SHIFT)

typedef struct t4_hwtid_dir {
	uint32_t	hd_nchunks;
	t4_tid_ent_t	**hd_chunk;
	uint16_t	*hd_used;	/* entries not free, per chunk */
} t4_hwtid_dir_t;

static int
t4_tid_tab_init(t4_tid_tab_t *tt, uint32_t n, uint32_t base)
{
	tt->tt_n = n;
	tt->tt_base = base;
	tt->tt_inuse = 0;
	tt->tt_rotor = 0;
	tt->tt_head = tt->tt_tail = T4_TID_NIL;
	if (n == 0)
		return (0);
	tt->tt_ent = kmem_zalloc(n * sizeof (t4_tid_ent_t), KM_SLEEP);
	return (0);
}

int
t4_tids_init(t4_ofld_t *of)
{
	t4_tids_t *td = &of->of_tids;
	t4_hwtid_dir_t *dir;

	mutex_init(&td->td_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(of->of_sc->intr_pri));
	cv_init(&td->td_cv, NULL, CV_DRIVER, NULL);

	(void) t4_tid_tab_init(&td->td_atid, of->of_natids, 0);
	for (uint32_t i = 0; i < of->of_natids; i++) {
		td->td_atid.tt_ent[i].te_next = i + 1 < of->of_natids ?
		    i + 1 : T4_TID_NIL;
	}
	if (of->of_natids != 0) {
		td->td_atid.tt_head = 0;
		td->td_atid.tt_tail = of->of_natids - 1;
	}
	(void) t4_tid_tab_init(&td->td_stid, of->of_nstids, of->of_stid_base);

	/* td_hw.tt_ent holds the chunk directory, not entries. */
	dir = kmem_zalloc(sizeof (*dir), KM_SLEEP);
	dir->hd_nchunks = howmany(of->of_ntids, T4_TID_CHUNK);
	dir->hd_chunk = kmem_zalloc(dir->hd_nchunks * sizeof (t4_tid_ent_t *),
	    KM_SLEEP);
	dir->hd_used = kmem_zalloc(dir->hd_nchunks * sizeof (uint16_t),
	    KM_SLEEP);
	for (uint32_t i = 0; i < dir->hd_nchunks; i++) {
		dir->hd_chunk[i] = kmem_zalloc(T4_TID_CHUNK *
		    sizeof (t4_tid_ent_t), KM_SLEEP);
	}
	td->td_hw.tt_n = of->of_ntids;
	td->td_hw.tt_base = of->of_tid_base;
	td->td_hw.tt_ent = (t4_tid_ent_t *)dir;
	return (0);
}

void
t4_tids_fini(t4_ofld_t *of)
{
	t4_tids_t *td = &of->of_tids;
	t4_hwtid_dir_t *dir = (t4_hwtid_dir_t *)td->td_hw.tt_ent;

	if (td->td_atid.tt_ent != NULL) {
		kmem_free(td->td_atid.tt_ent,
		    td->td_atid.tt_n * sizeof (t4_tid_ent_t));
	}
	if (td->td_stid.tt_ent != NULL) {
		kmem_free(td->td_stid.tt_ent,
		    td->td_stid.tt_n * sizeof (t4_tid_ent_t));
	}
	if (dir != NULL) {
		for (uint32_t i = 0; i < dir->hd_nchunks; i++) {
			if (dir->hd_chunk[i] != NULL) {
				kmem_free(dir->hd_chunk[i],
				    T4_TID_CHUNK * sizeof (t4_tid_ent_t));
			}
		}
		kmem_free(dir->hd_chunk,
		    dir->hd_nchunks * sizeof (t4_tid_ent_t *));
		kmem_free(dir->hd_used, dir->hd_nchunks * sizeof (uint16_t));
		kmem_free(dir, sizeof (*dir));
		cv_destroy(&td->td_cv);
		mutex_destroy(&td->td_lock);
	}
	bzero(td, sizeof (*td));
}

t4_tid_tab_t *
t4_tid_tab(t4_ofld_t *of, t4_tid_kind_t kind)
{
	switch (kind) {
	case T4_TID_ATID:
		return (&of->of_tids.td_atid);
	case T4_TID_STID:
		return (&of->of_tids.td_stid);
	default:
		return (&of->of_tids.td_hw);
	}
}

/*
 * The entry for a hardware ID, or NULL if the ID is outside the table.  The
 * caller holds td_lock.
 */
static t4_tid_ent_t *
t4_tid_lookup(t4_ofld_t *of, t4_tid_kind_t kind, uint32_t id)
{
	t4_tid_tab_t *tt = t4_tid_tab(of, kind);
	t4_hwtid_dir_t *dir;
	uint32_t idx, c;

	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));

	if (tt->tt_ent == NULL || id < tt->tt_base)
		return (NULL);
	idx = id - tt->tt_base;
	if (idx >= tt->tt_n)
		return (NULL);
	if (kind != T4_TID_HW)
		return (&tt->tt_ent[idx]);

	dir = (t4_hwtid_dir_t *)tt->tt_ent;
	c = idx >> T4_TID_CHUNK_SHIFT;
	return (&dir->hd_chunk[c][idx & (T4_TID_CHUNK - 1)]);
}

t4_tid_ent_t *
t4_tid_ent(t4_ofld_t *of, t4_tid_kind_t kind, uint32_t id)
{
	return (t4_tid_lookup(of, kind, id));
}

/*
 * The first hwtid entry at or after *idp in a chunk with an entry in use, or
 * NULL.  The caller holds td_lock.
 */
t4_tid_ent_t *
t4_hwtid_next(t4_ofld_t *of, uint32_t *idp)
{
	t4_tid_tab_t *tt = &of->of_tids.td_hw;
	t4_hwtid_dir_t *dir = (t4_hwtid_dir_t *)tt->tt_ent;
	uint32_t idx;

	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));
	if (dir == NULL || *idp < tt->tt_base)
		return (NULL);
	for (idx = *idp - tt->tt_base; idx < tt->tt_n; ) {
		const uint32_t c = idx >> T4_TID_CHUNK_SHIFT;

		if (dir->hd_used[c] == 0) {
			idx = (idx | (T4_TID_CHUNK - 1)) + 1;
			continue;
		}
		*idp = tt->tt_base + idx;
		return (&dir->hd_chunk[c][idx & (T4_TID_CHUNK - 1)]);
	}
	return (NULL);
}

static void
t4_hwtid_used(t4_ofld_t *of, uint32_t id, int delta)
{
	t4_tid_tab_t *tt = &of->of_tids.td_hw;
	t4_hwtid_dir_t *dir = (t4_hwtid_dir_t *)tt->tt_ent;

	dir->hd_used[(id - tt->tt_base) >> T4_TID_CHUNK_SHIFT] += delta;
}

int
t4_atid_alloc(t4_ofld_t *of, uint32_t owner, void *ctx, uint32_t *atidp)
{
	t4_tids_t *td = &of->of_tids;
	t4_tid_tab_t *tt = &td->td_atid;
	t4_tid_ent_t *e;
	uint32_t atid;

	mutex_enter(&td->td_lock);
	if ((atid = tt->tt_head) == T4_TID_NIL) {
		mutex_exit(&td->td_lock);
		return (ENOSPC);
	}
	e = &tt->tt_ent[atid];
	VERIFY3U(e->te_state, ==, TTS_FREE);
	tt->tt_head = e->te_next;
	if (tt->tt_head == T4_TID_NIL)
		tt->tt_tail = T4_TID_NIL;
	e->te_next = T4_TID_NIL;
	e->te_state = TTS_OWNED;
	e->te_flags = 0;
	e->te_owner = owner;
	e->te_ctx = ctx;
	e->te_rxq = UINT16_MAX;
	tt->tt_inuse++;
	mutex_exit(&td->td_lock);

	*atidp = atid;
	return (0);
}

/*
 * An IPv6 server takes an aligned pair of stids.  The rotor starts each
 * search after the last allocation.
 */
int
t4_stid_alloc(t4_ofld_t *of, uint32_t owner, sa_family_t family, void *ctx,
    uint32_t *stidp)
{
	t4_tids_t *td = &of->of_tids;
	t4_tid_tab_t *tt = &td->td_stid;
	const uint32_t n = family == AF_INET6 ? 2 : 1;
	uint32_t i, idx;

	if (family != AF_INET && family != AF_INET6)
		return (EAFNOSUPPORT);

	mutex_enter(&td->td_lock);
	for (i = 0; i < tt->tt_n; i++) {
		idx = (tt->tt_rotor + i) % tt->tt_n;
		if (n == 2)
			idx &= ~1U;
		if (idx + n > tt->tt_n)
			continue;
		if (tt->tt_ent[idx].te_state == TTS_FREE &&
		    (n == 1 || tt->tt_ent[idx + 1].te_state == TTS_FREE))
			break;
	}
	if (i == tt->tt_n) {
		mutex_exit(&td->td_lock);
		return (ENOSPC);
	}
	for (i = 0; i < n; i++) {
		t4_tid_ent_t *e = &tt->tt_ent[idx + i];

		e->te_state = TTS_OWNED;
		e->te_flags = n == 2 ? TEF_V6 : 0;
		e->te_owner = owner;
		e->te_ctx = i == 0 ? ctx : NULL;
		e->te_rxq = UINT16_MAX;
	}
	tt->tt_inuse += n;
	tt->tt_rotor = (idx + n) % tt->tt_n;
	mutex_exit(&td->td_lock);

	*stidp = tt->tt_base + idx;
	return (0);
}

/*
 * Return an entry to its table.  Its hardware counterpart has already been
 * released.  The caller holds td_lock.
 */
void
t4_tid_free_locked(t4_ofld_t *of, t4_tid_kind_t kind, uint32_t id)
{
	t4_tid_tab_t *tt = t4_tid_tab(of, kind);
	t4_tid_ent_t *e = t4_tid_lookup(of, kind, id);
	uint32_t n = 1;

	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));
	if (e == NULL || e->te_state == TTS_FREE)
		return;
	if (kind == T4_TID_STID && (e->te_flags & TEF_V6) != 0 &&
	    ((id - tt->tt_base) & 1) == 0)
		n = 2;

	if (kind == T4_TID_HW && (e->te_flags & TEF_EMBRYO) != 0)
		of->of_tids.td_embryos--;
	for (uint32_t i = 0; i < n; i++) {
		e[i].te_state = TTS_FREE;
		e[i].te_flags = 0;
		e[i].te_ctx = NULL;
		e[i].te_owner = 0;
		e[i].te_rxq = 0;
		e[i].te_ri = T4_TID_NIL;
	}
	ASSERT3U(tt->tt_inuse, >=, n);
	tt->tt_inuse -= n;
	if (kind == T4_TID_HW)
		t4_hwtid_used(of, id, -1);

	if (kind == T4_TID_ATID) {
		const uint32_t idx = id - tt->tt_base;

		e->te_next = T4_TID_NIL;
		if (tt->tt_tail == T4_TID_NIL) {
			tt->tt_head = tt->tt_tail = idx;
		} else {
			tt->tt_ent[tt->tt_tail].te_next = idx;
			tt->tt_tail = idx;
		}
	}
	cv_broadcast(&of->of_tids.td_cv);
}

/*
 * The entry, if owner owns it.  The caller holds td_lock.
 */
t4_tid_ent_t *
t4_tid_owned(t4_ofld_t *of, t4_tid_kind_t kind, uint32_t id, uint32_t owner)
{
	t4_tid_tab_t *tt = t4_tid_tab(of, kind);
	t4_tid_ent_t *e = t4_tid_lookup(of, kind, id);

	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));
	if (e == NULL || e->te_state != TTS_OWNED || e->te_owner != owner)
		return (NULL);
	/* The second stid of an IPv6 pair is not an ID of its own. */
	if (kind == T4_TID_STID && (e->te_flags & TEF_V6) != 0 &&
	    ((id - tt->tt_base) & 1) != 0)
		return (NULL);
	return (e);
}

/*
 * Hold an entry owner owns for the length of one CPL handler.  rxq is the
 * queue the CPL arrived on; an entry bound to a queue accepts CPLs only from
 * that queue.
 */
int
t4_tid_hold(t4_ofld_t *of, t4_tid_kind_t kind, uint32_t id, uint32_t owner,
    uint16_t rxq, void **ctxp)
{
	t4_tids_t *td = &of->of_tids;
	t4_tid_ent_t *e;
	int rc = 0;

	mutex_enter(&td->td_lock);
	if ((e = t4_tid_lookup(of, kind, id)) == NULL) {
		rc = ERANGE;
	} else if (e->te_state != TTS_OWNED || e->te_owner != owner ||
	    (e->te_flags & TEF_RELEASING) != 0 ||
	    (kind == T4_TID_STID && (e->te_flags & TEF_V6) != 0 &&
	    ((id - t4_tid_tab(of, kind)->tt_base) & 1) != 0)) {
		rc = ESTALE;
	} else if (e->te_rxq != UINT16_MAX && e->te_rxq != rxq) {
		rc = EXDEV;
	} else if (e->te_refs == UINT16_MAX) {
		rc = EBUSY;
	} else {
		e->te_refs++;
		*ctxp = e->te_ctx;
	}
	mutex_exit(&td->td_lock);
	return (rc);
}

/*
 * Claim a hwtid the chip just assigned.  A live entry cannot be claimed, but
 * one whose TID_RELEASE may have reached the chip can: the chip gives an ID
 * out again only after it processed the release.  A SYN (TEF_EMBRYO) for the
 * client past T4_OFLD_MAX_EMBRYOS fails with EAGAIN.
 */
int
t4_hwtid_claim(t4_ofld_t *of, uint32_t tid, t4_tid_state_t state,
    uint32_t owner, uint8_t port, uint16_t rxq, uint16_t flags, void *ctx)
{
	t4_tids_t *td = &of->of_tids;
	t4_tid_ent_t *e;
	int rc = 0;

	mutex_enter(&td->td_lock);
	if ((e = t4_tid_lookup(of, T4_TID_HW, tid)) == NULL) {
		rc = ERANGE;
	} else if (e->te_state != TTS_FREE &&
	    (e->te_flags & TEF_RELEASING) == 0) {
		rc = EEXIST;
	} else if (state == TTS_OWNED && (flags & TEF_EMBRYO) != 0 &&
	    (e->te_state == TTS_FREE || (e->te_flags & TEF_EMBRYO) == 0) &&
	    td->td_embryos >= T4_OFLD_MAX_EMBRYOS) {
		rc = EAGAIN;
	} else {
		if (e->te_state == TTS_FREE) {
			td->td_hw.tt_inuse++;
			t4_hwtid_used(of, tid, 1);
		} else if ((e->te_flags & TEF_EMBRYO) != 0) {
			td->td_embryos--;
		}
		if ((flags & TEF_EMBRYO) != 0)
			td->td_embryos++;
		e->te_state = state;
		e->te_flags = flags;
		e->te_owner = owner;
		e->te_port = port;
		e->te_rxq = rxq;
		e->te_ctx = ctx;
		e->te_ri = T4_TID_NIL;
		e->te_seq++;
	}
	mutex_exit(&td->td_lock);
	return (rc);
}

void
t4_tid_rele(t4_ofld_t *of, t4_tid_kind_t kind, uint32_t id)
{
	t4_tids_t *td = &of->of_tids;
	t4_tid_ent_t *e;

	mutex_enter(&td->td_lock);
	e = t4_tid_lookup(of, kind, id);
	VERIFY(e != NULL && e->te_refs > 0);
	if (--e->te_refs == 0)
		cv_broadcast(&td->td_cv);
	mutex_exit(&td->td_lock);
}

/*
 * Wait until no CPL handler holds the entry.  A handler runs in interrupt
 * context and may release its own entry, so only thread context waits.  The
 * caller holds td_lock.
 */
void
t4_tid_wait_idle(t4_ofld_t *of, t4_tid_ent_t *e)
{
	ASSERT(MUTEX_HELD(&of->of_tids.td_lock));
	if (servicing_interrupt())
		return;
	while (e->te_refs != 0)
		cv_wait(&of->of_tids.td_cv, &of->of_tids.td_lock);
}

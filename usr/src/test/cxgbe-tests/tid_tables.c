/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Runs the real t4_tid.c (copied in by tid_tables.py beside a t4_ofld.h
 * built from the real header's TID definitions).
 */

#include "t4_tid_under_test.c"

static t4_ofld_t *
setup(uint32_t natids, uint32_t nstids, uint32_t sbase, uint32_t ntids,
    uint32_t tbase)
{
	static struct adapter sc;
	t4_ofld_t *of = calloc(1, sizeof (*of));

	of->of_sc = &sc;
	of->of_natids = natids;
	of->of_nstids = nstids;
	of->of_stid_base = sbase;
	of->of_ntids = ntids;
	of->of_tid_base = tbase;
	CHECK(t4_tids_init(of) == 0);
	return (of);
}

static void
teardown(t4_ofld_t *of)
{
	t4_tids_fini(of);
	free(of);
}

static void
free_id(t4_ofld_t *of, t4_tid_kind_t kind, uint32_t id)
{
	mutex_enter(&of->of_tids.td_lock);
	t4_tid_free_locked(of, kind, id);
	mutex_exit(&of->of_tids.td_lock);
}

/* A freed atid goes to the back of the line. */
static void
test_atid_fifo(void)
{
	t4_ofld_t *of = setup(4, 0, 0, 16, 0);
	uint32_t a[4], x;
	int ctx;

	for (uint_t i = 0; i < 4; i++) {
		CHECK(t4_atid_alloc(of, 7, &ctx, &a[i]) == 0);
		CHECK(a[i] == i);
	}
	CHECK(t4_atid_alloc(of, 7, &ctx, &x) == ENOSPC);
	free_id(of, T4_TID_ATID, 1);
	free_id(of, T4_TID_ATID, 3);
	CHECK(t4_atid_alloc(of, 7, &ctx, &x) == 0 && x == 1);
	free_id(of, T4_TID_ATID, 0);
	CHECK(t4_atid_alloc(of, 7, &ctx, &x) == 0 && x == 3);
	CHECK(t4_atid_alloc(of, 7, &ctx, &x) == 0 && x == 0);
	CHECK(of->of_tids.td_atid.tt_inuse == 4);
	/* Freeing twice, or out of range, changes nothing. */
	free_id(of, T4_TID_ATID, 2);
	free_id(of, T4_TID_ATID, 2);
	free_id(of, T4_TID_ATID, 99);
	CHECK(of->of_tids.td_atid.tt_inuse == 3);
	teardown(of);
}

/* IPv6 servers take an aligned pair; the odd half is not an ID. */
static void
test_stid_pairs(void)
{
	t4_ofld_t *of = setup(2, 8, 100, 16, 0);
	uint32_t s4, s6, x;
	void *ctx;
	int c;

	CHECK(t4_stid_alloc(of, 3, AF_INET, &c, &s4) == 0 && s4 == 100);
	CHECK(t4_stid_alloc(of, 3, AF_INET6, &c, &s6) == 0 && s6 == 102);
	CHECK(of->of_tids.td_stid.tt_inuse == 3);
	CHECK(t4_stid_alloc(of, 3, AF_UNIX, &c, &x) == EAFNOSUPPORT);

	mutex_enter(&of->of_tids.td_lock);
	CHECK(t4_tid_owned(of, T4_TID_STID, 102, 3) != NULL);
	CHECK(t4_tid_owned(of, T4_TID_STID, 103, 3) == NULL);
	CHECK(t4_tid_owned(of, T4_TID_STID, 102, 4) == NULL);
	CHECK(t4_tid_owned(of, T4_TID_STID, 99, 3) == NULL);
	CHECK(t4_tid_owned(of, T4_TID_STID, 108, 3) == NULL);
	mutex_exit(&of->of_tids.td_lock);
	CHECK(t4_tid_hold(of, T4_TID_STID, 103, 3, 0, &ctx) == ESTALE);
	CHECK(t4_tid_hold(of, T4_TID_STID, 102, 3, 0, &ctx) == 0 &&
	    ctx == &c);
	t4_tid_rele(of, T4_TID_STID, 102);

	free_id(of, T4_TID_STID, 102);
	CHECK(of->of_tids.td_stid.tt_inuse == 1);
	/* The rotor moved on: the freed pair is not handed out next. */
	CHECK(t4_stid_alloc(of, 3, AF_INET, &c, &x) == 0 && x == 104);
	/* Fill every slot but the freed pair, which IPv6 can take again. */
	for (uint_t i = 0; i < 4; i++)
		CHECK(t4_stid_alloc(of, 3, AF_INET, &c, &x) == 0 && x != 102 &&
		    x != 103);
	CHECK(t4_stid_alloc(of, 3, AF_INET6, &c, &x) == 0 && x == 102);
	CHECK(t4_stid_alloc(of, 3, AF_INET, &c, &x) == ENOSPC);
	/* Two free slots that are not an aligned pair take no IPv6 server. */
	free_id(of, T4_TID_STID, 101);
	free_id(of, T4_TID_STID, 104);
	CHECK(of->of_tids.td_stid.tt_inuse == 6);
	CHECK(t4_stid_alloc(of, 3, AF_INET6, &c, &x) == ENOSPC);
	CHECK(t4_stid_alloc(of, 3, AF_INET, &c, &x) == 0 && x == 104);
	/* Freeing the first stid of a pair frees both. */
	free_id(of, T4_TID_STID, 102);
	CHECK(of->of_tids.td_stid.tt_inuse == 5);
	teardown(of);
}

/* Claims of chip-assigned TIDs are bounded and exclusive. */
static void
test_hwtid_claim(void)
{
	t4_ofld_t *of = setup(2, 2, 0, 1000, 500);
	t4_tid_ent_t *e;
	uint32_t id;
	void *ctx;
	int c;

	CHECK(t4_hwtid_claim(of, 499, TTS_OWNED, 5, 0, 7, 0, &c) == ERANGE);
	CHECK(t4_hwtid_claim(of, 1500, TTS_OWNED, 5, 0, 7, 0, &c) == ERANGE);
	CHECK(t4_hwtid_claim(of, UINT32_MAX, TTS_OWNED, 5, 0, 7, 0, &c) ==
	    ERANGE);
	/* Every entry exists from the start: a claim never allocates. */
	stub_nosleep_fail = 1000;
	CHECK(t4_hwtid_claim(of, 1499, TTS_OWNED, 5, 0, 7, 0, &c) == 0);
	free_id(of, T4_TID_HW, 1499);
	CHECK(t4_hwtid_claim(of, 600, TTS_OWNED, 5, 1, 7, TEF_EMBRYO, &c) ==
	    0);
	stub_nosleep_fail = 0;
	CHECK(t4_hwtid_claim(of, 600, TTS_OWNED, 5, 1, 7, 0, &c) == EEXIST);
	CHECK(of->of_tids.td_hw.tt_inuse == 1);

	/* Only the queue the connection lives on may deliver its CPLs. */
	CHECK(t4_tid_hold(of, T4_TID_HW, 600, 5, 8, &ctx) == EXDEV);
	CHECK(t4_tid_hold(of, T4_TID_HW, 600, 6, 7, &ctx) == ESTALE);
	CHECK(t4_tid_hold(of, T4_TID_HW, 601, 5, 7, &ctx) == ESTALE);
	CHECK(t4_tid_hold(of, T4_TID_HW, 1499, 5, 7, &ctx) == ESTALE);
	CHECK(t4_tid_hold(of, T4_TID_HW, 1500, 5, 7, &ctx) == ERANGE);
	CHECK(t4_tid_hold(of, T4_TID_HW, 600, 5, 7, &ctx) == 0 && ctx == &c);

	/* A release on its way: new holds fail, and the chip may reuse it. */
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_HW, 600);
	CHECK(e != NULL && e->te_refs == 1);
	e->te_flags |= TEF_RELEASING;
	const uint32_t seq = e->te_seq;
	stub_in_intr = 1;
	t4_tid_wait_idle(of, e);
	stub_in_intr = 0;
	mutex_exit(&of->of_tids.td_lock);
	CHECK(t4_tid_hold(of, T4_TID_HW, 600, 5, 7, &ctx) == ESTALE);
	t4_tid_rele(of, T4_TID_HW, 600);
	CHECK(t4_hwtid_claim(of, 600, TTS_OWNED, 9, 1, 7, 0, NULL) == 0);
	CHECK(e->te_seq == seq + 1 && e->te_owner == 9 && e->te_flags == 0);
	CHECK(of->of_tids.td_hw.tt_inuse == 1);

	/* The walk visits every ID once and ends at the last. */
	CHECK(t4_hwtid_claim(of, 1499, TTS_ORPHAN, 0, 0, 7, 0, NULL) == 0);
	mutex_enter(&of->of_tids.td_lock);
	uint_t n = 0;
	for (id = 500; (e = t4_hwtid_next(of, &id)) != NULL; id++) {
		if (e->te_state != TTS_FREE) {
			CHECK(id == 600 || id == 1499);
			n++;
		}
	}
	CHECK(n == 2);
	t4_tid_free_locked(of, T4_TID_HW, 600);
	t4_tid_free_locked(of, T4_TID_HW, 1499);
	mutex_exit(&of->of_tids.td_lock);
	CHECK(of->of_tids.td_hw.tt_inuse == 0);
	teardown(of);
}

/* A SYN flood cannot pin more than T4_OFLD_MAX_EMBRYOS TIDs. */
static void
test_embryo_cap(void)
{
	const uint32_t max = T4_OFLD_MAX_EMBRYOS;
	t4_ofld_t *of = setup(2, 2, 0, max + 16, 0);
	t4_tid_ent_t *e;

	for (uint32_t i = 0; i < max; i++)
		CHECK(t4_hwtid_claim(of, i, TTS_OWNED, 5, 0, 7, TEF_EMBRYO,
		    NULL) == 0);
	CHECK(of->of_tids.td_embryos == max);
	CHECK(t4_hwtid_claim(of, max, TTS_OWNED, 5, 0, 7, TEF_EMBRYO,
	    NULL) == EAGAIN);
	CHECK(t4_hwtid_claim(of, max, TTS_OWNED, 5, 0, 7, 0, NULL) == 0);
	/* A SYN being refused is never itself refused by the cap. */
	CHECK(t4_hwtid_claim(of, max + 2, TTS_ORPHAN, 0, 0, 7, TEF_EMBRYO,
	    NULL) == 0);
	free_id(of, T4_TID_HW, max + 2);
	CHECK(of->of_tids.td_embryos == max);

	/* Freeing or accepting an embryo makes room for one more. */
	free_id(of, T4_TID_HW, 0);
	CHECK(of->of_tids.td_embryos == max - 1);
	CHECK(t4_hwtid_claim(of, max + 1, TTS_OWNED, 5, 0, 7, TEF_EMBRYO,
	    NULL) == 0);

	/* Taking over a releasing embryo does not count it twice. */
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_HW, 1);
	e->te_flags |= TEF_RELEASING;
	mutex_exit(&of->of_tids.td_lock);
	CHECK(t4_hwtid_claim(of, 1, TTS_OWNED, 5, 0, 7, TEF_EMBRYO,
	    NULL) == 0);
	CHECK(of->of_tids.td_embryos == max);
	mutex_enter(&of->of_tids.td_lock);
	e->te_flags |= TEF_RELEASING;
	mutex_exit(&of->of_tids.td_lock);
	CHECK(t4_hwtid_claim(of, 1, TTS_OWNED, 5, 0, 7, 0, NULL) == 0);
	CHECK(of->of_tids.td_embryos == max - 1);

	mutex_enter(&of->of_tids.td_lock);
	for (uint32_t i = 0; i < max + 2; i++)
		t4_tid_free_locked(of, T4_TID_HW, i);
	mutex_exit(&of->of_tids.td_lock);
	CHECK(of->of_tids.td_embryos == 0 && of->of_tids.td_hw.tt_inuse == 0);
	teardown(of);
}

int
main(void)
{
	test_atid_fifo();
	test_stid_pairs();
	test_hwtid_claim();
	test_embryo_cap();
	(void) printf("tid tables: 4 scenarios passed\n");
	return (0);
}

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
 * The RoCEv2 GID table against fake devices whose slots can fill up and
 * stay in use by QPs: directed cases, then a seeded run of random address
 * changes, moves, busy slots, failures and device removals checked against
 * a model after every pass.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdk_cm_gidtab.h"

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		exit(1);						\
	}								\
} while (0)

#define	NDEV	2
#define	NPORT	2
#define	CAP	8
#define	NKEY	40

typedef struct slot {
	int		used;
	int		busy;		/* a QP or AH uses it */
	int		withdrawn;
	rdk_gidkey_t	key;
	uint8_t		mac[6];
} slot_t;

typedef struct fdev {
	slot_t		s[NPORT][CAP];
	int		fail;		/* the next adds fail with EIO */
	int		removing;	/* its QPs and AHs are gone */
} fdev_t;

static fdev_t devs[NDEV];
static uint64_t seed = 0x9e3779b97f4a7c15ULL;

static uint32_t
rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return ((uint32_t)(seed >> 11));
}

static int
f_add(void *arg, void *dev, uint32_t port, const rdk_gidkey_t *key,
    const uint8_t *mac, uint16_t *idx)
{
	fdev_t *d = dev;
	uint16_t i;

	(void) arg;
	CHECK(!d->removing && port < NPORT);
	if (d->fail > 0) {
		d->fail--;
		return (EIO);
	}
	for (i = 0; i < CAP; i++) {
		CHECK(!d->s[port][i].used ||
		    rdk_gidkey_cmp(&d->s[port][i].key, key) != 0);
	}
	for (i = 0; i < CAP; i++) {
		if (!d->s[port][i].used) {
			memset(&d->s[port][i], 0, sizeof (slot_t));
			d->s[port][i].used = 1;
			d->s[port][i].key = *key;
			memcpy(d->s[port][i].mac, mac, 6);
			*idx = i;
			return (0);
		}
	}
	return (ENOSPC);
}

static int
f_del(void *arg, void *dev, uint32_t port, uint16_t idx)
{
	fdev_t *d = dev;
	slot_t *s;

	(void) arg;
	CHECK(port < NPORT && idx < CAP);
	s = &d->s[port][idx];
	CHECK(s->used);
	if (d->removing) {
		memset(s, 0, sizeof (*s));
		return (0);
	}
	/* A slot in use is withdrawn before anyone tries to delete it. */
	CHECK(s->withdrawn);
	if (s->busy)
		return (EBUSY);
	memset(s, 0, sizeof (*s));
	return (0);
}

static void
f_withdraw(void *arg, void *dev, uint32_t port, uint16_t idx, boolean_t on)
{
	fdev_t *d = dev;
	slot_t *s = &d->s[port][idx];

	(void) arg;
	CHECK(s->used && s->withdrawn == !on);
	s->withdrawn = on;
}

static const rdk_gidtab_ops_t ops = { f_add, f_del, f_withdraw };

static void
mkkey(rdk_gidkey_t *k, int n)
{
	memset(k, 0, sizeof (*k));
	k->gk_stack = 0;
	k->gk_ifindex = 2 + n % 3;
	k->gk_vlan = 0xffff;
	k->gk_type = RDK_GID_TYPE_ROCEV2;
	k->gk_addr[10] = k->gk_addr[11] = 0xff;
	k->gk_addr[12] = 10;
	k->gk_addr[15] = (uint8_t)n;
}

static const uint8_t *
mac_of(int dev, int port)
{
	static uint8_t m[NDEV][NPORT][6];

	m[dev][port][0] = 2;
	m[dev][port][4] = (uint8_t)dev;
	m[dev][port][5] = (uint8_t)port;
	return (m[dev][port]);
}

/* Where the model wants each key this pass: -1, or dev * NPORT + port. */
static int where[NKEY];

static rdk_gidtab_stats_t
pass(rdk_gidtab_t *gt)
{
	rdk_gidtab_stats_t st;
	rdk_gidkey_t k;
	int i, d, p;

	rdk_gidtab_begin(gt);
	for (i = 0; i < NKEY; i++) {
		if (where[i] < 0)
			continue;
		d = where[i] / NPORT;
		p = where[i] % NPORT;
		mkkey(&k, i);
		CHECK(rdk_gidtab_want(gt, &k, &devs[d], p,
		    mac_of(d, p)) == 0);
	}
	rdk_gidtab_end(gt, &st);
	return (st);
}

static slot_t *
slot_of(const rdk_gident_t *e)
{
	fdev_t *d = e->ge_dev;

	return (&d->s[e->ge_port][e->ge_index]);
}

/*
 * After a pass: every wanted key is installed where it is wanted and in
 * use, or waits for room; every used slot belongs to exactly one entry;
 * a slot no longer wanted is withdrawn and stays only while it is busy.
 */
static void
verify(const rdk_gidtab_t *gt)
{
	int owners[NDEV][NPORT][CAP];
	const rdk_gident_t *e;
	rdk_gidkey_t k;
	int i, j, d, p, n = 0, found, transient = 0, full;

	memset(owners, 0, sizeof (owners));
	for (j = 0; j < RDK_GIDTAB_MAX; j++) {
		e = &gt->gt_ents[j];
		if (e->ge_state == RGS_FREE)
			continue;
		n++;
		CHECK(e->ge_state == RGS_WANT || e->ge_state == RGS_INSTALLED ||
		    e->ge_state == RGS_STALE);
		CHECK(!e->ge_revive);
		if (e->ge_state == RGS_WANT) {
			CHECK(e->ge_marked && e->ge_err != 0);
			transient = 1;
			continue;
		}
		d = (int)((fdev_t *)e->ge_dev - devs);
		CHECK(d >= 0 && d < NDEV);
		owners[d][e->ge_port][e->ge_index]++;
		CHECK(rdk_gidkey_cmp(&slot_of(e)->key, &e->ge_key) == 0);
		CHECK(memcmp(slot_of(e)->mac, mac_of(d, e->ge_port), 6) == 0);
		if (e->ge_state == RGS_INSTALLED) {
			CHECK(e->ge_marked && !slot_of(e)->withdrawn);
		} else {
			CHECK(slot_of(e)->withdrawn && slot_of(e)->busy);
			transient = 1;
		}
	}
	CHECK(n == (int)gt->gt_n);
	CHECK((rdk_gidtab_retry_needed(gt) != B_FALSE) == transient);

	for (d = 0; d < NDEV; d++) {
		for (p = 0; p < NPORT; p++) {
			full = 1;
			for (i = 0; i < CAP; i++) {
				if (!devs[d].s[p][i].used) {
					full = 0;
					CHECK(owners[d][p][i] == 0);
				} else {
					CHECK(owners[d][p][i] == 1);
				}
			}
			for (i = 0; i < NKEY; i++) {
				if (where[i] != d * NPORT + p)
					continue;
				mkkey(&k, i);
				found = 0;
				for (j = 0; j < RDK_GIDTAB_MAX; j++) {
					e = &gt->gt_ents[j];
					if (e->ge_state == RGS_FREE ||
					    rdk_gidkey_cmp(&e->ge_key, &k) != 0)
						continue;
					if (e->ge_dev != &devs[d] ||
					    e->ge_port != (uint32_t)p) {
						CHECK(e->ge_state == RGS_STALE);
						continue;
					}
					CHECK(!found);
					found = 1;
					CHECK(e->ge_state == RGS_INSTALLED ||
					    (e->ge_state == RGS_WANT &&
					    (full || e->ge_err == EIO)));
				}
				CHECK(found);
			}
		}
	}
}

static int
installed_at(const rdk_gidtab_t *gt, int key, int d, int p)
{
	rdk_gidkey_t k;
	int j;

	mkkey(&k, key);
	for (j = 0; j < RDK_GIDTAB_MAX; j++) {
		const rdk_gident_t *e = &gt->gt_ents[j];

		if (e->ge_state == RGS_INSTALLED && e->ge_dev == &devs[d] &&
		    e->ge_port == (uint32_t)p &&
		    rdk_gidkey_cmp(&e->ge_key, &k) == 0)
			return (e->ge_index);
	}
	return (-1);
}

static void
reset(rdk_gidtab_t *gt)
{
	int i;

	memset(devs, 0, sizeof (devs));
	for (i = 0; i < NKEY; i++)
		where[i] = -1;
	rdk_gidtab_init(gt, &ops, NULL);
}

static void
t_directed(rdk_gidtab_t *gt)
{
	rdk_gidtab_stats_t st;
	rdk_gidkey_t k;
	int i, idx;

	/* Add, then remove. */
	reset(gt);
	where[0] = 0;
	where[1] = 1;
	st = pass(gt);
	CHECK(st.gs_added == 2 && installed_at(gt, 0, 0, 0) >= 0 &&
	    installed_at(gt, 1, 0, 1) >= 0);
	verify(gt);
	st = pass(gt);
	CHECK(st.gs_added == 0 && st.gs_deleted == 0);
	where[0] = -1;
	st = pass(gt);
	CHECK(st.gs_withdrawn == 1 && st.gs_deleted == 1 && gt->gt_n == 1);
	verify(gt);

	/* No room: the rest wait, and get in when a slot frees. */
	reset(gt);
	for (i = 0; i < CAP + 2; i++)
		where[i] = 0;
	st = pass(gt);
	CHECK(st.gs_added == CAP && st.gs_nospc == 2);
	CHECK(rdk_gidtab_retry_needed(gt));
	verify(gt);
	where[0] = -1;
	st = pass(gt);
	CHECK(st.gs_deleted == 1 && st.gs_added == 1 && st.gs_nospc == 1);
	verify(gt);

	/* A slot in use stays until it is free; wanted again, it revives. */
	reset(gt);
	where[5] = 0;
	(void) pass(gt);
	idx = installed_at(gt, 5, 0, 0);
	devs[0].s[0][idx].busy = 1;
	where[5] = -1;
	st = pass(gt);
	CHECK(st.gs_withdrawn == 1 && st.gs_busy == 1 && gt->gt_n == 1);
	CHECK(rdk_gidtab_retry_needed(gt));
	verify(gt);
	st = pass(gt);
	CHECK(st.gs_withdrawn == 0 && st.gs_busy == 1);
	where[5] = 0;
	st = pass(gt);
	CHECK(st.gs_revived == 1 && st.gs_added == 0 &&
	    installed_at(gt, 5, 0, 0) == idx && !devs[0].s[0][idx].withdrawn);
	CHECK(!rdk_gidtab_retry_needed(gt));
	verify(gt);
	where[5] = -1;
	(void) pass(gt);
	devs[0].s[0][idx].busy = 0;
	st = pass(gt);
	CHECK(st.gs_deleted == 1 && gt->gt_n == 0 &&
	    !rdk_gidtab_retry_needed(gt));
	verify(gt);

	/* An address moves port while its old slot is busy, and back. */
	reset(gt);
	where[7] = 0;
	(void) pass(gt);
	idx = installed_at(gt, 7, 0, 0);
	devs[0].s[0][idx].busy = 1;
	where[7] = NPORT + 1;
	st = pass(gt);
	CHECK(st.gs_withdrawn == 1 && st.gs_busy == 1 && st.gs_added == 1);
	CHECK(installed_at(gt, 7, 1, 1) >= 0 && gt->gt_n == 2);
	verify(gt);
	where[7] = 0;
	st = pass(gt);
	CHECK(st.gs_revived == 1 && st.gs_withdrawn == 1 &&
	    st.gs_deleted == 1 && installed_at(gt, 7, 0, 0) == idx);
	verify(gt);

	/* A failed add is retried; the device leaving drops its entries. */
	reset(gt);
	where[3] = 0;
	where[4] = NPORT;
	devs[0].fail = 1;
	st = pass(gt);
	CHECK(st.gs_add_failed == 1 && st.gs_added == 1);
	CHECK(rdk_gidtab_retry_needed(gt));
	verify(gt);
	st = pass(gt);
	CHECK(st.gs_added == 1 && !rdk_gidtab_retry_needed(gt));
	devs[0].removing = 1;
	rdk_gidtab_forget_dev(gt, &devs[0], B_TRUE);
	CHECK(gt->gt_n == 1 && !devs[0].s[0][0].used);
	rdk_gidtab_forget_dev(gt, &devs[1], B_FALSE);
	CHECK(gt->gt_n == 0 && devs[1].s[0][0].used);

	/* More addresses than entries: the extra are refused and counted. */
	reset(gt);
	rdk_gidtab_begin(gt);
	for (i = 0; i < RDK_GIDTAB_MAX + 3; i++) {
		mkkey(&k, 0);
		k.gk_addr[14] = (uint8_t)(i >> 8);
		k.gk_addr[15] = (uint8_t)i;
		CHECK(rdk_gidtab_want(gt, &k, &devs[0], 0, mac_of(0, 0)) ==
		    (i < RDK_GIDTAB_MAX ? 0 : ENOSPC));
	}
	CHECK(gt->gt_dropped == 3 && gt->gt_n == RDK_GIDTAB_MAX);
	rdk_gidtab_end(gt, &st);
	CHECK(st.gs_added == CAP && st.gs_nospc == RDK_GIDTAB_MAX - CAP);
	rdk_gidtab_begin(gt);
	rdk_gidtab_end(gt, &st);
	CHECK(st.gs_deleted == CAP && gt->gt_n == 0);
}

static void
t_random(rdk_gidtab_t *gt, int rounds)
{
	rdk_gidtab_stats_t st, sum;
	int r, i, d, p;

	reset(gt);
	memset(&sum, 0, sizeof (sum));
	for (r = 0; r < rounds; r++) {
		for (i = 0; i < NKEY; i++) {
			switch (rnd() % 16) {
			case 0:
				where[i] = -1;
				break;
			case 1:
			case 2:
				where[i] = (int)(rnd() % (NDEV * NPORT));
				break;
			default:
				break;
			}
		}
		for (d = 0; d < NDEV; d++) {
			for (p = 0; p < NPORT; p++) {
				for (i = 0; i < CAP; i++) {
					if (devs[d].s[p][i].used &&
					    rnd() % 8 == 0)
						devs[d].s[p][i].busy ^= 1;
				}
			}
			if (rnd() % 64 == 0)
				devs[d].fail = 1 + rnd() % 2;
		}
		if (rnd() % 200 == 0) {
			d = rnd() % NDEV;
			devs[d].removing = 1;
			rdk_gidtab_forget_dev(gt, &devs[d], B_TRUE);
			memset(&devs[d], 0, sizeof (devs[d]));
			for (i = 0; i < NKEY; i++) {
				if (where[i] >= 0 && where[i] / NPORT == d)
					where[i] = -1;
			}
		}
		st = pass(gt);
		sum.gs_added += st.gs_added;
		sum.gs_deleted += st.gs_deleted;
		sum.gs_busy += st.gs_busy;
		sum.gs_nospc += st.gs_nospc;
		sum.gs_revived += st.gs_revived;
		sum.gs_add_failed += st.gs_add_failed;
		verify(gt);
	}
	/* With nothing busy and nothing wanted, everything goes. */
	for (d = 0; d < NDEV; d++) {
		for (p = 0; p < NPORT; p++) {
			for (i = 0; i < CAP; i++)
				devs[d].s[p][i].busy = 0;
		}
	}
	for (i = 0; i < NKEY; i++)
		where[i] = -1;
	(void) pass(gt);
	CHECK(gt->gt_n == 0 && !rdk_gidtab_retry_needed(gt));
	for (d = 0; d < NDEV; d++) {
		for (p = 0; p < NPORT; p++) {
			for (i = 0; i < CAP; i++)
				CHECK(!devs[d].s[p][i].used);
		}
	}
	(void) printf("random: %d passes, %u added, %u deleted, %u busy, "
	    "%u no room, %u revived, %u failed\n", rounds, sum.gs_added,
	    sum.gs_deleted, sum.gs_busy, sum.gs_nospc, sum.gs_revived,
	    sum.gs_add_failed);
}

int
main(int argc, char **argv)
{
	static rdk_gidtab_t gt;
	int n = argc > 1 ? atoi(argv[1]) : 20000;

	t_directed(&gt);
	t_random(&gt, n);
	(void) printf("directed: add, remove, no room, busy, revive, move, "
	    "failure, device removal, table full\n");
	return (0);
}

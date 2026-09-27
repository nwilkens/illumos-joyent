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
 * Reconciliation of the wanted RoCEv2 GIDs with the installed ones.
 */

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#endif

#include "rdk_cm_gidtab.h"

int
rdk_gidkey_cmp(const rdk_gidkey_t *a, const rdk_gidkey_t *b)
{
	if (a->gk_stack != b->gk_stack)
		return (a->gk_stack < b->gk_stack ? -1 : 1);
	if (a->gk_ifindex != b->gk_ifindex)
		return (a->gk_ifindex < b->gk_ifindex ? -1 : 1);
	if (a->gk_vlan != b->gk_vlan)
		return (a->gk_vlan < b->gk_vlan ? -1 : 1);
	if (a->gk_type != b->gk_type)
		return (a->gk_type < b->gk_type ? -1 : 1);
	return (memcmp(a->gk_addr, b->gk_addr, sizeof (a->gk_addr)));
}

void
rdk_gidtab_init(rdk_gidtab_t *gt, const rdk_gidtab_ops_t *ops, void *arg)
{
	bzero(gt, sizeof (*gt));
	gt->gt_ops = ops;
	gt->gt_arg = arg;
}

void
rdk_gidtab_begin(rdk_gidtab_t *gt)
{
	uint32_t i;

	for (i = 0; i < RDK_GIDTAB_MAX; i++)
		gt->gt_ents[i].ge_marked = B_FALSE;
	gt->gt_dropped = 0;
}

static boolean_t
rdk_gident_at(const rdk_gident_t *e, void *dev, uint32_t port,
    const uint8_t *mac)
{
	return (e->ge_dev == dev && e->ge_port == port &&
	    memcmp(e->ge_mac, mac, sizeof (e->ge_mac)) == 0);
}

static void
rdk_gident_free(rdk_gidtab_t *gt, rdk_gident_t *e)
{
	bzero(e, sizeof (*e));
	gt->gt_n--;
}

/*
 * Mark a key wanted on the device port.  An installed entry for the key on
 * another port is left unmarked, so the pass deletes it, and a new one is
 * made.
 */
int
rdk_gidtab_want(rdk_gidtab_t *gt, const rdk_gidkey_t *key, void *dev,
    uint32_t port, const uint8_t *mac)
{
	rdk_gident_t *e, *free = NULL;
	uint32_t i;

	for (i = 0; i < RDK_GIDTAB_MAX; i++) {
		e = &gt->gt_ents[i];
		if (e->ge_state == RGS_FREE) {
			if (free == NULL)
				free = e;
			continue;
		}
		if (rdk_gidkey_cmp(&e->ge_key, key) != 0)
			continue;
		if (!rdk_gident_at(e, dev, port, mac)) {
			/* An installed one, unmarked, is deleted at the end. */
			if (e->ge_state == RGS_WANT) {
				rdk_gident_free(gt, e);
				if (free == NULL)
					free = e;
			}
			continue;
		}
		if (e->ge_state == RGS_STALE)
			e->ge_revive = B_TRUE;
		e->ge_marked = B_TRUE;
		return (0);
	}
	if (free == NULL) {
		gt->gt_dropped++;
		return (ENOSPC);
	}
	e = free;
	bzero(e, sizeof (*e));
	e->ge_key = *key;
	e->ge_state = RGS_WANT;
	e->ge_marked = B_TRUE;
	e->ge_dev = dev;
	e->ge_port = port;
	bcopy(mac, e->ge_mac, sizeof (e->ge_mac));
	gt->gt_n++;
	return (0);
}

/* Delete first, so that the slots are free for the additions. */
void
rdk_gidtab_end(rdk_gidtab_t *gt, rdk_gidtab_stats_t *st)
{
	rdk_gident_t *e;
	uint32_t i;
	uint16_t idx;
	int ret;

	bzero(st, sizeof (*st));
	for (i = 0; i < RDK_GIDTAB_MAX; i++) {
		e = &gt->gt_ents[i];
		if (e->ge_revive) {
			e->ge_revive = B_FALSE;
			e->ge_state = RGS_INSTALLED;
			gt->gt_ops->gto_withdraw(gt->gt_arg, e->ge_dev,
			    e->ge_port, e->ge_index, B_FALSE);
			st->gs_revived++;
			continue;
		}
		if (e->ge_state == RGS_INSTALLED && !e->ge_marked) {
			e->ge_state = RGS_STALE;
			gt->gt_ops->gto_withdraw(gt->gt_arg, e->ge_dev,
			    e->ge_port, e->ge_index, B_TRUE);
			st->gs_withdrawn++;
		}
		if (e->ge_state != RGS_STALE)
			continue;
		ret = gt->gt_ops->gto_del(gt->gt_arg, e->ge_dev, e->ge_port,
		    e->ge_index);
		if (ret == 0 || ret == ENOENT || ret == ENXIO) {
			rdk_gident_free(gt, e);
			st->gs_deleted++;
		} else {
			e->ge_err = ret;
			st->gs_busy++;
		}
	}
	for (i = 0; i < RDK_GIDTAB_MAX; i++) {
		e = &gt->gt_ents[i];
		if (e->ge_state != RGS_WANT)
			continue;
		if (!e->ge_marked) {
			rdk_gident_free(gt, e);
			continue;
		}
		ret = gt->gt_ops->gto_add(gt->gt_arg, e->ge_dev, e->ge_port,
		    &e->ge_key, e->ge_mac, &idx);
		if (ret == 0) {
			e->ge_state = RGS_INSTALLED;
			e->ge_index = idx;
			e->ge_err = 0;
			st->gs_added++;
		} else {
			e->ge_err = ret;
			st->gs_add_failed++;
			if (ret == ENOSPC)
				st->gs_nospc++;
		}
	}
}

/* The device is going: its entries are dropped, deleted first if asked. */
void
rdk_gidtab_forget_dev(rdk_gidtab_t *gt, void *dev, boolean_t del)
{
	rdk_gident_t *e;
	uint32_t i;

	for (i = 0; i < RDK_GIDTAB_MAX; i++) {
		e = &gt->gt_ents[i];
		if (e->ge_state == RGS_FREE || e->ge_dev != dev)
			continue;
		if (del && (e->ge_state == RGS_INSTALLED ||
		    e->ge_state == RGS_STALE)) {
			(void) gt->gt_ops->gto_del(gt->gt_arg, e->ge_dev,
			    e->ge_port, e->ge_index);
		}
		rdk_gident_free(gt, e);
	}
}

/* A deletion is pending, or an addition failed for a reason that may pass. */
boolean_t
rdk_gidtab_retry_needed(const rdk_gidtab_t *gt)
{
	const rdk_gident_t *e;
	uint32_t i;

	for (i = 0; i < RDK_GIDTAB_MAX; i++) {
		e = &gt->gt_ents[i];
		if (e->ge_state == RGS_STALE)
			return (B_TRUE);
		if (e->ge_state == RGS_WANT && e->ge_err != 0 &&
		    e->ge_err != ENOTSUP && e->ge_err != EINVAL)
			return (B_TRUE);
	}
	return (B_FALSE);
}

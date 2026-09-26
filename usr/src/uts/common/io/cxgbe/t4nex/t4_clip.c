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
 * The compressed local IPv6 (CLIP) table.  The chip looks up the local
 * address of an IPv6 server or connection here, so the address must be in
 * the table before the CPL that uses it is sent.  Entries are counted; the
 * firmware adds an address on its first use and removes it after its last.
 * Only addresses a client asked for are added.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "common/common.h"
#include "t4_ofld.h"

void
t4_clip_init(t4_ofld_t *of)
{
	mutex_init(&of->of_clip.cl_lock, NULL, MUTEX_DRIVER, NULL);
}

static int
t4_clip_cmd(t4_ofld_t *of, const in6_addr_t *addr, boolean_t add)
{
	struct adapter *sc = of->of_sc;
	struct fw_clip_cmd c;

	bzero(&c, sizeof (c));
	c.op_to_write = BE_32(V_FW_CMD_OP(FW_CLIP_CMD) | F_FW_CMD_REQUEST |
	    (add ? F_FW_CMD_WRITE : F_FW_CMD_READ));
	c.alloc_to_len16 = BE_32((add ? F_FW_CLIP_CMD_ALLOC :
	    F_FW_CLIP_CMD_FREE) | FW_LEN16(c));
	bcopy(&addr->s6_addr[0], &c.ip_hi, sizeof (c.ip_hi));
	bcopy(&addr->s6_addr[8], &c.ip_lo, sizeof (c.ip_lo));
	return (-t4_wr_mbox(sc, sc->mbox, &c, sizeof (c), &c));
}

void
t4_clip_fini(t4_ofld_t *of)
{
	t4_clip_t *cl = &of->of_clip;

	if (!of->of_ready)
		return;
	mutex_enter(&cl->cl_lock);
	for (uint_t i = 0; i < cl->cl_n; i++) {
		if (cl->cl_ent[i].ce_refs != 0)
			(void) t4_clip_cmd(of, &cl->cl_ent[i].ce_addr, B_FALSE);
	}
	cl->cl_n = 0;
	mutex_exit(&cl->cl_lock);
	mutex_destroy(&cl->cl_lock);
}

static int
t4_clip_find(t4_clip_t *cl, const in6_addr_t *addr)
{
	for (uint_t i = 0; i < cl->cl_n; i++) {
		if (cl->cl_ent[i].ce_refs != 0 &&
		    IN6_ARE_ADDR_EQUAL(&cl->cl_ent[i].ce_addr, addr))
			return ((int)i);
	}
	return (-1);
}

/* Only a global or unique local unicast address may go in the table. */
static boolean_t
t4_clip_addr_ok(const in6_addr_t *addr)
{
	return (!IN6_IS_ADDR_UNSPECIFIED(addr) &&
	    !IN6_IS_ADDR_LOOPBACK(addr) && !IN6_IS_ADDR_MULTICAST(addr) &&
	    !IN6_IS_ADDR_V4MAPPED(addr) && !IN6_IS_ADDR_LINKLOCAL(addr));
}

int
t4_clip_get(t4_ofld_t *of, const in6_addr_t *addr)
{
	t4_clip_t *cl = &of->of_clip;
	int i, rc = 0;

	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if (addr == NULL || !t4_clip_addr_ok(addr))
		return (EINVAL);

	mutex_enter(&cl->cl_lock);
	if ((i = t4_clip_find(cl, addr)) >= 0) {
		if (cl->cl_ent[i].ce_refs == UINT32_MAX)
			rc = EOVERFLOW;
		else
			cl->cl_ent[i].ce_refs++;
		mutex_exit(&cl->cl_lock);
		return (rc);
	}
	for (i = 0; i < (int)cl->cl_n; i++) {
		if (cl->cl_ent[i].ce_refs == 0)
			break;
	}
	if (i == T4_OFLD_MAX_CLIP) {
		mutex_exit(&cl->cl_lock);
		return (ENOSPC);
	}
	if ((rc = t4_clip_cmd(of, addr, B_TRUE)) == 0) {
		cl->cl_ent[i].ce_addr = *addr;
		cl->cl_ent[i].ce_refs = 1;
		if ((uint_t)i == cl->cl_n)
			cl->cl_n++;
	}
	mutex_exit(&cl->cl_lock);
	return (rc);
}

void
t4_clip_put(t4_ofld_t *of, const in6_addr_t *addr)
{
	t4_clip_t *cl = &of->of_clip;
	int i;

	if (addr == NULL || servicing_interrupt())
		return;
	mutex_enter(&cl->cl_lock);
	if ((i = t4_clip_find(cl, addr)) >= 0 && --cl->cl_ent[i].ce_refs == 0)
		(void) t4_clip_cmd(of, addr, B_FALSE);
	mutex_exit(&cl->cl_lock);
}

boolean_t
t4_clip_held(t4_ofld_t *of, const in6_addr_t *addr)
{
	t4_clip_t *cl = &of->of_clip;
	boolean_t held;

	mutex_enter(&cl->cl_lock);
	held = t4_clip_find(cl, addr) >= 0;
	mutex_exit(&cl->cl_lock);
	return (held);
}

/* Drop every reference, for a client that left. */
void
t4_clip_reset(t4_ofld_t *of)
{
	t4_clip_t *cl = &of->of_clip;

	mutex_enter(&cl->cl_lock);
	for (uint_t i = 0; i < cl->cl_n; i++) {
		if (cl->cl_ent[i].ce_refs != 0) {
			(void) t4_clip_cmd(of, &cl->cl_ent[i].ce_addr, B_FALSE);
			cl->cl_ent[i].ce_refs = 0;
		}
	}
	cl->cl_n = 0;
	mutex_exit(&cl->cl_lock);
}

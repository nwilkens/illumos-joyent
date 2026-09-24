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
 * Run rdk_modify_qp_is_ok() and rdk_sg_to_pages() from rdk_verbs.c.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int boolean_t;
typedef unsigned int uint_t;
#define	B_TRUE	1
#define	B_FALSE	0

typedef struct {
	uint64_t	dmac_laddress;
	uint64_t	dmac_size;
} ddi_dma_cookie_t;

struct rdk_mr {
	uint64_t	iova;
	uint64_t	length;
	uint32_t	page_size;
};

#include "rdk_bodies.h"

#define	PG	4096ULL
#define	MAXP	64

static uint64_t pages[MAXP];
static int npages, page_limit;

static int
set_page(struct rdk_mr *mr, uint64_t addr)
{
	(void) mr;
	if (npages >= page_limit)
		return (-ENOMEM);
	pages[npages++] = addr;
	return (0);
}

static int
walk(const ddi_dma_cookie_t *c, uint_t n, uint64_t *off, int limit,
    struct rdk_mr *mr)
{
	memset(mr, 0, sizeof (*mr));
	mr->page_size = PG;
	npages = 0;
	page_limit = limit;
	return (rdk_sg_to_pages(mr, c, n, off, set_page));
}

static void
state_table(void)
{
	int rc_init = RDK_QP_STATE | RDK_QP_PKEY_INDEX | RDK_QP_PORT |
	    RDK_QP_ACCESS_FLAGS;
	int rc_rtr = RDK_QP_STATE | RDK_QP_AV | RDK_QP_PATH_MTU |
	    RDK_QP_DEST_QPN | RDK_QP_RQ_PSN | RDK_QP_MAX_DEST_RD_ATOMIC |
	    RDK_QP_MIN_RNR_TIMER;
	int rc_rts = RDK_QP_STATE | RDK_QP_TIMEOUT | RDK_QP_RETRY_CNT |
	    RDK_QP_RNR_RETRY | RDK_QP_SQ_PSN | RDK_QP_MAX_QP_RD_ATOMIC;

	assert(rdk_modify_qp_is_ok(RDK_QPS_RESET, RDK_QPS_INIT, RDK_QPT_RC,
	    rc_init));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_RESET, RDK_QPS_INIT, RDK_QPT_RC,
	    rc_init & ~RDK_QP_PORT));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_RESET, RDK_QPS_INIT, RDK_QPT_RC,
	    rc_init | RDK_QP_AV));
	assert(rdk_modify_qp_is_ok(RDK_QPS_INIT, RDK_QPS_RTR, RDK_QPT_RC,
	    rc_rtr));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_INIT, RDK_QPS_RTR, RDK_QPT_RC,
	    rc_rtr & ~RDK_QP_AV));
	assert(rdk_modify_qp_is_ok(RDK_QPS_RTR, RDK_QPS_RTS, RDK_QPT_RC,
	    rc_rts));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_INIT, RDK_QPS_RTS, RDK_QPT_RC,
	    rc_rts));
	assert(rdk_modify_qp_is_ok(RDK_QPS_RESET, RDK_QPS_INIT, RDK_QPT_UD,
	    RDK_QP_STATE | RDK_QP_PKEY_INDEX | RDK_QP_PORT | RDK_QP_QKEY));
	assert(rdk_modify_qp_is_ok(RDK_QPS_INIT, RDK_QPS_RTR, RDK_QPT_UD,
	    RDK_QP_STATE));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_INIT, RDK_QPS_RTR, RDK_QPT_UD,
	    RDK_QP_STATE | RDK_QP_AV));
	assert(rdk_modify_qp_is_ok(RDK_QPS_RTS, RDK_QPS_ERR, RDK_QPT_RC,
	    RDK_QP_STATE));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_ERR, RDK_QPS_RTS, RDK_QPT_RC,
	    RDK_QP_STATE));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_INIT, RDK_QPS_INIT, RDK_QPT_RC,
	    RDK_QP_STATE | RDK_QP_CUR_STATE));
	/* Values from a caller that the table does not index. */
	assert(!rdk_modify_qp_is_ok((enum rdk_qp_state)7, RDK_QPS_ERR,
	    RDK_QPT_RC, RDK_QP_STATE));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_RESET, (enum rdk_qp_state)-1,
	    RDK_QPT_RC, RDK_QP_STATE));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_RESET, RDK_QPS_INIT,
	    (enum rdk_qp_type)RDK_QPT_MAX, RDK_QP_STATE));
	assert(!rdk_modify_qp_is_ok(RDK_QPS_RESET, RDK_QPS_INIT,
	    (enum rdk_qp_type)-2, RDK_QP_STATE));
}

static void
page_walk(void)
{
	struct rdk_mr mr;
	ddi_dma_cookie_t c[4];
	uint64_t off;
	int n;

	/* Three page-aligned cookies, the middle one not adjacent. */
	c[0].dmac_laddress = 0x100000; c[0].dmac_size = 2 * PG;
	c[1].dmac_laddress = 0x300000; c[1].dmac_size = PG;
	c[2].dmac_laddress = 0x500000; c[2].dmac_size = 3 * PG;
	n = walk(c, 3, NULL, MAXP, &mr);
	assert(n == 3 && npages == 6 && mr.length == 6 * PG);
	assert(mr.iova == 0x100000 && pages[2] == 0x300000);

	/* An offset into the first cookie. */
	off = 100;
	n = walk(c, 3, &off, MAXP, &mr);
	assert(n == 3 && off == 0 && mr.iova == 0x100000 + 100);
	assert(mr.length == 6 * PG - 100);

	/* An offset at or past the first cookie is refused. */
	off = 2 * PG;
	assert(walk(c, 3, &off, MAXP, &mr) == -EINVAL);
	assert(walk(c, 0, NULL, MAXP, &mr) == -EINVAL);

	/* A cookie that ends inside a page stops at the next gap. */
	c[0].dmac_size = PG + 10;
	n = walk(c, 3, NULL, MAXP, &mr);
	assert(n == 1 && npages == 2 && mr.length == PG + 10);
	c[0].dmac_size = 2 * PG;

	/*
	 * A full page list maps a prefix; the length covers only the
	 * cookies mapped whole, and mapping resumes at the third.
	 */
	off = 0;
	n = walk(c, 3, &off, 4, &mr);
	assert(n == 2 && npages == 4 && off == 0 && mr.length == 3 * PG);
	off = 0;
	n = walk(c, 3, &off, 5, &mr);
	assert(n == 2 && npages == 5 && off == PG && mr.length == 4 * PG);

	/* A cookie whose end wraps the address space is not mapped. */
	c[1].dmac_laddress = UINT64_MAX - PG + 1;
	c[1].dmac_size = 2 * PG;
	n = walk(c, 3, NULL, MAXP, &mr);
	assert(n == 1 && npages == 2);
}

int
main(void)
{
	state_table();
	page_walk();
	printf("rdk_verbs: state table and page walk hold\n");
	return (0);
}

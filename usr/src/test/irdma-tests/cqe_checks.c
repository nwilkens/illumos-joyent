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
 * Run irdma_uk_cq_poll_cmpl() and irdma_uk_clean_cq() from core/uk.c, as
 * the driver builds them, against completions a hostile device writes: a
 * forged QP pointer, an SRQ entry, a WQE index outside the posted work, a
 * flush over a queue of NOPs and a CQ whose every entry stays valid.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "uk.c"

#define	SQ_SIZE	64
#define	RQ_SIZE	16
#define	CQ_SIZE	8
#define	QPN	7

static struct irdma_uk_attrs attrs;
static struct irdma_qp_uk qp;
static struct irdma_qp_quanta sq[SQ_SIZE], rq[RQ_SIZE];
static struct irdma_sq_uk_wr_trk_info wrtrk[SQ_SIZE];
static u64 rqwrid[RQ_SIZE];
static __le64 shadow[8];
static struct irdma_cq_uk cq;
static struct irdma_cqe cqes[CQ_SIZE];
static __le64 cqshadow[8];
static int lookups;

struct ib_device *
to_ibdev(struct irdma_sc_dev *dev)
{
	(void) dev;
	return (NULL);
}

/* The driver's check: this pointer at this QP number, and nothing else. */
struct irdma_qp_uk *
irdma_osdep_cqe_qp(struct irdma_cq_uk *c, u64 ctx, u32 qpn)
{
	assert(c == &cq);
	lookups++;
	if (ctx == (uintptr_t)&qp && qpn == QPN)
		return (&qp);
	return (NULL);
}

static void
reset(void)
{
	memset(&qp, 0, sizeof (qp));
	memset(wrtrk, 0, sizeof (wrtrk));
	memset(sq, 0, sizeof (sq));
	memset(cqes, 0, sizeof (cqes));
	attrs.hw_rev = IRDMA_GEN_2;
	qp.uk_attrs = &attrs;
	qp.sq_base = sq;
	qp.rq_base = rq;
	qp.sq_wrtrk_array = wrtrk;
	qp.rq_wrid_array = rqwrid;
	qp.shadow_area = shadow;
	qp.qp_id = QPN;
	qp.qp_type = IRDMA_QP_TYPE_ROCE_RC;
	qp.rq_wqe_size_multiplier = 1;
	qp.rq_size = RQ_SIZE;
	IRDMA_RING_INIT(qp.sq_ring, SQ_SIZE);
	IRDMA_RING_INIT(qp.rq_ring, RQ_SIZE);
	cq.cq_base = cqes;
	cq.shadow_area = cqshadow;
	cq.cq_size = CQ_SIZE;
	IRDMA_RING_INIT(cq.cq_ring, CQ_SIZE);
	cq.polarity = 1;
	lookups = 0;
}

/* Write a valid CQE at the CQ head. */
static void
cqe(u64 ctx, u32 qpn, u32 wqe_idx, int sq_side, int srq, u64 err)
{
	__le64 *e = cqes[cq.cq_ring.head].buf;

	e[1] = ctx;
	e[2] = FIELD_PREP(IRDMACQ_QPID, qpn);
	e[0] = 100;
	e[3] = FIELD_PREP(IRDMA_CQ_WQEIDX, wqe_idx) |
	    (sq_side ? IRDMA_CQ_SQ : 0) | (srq ? IRDMA_CQ_SRQ : 0) | err |
	    FIELD_PREP(IRDMA_CQ_VALID, cq.polarity);
}

static int
poll(struct irdma_cq_poll_info *info)
{
	memset(info, 0, sizeof (*info));
	return (irdma_uk_cq_poll_cmpl(&cq, info));
}

int
main(void)
{
	struct irdma_cq_poll_info info;
	u32 head;
	int i;

	/* A forged pointer, or the right pointer at the wrong number. */
	reset();
	cqe(0xdeadbeef000ULL, QPN, 0, 1, 0, 0);
	assert(poll(&info) == -EFAULT && cq.cq_ring.head == 1);
	cqe((uintptr_t)&qp, QPN + 1, 0, 1, 0, 0);
	assert(poll(&info) == -EFAULT && cq.cq_ring.head == 2);
	/* The error path read the QP type before the check upstream. */
	cqe(0x1000, QPN, 0, 1, 0, IRDMA_CQ_ERROR |
	    FIELD_PREP(IRDMA_CQ_MAJERR, 0xe000));
	assert(poll(&info) == -EFAULT && lookups == 3);
	/* An SRQ entry: E810 has none, and it named a pointer at +40. */
	cqe((uintptr_t)&qp, QPN, 0, 0, 1, 0);
	assert(poll(&info) == -EFAULT);

	/* Sends: only an index between tail and head completes. */
	reset();
	qp.sq_ring.head = 4;
	for (i = 0; i < 4; i++) {
		wrtrk[i].wrid = 1000 + i;
		wrtrk[i].quanta = 1;
	}
	cqe((uintptr_t)&qp, QPN, 30, 1, 0, 0);
	assert(poll(&info) == -EFAULT && qp.sq_ring.tail == 0);
	cqe((uintptr_t)&qp, QPN, 0x7fff, 1, 0, 0);
	assert(poll(&info) == -EFAULT && qp.sq_ring.tail == 0);
	cqe((uintptr_t)&qp, QPN, 2, 1, 0, 0);
	assert(poll(&info) == 0 && info.wr_id == 1002 && qp.sq_ring.tail == 3);
	/* Index 1 is behind the tail now: already complete. */
	cqe((uintptr_t)&qp, QPN, 1, 1, 0, 0);
	assert(poll(&info) == -EFAULT && qp.sq_ring.tail == 3);
	/* A zero quanta count still moves the tail. */
	wrtrk[3].quanta = 0;
	cqe((uintptr_t)&qp, QPN, 3, 1, 0, 0);
	assert(poll(&info) == 0 && qp.sq_ring.tail == 4);

	/* Receives: the same window on the RQ. */
	reset();
	qp.rq_ring.head = 2;
	rqwrid[0] = 5;
	rqwrid[1] = 6;
	cqe((uintptr_t)&qp, QPN, 9, 0, 0, 0);
	assert(poll(&info) == -EFAULT && qp.rq_ring.tail == 0);
	cqe((uintptr_t)&qp, QPN, 0, 0, 0, 0);
	assert(poll(&info) == 0 && info.wr_id == 5 && qp.rq_ring.tail == 1);

	/* A flushed send over NOPs ends when the posted work does. */
	reset();
	qp.sq_ring.head = 3;
	for (i = 0; i < 3; i++) {
		sq[i].elem[3] = FIELD_PREP(IRDMAQPSQ_OPCODE, IRDMAQP_OP_NOP);
		wrtrk[i].quanta = 0;
	}
	cqe((uintptr_t)&qp, QPN, 0, 1, 0, IRDMA_CQ_ERROR |
	    FIELD_PREP(IRDMA_CQ_MAJERR, IRDMA_FLUSH_MAJOR_ERR) |
	    FIELD_PREP(IRDMA_CQ_MINERR, FLUSH_GENERAL_ERR));
	assert(poll(&info) == -ENOENT && qp.sq_ring.tail == 3);

	/* Cleaning a CQ whose entries all stay valid stops after one pass. */
	reset();
	for (i = 0; i < CQ_SIZE; i++) {
		cqes[i].buf[1] = (uintptr_t)&qp;
		cqes[i].buf[3] = IRDMA_CQ_VALID;
	}
	head = cq.cq_ring.head;
	cq.polarity = 1;
	irdma_uk_clean_cq(&qp, &cq);
	for (i = 0; i < CQ_SIZE; i++)
		assert(cqes[i].buf[1] == 0);
	assert(cq.cq_ring.head == head);

	printf("cqe_checks: forged CQEs are dropped and every walk ends\n");
	return (0);
}

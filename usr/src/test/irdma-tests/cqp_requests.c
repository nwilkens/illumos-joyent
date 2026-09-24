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
 * Run the CQP request matching from irdma_ctl.c.  A completion names its
 * request only through the scratch value, which the device echoes through a
 * WQE index; a stale, forged or reused value must never complete a request
 * that did not issue it.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef unsigned long long u64;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint8_t u8;
typedef unsigned int uint_t;
typedef int kmutex_t;
typedef int kcondvar_t;

#define	ASSERT(x)		assert(x)
#define	MUTEX_HELD(m)		(*(m) != 0)
#define	IRDMA_CQP_NREQS		64

typedef enum irdma_req_state {
	IRDMA_REQ_FREE = 0, IRDMA_REQ_BUSY, IRDMA_REQ_DONE,
	IRDMA_REQ_ABANDONED
} irdma_req_state_t;

struct irdma_ccq_cqe_info {
	void *cqp;
	u64 scratch;
	u32 op_ret_val;
	u16 maj_err_code;
	u16 min_err_code;
	u8 op_code;
	int error;
	int pending;
};

typedef struct irdma_cqp_req {
	irdma_req_state_t	icr_state;
	uint16_t		icr_gen;
	kcondvar_t		icr_cv;
	struct irdma_ccq_cqe_info icr_cqe;
} irdma_cqp_req_t;

typedef struct irdma {
	kmutex_t		irdma_req_lock;
	kcondvar_t		irdma_req_cv;
	irdma_cqp_req_t		irdma_reqs[IRDMA_CQP_NREQS];
	uint64_t		irdma_cqp_completed;
	uint64_t		irdma_cqp_errors;
	uint64_t		irdma_bad_entries;
} irdma_t;

static int wakes, frees;

static void
cv_broadcast(kcondvar_t *cv)
{
	if (cv != NULL)
		wakes++;
}

#include "req_bodies.h"

static irdma_t ir;

static irdma_cqp_req_t *
take(uint_t i)
{
	irdma_cqp_req_t *r = &ir.irdma_reqs[i];

	r->icr_state = IRDMA_REQ_BUSY;
	r->icr_gen++;
	return (r);
}

static void
complete(u64 scratch, int error)
{
	struct irdma_ccq_cqe_info info;

	memset(&info, 0, sizeof (info));
	info.scratch = scratch;
	info.error = error;
	info.op_ret_val = 0x1234;
	ir.irdma_req_lock = 1;
	irdma_req_complete(&ir, &info);
	ir.irdma_req_lock = 0;
}

int
main(void)
{
	irdma_cqp_req_t *a, *b;
	u64 sa, sb, stale;

	memset(&ir, 0, sizeof (ir));
	a = take(3);
	sa = irdma_req_scratch(&ir, a);
	assert((sa & UINT32_MAX) == 4 && (sa >> 32) == a->icr_gen);

	/* A matching completion finishes the request with its result. */
	complete(sa, 0);
	assert(a->icr_state == IRDMA_REQ_DONE);
	assert(a->icr_cqe.op_ret_val == 0x1234);
	assert(ir.irdma_cqp_completed == 1 && ir.irdma_bad_entries == 0);

	/* A second completion for the same slot is not accepted. */
	complete(sa, 0);
	assert(ir.irdma_bad_entries == 1);

	/* A reused slot does not take a completion of its previous use. */
	stale = sa;
	a->icr_state = IRDMA_REQ_FREE;
	a = take(3);
	complete(stale, 0);
	assert(a->icr_state == IRDMA_REQ_BUSY && ir.irdma_bad_entries == 2);

	/* Forged indexes: none, past the table, and a free slot. */
	complete(0x100000000ULL, 0);
	complete(((u64)1 << 32) | (IRDMA_CQP_NREQS + 1), 0);
	complete(((u64)1 << 32) | 0xffffffffULL, 0);
	complete(((u64)ir.irdma_reqs[10].icr_gen << 32) | 11, 0);
	assert(ir.irdma_bad_entries == 6);

	/* Commands the core polls for carry no request and are only counted. */
	complete(0, 0);
	assert(ir.irdma_bad_entries == 6 && ir.irdma_cqp_completed == 8);

	/* An abandoned request is freed by its late completion, not woken. */
	b = take(7);
	sb = irdma_req_scratch(&ir, b);
	b->icr_state = IRDMA_REQ_ABANDONED;
	complete(sb, 1);
	assert(b->icr_state == IRDMA_REQ_FREE && ir.irdma_cqp_errors == 1);

	/* An error completion reaches its waiter with the error set. */
	sa = irdma_req_scratch(&ir, a);
	complete(sa, 1);
	assert(a->icr_state == IRDMA_REQ_DONE && a->icr_cqe.error);
	assert(ir.irdma_cqp_errors == 2);
	(void) frees;

	(void) printf("PASS: CQP completions match only their live request\n");
	return (0);
}

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
 * RDMAT_IOC_BUF: fill, check or clear part of a QP's buffer with a seeded
 * pattern, contiguous or in the cookie layout of RDMAT_OP_RW.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include "rdmat_impl.h"

static uint8_t
rdmat_pattern(uint64_t seed, uint64_t off)
{
	uint64_t x = seed ^ ((off >> 3) * 0x9e3779b97f4a7c15ULL);

	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return ((uint8_t)(x >> ((off & 7) * 8)));
}

int
rdmat_buf(rdmat_sess_t *ts, rdmat_buf_t *rb)
{
	rdmat_qp_t *tq;
	hrtime_t t0 = gethrtime();
	uint64_t i, off, span;
	uint8_t *p;

	if (rb->rb_op != RDMAT_BUF_FILL && rb->rb_op != RDMAT_BUF_VERIFY &&
	    rb->rb_op != RDMAT_BUF_ZERO && rb->rb_op != RDMAT_BUF_VERIFY_ZERO)
		return (EINVAL);
	if ((tq = rdmat_qp(ts, rb->rb_qp)) == NULL ||
	    rb->rb_offset > tq->tq_len || rb->rb_len > tq->tq_len ||
	    (rb->rb_frag != 0 && rb->rb_stride < rb->rb_frag))
		return (EINVAL);
	span = rb->rb_len;
	if (rb->rb_frag != 0 && rb->rb_len != 0) {
		span = ((rb->rb_len - 1) / rb->rb_frag) * rb->rb_stride +
		    (rb->rb_len - 1) % rb->rb_frag + 1;
	}
	if (span > tq->tq_len - rb->rb_offset)
		return (EINVAL);
	rb->rb_mismatch = -1;
	for (i = 0; i < rb->rb_len; i++) {
		off = rb->rb_offset + (rb->rb_frag == 0 ? i :
		    (i / rb->rb_frag) * rb->rb_stride + i % rb->rb_frag);
		p = (uint8_t *)tq->tq_chunks[off / RDMAT_CHUNK].rdb_va +
		    (off % RDMAT_CHUNK);
		switch (rb->rb_op) {
		case RDMAT_BUF_FILL:
			*p = rdmat_pattern(rb->rb_seed,
			    rb->rb_pattern_base + i);
			break;
		case RDMAT_BUF_ZERO:
			*p = 0;
			break;
		case RDMAT_BUF_VERIFY_ZERO:
			if (*p != 0) {
				rb->rb_mismatch = (int64_t)off;
				rb->rb_ns = (uint64_t)(gethrtime() - t0);
				return (0);
			}
			break;
		case RDMAT_BUF_VERIFY:
			if (*p != rdmat_pattern(rb->rb_seed,
			    rb->rb_pattern_base + i)) {
				rb->rb_mismatch = (int64_t)off;
				rb->rb_ns = (uint64_t)(gethrtime() - t0);
				return (0);
			}
			break;
		default:
			return (EINVAL);
		}
	}
	rb->rb_ns = (uint64_t)(gethrtime() - t0);
	return (0);
}

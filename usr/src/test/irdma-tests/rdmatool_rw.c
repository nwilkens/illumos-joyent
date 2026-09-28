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
 * rdmatool tests of rdk_rw (RDMAT_OP_RW), side A moving data to or from
 * side B's buffer as a storage target would:
 *
 *	rwshapes	WRITE and READ over local cookie layouts (one cookie,
 *			cookies crossing chunks, gapped and unaligned
 *			cookies) and remote segment splits, with the local
 *			DMA lkey and with FRWR MRs; every byte and every gap
 *			is checked
 *	rwsend		a WRITE with its response SEND chained in the same
 *			post, the SEND invalidating B's rkey
 *	rwstream	a stream of FRWR READs for -t seconds, depth 16
 */

#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "rdmatool.h"

#define	RW_WC_WITH_INVALIDATE	(1 << 2)

typedef struct rw_shape {
	uint32_t	rs_len;
	uint64_t	rs_off;		/* in A's buffer */
	uint32_t	rs_frag;
	uint32_t	rs_stride;
	uint32_t	rs_nsegs;
	uint32_t	rs_mr_pages;
} rw_shape_t;

static const rw_shape_t shapes[] = {
	{ 1, 0, 0, 0, 1, 2 },
	{ 4095, 100, 0, 0, 2, 2 },
	{ 65537, 4095, 0, 0, 3, 4 },
	{ 1 << 20, 0, 0, 0, 1, 257 },
	{ 256 << 10, (1 << 20) - 3000, 0, 0, 4, 16 },
	{ 65536, 0, 4096, 8192, 1, 4 },
	{ 50000, 7, 3000, 5000, 4, 4 },
	{ 100000, 0, 1000, 1500, 2, 64 },
	{ 1 << 20, 4096, 65536, 69632, 2, 17 }
};

/* The bytes a cookie layout spans from its first byte. */
static uint64_t
rw_span(const rw_shape_t *s)
{
	if (s->rs_frag == 0)
		return (s->rs_len);
	return ((uint64_t)((s->rs_len - 1) / s->rs_frag) * s->rs_stride +
	    (s->rs_len - 1) % s->rs_frag + 1);
}

static int
rw_buf(peer_t *p, uint32_t op, uint64_t off, uint64_t len, uint32_t frag,
    uint32_t stride, uint64_t seed, int64_t *bad)
{
	rdmat_buf_t rb;
	int ret;

	bzero(&rb, sizeof (rb));
	rb.rb_op = op;
	rb.rb_offset = off;
	rb.rb_len = len;
	rb.rb_frag = frag;
	rb.rb_stride = stride;
	rb.rb_seed = seed;
	ret = pio(p, RDMAT_IOC_BUF, &rb);
	if (bad != NULL)
		*bad = ret == 0 ? rb.rb_mismatch : -2;
	return (ret);
}

static void
rw_run_init(rdmat_run_t *rr, peer_t *b, const rw_shape_t *s, uint32_t flags)
{
	run_init(rr, RDMAT_OP_RW, s->rs_len, 1);
	rr->rr_offset = s->rs_off;
	rr->rr_frag = s->rs_frag;
	rr->rr_stride = s->rs_stride;
	rr->rr_nsegs = s->rs_nsegs;
	rr->rr_mr_pages = s->rs_mr_pages < local_dev.rdi_max_mr_pages ?
	    s->rs_mr_pages : local_dev.rdi_max_mr_pages;
	rr->rr_rw_flags = flags;
	rr->rr_raddr = b->p_setup.rs_qp[0].rqi_addr;
	rr->rr_rkey = b->p_setup.rs_qp[0].rqi_rkey;
	rr->rr_rlen = s->rs_len;
}

static int
rw_fresh(peer_t *a, peer_t *b)
{
	fresh_max_sge = local_dev.rdi_max_sge < 4 ? local_dev.rdi_max_sge : 4;
	fresh_sq_depth = local_dev.rdi_max_qp_wr < 4096 ?
	    local_dev.rdi_max_qp_wr : 4096;
	return (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ));
}

static void
rw_fresh_done(void)
{
	fresh_max_sge = 0;
	fresh_sq_depth = 0;
}

/* One shape one way; returns 0 or prints why not. */
static int
rw_one(peer_t *a, peer_t *b, const rw_shape_t *s, uint32_t flags,
    uint32_t *wrs, uint32_t *mrs)
{
	int read = (flags & RDMAT_RW_READ) != 0;
	uint64_t seed = seed_of("rwshape", s->rs_len), span = rw_span(s);
	rdmat_run_t rr;
	int64_t bad;
	int ret;

	if (s->rs_off + span + 4096 > (o_buf_mb << 20))
		return (0);
	if (read) {
		(void) rw_buf(b, RDMAT_BUF_FILL, 0, s->rs_len, 0, 0, seed,
		    NULL);
		(void) rw_buf(a, RDMAT_BUF_ZERO, s->rs_off, span + 4096, 0, 0,
		    0, NULL);
	} else {
		(void) rw_buf(a, RDMAT_BUF_FILL, s->rs_off, s->rs_len,
		    s->rs_frag, s->rs_stride, seed, NULL);
		(void) rw_buf(b, RDMAT_BUF_ZERO, 0, s->rs_len + 4096, 0, 0, 0,
		    NULL);
	}
	rw_run_init(&rr, b, s, flags);
	if ((ret = run(a, &rr)) != 0) {
		(void) printf("  %s %u frag %u: %s status %u opcode %u\n",
		    read ? "read" : "write", s->rs_len, s->rs_frag,
		    strerror(ret), rr.rr_status, rr.rr_err_opcode);
		return (-1);
	}
	if (rr.rr_rw_wrs > rr.rr_rw_limit_wrs ||
	    rr.rr_rw_mrs > rr.rr_rw_limit_mrs) {
		(void) printf("  %u requests and %u MRs over the limits %u "
		    "and %u\n", rr.rr_rw_wrs, rr.rr_rw_mrs,
		    rr.rr_rw_limit_wrs, rr.rr_rw_limit_mrs);
		return (-1);
	}
	*wrs = rr.rr_rw_wrs;
	*mrs = rr.rr_rw_mrs;
	if (read) {
		(void) rw_buf(a, RDMAT_BUF_VERIFY, s->rs_off, s->rs_len,
		    s->rs_frag, s->rs_stride, seed, &bad);
		if (bad == -1 && s->rs_frag != 0 && s->rs_len > s->rs_frag) {
			/* The gaps between the cookies stay zero. */
			(void) rw_buf(a, RDMAT_BUF_VERIFY_ZERO,
			    s->rs_off + s->rs_frag,
			    (uint64_t)((s->rs_len - 1) / s->rs_frag) *
			    (s->rs_stride - s->rs_frag),
			    s->rs_stride - s->rs_frag, s->rs_stride, 0, &bad);
		}
		if (bad == -1) {
			(void) rw_buf(a, RDMAT_BUF_VERIFY_ZERO,
			    s->rs_off + span, 4096, 0, 0, 0, &bad);
		}
	} else {
		(void) rw_buf(b, RDMAT_BUF_VERIFY, 0, s->rs_len, 0, 0, seed,
		    &bad);
		if (bad == -1) {
			(void) rw_buf(b, RDMAT_BUF_VERIFY_ZERO, s->rs_len, 4096,
			    0, 0, 0, &bad);
		}
	}
	if (bad != -1) {
		(void) printf("  %s %u frag %u stride %u segs %u: differs at "
		    "%lld\n", read ? "read" : "write", s->rs_len, s->rs_frag,
		    s->rs_stride, s->rs_nsegs, (long long)bad);
		return (-1);
	}
	return (0);
}

static void
t_rwshapes(peer_t *a, peer_t *b)
{
	static const uint32_t modes[] = { 0, RDMAT_RW_READ,
	    RDMAT_RW_READ | RDMAT_RW_MR };
	uint32_t wrs, mrs, max_wrs = 0, max_mrs = 0;
	uint_t i, m, n = 0;

	if (rw_fresh(a, b) != 0) {
		rw_fresh_done();
		result(0, "rwshapes", "setup failed");
		return;
	}
	rw_fresh_done();
	for (m = 0; m < sizeof (modes) / sizeof (modes[0]); m++) {
		for (i = 0; i < sizeof (shapes) / sizeof (shapes[0]); i++) {
			wrs = mrs = 0;
			if (rw_one(a, b, &shapes[i], modes[m], &wrs,
			    &mrs) != 0) {
				result(0, "rwshapes", "shape %u mode %u", i,
				    modes[m]);
				return;
			}
			max_wrs = wrs > max_wrs ? wrs : max_wrs;
			max_mrs = mrs > max_mrs ? mrs : max_mrs;
			n++;
		}
	}
	result(1, "rwshapes", "%u transfers (WRITE, READ, FRWR READ) over %u "
	    "cookie layouts, every byte and gap checked; up to %u requests "
	    "and %u MRs a transfer", n, (uint_t)(sizeof (shapes) /
	    sizeof (shapes[0])), max_wrs, max_mrs);
}

static void
t_rwsend(peer_t *a, peer_t *b)
{
	const rw_shape_t s = { 3 * 4096 + 5, 0, 0, 0, 2, 4 };
	uint64_t seed = seed_of("rwsend", 1);
	uint32_t rkey;
	rdmat_run_t rr, rw;
	int64_t bad;
	int ret;

	if (rw_fresh(a, b) != 0) {
		rw_fresh_done();
		result(0, "rwsend", "setup failed");
		return;
	}
	rw_fresh_done();
	rkey = b->p_setup.rs_qp[0].rqi_rkey;
	(void) rw_buf(a, RDMAT_BUF_FILL, 0, s.rs_len, 0, 0, seed, NULL);
	(void) rw_buf(b, RDMAT_BUF_ZERO, 0, s.rs_len, 0, 0, 0, NULL);
	/* B receives into the end of its buffer, past the WRITE. */
	run_init(&rr, RDMAT_OP_POST_RECV, RDMAT_RW_SEND_LEN, 1);
	rr.rr_offset = (o_buf_mb << 20) - RDMAT_RW_SEND_LEN * o_depth;
	rr.rr_flags = RDMAT_F_DMA_LKEY;
	if ((ret = run(b, &rr)) != 0) {
		result(0, "rwsend", "post recv: %s", strerror(ret));
		return;
	}
	run_init(&rw, RDMAT_OP_WAIT_RECV, RDMAT_RW_SEND_LEN, 1);
	run_start(b, &rw);
	rw_run_init(&rr, b, &s, RDMAT_RW_SEND | RDMAT_RW_SEND_INV);
	ret = run(a, &rr);
	if (run_finish(b, &rw) != 0 || ret != 0 || rw.rr_done != 1) {
		result(0, "rwsend", "write %s status %u, recv done %llu "
		    "status %u", strerror(ret), rr.rr_status,
		    (u_longlong_t)rw.rr_done, rw.rr_status);
		return;
	}
	(void) rw_buf(b, RDMAT_BUF_VERIFY, 0, s.rs_len, 0, 0, seed, &bad);
	if (bad != -1 || rr.rr_rw_send_inv != 1 ||
	    (rw.rr_wc_flags & RW_WC_WITH_INVALIDATE) == 0 ||
	    rw.rr_inv_rkey != rkey) {
		result(0, "rwsend", "data at %lld, send_inv %u, wc_flags 0x%x, "
		    "invalidated 0x%x (rkey 0x%x)", (long long)bad,
		    rr.rr_rw_send_inv, rw.rr_wc_flags, rw.rr_inv_rkey, rkey);
		return;
	}
	/* B's rkey is gone: a WRITE with it must fail. */
	rw_run_init(&rr, b, &s, 0);
	rr.rr_timeout_ms = 5000;
	ret = run(a, &rr);
	if (ret == 0) {
		result(0, "rwsend", "a WRITE with the invalidated rkey 0x%x "
		    "succeeded", rkey);
		return;
	}
	result(1, "rwsend", "WRITE of %u B and its SEND in one post, the data "
	    "there before the SEND, SEND_WITH_INV invalidated B's rkey 0x%x "
	    "and a WRITE with it then failed (status %u)", s.rs_len, rkey,
	    rr.rr_status);
}

static void
t_rwstream(peer_t *a, peer_t *b)
{
	const rw_shape_t s = { 256 << 10, 0, 0, 0, 1, 65 };
	rdmat_run_t rr;
	int ret;

	if (rw_fresh(a, b) != 0) {
		rw_fresh_done();
		result(0, "rwstream", "setup failed");
		return;
	}
	rw_fresh_done();
	rw_run_init(&rr, b, &s, RDMAT_RW_READ | RDMAT_RW_MR);
	rr.rr_count = RDMAT_MAX_COUNT;
	rr.rr_depth = 16 < o_depth ? 16 : o_depth;
	rr.rr_rlen = b->p_setup.rs_qp[0].rqi_len;
	rr.rr_run_ms = (uint32_t)o_secs * 1000;
	rr.rr_timeout_ms = rr.rr_run_ms + 20000;
	if (rr.rr_timeout_ms > RDMAT_MAX_TIMEOUT_MS)
		rr.rr_timeout_ms = RDMAT_MAX_TIMEOUT_MS;
	ret = run(a, &rr);
	if (ret != 0 || rr.rr_ns == 0) {
		result(0, "rwstream", "%s status %u after %llu transfers",
		    strerror(ret), rr.rr_status, (u_longlong_t)rr.rr_done);
		return;
	}
	result(1, "rwstream", "%llu FRWR READs of %u B, depth %u, %u requests "
	    "and %u MRs each: %.2f Gb/s", (u_longlong_t)rr.rr_done,
	    s.rs_len, rr.rr_depth, rr.rr_rw_wrs, rr.rr_rw_mrs,
	    (double)rr.rr_bytes * 8 / rr.rr_ns);
}

/* Returns -1 if t is not an rdk_rw test. */
int
rw_test(peer_t *a, peer_t *b, const char *t)
{
	if (strcmp(t, "rwshapes") == 0)
		t_rwshapes(a, b);
	else if (strcmp(t, "rwsend") == 0)
		t_rwsend(a, b);
	else if (strcmp(t, "rwstream") == 0)
		t_rwstream(a, b);
	else
		return (-1);
	return (0);
}

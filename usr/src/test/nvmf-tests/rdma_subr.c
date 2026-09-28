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
 * Run nvmf_rdma_subr.c against independent models: CM private data from
 * the host (random bytes, each rejection by construction, the REP and REJ
 * layouts), queue sizing against random device limits, and the transfer
 * range check with 128-bit arithmetic.  Private data sits at the end of
 * its allocation, so ASan reports a read past what the host sent.
 *
 *	rdma_subr pdata | sizing | range
 */

#include "subr.h"

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s) case %d\n",	\
		    __FILE__, __LINE__, #x, case_no);			\
		exit(1);						\
	}								\
} while (0)

static int case_no;
static uint64_t rng = 0x243f6a8885a308d3ULL;

static uint64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (rng);
}

static uint16_t
le16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | (p[1] << 8)));
}

/* The transport binding's rules, written out again. */
static nvmf_rdma_rej_t
model_req(const uint8_t *p, size_t len, uint32_t ird,
    const nvmf_rdma_limits_t *lim)
{
	uint32_t qid, hrq, hsq, max;

	if (len < 32)
		return (NVMF_RDMA_REJ_INVALID_LEN);
	if (le16(p) != 0)
		return (NVMF_RDMA_REJ_INVALID_RECFMT);
	qid = le16(p + 2);
	hrq = le16(p + 4);
	hsq = le16(p + 6);
	if (qid > lim->nrl_max_qid)
		return (NVMF_RDMA_REJ_INVALID_QID);
	max = qid == 0 ? lim->nrl_admin_entries : lim->nrl_io_entries;
	if (max > 1024)
		max = 1024;
	if (hsq + 1 < 2 || hsq + 1 > max)
		return (NVMF_RDMA_REJ_INVALID_HSQSIZE);
	if (hrq < hsq + 1)
		return (NVMF_RDMA_REJ_INVALID_HRQSIZE);
	if (ird == 0)
		return (NVMF_RDMA_REJ_INVALID_IRD);
	return (NVMF_RDMA_OK);
}

static void
check_req(const uint8_t *src, size_t len, uint32_t ird,
    const nvmf_rdma_limits_t *lim)
{
	uint8_t *p = malloc(len == 0 ? 1 : len) + (len == 0 ? 1 : 0);
	nvmf_rdma_req_t req;
	nvmf_rdma_rej_t got, want;

	if (len != 0)
		memcpy(p, src, len);
	got = nvmf_rdma_req_parse(p, len, ird, lim, &req);
	want = model_req(src, len, ird, lim);
	if (got != want) {
		(void) fprintf(stderr, "len %zu ird %u: got %d want %d\n", len,
		    ird, got, want);
	}
	CHECK(got == want);
	if (got == NVMF_RDMA_OK) {
		CHECK(req.nrq_qid == le16(src + 2));
		CHECK(req.nrq_hrqsize == le16(src + 4));
		CHECK(req.nrq_hsqsize == le16(src + 6));
		CHECK(req.nrq_cntlid == le16(src + 8));
	}
	free(p - (len == 0 ? 1 : 0));
}

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void
test_pdata(void)
{
	nvmf_rdma_limits_t lim = { 64, 32, 128 };
	uint8_t b[64], rep[NVMF_RDMA_REP_LEN], rej[NVMF_RDMA_REJ_LEN];
	int i, j, seen[16] = { 0 };

	/* One of each: a good request, then each refusal. */
	bzero(b, sizeof (b));
	put16(b + 2, 3);
	put16(b + 4, 128);
	put16(b + 6, 127);
	put16(b + 8, 1);
	for (i = 0; i < 12; i++) {
		uint8_t c[64];
		size_t len = 32;
		uint32_t ird = 4;

		memcpy(c, b, sizeof (c));
		switch (i) {
		case 1: len = 31; break;
		case 2: put16(c, 1); break;
		case 3: put16(c + 2, 65); break;
		case 4: put16(c + 6, 128); break;
		case 5: put16(c + 6, 0); break;
		case 6: put16(c + 2, 0); put16(c + 6, 32); break;
		case 7: put16(c + 4, 127); break;
		case 8: ird = 0; break;
		case 9: len = 56; break;	/* RoCE pads to its message */
		case 10: put16(c + 6, 0xffff); put16(c + 4, 0xffff); break;
		case 11: len = 0; break;
		default: break;
		}
		case_no = i;
		check_req(c, len, ird, &lim);
	}

	/* Random bytes and limits, biased to land near the edges. */
	for (i = 0; i < 2000000; i++) {
		size_t len = (size_t)(rnd() % 64);
		uint32_t ird = (uint32_t)(rnd() % 3);

		case_no = 1000 + i;
		for (j = 0; j < 64; j++)
			b[j] = (uint8_t)rnd();
		if (rnd() & 1)
			put16(b, 0);
		if (rnd() & 1)
			put16(b + 2, (uint16_t)(rnd() % 70));
		if (rnd() & 1)
			put16(b + 6, (uint16_t)(rnd() % 140));
		if (rnd() & 1)
			put16(b + 4, (uint16_t)(le16(b + 6) + rnd() % 3));
		if (rnd() & 1)
			len = 32 + rnd() % 32;
		lim.nrl_max_qid = (uint16_t)(rnd() % 70);
		lim.nrl_admin_entries = (uint32_t)(rnd() % 40);
		lim.nrl_io_entries = (uint32_t)(rnd() % 2048);
		check_req(b, len, ird, &lim);
		seen[model_req(b, len, ird, &lim)]++;
	}
	/* Every answer the parser gives came up; NO_RESOURCES is not one. */
	for (i = 0; i <= 7; i++)
		CHECK(i == NVMF_RDMA_REJ_NO_RESOURCES || seen[i] > 0);

	nvmf_rdma_rep_build(rep, 0x1234);
	CHECK(le16(rep) == 0 && le16(rep + 2) == 0x1234);
	for (i = 4; i < NVMF_RDMA_REP_LEN; i++)
		CHECK(rep[i] == 0);
	nvmf_rdma_rej_build(rej, NVMF_RDMA_REJ_INVALID_CNTLID);
	CHECK(le16(rej) == 0 && le16(rej + 2) == 9);
	(void) printf("PASS: CM private data\n");
}

static void
check_sizes(uint32_t depth, uint32_t icd, const nvmf_rdma_devlim_t *dl)
{
	nvmf_rdma_sizes_t sz;
	nvmf_rdma_rej_t got, want = NVMF_RDMA_OK;
	uint64_t rq = (uint64_t)depth + 1, base = 2ULL * depth + 1, max_sq;

	if (depth < 2 || depth > 1024) {
		want = NVMF_RDMA_REJ_INVALID_HSQSIZE;
	} else if (icd > 16384 || dl->ndl_rw_wrs == 0) {
		want = NVMF_RDMA_REJ_NO_RESOURCES;
	} else if (rq > dl->ndl_max_qp_wr || rq >= dl->ndl_max_cqe) {
		want = NVMF_RDMA_REJ_INVALID_HSQSIZE;
	} else {
		max_sq = dl->ndl_max_qp_wr;
		if (dl->ndl_max_cqe - rq < max_sq)
			max_sq = dl->ndl_max_cqe - rq;
		if (base + dl->ndl_rw_wrs > max_sq)
			want = NVMF_RDMA_REJ_INVALID_HSQSIZE;
	}
	got = nvmf_rdma_size_queue(depth, icd, dl, &sz);
	if (got != want) {
		(void) fprintf(stderr, "depth %u icd %u qp_wr %u cqe %u wrs %u:"
		    " got %d want %d\n", depth, icd, dl->ndl_max_qp_wr,
		    dl->ndl_max_cqe, dl->ndl_rw_wrs, got, want);
	}
	CHECK(got == want);
	if (got != NVMF_RDMA_OK)
		return;
	CHECK(sz.nrs_depth == depth && sz.nrs_rq == depth + 1);
	CHECK(sz.nrs_cmds == 2 * depth);
	CHECK(sz.nrs_xfers >= 1 && sz.nrs_xfers <= depth);
	/* Every SEND and every transfer fit, with the drain's entry. */
	CHECK((uint64_t)sz.nrs_sq >= (uint64_t)sz.nrs_cmds + 1 +
	    (uint64_t)sz.nrs_xfers * dl->ndl_rw_wrs);
	CHECK(sz.nrs_sq <= dl->ndl_max_qp_wr && sz.nrs_rq <=
	    dl->ndl_max_qp_wr);
	CHECK(sz.nrs_cq == sz.nrs_rq + sz.nrs_sq && sz.nrs_cq <=
	    dl->ndl_max_cqe);
	CHECK(sz.nrs_slot >= 64 + icd && sz.nrs_slot % 64 == 0 &&
	    sz.nrs_slot < 64 + icd + 64);
	CHECK(sz.nrs_bytes >= (uint64_t)depth * sz.nrs_slot);
	/* A device with room for more transfers gets as many as entries. */
	if ((uint64_t)dl->ndl_max_qp_wr >= base + (uint64_t)depth *
	    dl->ndl_rw_wrs && (uint64_t)dl->ndl_max_cqe >= rq + base +
	    (uint64_t)depth * dl->ndl_rw_wrs)
		CHECK(sz.nrs_xfers == depth);
}

static void
test_sizing(void)
{
	nvmf_rdma_devlim_t dl;
	int i;

	dl.ndl_max_qp_wr = 4096;
	dl.ndl_max_cqe = 65536;
	dl.ndl_rw_wrs = 3;
	case_no = 0;
	check_sizes(128, 8192, &dl);
	check_sizes(32, 0, &dl);
	check_sizes(1024, 16384, &dl);
	check_sizes(1, 0, &dl);
	check_sizes(1025, 0, &dl);
	check_sizes(8, 16400, &dl);
	dl.ndl_max_qp_wr = 17 + 6;
	check_sizes(8, 4096, &dl);
	dl.ndl_max_qp_wr = 17;
	check_sizes(8, 4096, &dl);
	for (i = 0; i < 2000000; i++) {
		case_no = i + 1;
		dl.ndl_max_qp_wr = (uint32_t)(rnd() % (rnd() & 1 ? 64 : 70000));
		dl.ndl_max_cqe = (uint32_t)(rnd() % (rnd() & 1 ? 96 : 140000));
		dl.ndl_rw_wrs = (uint32_t)(rnd() % 40);
		check_sizes((uint32_t)(rnd() % (rnd() & 1 ? 40 : 1100)),
		    (uint32_t)(rnd() % 17000), &dl);
	}
	(void) printf("PASS: queue sizing\n");
}

static void
test_range(void)
{
	unsigned __int128 end;
	uint32_t off, len, total;
	int i;

	for (i = 0; i < 4000000; i++) {
		case_no = i;
		off = (uint32_t)rnd();
		len = (uint32_t)rnd();
		total = (uint32_t)rnd();
		if (rnd() & 1) {
			total >>= rnd() % 32;
			off = total - (uint32_t)(rnd() % 3);
			len = (uint32_t)(rnd() % 4);
		}
		end = (unsigned __int128)off + len;
		CHECK(nvmf_rdma_range_ok(off, len, total) == (end <= total));
	}
	for (i = 0; i < 65536; i++)
		CHECK(nvmf_rdma_cid_hash((uint16_t)i) < NR_CIDHASH);
	(void) printf("PASS: transfer ranges\n");
}

int
main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "all";

	if (strcmp(which, "all") == 0 || strcmp(which, "pdata") == 0)
		test_pdata();
	if (strcmp(which, "all") == 0 || strcmp(which, "sizing") == 0)
		test_sizing();
	if (strcmp(which, "all") == 0 || strcmp(which, "range") == 0)
		test_range();
	return (0);
}

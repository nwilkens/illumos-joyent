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
 * Run nvmf_sgl_decode() against hand-built and random SGL1 descriptors, checked
 * by an independent model that uses 128-bit arithmetic, and run the memdesc
 * copy helpers against a flat reference buffer.  The SQE is the last 64 bytes
 * of its allocation, so ASan reports any read past it.
 */
#include "sgl.h"

static uint64_t rng = 0x243f6a8885a308d3ULL;

static uint64_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (rng);
}

static uint8_t *
new_sqe(uint8_t opc, uint8_t fctype, uint8_t type, uint64_t addr,
    uint32_t len, uint32_t key)
{
	uint8_t *b = calloc(1, 64), *d = b + 24;
	int i;

	b[0] = opc;
	/* PSDT, bits 7:6 of byte 1, is 01b: SGLs, as Linux sends. */
	b[1] = 0x40;
	b[4] = fctype;
	for (i = 0; i < 8; i++)
		d[i] = (uint8_t)(addr >> (8 * i));
	if ((type >> 4) == NVMF_SGL_KEYED_DATA_BLOCK) {
		for (i = 0; i < 3; i++)
			d[8 + i] = (uint8_t)(len >> (8 * i));
		for (i = 0; i < 4; i++)
			d[11 + i] = (uint8_t)(key >> (8 * i));
	} else {
		for (i = 0; i < 4; i++)
			d[8 + i] = (uint8_t)(len >> (8 * i));
	}
	d[15] = type;
	return (b);
}

static uint8_t
model(const uint8_t *b, size_t icd, uint64_t max, nvmf_sgl_t *out)
{
	const uint8_t *d = b + 24;
	uint8_t type = d[15] >> 4, sub = d[15] & 0xf, dir;
	unsigned __int128 addr = 0, len;
	int i;

	if ((b[1] >> 6) != 1)
		return (NVME_CQE_SC_GEN_INV_FLD);
	for (i = 0; i < 8; i++)
		addr |= (unsigned __int128)d[i] << (8 * i);
	dir = b[0] == 0x7f ? (b[4] & 3) : (b[0] & 3);
	memset(out, 0, sizeof (*out));
	out->nsl_addr = (uint64_t)addr;
	if (type == 0 && sub == 1) {
		if (dir != 1)
			return (NVME_CQE_SC_GEN_INV_SGL_DESC);
		len = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
		out->nsl_len = (uint32_t)len;
		if (addr > icd)
			return (NVME_CQE_SC_GEN_INV_SGL_OFF);
		if (addr + len > icd)
			return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
		return (NVME_CQE_SC_GEN_SUCCESS);
	}
	if (type == 4 && (sub == 0 || sub == 0xf)) {
		if (icd != 0)
			return (NVME_CQE_SC_GEN_INV_FLD);
		len = d[8] | d[9] << 8 | d[10] << 16;
		out->nsl_keyed = B_TRUE;
		out->nsl_invalidate = sub == 0xf;
		out->nsl_len = (uint32_t)len;
		out->nsl_key = d[11] | d[12] << 8 | d[13] << 16 |
		    (uint32_t)d[14] << 24;
		if ((max != 0 && len > max) ||
		    addr + len >= ((unsigned __int128)1 << 64))
			return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
		return (NVME_CQE_SC_GEN_SUCCESS);
	}
	return (NVME_CQE_SC_GEN_INV_SGL_DESC);
}

static void
check(uint8_t *b, size_t icd, uint64_t max, uint8_t expect, int line)
{
	nvmf_sgl_t got, want;
	uint8_t sc = nvmf_sgl_decode((nvme_sqe_t *)b, icd, max, &got);

	if (sc != expect || model(b, icd, max, &want) != expect ||
	    (sc == 0 && memcmp(&got, &want, sizeof (got)) != 0)) {
		fprintf(stderr, "line %d: sc %#x expect %#x\n", line, sc,
		    expect);
		abort();
	}
	free(b);
}

#define	CHECK(b, icd, max, e)	check(b, icd, max, e, __LINE__)
#define	OK	NVME_CQE_SC_GEN_SUCCESS
#define	DESC	NVME_CQE_SC_GEN_INV_SGL_DESC
#define	LEN	NVME_CQE_SC_GEN_INV_DSGL_LEN

static void
cases(void)
{
	uint8_t *b;

	/* Linux sends a zeroed keyed descriptor for a command with no data. */
	CHECK(new_sqe(0x00, 0, 0x40, 0, 0, 0), 0, 1 << 20, OK);
	CHECK(new_sqe(0x02, 0, 0x40, 0x1000, 4096, 0x1234), 0, 1 << 20, OK);
	CHECK(new_sqe(0x02, 0, 0x4f, 0x1000, 4096, 0x1234), 0, 1 << 20, OK);
	CHECK(new_sqe(0x02, 0, 0x41, 0x1000, 4096, 0x1234), 0, 1 << 20, DESC);
	CHECK(new_sqe(0x02, 0, 0x4e, 0x1000, 4096, 0x1234), 0, 1 << 20, DESC);
	CHECK(new_sqe(0x02, 0, 0x40, 0, 1 << 20, 1), 0, 1 << 20, OK);
	CHECK(new_sqe(0x02, 0, 0x40, 0, (1 << 20) + 1, 1), 0, 1 << 20, LEN);
	CHECK(new_sqe(0x02, 0, 0x40, 0, 0xffffff, 1), 0, 0, OK);
	/* The exclusive end must not wrap either. */
	CHECK(new_sqe(0x02, 0, 0x40, UINT64_MAX, 0, 1), 0, 0, OK);
	CHECK(new_sqe(0x02, 0, 0x40, UINT64_MAX, 1, 1), 0, 0, LEN);
	CHECK(new_sqe(0x02, 0, 0x40, UINT64_MAX - 1, 1, 1), 0, 0, OK);
	CHECK(new_sqe(0x02, 0, 0x40, UINT64_MAX - 4095, 4096, 1), 0, 0, LEN);
	CHECK(new_sqe(0x02, 0, 0x40, UINT64_MAX - 4096, 4096, 1), 0, 0, OK);
	CHECK(new_sqe(0x01, 0, 0x40, 0, 512, 1), 512, 0,
	    NVME_CQE_SC_GEN_INV_FLD);

	/* In-capsule data: writes and Fabrics commands that carry data. */
	CHECK(new_sqe(0x01, 0, 0x01, 0, 4096, 0), 4096, 0, OK);
	CHECK(new_sqe(0x01, 0, 0x01, 0, 0, 0), 0, 0, OK);
	CHECK(new_sqe(0x01, 0, 0x01, 4096, 0, 0), 4096, 0, OK);
	CHECK(new_sqe(0x01, 0, 0x01, 4097, 0, 0), 4096, 0,
	    NVME_CQE_SC_GEN_INV_SGL_OFF);
	CHECK(new_sqe(0x01, 0, 0x01, 100, 3997, 0), 4096, 0, LEN);
	CHECK(new_sqe(0x01, 0, 0x01, 100, 3996, 0), 4096, 0, OK);
	CHECK(new_sqe(0x01, 0, 0x01, UINT64_MAX, 2, 0), 4096, 0,
	    NVME_CQE_SC_GEN_INV_SGL_OFF);
	CHECK(new_sqe(0x01, 0, 0x01, 1, UINT32_MAX, 0), 4096, 0, LEN);
	CHECK(new_sqe(0x7f, 0x01, 0x01, 0, 1024, 0), 1024, 0, OK);
	CHECK(new_sqe(0x02, 0, 0x01, 0, 4096, 0), 4096, 0, DESC);
	CHECK(new_sqe(0x7f, 0x06, 0x01, 0, 16, 0), 16, 0, DESC);
	CHECK(new_sqe(0x03, 0, 0x01, 0, 16, 0), 16, 0, DESC);
	/* Linux leaves an old key in the reserved bytes of this descriptor. */
	b = new_sqe(0x01, 0, 0x01, 0, 4096, 0);
	b[24 + 12] = 0xbd;
	b[24 + 13] = 0x01;
	CHECK(b, 4096, 0, OK);

	/* Descriptor forms nobody should send here. */
	CHECK(new_sqe(0x01, 0, 0x00, 0, 16, 0), 16, 0, DESC);
	CHECK(new_sqe(0x01, 0, 0x5a, 0, 16, 0), 16, 0, DESC);
	CHECK(new_sqe(0x01, 0, 0x20, 0, 16, 0), 16, 0, DESC);
	CHECK(new_sqe(0x01, 0, 0x30, 0, 16, 0), 16, 0, DESC);
	CHECK(new_sqe(0x01, 0, 0xf0, 0, 16, 0), 16, 0, DESC);
	/* PRP, and the PSDT values the spec reserves or gives to metadata. */
	b = new_sqe(0x02, 0, 0x40, 0, 16, 1);
	b[1] = 0x00;
	CHECK(b, 0, 0, NVME_CQE_SC_GEN_INV_FLD);
	b = new_sqe(0x02, 0, 0x40, 0, 16, 1);
	b[1] = 0x80;
	CHECK(b, 0, 0, NVME_CQE_SC_GEN_INV_FLD);
	b = new_sqe(0x02, 0, 0x40, 0, 16, 1);
	b[1] = 0xc0;
	CHECK(b, 0, 0, NVME_CQE_SC_GEN_INV_FLD);
	/* The other bits of byte 1 (FUSE and reserved) do not matter here. */
	b = new_sqe(0x02, 0, 0x40, 0, 16, 1);
	b[1] = 0x7f;
	CHECK(b, 0, 0, OK);
}

static void
fuzz(unsigned long iters)
{
	static const uint8_t types[] = { 0x00, 0x01, 0x02, 0x0f, 0x40, 0x41,
	    0x4f, 0x4e, 0x5a, 0x10, 0x20, 0x30 };
	unsigned long i, ok = 0;

	for (i = 0; i < iters; i++) {
		uint8_t *b = calloc(1, 64);
		nvmf_sgl_t got, want;
		size_t icd = (next() & 1) ? 0 : (size_t)(next() % 20000);
		uint64_t max = (next() & 1) ? 0 : next() % (1ULL << 25);
		uint8_t sc, msc;
		int j;

		for (j = 0; j < 64; j++)
			b[j] = (uint8_t)next();
		if (next() & 1) {
			b[1] = (b[1] & 0x3f) | 0x40;
			b[24 + 15] = types[next() % sizeof (types)];
		}
		if (next() & 1) {
			/* Small offsets and lengths reach the success paths. */
			memset(b + 24, 0, 15);
			b[24] = (uint8_t)next();
			b[24 + 1] = (uint8_t)(next() & 0x3f);
			b[24 + 8] = (uint8_t)next();
			b[24 + 9] = (uint8_t)(next() & 0x3f);
			if (next() & 1)
				b[0] = 0x01;
		}
		if (next() % 16 == 0)
			memset(b + 24, 0xff, 8);

		sc = nvmf_sgl_decode((nvme_sqe_t *)b, icd, max, &got);
		msc = model(b, icd, max, &want);
		if (sc != msc || (sc == 0 && memcmp(&got, &want,
		    sizeof (got)) != 0)) {
			fprintf(stderr, "fuzz %lu: sc %#x model %#x\n", i, sc,
			    msc);
			abort();
		}
		if (sc == 0) {
			ok++;
			if (!got.nsl_keyed) {
				assert(got.nsl_addr <= icd &&
				    got.nsl_len <= icd - got.nsl_addr);
			} else {
				assert(max == 0 || got.nsl_len <= max);
				assert(got.nsl_addr <=
				    UINT64_MAX - got.nsl_len);
			}
		}
		free(b);
	}
	assert(ok > iters / 100);
	printf("sgl fuzz: %lu descriptors, %lu accepted\n", iters, ok);
}

/* A random memdesc over the bytes ref[0 .. len), in pieces. */
typedef struct built {
	nvmf_memdesc_t md;
	uint8_t *flat;
	nvmf_seg_t segs[16];
	uint8_t *bufs[16];
	mblk_t mb[16];
	int n;
} built_t;

static void
build(built_t *bt, size_t len)
{
	size_t done = 0;
	int t = (int)(next() % 3);

	memset(bt, 0, sizeof (*bt));
	bt->md.nmd_len = len;
	if (t == 0) {
		bt->md.nmd_type = NVMF_MEMDESC_VADDR;
		bt->flat = malloc(len + 1);
		bt->md.nmd_u.nmd_vaddr = bt->flat;
		return;
	}
	while (bt->n < 15 && done < len) {
		size_t piece = (next() % 4 == 0) ? 0 :
		    1 + next() % (len - done);

		if (bt->n == 14)
			piece = len - done;
		bt->bufs[bt->n] = malloc(piece + 1);
		bt->segs[bt->n].nsg_len = (uint32_t)piece;
		bt->segs[bt->n].nsg_addr = bt->bufs[bt->n];
		bt->mb[bt->n].b_rptr = bt->bufs[bt->n];
		bt->mb[bt->n].b_wptr = bt->bufs[bt->n] + piece;
		if (bt->n > 0)
			bt->mb[bt->n - 1].b_cont = &bt->mb[bt->n];
		done += piece;
		bt->n++;
	}
	if (t == 1) {
		bt->md.nmd_type = NVMF_MEMDESC_MBLK;
		bt->md.nmd_u.nmd_mp = bt->n > 0 ? &bt->mb[0] : NULL;
	} else {
		bt->md.nmd_type = NVMF_MEMDESC_SGL;
		bt->md.nmd_u.nmd_sgl.nmd_segs = bt->segs;
		bt->md.nmd_u.nmd_sgl.nmd_nsegs = (uint_t)bt->n;
	}
}

static void
destroy(built_t *bt)
{
	int i;

	free(bt->flat);
	for (i = 0; i < bt->n; i++)
		free(bt->bufs[i]);
}

static void
copies(unsigned long iters)
{
	unsigned long i;

	for (i = 0; i < iters; i++) {
		size_t len = 1 + next() % 300, off, n;
		uint8_t ref[301], in[700], out[700];
		built_t bt;
		size_t j;

		build(&bt, len);
		for (j = 0; j < len; j++)
			ref[j] = (uint8_t)next();
		nvmf_memdesc_copyin(&bt.md, 0, ref, len);
		off = next() % (len + 1);
		n = next() % (len - off + 1);
		/* Sometimes ask for more than fits: nothing past nmd_len. */
		if (next() % 8 == 0)
			n += next() % 300;
		if (next() % 16 == 0)
			off = len + next() % 50;
		for (j = 0; j < sizeof (in); j++)
			in[j] = (uint8_t)next();
		nvmf_memdesc_copyin(&bt.md, off, in, n);
		for (j = 0; off < len && j < n && off + j < len; j++)
			ref[off + j] = in[j];
		memset(out, 0, sizeof (out));
		nvmf_memdesc_copyout(&bt.md, 0, out, len);
		assert(memcmp(out, ref, len) == 0);
		destroy(&bt);
	}
	printf("memdesc copies: %lu\n", iters);
}

int
main(void)
{
	cases();
	fuzz(3000000);
	copies(300000);
	printf("sgl decode passed\n");
	return (0);
}

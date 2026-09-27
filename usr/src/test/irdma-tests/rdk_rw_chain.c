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
 * Run rdk_rw.c with the page walk of rdk_verbs.c against a fake provider,
 * and execute each posted chain on a model of the device and the peer.
 * For every transfer shape (cookie layout, offset, length, remote segments,
 * SGE limits, MR sizes, iWARP or not, READ_WITH_INV or not) the model
 * checks that the bytes land where they should and nowhere else, that no
 * request exceeds its SGE limit, that each registration covers exactly the
 * bytes its READs write, with a new key and only the access it needs, that
 * every MR is invalidated by the end, a LOCAL_INV only behind a fence, that
 * only the last request is signaled and all carry the cqe, and that the
 * transfer fits the limits rdk_rw_limits() gave.
 */

#include "rdk_unit.h"

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s) case %d\n",	\
		    __FILE__, __LINE__, #x, case_no);			\
		exit(1);						\
	}								\
} while (0)

static int case_no;
static boolean_t fake_iwarp;

/* rdk_device.c and rdk_cm_iw.c, which are not built here. */
int rdk_obj_hold(struct rdk_device *d) { (void) d; return (0); }
void rdk_obj_rele(struct rdk_device *d) { (void) d; }
boolean_t rdk_port_valid(struct rdk_device *d, uint32_t p) { return (p == 1); }
int
rdk_resolve_ah_attr(struct rdk_device *d, struct rdk_ah_attr *a)
{
	(void) d; (void) a;
	return (EINVAL);
}
void rdk_put_gid_attr(const struct rdk_gid_attr *a) { (void) a; }
void rdk_device_taint(struct rdk_device *d) { d->rd_tainted = 1; }
boolean_t
rdk_device_iwarp(struct rdk_device *d)
{
	(void) d;
	return (fake_iwarp);
}

/* Local memory at LBASE, the peer's at RBASE. */
#define	PG		4096ULL
#define	LBASE		0x40000000ULL
#define	LSIZE		(1024 * 1024)
#define	RBASE		0x7000000000ULL
#define	RSIZE		(512 * 1024)
#define	MAXMR		512
#define	MAXPG		128

static uint8_t lmem[LSIZE], lwant[LSIZE], rmem[RSIZE], rwant[RSIZE];

struct fake_mr {
	struct rdk_mr	fm_mr;
	int		fm_cap;
	int		fm_n;
	uint64_t	fm_pages[MAXPG];
	/* The device's view, from REG_MR to invalidation. */
	int		fm_valid;
	uint32_t	fm_key;
	int		fm_access;
	uint64_t	fm_iova;
	uint64_t	fm_len;
	uint64_t	fm_hw_pages[MAXPG];
	uint64_t	fm_written;	/* READ bytes placed since REG */
	int		fm_reads;	/* READs not yet fenced */
	int		fm_regs;
	uint32_t	fm_last_key;
};

static int
fake_set_page(struct rdk_mr *mr, uint64_t addr)
{
	struct fake_mr *fm = (struct fake_mr *)mr;

	if (fm->fm_n >= fm->fm_cap)
		return (-ENOMEM);
	fm->fm_pages[fm->fm_n++] = addr;
	return (0);
}

static int
fake_map_mr_sg(struct rdk_mr *mr, const ddi_dma_cookie_t *ck, uint_t n,
    uint64_t *off)
{
	((struct fake_mr *)mr)->fm_n = 0;
	return (rdk_sg_to_pages(mr, ck, n, off, fake_set_page));
}

static int
fake_alloc_mr(struct rdk_pd *pd, enum rdk_mr_type t, uint32_t n,
    struct rdk_mr **mrp)
{
	static uint32_t idx = 1;
	struct fake_mr *fm = kmem_zalloc(sizeof (*fm), KM_SLEEP);

	(void) pd; (void) t;
	CHECK(n <= MAXPG);
	fm->fm_cap = (int)n;
	fm->fm_mr.lkey = fm->fm_mr.rkey = (idx++ << 8) | 0x10;
	*mrp = &fm->fm_mr;
	return (0);
}

static int
fake_dereg_mr(struct rdk_mr *mr)
{
	kmem_free(mr, sizeof (struct fake_mr));
	return (0);
}

/* What was posted, copied as the device reads it at post time. */
#define	MAXWR	4096
#define	MAXSG	8192
static struct {
	struct rdk_send_wr	wr;
	uint64_t		raddr;
	uint32_t		rkey;
	struct rdk_mr		*mr;
	uint32_t		key;
	int			access;
	int			sge0;
} posted[MAXWR];
static struct rdk_sge psge[MAXSG];
static int nposted, npsge, post_fail_at = -1;

static int
fake_post_send(struct rdk_qp *qp, const struct rdk_send_wr *wr,
    const struct rdk_send_wr **bad)
{
	int i;

	(void) qp;
	nposted = npsge = 0;
	for (; wr != NULL; wr = wr->next) {
		if (nposted == post_fail_at) {
			*bad = wr;
			return (ENOMEM);
		}
		CHECK(nposted < MAXWR && npsge + wr->num_sge <= MAXSG);
		posted[nposted].wr = *wr;
		posted[nposted].sge0 = npsge;
		for (i = 0; i < wr->num_sge; i++)
			psge[npsge++] = wr->sg_list[i];
		if (wr->opcode == RDK_WR_REG_MR) {
			posted[nposted].mr = RDK_REG_WR(wr)->mr;
			posted[nposted].key = RDK_REG_WR(wr)->key;
			posted[nposted].access = RDK_REG_WR(wr)->access;
		} else if (wr->opcode == RDK_WR_RDMA_WRITE ||
		    wr->opcode == RDK_WR_RDMA_READ ||
		    wr->opcode == RDK_WR_RDMA_READ_WITH_INV) {
			posted[nposted].raddr = RDK_RDMA_WR(wr)->remote_addr;
			posted[nposted].rkey = RDK_RDMA_WR(wr)->rkey;
		}
		nposted++;
	}
	return (0);
}

static struct rdk_device_ops fake_ops = {
	.version = RDK_ABI_VERSION,
	.post_send = fake_post_send,
	.alloc_mr = fake_alloc_mr,
	.map_mr_sg = fake_map_mr_sg,
	.dereg_mr = fake_dereg_mr
};

static struct rdk_device dev;
static struct rdk_pd pd;
static struct rdk_qp qp;
static struct rdk_mr *mrs[MAXMR];
static struct rdk_cqe cqe;

/* The transfer under test. */
static ddi_dma_cookie_t ck[16];
static uint_t nck;
static struct rdk_rw_seg segs[RDK_RW_MAX_SEGS];
static uint_t nsegs;

static uint8_t *
lptr(uint64_t addr, uint64_t len)
{
	CHECK(addr >= LBASE && addr + len <= LBASE + LSIZE);
	return (&lmem[addr - LBASE]);
}

/* The peer checks its keys and bounds, as its device would. */
static uint8_t *
rptr(uint64_t addr, uint64_t len, uint32_t key)
{
	uint_t i;

	for (i = 0; i < nsegs; i++) {
		if (segs[i].rs_key == key && addr >= segs[i].rs_addr &&
		    addr + len <= segs[i].rs_addr + segs[i].rs_len)
			return (&rmem[addr - RBASE]);
	}
	CHECK(!"remote access outside the segments of its key");
	return (NULL);
}

static struct fake_mr *
mr_by_key(uint32_t key)
{
	int i;

	for (i = 0; i < MAXMR; i++) {
		struct fake_mr *fm = (struct fake_mr *)mrs[i];

		if (fm != NULL && fm->fm_valid && fm->fm_key == key)
			return (fm);
	}
	return (NULL);
}

/* The local bytes an MR's element names, through its page list. */
static uint8_t *
mr_ptr(struct fake_mr *fm, uint64_t addr, uint64_t len, uint64_t *phys_out)
{
	uint64_t base = fm->fm_iova & ~(PG - 1), pg, phys;

	CHECK(addr >= fm->fm_iova && addr + len <= fm->fm_iova + fm->fm_len);
	pg = (addr - base) / PG;
	CHECK((addr + len - 1 - base) / PG == pg ||
	    fm->fm_hw_pages[(addr + len - 1 - base) / PG] ==
	    fm->fm_hw_pages[pg] + ((addr + len - 1 - base) / PG - pg) * PG);
	phys = fm->fm_hw_pages[pg] + ((addr - base) % PG);
	*phys_out = phys;
	return (lptr(phys, len));
}

/* A READ element in an MR is placed page by page, through its page list. */
static void
exec_read(int i, boolean_t rdinv_cap)
{
	struct rdk_send_wr *w = &posted[i].wr;
	struct rdk_sge *sg = &psge[posted[i].sge0];
	uint64_t roff = posted[i].raddr, done, a, n, phys;
	struct fake_mr *fm = NULL;
	int j;

	CHECK(w->wr_cqe == &cqe);
	for (j = 0; j < w->num_sge; j++) {
		if (sg[j].lkey == pd.local_dma_lkey) {
			CHECK(!fake_iwarp);
			memcpy(lptr(sg[j].addr, sg[j].length),
			    rptr(roff, sg[j].length, posted[i].rkey),
			    sg[j].length);
			roff += sg[j].length;
			continue;
		}
		fm = mr_by_key(sg[j].lkey);
		CHECK(fm != NULL && w->num_sge == 1);
		CHECK((fm->fm_access & RDK_ACCESS_LOCAL_WRITE) != 0);
		for (done = 0; done < sg[j].length; done += n) {
			a = sg[j].addr + done;
			n = MIN(sg[j].length - done, PG - (a % PG));
			memcpy(mr_ptr(fm, a, n, &phys),
			    rptr(roff + done, n, posted[i].rkey), n);
		}
		roff += sg[j].length;
		fm->fm_written += sg[j].length;
		fm->fm_reads++;
	}
	if (w->opcode == RDK_WR_RDMA_READ_WITH_INV) {
		CHECK(rdinv_cap && fm != NULL);
		CHECK(w->ex.invalidate_rkey == sg[0].lkey);
		CHECK(fm->fm_written == fm->fm_len);
		fm->fm_valid = 0;
	}
}

/* Execute the posted chain in order, as the device and the peer would. */
static void
execute(boolean_t rdinv_cap, int access_want, boolean_t chained)
{
	int i, j, signaled = 0, last_data = -1;

	for (i = 0; i < nposted; i++) {
		struct rdk_send_wr *w = &posted[i].wr;
		struct rdk_sge *sg = &psge[posted[i].sge0];
		struct fake_mr *fm;

		if ((w->send_flags & RDK_SEND_SIGNALED) != 0)
			signaled++;
		switch (w->opcode) {
		case RDK_WR_REG_MR:
			CHECK(w->wr_cqe == &cqe);
			fm = (struct fake_mr *)posted[i].mr;
			CHECK(!fm->fm_valid);
			CHECK(posted[i].key == fm->fm_mr.rkey);
			CHECK(fm->fm_regs == 0 ||
			    (posted[i].key & 0xff) != (fm->fm_last_key & 0xff));
			CHECK((posted[i].key >> 8) == (fm->fm_last_key >> 8) ||
			    fm->fm_regs == 0);
			CHECK(posted[i].access == access_want);
			fm->fm_valid = 1;
			fm->fm_key = posted[i].key;
			fm->fm_last_key = posted[i].key;
			fm->fm_regs++;
			fm->fm_access = posted[i].access;
			fm->fm_iova = fm->fm_mr.iova;
			fm->fm_len = fm->fm_mr.length;
			fm->fm_written = 0;
			fm->fm_reads = 0;
			CHECK(fm->fm_len <= (uint64_t)fm->fm_n * PG);
			memcpy(fm->fm_hw_pages, fm->fm_pages,
			    sizeof (fm->fm_hw_pages));
			break;
		case RDK_WR_RDMA_READ:
		case RDK_WR_RDMA_READ_WITH_INV:
			exec_read(i, rdinv_cap);
			last_data = i;
			break;
		case RDK_WR_RDMA_WRITE: {
			uint64_t roff = posted[i].raddr;

			CHECK(w->wr_cqe == &cqe);
			last_data = i;
			for (j = 0; j < w->num_sge; j++) {
				CHECK(sg[j].lkey == pd.local_dma_lkey);
				memcpy(rptr(roff, sg[j].length, posted[i].rkey),
				    lptr(sg[j].addr, sg[j].length),
				    sg[j].length);
				roff += sg[j].length;
			}
			break;
		}
		case RDK_WR_LOCAL_INV:
			CHECK(w->wr_cqe == &cqe);
			CHECK(!rdinv_cap);
			fm = mr_by_key(w->ex.invalidate_rkey);
			CHECK(fm != NULL);
			/* Placed READs must be waited for. */
			CHECK(fm->fm_reads == 0 ||
			    (w->send_flags & RDK_SEND_FENCE) != 0);
			CHECK(fm->fm_written == fm->fm_len);
			fm->fm_valid = 0;
			last_data = i;
			break;
		case RDK_WR_SEND:
		case RDK_WR_SEND_WITH_INV:
			CHECK(chained && i == nposted - 1);
			CHECK(w->wr_cqe != &cqe);
			break;
		default:
			CHECK(!"unexpected opcode");
		}
	}
	for (i = 0; i < MAXMR; i++) {
		if (mrs[i] != NULL)
			CHECK(!((struct fake_mr *)mrs[i])->fm_valid);
	}
	/* Only the transfer's last request is signaled, and a chained SEND. */
	CHECK(last_data >= 0);
	CHECK((posted[last_data].wr.send_flags & RDK_SEND_SIGNALED) != 0);
	CHECK(signaled == (chained ? 2 : 1));
}

/* A deterministic generator, so that a failure can be replayed. */
static uint64_t seed = 0x9e3779b97f4a7c15ULL;

static uint64_t
rnd(uint64_t n)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (n == 0 ? 0 : seed % n);
}

struct shape {
	boolean_t	iwarp;
	boolean_t	rdinv;
	uint32_t	flags;
	uint32_t	max_sge;
	uint32_t	sge_rd;
	uint32_t	mr_pages;
	enum rdk_rw_dir	dir;
	uint64_t	off;
	uint32_t	len;
};

/* Lay out cookies: each one page-aligned or not, adjacent or not. */
static uint64_t
make_cookies(void)
{
	uint64_t addr = LBASE + rnd(8) * PG + (rnd(2) ? rnd(PG) : 0), total = 0;
	uint_t i;

	nck = 1 + (uint_t)rnd(6);
	for (i = 0; i < nck; i++) {
		uint64_t size;

		switch (rnd(4)) {
		case 0:
			size = 1 + rnd(PG);
			break;
		case 1:
			size = PG * (1 + rnd(3));
			break;
		default:
			size = 1 + rnd(3 * PG);
			break;
		}
		ck[i].dmac_laddress = addr;
		ck[i].dmac_size = size;
		total += size;
		addr += size;
		switch (rnd(3)) {
		case 0:
			break;
		case 1:
			addr = P2ROUNDUP(addr, PG) + rnd(3) * PG;
			break;
		default:
			addr += 1 + rnd(PG);
			break;
		}
	}
	CHECK(addr <= LBASE + LSIZE);
	return (total);
}

static void
make_segs(uint32_t len, boolean_t one_key)
{
	uint64_t raddr = RBASE + rnd(64);
	uint32_t left = len;
	uint_t i;

	nsegs = 1 + (uint_t)rnd(MIN(4, len));
	for (i = 0; i < nsegs; i++) {
		uint32_t n = i + 1 == nsegs ? left :
		    1 + (uint32_t)rnd(left - (nsegs - 1 - i));

		segs[i].rs_addr = raddr;
		segs[i].rs_len = n;
		segs[i].rs_key = one_key ? 0x1234 : 0x1000 + i;
		raddr += n + rnd(100);
		left -= n;
	}
}

static void
fill(uint8_t *p, size_t n)
{
	uint64_t x = rnd(UINT64_MAX) | 1;
	size_t i;

	for (i = 0; i + 8 <= n; i += 8) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		memcpy(&p[i], &x, 8);
	}
}

static void
setup_device(const struct shape *s)
{
	fake_iwarp = s->iwarp;
	dev.rd_attr.max_send_sge = 8;
	dev.rd_attr.max_sge_rd = (int)s->sge_rd;
	dev.rd_attr.max_qp_wr = 32768;
	dev.rd_attr.max_fast_reg_page_list_len = MAXPG;
	dev.rd_attr.page_size_cap = PG;
	dev.rd_attr.device_cap_flags = RDK_DEVICE_MEM_MGT_EXTENSIONS;
	dev.rd_attr.kernel_cap_flags = RDK_KCAP_LOCAL_DMA_LKEY |
	    (s->rdinv ? RDK_KCAP_READ_WITH_INV : 0);
}

/* One transfer end to end; returns rdk_rw_init()'s error. */
static int
run_shape(const struct shape *s)
{
	struct rdk_rw_attr a;
	struct rdk_rw_limits l;
	struct rdk_send_wr resp;
	rdk_rw_ctx_t *ctx;
	boolean_t chained, mr;
	uint_t i, nmr;
	size_t k;
	int ret;

	setup_device(s);
	memset(&a, 0, sizeof (a));
	a.rwa_flags = s->flags;
	a.rwa_max_sge = s->max_sge;
	a.rwa_mr_pages = s->mr_pages;
	a.rwa_max_cookies = nck;
	a.rwa_max_segs = nsegs;
	a.rwa_max_len = s->len;
	CHECK(rdk_rw_limits(&dev, &a, &l) == 0);
	CHECK(rdk_rw_ctx_alloc(&dev, &a, &ctx) == 0);
	mr = s->dir == RDK_RW_READ && (s->iwarp || s->flags != 0);

	CHECK(l.rwl_mrs <= MAXMR);
	nmr = l.rwl_mrs;
	for (i = 0; i < nmr; i++) {
		if (mrs[i] == NULL)
			CHECK(rdk_alloc_mr(&pd, RDK_MR_TYPE_MEM_REG, MAXPG,
			    &mrs[i]) == 0);
		((struct fake_mr *)mrs[i])->fm_cap = (int)s->mr_pages;
		((struct fake_mr *)mrs[i])->fm_valid = 0;
	}

	fill(lmem, sizeof (lmem));
	fill(rmem, sizeof (rmem));
	memcpy(lwant, lmem, sizeof (lmem));
	memcpy(rwant, rmem, sizeof (rmem));
	ret = rdk_rw_init(ctx, &qp, s->dir, ck, nck, s->off, s->len, segs,
	    nsegs, mrs, nmr);
	if (ret != 0) {
		rdk_rw_ctx_free(ctx);
		return (ret);
	}
	CHECK(rdk_rw_nwr(ctx) <= l.rwl_wrs);
	CHECK(rdk_rw_nmr(ctx) <= l.rwl_mrs);
	CHECK(mr || rdk_rw_nmr(ctx) == 0);

	/* The bytes that should move. */
	{
		uint64_t left = s->len, o = s->off;
		uint_t c = 0, g = 0;
		uint64_t so = 0;

		while (o >= ck[c].dmac_size) {
			o -= ck[c].dmac_size;
			c++;
		}
		while (left != 0) {
			uint64_t n = MIN(ck[c].dmac_size - o,
			    segs[g].rs_len - so);
			uint64_t la = ck[c].dmac_laddress + o - LBASE;
			uint64_t ra = segs[g].rs_addr + so - RBASE;

			n = MIN(n, left);
			if (s->dir == RDK_RW_WRITE)
				memcpy(&rwant[ra], &lmem[la], n);
			else
				memcpy(&lwant[la], &rmem[ra], n);
			left -= n;
			o += n;
			so += n;
			if (o == ck[c].dmac_size) {
				c++;
				o = 0;
			}
			if (so == segs[g].rs_len) {
				g++;
				so = 0;
			}
		}
	}

	chained = s->dir == RDK_RW_WRITE && rnd(2) == 0;
	memset(&resp, 0, sizeof (resp));
	resp.opcode = RDK_WR_SEND;
	resp.send_flags = RDK_SEND_SIGNALED;
	CHECK(rdk_rw_post(ctx, &cqe, chained ? &resp : NULL) == 0);
	CHECK(rdk_rw_post(ctx, &cqe, NULL) == EINVAL);
	execute(s->rdinv, RDK_ACCESS_LOCAL_WRITE |
	    (s->iwarp ? RDK_ACCESS_REMOTE_WRITE : 0), chained);

	if (memcmp(lmem, lwant, sizeof (lmem)) != 0) {
		for (k = 0; lmem[k] == lwant[k]; k++)
			;
		(void) fprintf(stderr, "local byte %zu\n", k);
		CHECK(!"local memory differs");
	}
	if (memcmp(rmem, rwant, sizeof (rmem)) != 0) {
		for (k = 0; rmem[k] == rwant[k]; k++)
			;
		(void) fprintf(stderr, "remote byte %zu\n", k);
		CHECK(!"remote memory differs");
	}
	/* Each WRITE and plain READ within its SGE limit. */
	for (i = 0; i < (uint_t)nposted; i++) {
		struct rdk_send_wr *w = &posted[i].wr;

		if (w->opcode == RDK_WR_RDMA_WRITE)
			CHECK(w->num_sge >= 1 && w->num_sge <= (int)s->max_sge);
		if (w->opcode == RDK_WR_RDMA_READ && !mr)
			CHECK(w->num_sge >= 1 &&
			    w->num_sge <= (int)MIN(s->max_sge, s->sge_rd));
	}
	rdk_rw_ctx_free(ctx);
	return (0);
}

static void
random_shapes(int count)
{
	struct shape s;
	uint64_t total;
	int ret;

	for (case_no = 0; case_no < count; case_no++) {
		memset(&s, 0, sizeof (s));
		s.iwarp = rnd(3) == 0;
		s.rdinv = !s.iwarp && rnd(2);
		s.flags = !s.iwarp && rnd(2) ? RDK_RW_F_MR : 0;
		s.max_sge = 1 + (uint32_t)rnd(5);
		s.sge_rd = 1 + (uint32_t)rnd(4);
		s.mr_pages = 2 + (uint32_t)rnd(7);
		if (rnd(8) == 0)
			s.mr_pages = 64;
		s.dir = rnd(2) ? RDK_RW_READ : RDK_RW_WRITE;
		total = make_cookies();
		s.off = rnd(total);
		s.len = 1 + (uint32_t)rnd(MIN(total - s.off, RSIZE / 2));
		make_segs(s.len, rnd(2));
		ret = run_shape(&s);
		CHECK(ret == 0);
	}
}

/* Hand-made shapes at the edges of pages and page lists. */
static void
edge_shapes(void)
{
	static const struct {
		uint64_t	start;
		uint64_t	size;
	} one[] = {
		{ 0, PG }, { 1, PG }, { PG - 1, 2 }, { 0, 8 * PG },
		{ 100, 8 * PG - 100 }, { 0, 16 * PG }, { PG - 1, 16 * PG }
	};
	struct shape s;
	uint_t i, p, d;

	for (i = 0; i < ARRAY_SIZE(one); i++) {
		for (p = 2; p <= 9; p++) {
			for (d = 0; d < 3; d++) {
				case_no = 100000 + (int)(i * 100 + p * 10 + d);
				memset(&s, 0, sizeof (s));
				s.iwarp = d == 0;
				s.rdinv = d == 2;
				s.flags = d != 0 ? RDK_RW_F_MR : 0;
				s.max_sge = 1;
				s.sge_rd = 1;
				s.mr_pages = p;
				s.dir = RDK_RW_READ;
				nck = 1;
				ck[0].dmac_laddress = LBASE + PG + one[i].start;
				ck[0].dmac_size = one[i].size;
				s.off = 0;
				s.len = (uint32_t)one[i].size;
				make_segs(s.len, B_TRUE);
				CHECK(run_shape(&s) == 0);
			}
		}
	}
}

static void
bad_args(void)
{
	struct rdk_rw_attr a = { .rwa_max_sge = 2, .rwa_mr_pages = 4,
	    .rwa_max_cookies = 4, .rwa_max_segs = 2, .rwa_max_len = 65536 };
	struct shape s = { .iwarp = B_TRUE, .max_sge = 2, .sge_rd = 1 };
	struct rdk_rw_attr b;
	struct rdk_rw_limits l;
	struct rdk_rw_seg sg[2];
	struct rdk_send_wr send;
	struct rdk_qp other = qp;
	struct rdk_pd pd2 = pd;
	struct rdk_mr *bad[1];
	rdk_rw_ctx_t *ctx;

	case_no = -1;
	setup_device(&s);
	b = a; b.rwa_max_sge = 9;
	CHECK(rdk_rw_limits(&dev, &b, &l) == EINVAL);
	b = a; b.rwa_mr_pages = 1;
	CHECK(rdk_rw_limits(&dev, &b, &l) == EINVAL);
	b = a; b.rwa_max_segs = RDK_RW_MAX_SEGS + 1;
	CHECK(rdk_rw_limits(&dev, &b, &l) == EINVAL);
	b = a; b.rwa_max_len = RDK_RW_MAX_LEN + 1;
	CHECK(rdk_rw_limits(&dev, &b, &l) == EINVAL);
	b = a; b.rwa_flags = 0x80;
	CHECK(rdk_rw_limits(&dev, &b, &l) == EINVAL);
	b = a; b.rwa_max_cookies = 100000; b.rwa_max_sge = 1;
	dev.rd_attr.max_qp_wr = 1024;
	CHECK(rdk_rw_limits(&dev, &b, &l) == E2BIG);
	dev.rd_attr.max_qp_wr = 32768;

	CHECK(rdk_rw_ctx_alloc(&dev, &a, &ctx) == 0);
	nck = 1;
	ck[0].dmac_laddress = LBASE;
	ck[0].dmac_size = 8192;
	sg[0].rs_addr = RBASE;
	sg[0].rs_len = 4096;
	sg[0].rs_key = 7;
	sg[1] = sg[0];
	sg[1].rs_addr += 4096;
	sg[1].rs_key = 8;
	/* Lengths that do not add up, wrap or exceed are refused. */
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 4095, sg, 1,
	    NULL, 0) == EINVAL);
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 4097, 4096, sg, 1,
	    NULL, 0) == EINVAL);
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 0, sg, 1,
	    NULL, 0) == EINVAL);
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 5, 0, 4096, sg, 1,
	    NULL, 0) == EINVAL);
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 4096, sg, 3,
	    NULL, 0) == EINVAL);
	sg[0].rs_addr = UINT64_MAX - 100;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 4096, sg, 1,
	    NULL, 0) == EINVAL);
	sg[0].rs_addr = RBASE;
	ck[0].dmac_laddress = UINT64_MAX - 100;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 4096, sg, 1,
	    NULL, 0) == EINVAL);
	ck[0].dmac_laddress = LBASE;
	ck[1].dmac_laddress = LBASE + 3 * PG;
	ck[1].dmac_size = 0;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 2, 0, 4096, sg, 1,
	    NULL, 0) == EINVAL);
	other.qp_type = RDK_QPT_UD;
	CHECK(rdk_rw_init(ctx, &other, RDK_RW_WRITE, ck, 1, 0, 4096, sg, 1,
	    NULL, 0) == EINVAL);
	CHECK(rdk_rw_init(ctx, &qp, (enum rdk_rw_dir)9, ck, 1, 0, 4096, sg, 1,
	    NULL, 0) == EINVAL);
	/* An iWARP READ needs MRs from the QP's PD. */
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_READ, ck, 1, 0, 4096, sg, 1,
	    NULL, 0) == ENOBUFS);
	CHECK(rdk_alloc_mr(&pd2, RDK_MR_TYPE_MEM_REG, 4, &bad[0]) == 0);
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_READ, ck, 1, 0, 4096, sg, 1,
	    bad, 1) == EINVAL);
	(void) rdk_dereg_mr(bad[0]);
	/* The MR vector: present, within the limit, each MR once. */
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_READ, ck, 1, 0, 4096, sg, 1,
	    NULL, 1) == EINVAL);
	CHECK(rdk_alloc_mr(&pd, RDK_MR_TYPE_MEM_REG, 4, &bad[0]) == 0);
	((struct fake_mr *)bad[0])->fm_cap = 2;
	{
		struct rdk_mr *dup[2] = { bad[0], bad[0] };
		struct rdk_mr *many[64];
		uint_t k;
		uint32_t len2 = 3 * PG;

		ck[0].dmac_size = 4 * PG;
		sg[0].rs_len = len2;
		CHECK(rdk_rw_init(ctx, &qp, RDK_RW_READ, ck, 1, 0, len2, sg, 1,
		    dup, 2) == EINVAL);
		for (k = 0; k < 64; k++)
			many[k] = bad[0];
		CHECK(rdk_rw_init(ctx, &qp, RDK_RW_READ, ck, 1, 0, len2, sg, 1,
		    many, 64) == EINVAL);
		ck[0].dmac_size = 8192;
		sg[0].rs_len = 4096;
	}
	(void) rdk_dereg_mr(bad[0]);
	/* No post without a build; a READ takes no chain. */
	CHECK(rdk_rw_post(ctx, &cqe, NULL) == EINVAL);
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 8192, sg, 2,
	    NULL, 0) == 0);
	CHECK(rdk_rw_post(ctx, NULL, NULL) == EINVAL);
	/* Two keys: no SEND_WITH_INV. */
	memset(&send, 0, sizeof (send));
	send.opcode = RDK_WR_SEND;
	CHECK(!rdk_rw_send_inv(ctx, &send) && send.opcode == RDK_WR_SEND);
	sg[1].rs_key = 7;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 8192, sg, 2,
	    NULL, 0) == 0);
	CHECK(rdk_rw_send_inv(ctx, &send));
	CHECK(send.opcode == RDK_WR_SEND_WITH_INV &&
	    send.ex.invalidate_rkey == 7);
	send.opcode = RDK_WR_RDMA_WRITE;
	CHECK(!rdk_rw_send_inv(ctx, &send));
	/* A failed build forgets the key of the one before. */
	send.opcode = RDK_WR_SEND;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 1, sg, 1,
	    NULL, 0) == EINVAL);
	CHECK(!rdk_rw_send_inv(ctx, &send));
	dev.rd_attr.device_cap_flags = 0;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 8192, sg, 2,
	    NULL, 0) == 0);
	CHECK(!rdk_rw_send_inv(ctx, &send));
	dev.rd_attr.device_cap_flags = RDK_DEVICE_MEM_MGT_EXTENSIONS;
	/* A READ with too few MRs lent is refused, not overrun. */
	nck = 1;
	CHECK(rdk_alloc_mr(&pd, RDK_MR_TYPE_MEM_REG, 4, &bad[0]) == 0);
	((struct fake_mr *)bad[0])->fm_cap = 4;
	sg[0].rs_len = 8191;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_READ, ck, 1, 1, 8191, sg, 1,
	    bad, 1) == 0);
	CHECK(rdk_rw_post(ctx, &cqe, &send) == EINVAL);
	ck[0].dmac_size = 5 * PG;
	sg[0].rs_len = 5 * PG - 1;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_READ, ck, 1, 1, 5 * PG - 1, sg, 1,
	    bad, 1) == ENOBUFS);
	(void) rdk_dereg_mr(bad[0]);
	rdk_rw_ctx_free(ctx);

	/* A failed post returns its error; the caller then drains. */
	s.iwarp = B_FALSE;
	setup_device(&s);
	CHECK(rdk_rw_ctx_alloc(&dev, &a, &ctx) == 0);
	ck[0].dmac_size = 8192;
	sg[0].rs_len = 4096;
	sg[1].rs_len = 4096;
	CHECK(rdk_rw_init(ctx, &qp, RDK_RW_WRITE, ck, 1, 0, 8192, sg, 2,
	    NULL, 0) == 0);
	post_fail_at = 1;
	CHECK(rdk_rw_post(ctx, &cqe, NULL) == ENOMEM);
	post_fail_at = -1;
	rdk_rw_ctx_free(ctx);
}

int
main(int argc, char **argv)
{
	int count = argc > 1 ? atoi(argv[1]) : 20000;

	(void) strcpy(dev.rd_name, "fake0");
	dev.rd_ops = &fake_ops;
	dev.rd_num_comp_vectors = 1;
	pd.device = &dev;
	pd.local_dma_lkey = 0;
	qp.device = &dev;
	qp.pd = &pd;
	qp.qp_type = RDK_QPT_RC;

	bad_args();
	edge_shapes();
	random_shapes(count);
	(void) printf("PASS: rdk_rw chains for %d shapes\n", count + 7 * 8 * 3);
	return (0);
}

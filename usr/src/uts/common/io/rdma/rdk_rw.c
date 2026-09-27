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
 * RDMA READ and WRITE for a transfer between local DMA memory and a peer's
 * keyed segments; see rdk.h.  Written for illumos from the verbs semantics.
 *
 * Without an MR, each work request moves bytes of one remote segment and
 * gathers them from up to a request's worth of local pieces, a piece being
 * the part of a cookie within that segment.  With an MR, the local memory
 * is registered in groups of whole pages that rdk_map_mr_sg() takes at
 * once, each group ending at the page list's end, at a cookie that does not
 * continue the previous one's page, or at the end of the transfer; each
 * READ then has one local element within its group's MR.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/errno.h>
#include <sys/kmem.h>

#include "rdk_impl.h"

union rdk_rw_wr {
	struct rdk_send_wr	wr;
	struct rdk_rdma_wr	rdma;
	struct rdk_reg_wr	reg;
};

struct rdk_rw_ctx {
	struct rdk_device	*rw_dev;
	struct rdk_rw_attr	rw_attr;
	struct rdk_rw_limits	rw_lim;
	boolean_t		rw_iwarp;
	struct rdk_qp		*rw_qp;
	enum rdk_rw_dir		rw_dir;
	boolean_t		rw_built;
	uint_t			rw_nwr;
	uint_t			rw_nsge;
	uint_t			rw_nmr;
	boolean_t		rw_one_key;
	uint32_t		rw_key;
	union rdk_rw_wr		*rw_wrs;
	struct rdk_sge		*rw_sges;
	ddi_dma_cookie_t	*rw_ck;		/* a group's cookies */
};

/* A position in the local memory or in the remote segments. */
typedef struct rdk_rw_pos {
	uint_t		rp_i;
	uint64_t	rp_off;
} rdk_rw_pos_t;

static boolean_t
rdk_rw_read_mr(const struct rdk_rw_ctx *ctx)
{
	return (ctx->rw_iwarp || (ctx->rw_attr.rwa_flags & RDK_RW_F_MR) != 0);
}

static uint32_t
rdk_rw_sge_rd(struct rdk_device *dev, const struct rdk_rw_attr *a)
{
	return (MIN(a->rwa_max_sge, (uint32_t)dev->rd_attr.max_sge_rd));
}

/*
 * The most MRs a READ takes.  A run of cookies, each continuing the last
 * one's page, of x pages takes one group if x fits the page list, and else
 * a group per M - 1 pages after the first M: a full list leaves its last
 * page to the next group.  With R runs, x adds up to at most
 * ceil(len / PAGESIZE) + 2R - 1.
 */
static uint64_t
rdk_rw_max_groups(const struct rdk_rw_attr *a)
{
	uint64_t m = a->rwa_mr_pages, r = a->rwa_max_cookies;
	uint64_t x = howmany((uint64_t)a->rwa_max_len, PAGESIZE) + 2 * r - 1;

	if (r == 1)
		return (x <= m ? 1 : 1 + howmany(x - m, m - 1));
	return (howmany(x, m - 1) + r - 1);
}

static int
rdk_rw_bounds(struct rdk_device *dev, boolean_t iwarp,
    const struct rdk_rw_attr *a, struct rdk_rw_limits *l)
{
	const struct rdk_device_attr *da = &dev->rd_attr;
	boolean_t inv = (da->kernel_cap_flags & RDK_KCAP_READ_WITH_INV) == 0;
	uint64_t pieces, wrs, sges, groups = 0;

	bzero(l, sizeof (*l));
	if ((a->rwa_flags & ~RDK_RW_F_MR) != 0 || a->rwa_max_sge == 0 ||
	    a->rwa_max_sge > (uint32_t)da->max_send_sge ||
	    da->max_sge_rd < 1 || a->rwa_max_cookies == 0 ||
	    a->rwa_max_cookies > RDK_RW_MAX_LEN || a->rwa_max_segs == 0 ||
	    a->rwa_max_segs > RDK_RW_MAX_SEGS || a->rwa_max_len == 0 ||
	    a->rwa_max_len > RDK_RW_MAX_LEN || a->rwa_mr_pages == 1 ||
	    a->rwa_mr_pages > da->max_fast_reg_page_list_len)
		return (EINVAL);

	/* A remote segment boundary splits at most one cookie. */
	pieces = (uint64_t)a->rwa_max_cookies + a->rwa_max_segs - 1;
	wrs = pieces / a->rwa_max_sge + a->rwa_max_segs;
	sges = pieces;
	if (!iwarp && (a->rwa_flags & RDK_RW_F_MR) == 0) {
		wrs = MAX(wrs, pieces / rdk_rw_sge_rd(dev, a) +
		    a->rwa_max_segs);
	} else if (a->rwa_mr_pages != 0 &&
	    (da->device_cap_flags & RDK_DEVICE_MEM_MGT_EXTENSIONS) != 0) {
		groups = rdk_rw_max_groups(a);
		wrs = MAX(wrs, groups * (inv ? 3 : 2) + a->rwa_max_segs - 1);
		sges = MAX(sges, groups + a->rwa_max_segs - 1);
	}
	/* One transfer must fit a send queue. */
	if (wrs > (uint64_t)da->max_qp_wr)
		return (E2BIG);
	l->rwl_wrs = (uint32_t)wrs;
	l->rwl_mrs = (uint32_t)groups;
	l->rwl_sges = (uint32_t)sges;
	return (0);
}

int
rdk_rw_limits(struct rdk_device *dev, const struct rdk_rw_attr *a,
    struct rdk_rw_limits *l)
{
	return (rdk_rw_bounds(dev, rdk_device_iwarp(dev), a, l));
}

int
rdk_rw_ctx_alloc(struct rdk_device *dev, const struct rdk_rw_attr *a,
    rdk_rw_ctx_t **ctxp)
{
	struct rdk_rw_ctx *ctx;
	struct rdk_rw_limits l;
	boolean_t iwarp = rdk_device_iwarp(dev);
	int ret;

	*ctxp = NULL;
	if ((ret = rdk_rw_bounds(dev, iwarp, a, &l)) != 0)
		return (ret);
	ctx = kmem_zalloc(sizeof (*ctx), KM_SLEEP);
	ctx->rw_dev = dev;
	ctx->rw_attr = *a;
	ctx->rw_lim = l;
	ctx->rw_iwarp = iwarp;
	ctx->rw_wrs = kmem_zalloc(sizeof (union rdk_rw_wr) * l.rwl_wrs,
	    KM_SLEEP);
	ctx->rw_sges = kmem_zalloc(sizeof (struct rdk_sge) * l.rwl_sges,
	    KM_SLEEP);
	if (l.rwl_mrs != 0) {
		ctx->rw_ck = kmem_zalloc(sizeof (ddi_dma_cookie_t) *
		    a->rwa_max_cookies, KM_SLEEP);
	}
	*ctxp = ctx;
	return (0);
}

void
rdk_rw_ctx_free(rdk_rw_ctx_t *ctx)
{
	if (ctx->rw_ck != NULL) {
		kmem_free(ctx->rw_ck, sizeof (ddi_dma_cookie_t) *
		    ctx->rw_attr.rwa_max_cookies);
	}
	kmem_free(ctx->rw_sges, sizeof (struct rdk_sge) *
	    ctx->rw_lim.rwl_sges);
	kmem_free(ctx->rw_wrs, sizeof (union rdk_rw_wr) *
	    ctx->rw_lim.rwl_wrs);
	kmem_free(ctx, sizeof (*ctx));
}

static union rdk_rw_wr *
rdk_rw_new_wr(struct rdk_rw_ctx *ctx, enum rdk_wr_opcode op)
{
	union rdk_rw_wr *w;

	if (ctx->rw_nwr == ctx->rw_lim.rwl_wrs)
		return (NULL);
	w = &ctx->rw_wrs[ctx->rw_nwr++];
	bzero(w, sizeof (*w));
	w->wr.opcode = op;
	return (w);
}

static struct rdk_sge *
rdk_rw_new_sge(struct rdk_rw_ctx *ctx)
{
	if (ctx->rw_nsge == ctx->rw_lim.rwl_sges)
		return (NULL);
	return (&ctx->rw_sges[ctx->rw_nsge++]);
}

/* Move a remote position n bytes on; n never crosses a segment's end. */
static void
rdk_rw_seg_advance(const struct rdk_rw_seg *segs, rdk_rw_pos_t *rp,
    uint64_t n)
{
	rp->rp_off += n;
	if (rp->rp_off == segs[rp->rp_i].rs_len) {
		rp->rp_i++;
		rp->rp_off = 0;
	}
}

static void
rdk_rw_ck_advance(const ddi_dma_cookie_t *ck, rdk_rw_pos_t *lp, uint64_t n)
{
	while (n != 0) {
		uint64_t left = ck[lp->rp_i].dmac_size - lp->rp_off;

		if (n < left) {
			lp->rp_off += n;
			return;
		}
		n -= left;
		lp->rp_i++;
		lp->rp_off = 0;
	}
}

/*
 * Work requests that each move part of one remote segment, gathering up to
 * nsge local pieces with the local DMA lkey.
 */
static int
rdk_rw_build_sge(struct rdk_rw_ctx *ctx, const ddi_dma_cookie_t *ck,
    uint64_t off, uint64_t len, const struct rdk_rw_seg *segs, uint32_t nsge)
{
	enum rdk_wr_opcode op = ctx->rw_dir == RDK_RW_WRITE ?
	    RDK_WR_RDMA_WRITE : RDK_WR_RDMA_READ;
	uint32_t lkey = ctx->rw_qp->pd->local_dma_lkey;
	rdk_rw_pos_t lp = { 0, off }, rp = { 0, 0 };
	union rdk_rw_wr *w;
	struct rdk_sge *sge;

	/* Start at the cookie that holds the first byte. */
	while (lp.rp_off >= ck[lp.rp_i].dmac_size) {
		lp.rp_off -= ck[lp.rp_i].dmac_size;
		lp.rp_i++;
	}
	while (len != 0) {
		const struct rdk_rw_seg *s = &segs[rp.rp_i];
		uint64_t seg_left = s->rs_len - rp.rp_off, wlen = 0;

		if ((w = rdk_rw_new_wr(ctx, op)) == NULL)
			return (E2BIG);
		w->rdma.remote_addr = s->rs_addr + rp.rp_off;
		w->rdma.rkey = s->rs_key;
		w->wr.sg_list = &ctx->rw_sges[ctx->rw_nsge];
		while (w->wr.num_sge < (int)nsge && wlen < seg_left) {
			uint64_t piece = MIN(seg_left - wlen,
			    ck[lp.rp_i].dmac_size - lp.rp_off);

			if ((sge = rdk_rw_new_sge(ctx)) == NULL)
				return (E2BIG);
			sge->addr = ck[lp.rp_i].dmac_laddress + lp.rp_off;
			sge->length = (uint32_t)piece;
			sge->lkey = lkey;
			w->wr.num_sge++;
			wlen += piece;
			rdk_rw_ck_advance(ck, &lp, piece);
		}
		rdk_rw_seg_advance(segs, &rp, wlen);
		len -= wlen;
	}
	return (0);
}

/*
 * The cookies from lp that hold the next len bytes, the last one cut to
 * end with them, so that the MR covers nothing past the transfer.
 */
static uint_t
rdk_rw_group_cookies(struct rdk_rw_ctx *ctx, const ddi_dma_cookie_t *ck,
    const rdk_rw_pos_t *lp, uint64_t len)
{
	uint64_t have = 0;
	uint_t n = 0, i;

	for (i = lp->rp_i; have < len; i++) {
		ctx->rw_ck[n] = ck[i];
		have += ck[i].dmac_size - (n == 0 ? lp->rp_off : 0);
		n++;
	}
	ctx->rw_ck[n - 1].dmac_size -= have - len;
	return (n);
}

/*
 * READs into the MRs lent: per group a registration, the READs and an
 * invalidation.
 */
static int
rdk_rw_build_mr(struct rdk_rw_ctx *ctx, const ddi_dma_cookie_t *ck,
    uint64_t off, uint64_t len, const struct rdk_rw_seg *segs,
    struct rdk_mr *const *mrs, uint_t nmrs)
{
	const boolean_t rdinv = (ctx->rw_dev->rd_attr.kernel_cap_flags &
	    RDK_KCAP_READ_WITH_INV) != 0;
	const int access = RDK_ACCESS_LOCAL_WRITE |
	    (ctx->rw_iwarp ? RDK_ACCESS_REMOTE_WRITE : 0);
	rdk_rw_pos_t lp = { 0, off }, rp = { 0, 0 };
	union rdk_rw_wr *w, *last;
	struct rdk_sge *sge;
	struct rdk_mr *mr;
	uint64_t moff, glen, gpos;
	uint_t n, k;
	int ret;

	while (lp.rp_off >= ck[lp.rp_i].dmac_size) {
		lp.rp_off -= ck[lp.rp_i].dmac_size;
		lp.rp_i++;
	}
	while (len != 0) {
		if (ctx->rw_nmr == nmrs)
			return (ENOBUFS);
		mr = mrs[ctx->rw_nmr];
		/* A lent MR maps one group: its REG reads the MR at post. */
		for (k = 0; k < ctx->rw_nmr; k++) {
			if (mrs[k] == mr)
				return (EINVAL);
		}
		ctx->rw_nmr++;
		rdk_update_fast_reg_key(mr, (uint8_t)rdk_inc_rkey(mr->rkey));
		n = rdk_rw_group_cookies(ctx, ck, &lp, len);
		moff = lp.rp_off;
		ret = rdk_map_mr_sg(mr, ctx->rw_ck, n, &moff, PAGESIZE);
		if (ret < 0)
			return (-ret);
		if (mr->length == 0 || mr->length > len)
			return (EINVAL);
		glen = mr->length;

		if ((w = rdk_rw_new_wr(ctx, RDK_WR_REG_MR)) == NULL)
			return (E2BIG);
		w->reg.mr = mr;
		w->reg.key = mr->rkey;
		w->reg.access = access;

		last = NULL;
		for (gpos = 0; gpos < glen; ) {
			const struct rdk_rw_seg *s = &segs[rp.rp_i];
			uint64_t piece = MIN(glen - gpos,
			    s->rs_len - rp.rp_off);

			if ((w = rdk_rw_new_wr(ctx, RDK_WR_RDMA_READ)) ==
			    NULL || (sge = rdk_rw_new_sge(ctx)) == NULL)
				return (E2BIG);
			w->rdma.remote_addr = s->rs_addr + rp.rp_off;
			w->rdma.rkey = s->rs_key;
			sge->addr = mr->iova + gpos;
			sge->length = (uint32_t)piece;
			sge->lkey = mr->lkey;
			w->wr.sg_list = sge;
			w->wr.num_sge = 1;
			rdk_rw_seg_advance(segs, &rp, piece);
			gpos += piece;
			last = w;
		}
		if (rdinv) {
			last->wr.opcode = RDK_WR_RDMA_READ_WITH_INV;
			last->wr.ex.invalidate_rkey = mr->lkey;
		} else {
			/* The fence holds it until the READs have landed. */
			if ((w = rdk_rw_new_wr(ctx, RDK_WR_LOCAL_INV)) == NULL)
				return (E2BIG);
			w->wr.send_flags = RDK_SEND_FENCE;
			w->wr.ex.invalidate_rkey = mr->rkey;
		}
		rdk_rw_ck_advance(ck, &lp, glen);
		len -= glen;
	}
	return (0);
}

/*
 * Build the transfer of len bytes at off in the local cookies to or from
 * the remote segments.  The caller's lengths are checked here, never
 * trusted: a transfer the context was not sized for is refused.
 */
int
rdk_rw_init(rdk_rw_ctx_t *ctx, struct rdk_qp *qp, enum rdk_rw_dir dir,
    const ddi_dma_cookie_t *ck, uint_t nck, uint64_t off, uint32_t len,
    const struct rdk_rw_seg *segs, uint_t nsegs, struct rdk_mr *const *mrs,
    uint_t nmrs)
{
	const struct rdk_rw_attr *a = &ctx->rw_attr;
	uint64_t total = 0, end;
	boolean_t mr;
	uint_t i;
	int ret;

	ctx->rw_built = B_FALSE;
	ctx->rw_one_key = B_FALSE;
	ctx->rw_nwr = ctx->rw_nsge = ctx->rw_nmr = 0;
	if (qp == NULL || qp->device != ctx->rw_dev ||
	    qp->qp_type != RDK_QPT_RC ||
	    (dir != RDK_RW_WRITE && dir != RDK_RW_READ) ||
	    ck == NULL || nck == 0 || nck > a->rwa_max_cookies ||
	    segs == NULL || nsegs == 0 || nsegs > a->rwa_max_segs ||
	    len == 0 || len > a->rwa_max_len)
		return (EINVAL);
	for (i = 0; i < nck; i++) {
		end = ck[i].dmac_laddress + ck[i].dmac_size;
		if (ck[i].dmac_size == 0 || end < ck[i].dmac_laddress ||
		    total + ck[i].dmac_size < total)
			return (EINVAL);
		total += ck[i].dmac_size;
	}
	if (off > total || len > total - off)
		return (EINVAL);
	total = 0;
	for (i = 0; i < nsegs; i++) {
		if (segs[i].rs_len == 0 ||
		    segs[i].rs_addr + segs[i].rs_len < segs[i].rs_addr)
			return (EINVAL);
		total += segs[i].rs_len;
	}
	if (total != len)
		return (EINVAL);

	ctx->rw_qp = qp;
	ctx->rw_dir = dir;
	mr = dir == RDK_RW_READ && rdk_rw_read_mr(ctx);
	if (mr) {
		if (ctx->rw_lim.rwl_mrs == 0)
			return (ENOTSUP);
		if ((mrs == NULL && nmrs != 0) || nmrs > ctx->rw_lim.rwl_mrs)
			return (EINVAL);
		for (i = 0; i < nmrs; i++) {
			if (mrs[i] == NULL || mrs[i]->device != ctx->rw_dev ||
			    mrs[i]->pd != qp->pd)
				return (EINVAL);
		}
		ret = rdk_rw_build_mr(ctx, ck, off, len, segs, mrs, nmrs);
	} else {
		ret = rdk_rw_build_sge(ctx, ck, off, len, segs,
		    dir == RDK_RW_WRITE ? a->rwa_max_sge :
		    rdk_rw_sge_rd(ctx->rw_dev, a));
	}
	if (ret != 0)
		return (ret);

	ctx->rw_one_key = B_TRUE;
	ctx->rw_key = segs[0].rs_key;
	for (i = 1; i < nsegs; i++) {
		if (segs[i].rs_key != ctx->rw_key)
			ctx->rw_one_key = B_FALSE;
	}
	ctx->rw_built = B_TRUE;
	return (0);
}

/*
 * Post the transfer: every request carries cqe, the last is signaled, and
 * next follows a WRITE in the same call.  The context must be built again
 * before another post, which gives each registration a new key.
 */
int
rdk_rw_post(rdk_rw_ctx_t *ctx, struct rdk_cqe *cqe, struct rdk_send_wr *next)
{
	uint_t i;

	if (!ctx->rw_built || cqe == NULL ||
	    (next != NULL && ctx->rw_dir != RDK_RW_WRITE))
		return (EINVAL);
	ctx->rw_built = B_FALSE;
	for (i = 0; i < ctx->rw_nwr; i++) {
		ctx->rw_wrs[i].wr.wr_cqe = cqe;
		ctx->rw_wrs[i].wr.next = i + 1 < ctx->rw_nwr ?
		    &ctx->rw_wrs[i + 1].wr : next;
	}
	ctx->rw_wrs[ctx->rw_nwr - 1].wr.send_flags |= RDK_SEND_SIGNALED;
	return (rdk_post_send(ctx->rw_qp, &ctx->rw_wrs[0].wr, NULL));
}

uint_t
rdk_rw_nwr(const rdk_rw_ctx_t *ctx)
{
	return (ctx->rw_nwr);
}

uint_t
rdk_rw_nmr(const rdk_rw_ctx_t *ctx)
{
	return (ctx->rw_nmr);
}

/* Only a SEND of a transfer that named a single key is changed. */
boolean_t
rdk_rw_send_inv(const rdk_rw_ctx_t *ctx, struct rdk_send_wr *wr)
{
	if (!ctx->rw_one_key ||
	    (ctx->rw_dev->rd_attr.device_cap_flags &
	    RDK_DEVICE_MEM_MGT_EXTENSIONS) == 0 ||
	    (wr->opcode != RDK_WR_SEND && wr->opcode != RDK_WR_SEND_WITH_INV))
		return (B_FALSE);
	wr->opcode = RDK_WR_SEND_WITH_INV;
	wr->ex.invalidate_rkey = ctx->rw_key;
	return (B_TRUE);
}

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
 * Run nvmf_rdma.c and nvmf_rdma_xfer.c, with rdk_rw and the rdmak CQ and
 * teardown code, against a model of the device and the host, with a small
 * nvmft in front.  The device executes each work request in order: it
 * moves bytes only between registered local memory and the host region
 * the key names, within that region and with the access the host gave it,
 * and a READ sink on iWARP must be a valid MR.  The host sends its next
 * command as soon as a response lands, and a SEND that finds no RECV is a
 * failure.  A response must find its read data in place.
 *
 *	rdma_datapath plan	transfers of every shape on RoCE and iWARP
 *	rdma_datapath credit	RECV, context and send queue credit
 *	rdma_datapath teardown	error and dead-device teardown order
 */

#include "nr_unit.h"

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		abort();						\
	}								\
} while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (rng);
}

static uint32_t
rndr(uint32_t lo, uint32_t hi)
{
	return (lo + (uint32_t)(rnd() % (hi - lo + 1)));
}

/*
 * rdmak and nvmf_rdma pieces that are not built here.
 */
static boolean_t fake_iwarp;
static struct rdk_device dev;

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
rdk_device_tainted(const struct rdk_device *d)
{
	return (d->rd_tainted != 0);
}
boolean_t
rdk_device_iwarp(struct rdk_device *d)
{
	(void) d;
	return (fake_iwarp);
}
void rdk_cm_roce_qp_gone(struct rdk_qp *qp) { (void) qp; }
uint32_t nvmf_rdma_dbuf_max = 128 * 1024;
taskq_t *nvmf_rdma_taskq;

/* The event log that the teardown order is checked against. */
enum { EV_DESTROY_QP = 1, EV_XFER_ERR, EV_XFER_OK, EV_REPORT, EV_DETACH,
    EV_CMID };
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static int evlog[1 << 16];
static int nev;

static void
ev(int e)
{
	(void) pthread_mutex_lock(&ev_lock);
	CHECK(nev < (int)ARRAY_SIZE(evlog));
	evlog[nev++] = e;
	(void) pthread_mutex_unlock(&ev_lock);
}

int
rdk_cm_destroy_id(rdk_cm_id_t *id)
{
	(void) id;
	ev(EV_CMID);
	return (0);
}

void
nr_queue_detach(nr_queue_t *q)
{
	(void) q;
	ev(EV_DETACH);
}

void
nr_queue_connected(nr_queue_t *q)
{
	(void) q;
}

void
nr_queue_gone(nr_queue_t *q)
{
	(void) q;
}

/*
 * Memory the device may reach: every DMA buffer and pool buffer.  Its
 * physical address differs from its virtual one, so that the device finds
 * nothing at a virtual address and the CPU nothing at a physical one.
 */
#define	DM_SKEW	(1ULL << 62)
#define	DM_PA(va)	((uint64_t)(uintptr_t)(va) ^ DM_SKEW)
#define	DM_VA(pa)	((void *)(uintptr_t)((pa) ^ DM_SKEW))

#define	NDMA	8192
static pthread_mutex_t dm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t pa; size_t len; } dm[NDMA];

static void *
dm_alloc(size_t len)
{
	void *p;
	int i;

	CHECK(posix_memalign(&p, 4096, P2ROUNDUP(len, 4096)) == 0);
	memset(p, 0xa5, len);
	(void) pthread_mutex_lock(&dm_lock);
	for (i = 0; i < NDMA && dm[i].len != 0; i++)
		;
	CHECK(i < NDMA);
	dm[i].pa = DM_PA(p);
	dm[i].len = len;
	(void) pthread_mutex_unlock(&dm_lock);
	return (p);
}

static void
dm_free(void *p)
{
	int i;

	(void) pthread_mutex_lock(&dm_lock);
	for (i = 0; i < NDMA && dm[i].pa != DM_PA(p); i++)
		;
	CHECK(i < NDMA);
	dm[i].pa = 0;
	dm[i].len = 0;
	(void) pthread_mutex_unlock(&dm_lock);
	free(p);
}

static boolean_t
dm_ok(uint64_t pa, uint64_t len)
{
	boolean_t ok = B_FALSE;
	int i;

	(void) pthread_mutex_lock(&dm_lock);
	for (i = 0; i < NDMA && !ok; i++) {
		ok = dm[i].len != 0 && pa >= dm[i].pa && len <= dm[i].len &&
		    pa - dm[i].pa <= dm[i].len - len;
	}
	(void) pthread_mutex_unlock(&dm_lock);
	return (ok);
}

int
rdk_dma_buf_alloc(struct rdk_device *d, size_t len, rdk_dma_buf_t *b)
{
	(void) d;
	bzero(b, sizeof (*b));
	b->rdb_va = dm_alloc(len);
	b->rdb_pa = DM_PA(b->rdb_va);
	b->rdb_len = len;
	return (0);
}

void
rdk_dma_buf_free(struct rdk_device *d, rdk_dma_buf_t *b)
{
	(void) d;
	if (b->rdb_va != NULL)
		dm_free(b->rdb_va);
	bzero(b, sizeof (*b));
}

static int pool_bufs;

nr_buf_t *
nr_buf_alloc(nr_dev_t *nd, size_t len, size_t min_len)
{
	nr_buf_t *b;

	len = MIN(len, nvmf_rdma_dbuf_max);
	if (len == 0 || min_len > len)
		return (NULL);
	b = kmem_zalloc(sizeof (*b), KM_SLEEP);
	b->nb_dev = nd;
	b->nb_len = len;
	b->nb_va = dm_alloc(len);
	b->nb_ck.dmac_laddress = DM_PA(b->nb_va);
	b->nb_ck.dmac_size = len;
	__atomic_add_fetch(&pool_bufs, 1, __ATOMIC_SEQ_CST);
	return (b);
}

void
nr_buf_free(nr_buf_t *b)
{
	dm_free(b->nb_va);
	kmem_free(b, sizeof (*b));
	__atomic_sub_fetch(&pool_bufs, 1, __ATOMIC_SEQ_CST);
}

int
nr_alloc_data_buf(struct nvmf_qpair *nq, size_t len, size_t min_len,
    nvmf_databuf_t *db)
{
	nr_buf_t *b = nr_buf_alloc(NR_Q(nq)->nq_dev, len, min_len);

	if (b == NULL)
		return (ENOMEM);
	db->ndb_addr = b->nb_va;
	db->ndb_len = b->nb_len;
	db->ndb_cookies = &b->nb_ck;
	db->ndb_ncookies = 1;
	db->ndb_priv = b;
	return (0);
}

void
nr_free_data_buf(nvmf_databuf_t *db)
{
	nr_buf_free(db->ndb_priv);
}

/*
 * The host: each command's data lives in a region the device finds by its
 * key, with the access the command gives the target.
 */
#define	HMAX		256
#define	H_READ		1	/* the target may READ it (host write) */
#define	H_WRITE		2	/* the target may WRITE it (host read) */
#define	OPC_FLUSH	0x00
#define	OPC_WRITE	0x01
#define	OPC_READ	0x02
#define	OPC_VREAD	0x06	/* a read that nvmft serves from kmem */

typedef struct hcmd {
	int		h_used;
	uint16_t	h_cid;
	uint8_t		h_opc;
	uint32_t	h_len;
	uint32_t	h_icd;
	uint32_t	h_key;
	uint64_t	h_addr;
	int		h_access;
	int		h_inv;
	uint8_t		h_sgl[16];
	uint8_t		*h_buf;
	uint8_t		*h_want;
	uint16_t	h_expect;	/* status a bad SGL must get */
	int		h_done;
} hcmd_t;

static struct {
	pthread_mutex_t	m;
	pthread_cond_t	cv;
	pthread_cond_t	hcv;
	pthread_t	thr;
	int		stop;
	/* The one QP and its CQ. */
	struct fqp	*qp;
	/* Host state. */
	hcmd_t		cmds[HMAX];
	int		depth;
	int		outstanding;
	int		to_send;
	int		sent;
	int		done;
	int		failed;
	uint16_t	next_cid;
	int		mix;
	int		bad_sgls;
	int		rnr;
	int		sq_over;
	int		remote_read_mr;
	int		wrong_inv;
	int		early_rsp;
	uint32_t	icd;
	int		fail_at;	/* the RDMA request that fails */
	int		hang_after;	/* WRs until the device stops */
	int		executed;
	int		lag;		/* requests between completion events */
	int		slow_us;	/* the device's time per request */
	int		nop[16];	/* executed, by opcode */
	uint64_t	bytes;
} M;

/* The device. */
#define	DMA_LKEY	0x0d0a
#define	MAXPG		128
#define	MAXMR		4096

struct fcq {
	struct rdk_cq	fc_cq;
	pthread_mutex_t	fc_lock;
	struct rdk_wc	*fc_q;
	int		fc_cap;
	int		fc_n;
};

struct fmr {
	struct rdk_mr	fm_mr;
	int		fm_cap;
	int		fm_n;
	uint64_t	fm_pages[MAXPG];
	int		fm_valid;
	uint32_t	fm_key;
	int		fm_access;
	uint64_t	fm_iova;
	uint64_t	fm_len;
	uint64_t	fm_hw[MAXPG];
	int		fm_hwn;
};

static struct fmr *mrs[MAXMR];
static int nmrs;

typedef struct swr {
	struct rdk_send_wr	w;
	struct rdk_sge		sge[8];
	uint64_t		raddr;
	uint32_t		rkey;
	struct fmr		*mr;
	uint32_t		key;
	int			access;
	uint64_t		iova;
	uint64_t		len;
	uint64_t		pages[MAXPG];
	int			npages;
	uint8_t			inl[16];
} swr_t;

typedef struct rwr {
	struct rdk_cqe		*cqe;
	struct rdk_sge		sge;
} rwr_t;

struct fqp {
	struct rdk_qp	fq_qp;
	int		fq_err;
	int		fq_dead;
	int		fq_hang;
	uint32_t	fq_sq_cap;
	uint32_t	fq_rq_cap;
	swr_t		*fq_sq;
	int		fq_sq_n;
	uint32_t	fq_sq_used;	/* posted and not yet polled */
	uint32_t	fq_sq_peak;
	uint32_t	fq_unsig;	/* unsignaled since the last CQE */
	rwr_t		*fq_rq;
	int		fq_rq_n;
};

static int
fake_create_cq(struct rdk_cq *cq, const struct rdk_cq_init_attr *attr)
{
	struct fcq *fc = (struct fcq *)cq;

	(void) pthread_mutex_init(&fc->fc_lock, NULL);
	fc->fc_cap = (int)attr->cqe;
	fc->fc_q = calloc(fc->fc_cap, sizeof (struct rdk_wc));
	return (0);
}

static void
fake_destroy_cq(struct rdk_cq *cq)
{
	struct fcq *fc = (struct fcq *)cq;

	free(fc->fc_q);
	(void) pthread_mutex_destroy(&fc->fc_lock);
}

static int
fake_poll_cq(struct rdk_cq *cq, int n, struct rdk_wc *wc)
{
	struct fcq *fc = (struct fcq *)cq;
	uint32_t slots = 0;
	int i;

	(void) pthread_mutex_lock(&fc->fc_lock);
	n = MIN(n, fc->fc_n);
	for (i = 0; i < n; i++) {
		wc[i] = fc->fc_q[i];
		slots += wc[i].vendor_err;
	}
	(void) memmove(fc->fc_q, fc->fc_q + n,
	    (fc->fc_n - n) * sizeof (fc->fc_q[0]));
	fc->fc_n -= n;
	(void) pthread_mutex_unlock(&fc->fc_lock);
	/* cq_push takes fc_lock with M.m held. */
	if (slots != 0) {
		(void) pthread_mutex_lock(&M.m);
		if (M.qp != NULL)
			M.qp->fq_sq_used -= slots;
		(void) pthread_mutex_unlock(&M.m);
	}
	return (n);
}

static int
fake_req_notify(struct rdk_cq *cq, enum rdk_cq_notify_flags flags)
{
	struct fcq *fc = (struct fcq *)cq;
	int more;

	(void) pthread_mutex_lock(&fc->fc_lock);
	more = fc->fc_n > 0;
	(void) pthread_mutex_unlock(&fc->fc_lock);
	return ((flags & RDK_CQ_REPORT_MISSED_EVENTS) != 0 && more ? 1 : 0);
}

static void
cq_push(struct rdk_cq *cq, struct rdk_cqe *cqe, enum rdk_wc_status st,
    enum rdk_wc_opcode op, uint32_t len)
{
	struct fcq *fc = (struct fcq *)cq;
	struct rdk_wc *wc;
	uint32_t slots = 0;

	/* A send completion frees its slot and those of unsignaled ones. */
	if (op != RDK_WC_RECV && M.qp != NULL) {
		slots = M.qp->fq_unsig + 1;
		M.qp->fq_unsig = 0;
	}

	(void) pthread_mutex_lock(&fc->fc_lock);
	CHECK(fc->fc_n < fc->fc_cap);
	wc = &fc->fc_q[fc->fc_n++];
	bzero(wc, sizeof (*wc));
	wc->wr_cqe = cqe;
	wc->status = st;
	wc->opcode = op;
	wc->byte_len = len;
	wc->vendor_err = slots;
	(void) pthread_mutex_unlock(&fc->fc_lock);
}

static int fake_alloc_pd(struct rdk_pd *pd) { pd->local_dma_lkey = DMA_LKEY;
    return (0); }
static void fake_dealloc_pd(struct rdk_pd *pd) { (void) pd; }

static int
fake_create_qp(struct rdk_qp *qp, struct rdk_qp_init_attr *init)
{
	struct fqp *fq = (struct fqp *)qp;

	CHECK(init->send_cq != NULL && init->recv_cq == init->send_cq);
	fq->fq_sq_cap = init->cap.max_send_wr;
	fq->fq_rq_cap = init->cap.max_recv_wr;
	fq->fq_sq = calloc(fq->fq_sq_cap, sizeof (swr_t));
	fq->fq_rq = calloc(fq->fq_rq_cap, sizeof (rwr_t));
	qp->qp_num = 7;
	(void) pthread_mutex_lock(&M.m);
	M.qp = fq;
	(void) pthread_mutex_unlock(&M.m);
	return (0);
}

static void
fake_destroy_qp(struct rdk_qp *qp)
{
	struct fqp *fq = (struct fqp *)qp;

	(void) pthread_mutex_lock(&M.m);
	M.qp = NULL;
	(void) pthread_mutex_unlock(&M.m);
	free(fq->fq_sq);
	free(fq->fq_rq);
	ev(EV_DESTROY_QP);
}

static int
fake_modify_qp(struct rdk_qp *qp, struct rdk_qp_attr *a, int mask)
{
	struct fqp *fq = (struct fqp *)qp;

	if ((mask & RDK_QP_STATE) != 0 && a->qp_state == RDK_QPS_ERR) {
		(void) pthread_mutex_lock(&M.m);
		fq->fq_err = 1;
		(void) pthread_cond_broadcast(&M.cv);
		(void) pthread_mutex_unlock(&M.m);
	}
	return (0);
}

static int
fake_query_qp(struct rdk_qp *qp, struct rdk_qp_attr *a, int mask,
    struct rdk_qp_init_attr *init)
{
	(void) mask; (void) init;
	a->qp_state = ((struct fqp *)qp)->fq_err ? RDK_QPS_ERR : RDK_QPS_RTS;
	return (0);
}

static int
fake_post_send(struct rdk_qp *qp, const struct rdk_send_wr *wr,
    const struct rdk_send_wr **bad)
{
	struct fqp *fq = (struct fqp *)qp;
	swr_t *s;
	int i;

	(void) bad;
	(void) pthread_mutex_lock(&M.m);
	for (; wr != NULL; wr = wr->next) {
		if (fq->fq_sq_used >= fq->fq_sq_cap) {
			M.sq_over++;
			(void) pthread_mutex_unlock(&M.m);
			return (ENOMEM);
		}
		fq->fq_sq_used++;
		fq->fq_sq_peak = MAX(fq->fq_sq_peak, fq->fq_sq_used);
		s = &fq->fq_sq[fq->fq_sq_n++];
		bzero(s, sizeof (*s));
		s->w = *wr;
		CHECK(wr->num_sge <= 8);
		for (i = 0; i < wr->num_sge; i++)
			s->sge[i] = wr->sg_list[i];
		if (wr->opcode == RDK_WR_RDMA_WRITE ||
		    wr->opcode == RDK_WR_RDMA_READ ||
		    wr->opcode == RDK_WR_RDMA_READ_WITH_INV) {
			s->raddr = RDK_RDMA_WR(wr)->remote_addr;
			s->rkey = RDK_RDMA_WR(wr)->rkey;
		} else if (wr->opcode == RDK_WR_REG_MR) {
			struct fmr *fm = (struct fmr *)RDK_REG_WR(wr)->mr;

			s->mr = fm;
			s->key = RDK_REG_WR(wr)->key;
			s->access = RDK_REG_WR(wr)->access;
			s->iova = fm->fm_mr.iova;
			s->len = fm->fm_mr.length;
			s->npages = fm->fm_n;
			memcpy(s->pages, fm->fm_pages, sizeof (s->pages));
		} else if ((wr->send_flags & RDK_SEND_INLINE) != 0) {
			CHECK(wr->num_sge == 1 && wr->sg_list[0].length <= 16);
			/* The provider copies inline data from a kernel VA. */
			CHECK(dm_ok(DM_PA(wr->sg_list[0].addr),
			    wr->sg_list[0].length));
			memcpy(s->inl, (void *)(uintptr_t)wr->sg_list[0].addr,
			    wr->sg_list[0].length);
		}
	}
	(void) pthread_cond_broadcast(&M.cv);
	(void) pthread_mutex_unlock(&M.m);
	return (0);
}

static int
fake_post_recv(struct rdk_qp *qp, const struct rdk_recv_wr *wr,
    const struct rdk_recv_wr **bad)
{
	struct fqp *fq = (struct fqp *)qp;

	(void) bad;
	(void) pthread_mutex_lock(&M.m);
	for (; wr != NULL; wr = wr->next) {
		CHECK((uint32_t)fq->fq_rq_n < fq->fq_rq_cap);
		fq->fq_rq[fq->fq_rq_n].cqe = wr->wr_cqe;
		if (wr->num_sge == 1)
			fq->fq_rq[fq->fq_rq_n].sge = wr->sg_list[0];
		fq->fq_rq_n++;
	}
	(void) pthread_cond_broadcast(&M.cv);
	(void) pthread_mutex_unlock(&M.m);
	return (0);
}

static int
fake_set_page(struct rdk_mr *mr, uint64_t addr)
{
	struct fmr *fm = (struct fmr *)mr;

	if (fm->fm_n >= fm->fm_cap)
		return (-ENOMEM);
	fm->fm_pages[fm->fm_n++] = addr;
	return (0);
}

static int
fake_map_mr_sg(struct rdk_mr *mr, const ddi_dma_cookie_t *ck, uint_t n,
    uint64_t *off)
{
	((struct fmr *)mr)->fm_n = 0;
	return (rdk_sg_to_pages(mr, ck, n, off, fake_set_page));
}

static int
fake_alloc_mr(struct rdk_pd *pd, enum rdk_mr_type t, uint32_t n,
    struct rdk_mr **mrp)
{
	static uint32_t idx = 1;
	struct fmr *fm = kmem_zalloc(sizeof (*fm), KM_SLEEP);

	(void) pd; (void) t;
	CHECK(n <= MAXPG);
	fm->fm_cap = (int)n;
	(void) pthread_mutex_lock(&M.m);
	fm->fm_mr.lkey = fm->fm_mr.rkey = (idx++ << 8) | 0x10;
	CHECK(nmrs < MAXMR);
	mrs[nmrs++] = fm;
	(void) pthread_mutex_unlock(&M.m);
	*mrp = &fm->fm_mr;
	return (0);
}

static int
fake_dereg_mr(struct rdk_mr *mr)
{
	int i;

	(void) pthread_mutex_lock(&M.m);
	for (i = 0; i < nmrs && mrs[i] != (struct fmr *)mr; i++)
		;
	CHECK(i < nmrs);
	mrs[i] = mrs[--nmrs];
	(void) pthread_mutex_unlock(&M.m);
	kmem_free(mr, sizeof (struct fmr));
	return (0);
}

static const struct rdk_device_ops fake_ops = {
	.version = RDK_ABI_VERSION,
	.alloc_pd = fake_alloc_pd,
	.dealloc_pd = fake_dealloc_pd,
	.create_cq = fake_create_cq,
	.destroy_cq = fake_destroy_cq,
	.poll_cq = fake_poll_cq,
	.req_notify_cq = fake_req_notify,
	.create_qp = fake_create_qp,
	.modify_qp = fake_modify_qp,
	.query_qp = fake_query_qp,
	.destroy_qp = fake_destroy_qp,
	.post_send = fake_post_send,
	.post_recv = fake_post_recv,
	.alloc_mr = fake_alloc_mr,
	.map_mr_sg = fake_map_mr_sg,
	.dereg_mr = fake_dereg_mr,
	.size_pd = sizeof (struct rdk_pd),
	.size_cq = sizeof (struct fcq),
	.size_qp = sizeof (struct fqp),
	.size_ah = sizeof (struct rdk_ah)
};

/* The host region a key names, or NULL. */
static hcmd_t *
host_region(uint32_t key)
{
	int i;

	for (i = 0; i < HMAX; i++) {
		if (M.cmds[i].h_used && !M.cmds[i].h_done &&
		    M.cmds[i].h_key == key && M.cmds[i].h_len != 0 &&
		    M.cmds[i].h_icd == 0)
			return (&M.cmds[i]);
	}
	return (NULL);
}

static struct fmr *
mr_by_key(uint32_t key)
{
	int i;

	for (i = 0; i < nmrs; i++) {
		if (mrs[i]->fm_valid && mrs[i]->fm_key == key)
			return (mrs[i]);
	}
	return (NULL);
}

/* The local bytes of one READ sink element, through its MR on iWARP. */
static uint8_t *
sink(const struct rdk_sge *sg, enum rdk_wc_status *st)
{
	struct fmr *fm;
	uint64_t off;
	int pg;

	if (sg->lkey == DMA_LKEY) {
		if (fake_iwarp || !dm_ok(sg->addr, sg->length)) {
			*st = RDK_WC_LOC_PROT_ERR;
			return (NULL);
		}
		return (DM_VA(sg->addr));
	}
	if ((fm = mr_by_key(sg->lkey)) == NULL ||
	    (fm->fm_access & RDK_ACCESS_LOCAL_WRITE) == 0 ||
	    (fake_iwarp && (fm->fm_access & RDK_ACCESS_REMOTE_WRITE) == 0) ||
	    sg->addr < fm->fm_iova || sg->length > fm->fm_len ||
	    sg->addr - fm->fm_iova > fm->fm_len - sg->length) {
		*st = RDK_WC_LOC_PROT_ERR;
		return (NULL);
	}
	off = sg->addr - fm->fm_iova + (fm->fm_iova & (PAGESIZE - 1));
	pg = (int)(off / PAGESIZE);
	CHECK(pg < fm->fm_hwn);
	if (!dm_ok(fm->fm_hw[pg] + off % PAGESIZE, 1)) {
		*st = RDK_WC_LOC_PROT_ERR;
		return (NULL);
	}
	return (DM_VA(fm->fm_hw[pg] + off % PAGESIZE));
}

static void host_response(const uint8_t *cqe, const swr_t *s);

/* Execute one work request; returns its completion status. */
static enum rdk_wc_status
exec_wr(swr_t *s, uint32_t *lenp)
{
	enum rdk_wc_status st = RDK_WC_SUCCESS;
	hcmd_t *h;
	uint64_t total = 0, pos;
	struct fmr *fm;
	uint8_t *p;
	int i;

	*lenp = 0;
	M.nop[s->w.opcode == RDK_WR_REG_MR ? 15 : s->w.opcode & 0xf]++;
	switch (s->w.opcode) {
	case RDK_WR_RDMA_WRITE:
	case RDK_WR_RDMA_READ:
	case RDK_WR_RDMA_READ_WITH_INV:
		for (i = 0; i < s->w.num_sge; i++)
			total += s->sge[i].length;
		h = host_region(s->rkey);
		if (h == NULL || s->raddr < h->h_addr || total > h->h_len ||
		    s->raddr - h->h_addr > h->h_len - total ||
		    (h->h_access & (s->w.opcode == RDK_WR_RDMA_WRITE ?
		    H_WRITE : H_READ)) == 0)
			return (RDK_WC_REM_ACCESS_ERR);
		pos = s->raddr - h->h_addr;
		for (i = 0; i < s->w.num_sge; i++) {
			const struct rdk_sge *sg = &s->sge[i];

			if (s->w.opcode == RDK_WR_RDMA_WRITE) {
				if (sg->lkey != DMA_LKEY ||
				    !dm_ok(sg->addr, sg->length))
					return (RDK_WC_LOC_PROT_ERR);
				memcpy(h->h_buf + pos, DM_VA(sg->addr),
				    sg->length);
			} else {
				uint32_t j;

				/* A page at a time, through the MR. */
				for (j = 0; j < sg->length; j++) {
					struct rdk_sge b = *sg;

					b.addr += j;
					b.length = 1;
					if ((p = sink(&b, &st)) == NULL)
						return (st);
					*p = h->h_buf[pos + j];
				}
			}
			pos += sg->length;
		}
		if (s->w.opcode == RDK_WR_RDMA_READ_WITH_INV) {
			if ((fm = mr_by_key(s->w.ex.invalidate_rkey)) == NULL)
				return (RDK_WC_LOC_PROT_ERR);
			fm->fm_valid = 0;
		}
		*lenp = (uint32_t)total;
		M.bytes += total;
		return (RDK_WC_SUCCESS);
	case RDK_WR_REG_MR:
		fm = s->mr;
		if ((s->access & RDK_ACCESS_REMOTE_READ) != 0)
			M.remote_read_mr++;
		fm->fm_valid = 1;
		fm->fm_key = s->key;
		fm->fm_access = s->access;
		fm->fm_iova = s->iova;
		fm->fm_len = s->len;
		fm->fm_hwn = s->npages;
		memcpy(fm->fm_hw, s->pages, sizeof (fm->fm_hw));
		return (RDK_WC_SUCCESS);
	case RDK_WR_LOCAL_INV:
		if ((fm = mr_by_key(s->w.ex.invalidate_rkey)) == NULL)
			return (RDK_WC_LOC_PROT_ERR);
		if ((s->w.send_flags & RDK_SEND_FENCE) == 0)
			return (RDK_WC_LOC_QP_OP_ERR);
		fm->fm_valid = 0;
		return (RDK_WC_SUCCESS);
	case RDK_WR_SEND:
	case RDK_WR_SEND_WITH_INV:
		if (s->w.num_sge != 1 || s->sge[0].length != 16)
			return (RDK_WC_LOC_LEN_ERR);
		if ((s->w.send_flags & RDK_SEND_INLINE) != 0) {
			host_response(s->inl, s);
		} else {
			if (s->sge[0].lkey != DMA_LKEY ||
			    !dm_ok(s->sge[0].addr, 16))
				return (RDK_WC_LOC_PROT_ERR);
			host_response(DM_VA(s->sge[0].addr), s);
		}
		*lenp = 16;
		return (RDK_WC_SUCCESS);
	default:
		return (RDK_WC_LOC_QP_OP_ERR);
	}
}

static enum rdk_wc_opcode
wc_op(enum rdk_wr_opcode op)
{
	switch (op) {
	case RDK_WR_RDMA_WRITE:
		return (RDK_WC_RDMA_WRITE);
	case RDK_WR_RDMA_READ:
	case RDK_WR_RDMA_READ_WITH_INV:
		return (RDK_WC_RDMA_READ);
	case RDK_WR_REG_MR:
		return (RDK_WC_REG_MR);
	case RDK_WR_LOCAL_INV:
		return (RDK_WC_LOCAL_INV);
	default:
		return (RDK_WC_SEND);
	}
}

/*
 * The host side of the wire.
 */
static void
put_le(uint8_t *p, uint64_t v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static uint8_t
pat(uint32_t seed, uint32_t i)
{
	return ((uint8_t)((seed * 131 + i * 7 + (i >> 9)) ^ (seed >> 3)));
}

/* Build the next command of the mix into a free slot; M.m is held. */
static hcmd_t *
host_new(void)
{
	hcmd_t *h = NULL;
	uint32_t i, kind;

	for (i = 0; i < HMAX; i++) {
		if (!M.cmds[i].h_used) {
			h = &M.cmds[i];
			break;
		}
	}
	CHECK(h != NULL);
	bzero(h, sizeof (*h));
	h->h_used = 1;
	h->h_cid = M.next_cid++;
	h->h_key = (uint32_t)rnd() | 1;
	h->h_addr = (rnd() & 0x0000fffffffff000ULL) + rndr(0, 4095);
	kind = rndr(0, 9);
	if (M.mix == 1 && kind < 6)
		kind = 3;
	if (M.mix == 2)
		kind = 4;
	if (kind <= 2) {
		h->h_opc = OPC_READ;
	} else if (kind <= 4) {
		h->h_opc = OPC_WRITE;
	} else if (kind <= 6) {
		h->h_opc = OPC_WRITE;
		h->h_icd = M.icd != 0;
	} else if (kind == 7) {
		h->h_opc = OPC_VREAD;
	} else if (kind == 8) {
		h->h_opc = OPC_FLUSH;
	} else {
		h->h_opc = OPC_READ;
	}
	if (h->h_opc == OPC_FLUSH) {
		h->h_len = 0;
	} else if (h->h_icd) {
		h->h_len = rndr(1, M.icd);
	} else if (h->h_opc == OPC_VREAD) {
		h->h_len = rndr(1, 4096);
	} else if (M.mix == 2) {
		h->h_len = rndr(128 * 1024, 512 * 1024);
	} else {
		h->h_len = rndr(1, rndr(0, 3) == 0 ? 256 * 1024 : 9000);
	}
	h->h_inv = h->h_icd == 0 && h->h_len != 0 && (rnd() & 1);
	h->h_access = h->h_opc == OPC_WRITE ? H_READ : H_WRITE;
	if (h->h_len != 0) {
		h->h_buf = malloc(h->h_len);
		h->h_want = malloc(h->h_len);
		for (i = 0; i < h->h_len; i++) {
			h->h_want[i] = pat(h->h_key, i);
			h->h_buf[i] = h->h_opc == OPC_WRITE ? h->h_want[i] :
			    (uint8_t)~h->h_want[i];
		}
	}
	/* SGL1 */
	if (h->h_icd) {
		put_le(h->h_sgl, 0, 8);
		put_le(h->h_sgl + 8, h->h_len, 4);
		h->h_sgl[15] = 0x01;
	} else {
		put_le(h->h_sgl, h->h_addr, 8);
		put_le(h->h_sgl + 8, h->h_len, 3);
		put_le(h->h_sgl + 11, h->h_key, 4);
		h->h_sgl[15] = 0x40 | (h->h_inv ? 0xf : 0);
	}
	return (h);
}

/* Break one of every bad_sgls commands' SGL in a way the target refuses. */
static void
host_break(hcmd_t *h)
{
	switch (rndr(0, 3)) {
	case 0:		/* a keyed SGL with in-capsule data */
		put_le(h->h_sgl, h->h_addr, 8);
		put_le(h->h_sgl + 8, 16, 3);
		put_le(h->h_sgl + 11, h->h_key, 4);
		h->h_sgl[15] = 0x40;
		h->h_icd = 1;
		h->h_expect = NVME_CQE_SC_GEN_INV_FLD;
		break;
	case 1:		/* longer than MDTS */
		put_le(h->h_sgl + 8, 0xffffff, 3);
		h->h_sgl[15] = 0x40;
		h->h_icd = 0;
		h->h_expect = NVME_CQE_SC_GEN_INV_DSGL_LEN;
		break;
	case 2:		/* a reserved descriptor type */
		h->h_sgl[15] = 0x20;
		h->h_icd = 0;
		h->h_expect = NVME_CQE_SC_GEN_INV_SGL_DESC;
		break;
	default:	/* the address wraps */
		put_le(h->h_sgl, UINT64_MAX - 2, 8);
		put_le(h->h_sgl + 8, 16, 3);
		h->h_sgl[15] = 0x40;
		h->h_icd = 0;
		h->h_expect = NVME_CQE_SC_GEN_INV_DSGL_LEN;
		break;
	}
	h->h_opc = OPC_WRITE;
	h->h_inv = 0;
	free(h->h_buf);
	free(h->h_want);
	h->h_len = 16;
	h->h_buf = calloc(1, 16);
	h->h_want = calloc(1, 16);
}

/* The host sends a command: the device places it in the next RECV. */
static void
host_send(hcmd_t *h)
{
	struct fqp *fq = M.qp;
	uint8_t sqe[64];
	uint32_t n;
	rwr_t r;

	bzero(sqe, sizeof (sqe));
	sqe[0] = h->h_opc;
	sqe[1] = 0x40;
	put_le(sqe + 2, h->h_cid, 2);
	put_le(sqe + 4, 1, 4);
	memcpy(sqe + 24, h->h_sgl, 16);
	put_le(sqe + 40, h->h_key, 4);
	n = h->h_icd ? h->h_len : 0;
	if (fq->fq_rq_n == 0) {
		M.rnr++;
		return;
	}
	r = fq->fq_rq[0];
	(void) memmove(fq->fq_rq, fq->fq_rq + 1,
	    (fq->fq_rq_n - 1) * sizeof (rwr_t));
	fq->fq_rq_n--;
	if (r.sge.length < 64 + n || !dm_ok(r.sge.addr, 64 + n)) {
		cq_push(fq->fq_qp.recv_cq, r.cqe, RDK_WC_LOC_LEN_ERR,
		    RDK_WC_RECV, 0);
		fq->fq_err = 1;
		return;
	}
	memcpy(DM_VA(r.sge.addr), sqe, 64);
	if (n != 0) {
		memcpy(DM_VA(r.sge.addr + 64), h->h_buf, n);
	}
	cq_push(fq->fq_qp.recv_cq, r.cqe, RDK_WC_SUCCESS, RDK_WC_RECV,
	    64 + n);
	M.sent++;
	M.outstanding++;
}

static void
host_fill(void)
{
	hcmd_t *h;

	while (M.qp != NULL && !M.qp->fq_err && M.outstanding < M.depth &&
	    M.sent < M.to_send && M.rnr == 0) {
		h = host_new();
		if (M.bad_sgls != 0 && rndr(1, M.bad_sgls) == 1)
			host_break(h);
		host_send(h);
	}
}

/* A response lands; the host checks it and sends its next command. */
static void
host_response(const uint8_t *cqe, const swr_t *s)
{
	uint16_t cid = (uint16_t)(cqe[12] | (cqe[13] << 8));
	uint16_t sc = (uint16_t)(((cqe[14] | (cqe[15] << 8)) >> 1) & 0xff);
	hcmd_t *h = NULL;
	int i;

	for (i = 0; i < HMAX; i++) {
		if (M.cmds[i].h_used && !M.cmds[i].h_done &&
		    M.cmds[i].h_cid == cid)
			h = &M.cmds[i];
	}
	CHECK(h != NULL);
	if (s->w.opcode == RDK_WR_SEND_WITH_INV &&
	    (!h->h_inv || s->w.ex.invalidate_rkey != h->h_key))
		M.wrong_inv++;
	if (h->h_expect != 0) {
		if (sc != h->h_expect) {
			(void) fprintf(stderr, "cid %u: status %x, want %x\n",
			    cid, sc, h->h_expect);
			M.failed++;
		}
	} else if (sc != 0) {
		(void) fprintf(stderr, "cid %u opc %u len %u: status %x\n",
		    cid, h->h_opc, h->h_len, sc);
		M.failed++;
	} else if (h->h_opc != OPC_FLUSH &&
	    memcmp(h->h_buf, h->h_want, h->h_len) != 0) {
		M.early_rsp++;
	}
	/* Every command the host may now send must find a RECV. */
	if (M.qp->fq_rq_n < M.depth - (M.outstanding - 1))
		M.rnr++;
	h->h_done = 1;
	free(h->h_buf);
	free(h->h_want);
	h->h_used = 0;
	M.outstanding--;
	M.done++;
	(void) pthread_cond_broadcast(&M.hcv);
	host_fill();
}

/*
 * The device thread: work requests in order, then the host's sends, and a
 * QP in error flushes everything unless the device is dead.
 */
static void *
device(void *arg)
{
	struct fqp *fq;
	struct rdk_cq *cq;
	enum rdk_wc_status st;
	uint32_t len = 0;
	int pushed;
	swr_t s;

	(void) arg;
	(void) pthread_mutex_lock(&M.m);
	for (;;) {
		fq = M.qp;
		while (!M.stop && (fq == NULL || fq->fq_hang ||
		    (fq->fq_sq_n == 0 && (fq->fq_rq_n == 0 || !fq->fq_err)))) {
			(void) pthread_cond_wait(&M.cv, &M.m);
			fq = M.qp;
		}
		if (M.stop)
			break;
		cq = fq->fq_qp.send_cq;
		pushed = 0;
		if (fq->fq_err) {
			while (fq->fq_sq_n != 0) {
				s = fq->fq_sq[0];
				(void) memmove(fq->fq_sq, fq->fq_sq + 1,
				    (fq->fq_sq_n - 1) * sizeof (swr_t));
				fq->fq_sq_n--;
				cq_push(cq, s.w.wr_cqe, RDK_WC_WR_FLUSH_ERR,
				    wc_op(s.w.opcode), 0);
				pushed++;
			}
			while (fq->fq_rq_n != 0) {
				cq_push(cq, fq->fq_rq[0].cqe,
				    RDK_WC_WR_FLUSH_ERR, RDK_WC_RECV, 0);
				(void) memmove(fq->fq_rq, fq->fq_rq + 1,
				    (fq->fq_rq_n - 1) * sizeof (rwr_t));
				fq->fq_rq_n--;
				pushed++;
			}
		} else {
			if (M.slow_us != 0) {
				(void) pthread_mutex_unlock(&M.m);
				(void) usleep(M.slow_us);
				(void) pthread_mutex_lock(&M.m);
				if ((fq = M.qp) == NULL || fq->fq_err ||
				    fq->fq_sq_n == 0)
					continue;
			}
			s = fq->fq_sq[0];
			(void) memmove(fq->fq_sq, fq->fq_sq + 1,
			    (fq->fq_sq_n - 1) * sizeof (swr_t));
			fq->fq_sq_n--;
			M.executed++;
			if (M.hang_after != 0 && M.executed == M.hang_after) {
				fq->fq_hang = 1;
				continue;
			}
			if (M.fail_at != 0 && M.executed >= M.fail_at &&
			    (s.w.opcode == RDK_WR_RDMA_WRITE ||
			    s.w.opcode == RDK_WR_RDMA_READ)) {
				M.fail_at = 0;
				st = RDK_WC_REM_ACCESS_ERR;
			} else {
				st = exec_wr(&s, &len);
			}
			if (st != RDK_WC_SUCCESS) {
				fq->fq_err = 1;
				cq_push(cq, s.w.wr_cqe, st, wc_op(s.w.opcode),
				    0);
				pushed++;
			} else if ((s.w.send_flags & RDK_SEND_SIGNALED) != 0) {
				cq_push(cq, s.w.wr_cqe, st, wc_op(s.w.opcode),
				    len);
				pushed++;
			} else {
				fq->fq_unsig++;
			}
		}
		/* With lag the poller runs only now and then, or when idle. */
		if (M.lag != 0 && (M.executed % M.lag) != 0 &&
		    fq->fq_sq_n != 0 && !fq->fq_err)
			continue;
		(void) pthread_mutex_unlock(&M.m);
		/* RECVs the host's sends filled were pushed as they went. */
		rdk_comp_upcall(cq);
		(void) pthread_mutex_lock(&M.m);
		(void) pushed;
	}
	(void) pthread_mutex_unlock(&M.m);
	return (NULL);
}

/*
 * A small nvmft: validate, move the data in chunks of up to 128 KiB with
 * at most four in flight as sbd does, and respond.  Transfers run in a
 * taskq, as STMF's workers do.
 */
typedef struct tcmd tcmd_t;

typedef struct tchunk {
	tcmd_t			*tk_tc;
	uint32_t		tk_off;
	uint32_t		tk_len;
	nvmf_databuf_t		tk_db;
	uint8_t			*tk_kbuf;
	nvmf_seg_t		tk_seg;
	int			tk_calls;
	int			tk_final;
} tchunk_t;

struct tcmd {
	taskq_ent_t		tc_ent;
	struct nvmf_capsule	*tc_nc;
	pthread_mutex_t		tc_lock;
	pthread_cond_t		tc_cv;
	int			tc_inflight;
	int			tc_failed;
	uint32_t		tc_seed;
	tchunk_t		tc_ch[64];
	int			tc_nch;
};

static taskq_t *tgt_tq;
static int tgt_reports, tgt_error_val, tgt_probe_range, tgt_linger_us;
static int tgt_hold;
static pthread_mutex_t held_lock = PTHREAD_MUTEX_INITIALIZER;
static struct nvmf_capsule *held[1024];
static int nheld;

static void
tgt_error(void *arg, int error)
{
	(void) arg;
	ev(EV_REPORT);
	__atomic_add_fetch(&tgt_reports, 1, __ATOMIC_SEQ_CST);
	tgt_error_val = error;
}

static void
chunk_end(tchunk_t *tk, int err)
{
	tcmd_t *tc = tk->tk_tc;

	ev(err != 0 ? EV_XFER_ERR : EV_XFER_OK);
	(void) pthread_mutex_lock(&tc->tc_lock);
	CHECK(++tk->tk_calls == 1);
	if (err != 0)
		tc->tc_failed = 1;
	tc->tc_inflight--;
	(void) pthread_cond_broadcast(&tc->tc_cv);
	(void) pthread_mutex_unlock(&tc->tc_lock);
}

static void
tgt_io_done(void *arg, size_t xfered, int error)
{
	tchunk_t *tk = arg;

	if (error == 0)
		CHECK(xfered == tk->tk_len);
	chunk_end(tk, error);
}

static void
tgt_send_done(void *arg, uint_t status)
{
	tchunk_t *tk = arg;
	int err;

	if (tk->tk_final)
		err = status != NVMF_SUCCESS_SENT;
	else
		err = status != NVME_CQE_SC_GEN_SUCCESS && status != NVMF_MORE;
	chunk_end(tk, err);
}

static void
tgt_respond(struct nvmf_qpair *nq, uint16_t cid, uint8_t sc)
{
	struct nvmf_capsule *rc;

	rc = nvmf_rdma_ops.allocate_capsule(nq, KM_SLEEP);
	CHECK(rc != NULL);
	rc->nc_qpair = nq;
	rc->nc_qe_len = sizeof (nvme_cqe_t);
	bzero(&rc->nc_cqe, sizeof (rc->nc_cqe));
	rc->nc_cqe.cqe_cid = cid;
	((uint8_t *)&rc->nc_cqe)[14] = (uint8_t)(sc << 1);
	(void) nvmf_rdma_ops.transmit_capsule(rc);
	nvmf_rdma_ops.free_capsule(rc);
}

static void
tgt_wait(tcmd_t *tc, int below)
{
	(void) pthread_mutex_lock(&tc->tc_lock);
	while (tc->tc_inflight >= below)
		(void) pthread_cond_wait(&tc->tc_cv, &tc->tc_lock);
	(void) pthread_mutex_unlock(&tc->tc_lock);
}

/* Transfers outside the SGL, or empty, are refused and never posted. */
static void
tgt_probe(struct nvmf_capsule *nc, uint32_t len, boolean_t write)
{
	nvmf_seg_t seg = { .nsg_len = 2, .nsg_addr = (uint8_t *)"xx" };
	struct nvmf_send_request req;
	struct nvmf_io_request io;
	nvmf_memdesc_t md;

	bzero(&md, sizeof (md));
	md.nmd_type = NVMF_MEMDESC_SGL;
	md.nmd_len = 2;
	md.nmd_u.nmd_sgl.nmd_segs = &seg;
	md.nmd_u.nmd_sgl.nmd_nsegs = 1;
	if (write) {
		io.io_mem = md;
		io.io_len = 2;
		io.io_complete = NULL;
		io.io_complete_arg = NULL;
		CHECK(nvmf_rdma_ops.receive_controller_data(nc, len - 1,
		    &io) == EFBIG);
		CHECK(nvmf_rdma_ops.receive_controller_data(nc, UINT32_MAX,
		    &io) == EFBIG);
		io.io_len = 0;
		CHECK(nvmf_rdma_ops.receive_controller_data(nc, 0, &io) ==
		    EFBIG);
	} else {
		req.nsr_mem = md;
		req.nsr_len = 2;
		req.nsr_complete = NULL;
		req.nsr_complete_arg = NULL;
		CHECK(nvmf_rdma_ops.send_controller_data_io(nc, len - 1, &req,
		    NULL) == EFBIG);
		CHECK(nvmf_rdma_ops.send_controller_data_io(nc,
		    UINT32_MAX - 1, &req, NULL) == EFBIG);
	}
}

static void
tgt_data(tcmd_t *tc, uint32_t len, boolean_t write, boolean_t kmem)
{
	struct nvmf_capsule *nc = tc->tc_nc;
	struct nvmf_qpair *nq = nc->nc_qpair;
	struct nvmf_send_request req;
	struct nvmf_io_request io;
	nvmf_memdesc_t md;
	nvme_cqe_t cqe;
	tchunk_t *tk;
	uint32_t off, i, r;
	uint8_t *va;
	int ret;

	for (off = 0; off < len; off += tk->tk_len) {
		CHECK(tc->tc_nch < (int)ARRAY_SIZE(tc->tc_ch));
		tk = &tc->tc_ch[tc->tc_nch++];
		tk->tk_tc = tc;
		tk->tk_off = off;
		r = rndr(1, 256) * 512;
		tk->tk_len = MIN(len - off, r);
		tk->tk_final = !write && off + tk->tk_len == len;
		bzero(&md, sizeof (md));
		if (kmem) {
			tk->tk_kbuf = malloc(tk->tk_len);
			va = tk->tk_kbuf;
			md.nmd_type = NVMF_MEMDESC_VADDR;
			md.nmd_u.nmd_vaddr = va;
		} else {
			CHECK(nvmf_rdma_ops.alloc_data_buf(nq, tk->tk_len,
			    tk->tk_len, &tk->tk_db) == 0);
			tk->tk_len = MIN(tk->tk_len, tk->tk_db.ndb_len);
			tk->tk_final = !write && off + tk->tk_len == len;
			va = tk->tk_db.ndb_addr;
			tk->tk_seg.nsg_len = tk->tk_len;
			tk->tk_seg.nsg_addr = va;
			md.nmd_type = NVMF_MEMDESC_SGL;
			md.nmd_u.nmd_sgl.nmd_segs = &tk->tk_seg;
			md.nmd_u.nmd_sgl.nmd_nsegs = 1;
			md.nmd_u.nmd_sgl.nmd_cookies = tk->tk_db.ndb_cookies;
			md.nmd_u.nmd_sgl.nmd_ncookies = 1;
		}
		md.nmd_len = tk->tk_len;
		if (!write) {
			for (i = 0; i < tk->tk_len; i++)
				va[i] = pat(tc->tc_seed, off + i);
		}
		tgt_wait(tc, 4);
		(void) pthread_mutex_lock(&tc->tc_lock);
		tc->tc_inflight++;
		(void) pthread_mutex_unlock(&tc->tc_lock);
		if (write) {
			io.io_mem = md;
			io.io_len = tk->tk_len;
			io.io_complete = tgt_io_done;
			io.io_complete_arg = tk;
			ret = nvmf_rdma_ops.receive_controller_data(nc, off,
			    &io);
		} else {
			bzero(&cqe, sizeof (cqe));
			cqe.cqe_cid = nc->nc_sqe.sqe_cid;
			req.nsr_mem = md;
			req.nsr_len = tk->tk_len;
			req.nsr_complete = tgt_send_done;
			req.nsr_complete_arg = tk;
			ret = nvmf_rdma_ops.send_controller_data_io(nc, off,
			    &req, tk->tk_final ? &cqe : NULL);
		}
		if (ret != 0) {
			(void) pthread_mutex_lock(&tc->tc_lock);
			tc->tc_inflight--;
			tc->tc_failed = 1;
			tk->tk_calls = 1;
			(void) pthread_mutex_unlock(&tc->tc_lock);
			if (tk->tk_final) {
				tgt_respond(nq, nc->nc_sqe.sqe_cid,
				    NVME_CQE_SC_GEN_DATA_XFR_ERR);
			}
			break;
		}
	}
	tgt_wait(tc, 1);
	for (i = 0; i < (uint32_t)tc->tc_nch; i++) {
		tk = &tc->tc_ch[i];
		va = kmem ? tk->tk_kbuf : tk->tk_db.ndb_addr;
		if (write && !tc->tc_failed) {
			for (off = 0; off < tk->tk_len; off++) {
				CHECK(va[off] ==
				    pat(tc->tc_seed, tk->tk_off + off));
			}
		}
		if (kmem)
			free(tk->tk_kbuf);
		else
			nvmf_rdma_ops.free_data_buf(&tk->tk_db);
	}
}

/*
 * nvmft's admin data: one mblk through the synchronous op, and then the
 * response, which must not overtake the data.
 */
static void
tgt_mblk_read(tcmd_t *tc, uint32_t len)
{
	struct nvmf_capsule *nc = tc->tc_nc;
	uint8_t *buf = malloc(len);
	mblk_t mb;
	uint32_t i;
	uint_t status;

	CHECK(buf != NULL);
	for (i = 0; i < len; i++)
		buf[i] = pat(tc->tc_seed, i);
	bzero(&mb, sizeof (mb));
	mb.b_rptr = buf;
	mb.b_wptr = buf + len;
	status = nvmf_rdma_ops.send_controller_data(nc, 0, &mb, len);
	free(buf);
	tgt_respond(nc->nc_qpair, nc->nc_sqe.sqe_cid,
	    status == NVME_CQE_SC_GEN_SUCCESS ? 0 : (uint8_t)status);
}

static void
tgt_work(void *arg)
{
	tcmd_t *tc = arg;
	struct nvmf_capsule *nc = tc->tc_nc;
	struct nvmf_qpair *nq = nc->nc_qpair;
	uint8_t opc = nc->nc_sqe.sqe_opc, sc;
	uint16_t cid = nc->nc_sqe.sqe_cid;
	uint32_t len;
	boolean_t late;

	tc->tc_seed = nc->nc_sqe.sqe_cdw10;
	sc = nvmf_rdma_ops.validate_command_capsule(nc);
	if (sc != 0) {
		tgt_respond(nq, cid, sc);
	} else {
		len = (uint32_t)nvmf_rdma_ops.capsule_data_len(nc);
		if (tgt_probe_range && len != 0) {
			tgt_probe(nc, len, opc == OPC_WRITE);
		}
		if (len != 0 && opc == OPC_WRITE)
			tgt_data(tc, len, B_TRUE, B_FALSE);
		else if (len != 0 && len <= 65536 && (cid & 7) == 1)
			tgt_mblk_read(tc, len);
		else if (len != 0)
			tgt_data(tc, len, B_FALSE, opc == OPC_VREAD);
		/* nvmft frees a Connect's capsule before it answers it. */
		late = !tgt_hold && tgt_linger_us == 0 &&
		    (cid & 3) == 0;
		if (late)
			nvmf_rdma_ops.free_capsule(nc);
		if (opc == OPC_WRITE || len == 0) {
			tgt_respond(nq, cid, tc->tc_failed ?
			    NVME_CQE_SC_GEN_DATA_XFR_ERR : 0);
		}
		if (late) {
			(void) pthread_mutex_destroy(&tc->tc_lock);
			(void) pthread_cond_destroy(&tc->tc_cv);
			free(tc);
			return;
		}
	}
	if (tgt_linger_us != 0)
		(void) usleep(tgt_linger_us);
	if (tgt_hold) {
		(void) pthread_mutex_lock(&held_lock);
		CHECK(nheld < (int)ARRAY_SIZE(held));
		held[nheld++] = nc;
		(void) pthread_mutex_unlock(&held_lock);
	} else {
		nvmf_rdma_ops.free_capsule(nc);
	}
	(void) pthread_mutex_destroy(&tc->tc_lock);
	(void) pthread_cond_destroy(&tc->tc_cv);
	free(tc);
}

/* The receive callback must not block: the work goes to the taskq. */
static void
tgt_receive(void *arg, struct nvmf_capsule *nc)
{
	tcmd_t *tc = calloc(1, sizeof (*tc));

	(void) arg;
	(void) pthread_mutex_init(&tc->tc_lock, NULL);
	(void) pthread_cond_init(&tc->tc_cv, NULL);
	tc->tc_nc = nc;
	taskq_dispatch_ent(tgt_tq, tgt_work, tc, 0, &tc->tc_ent);
}

static void
release_held(void)
{
	struct nvmf_capsule *nc;

	for (;;) {
		(void) pthread_mutex_lock(&held_lock);
		nc = nheld != 0 ? held[--nheld] : NULL;
		(void) pthread_mutex_unlock(&held_lock);
		if (nc == NULL)
			return;
		nvmf_rdma_ops.free_capsule(nc);
	}
}

static void *
releaser(void *arg)
{
	int *stop = arg;

	while (!__atomic_load_n(stop, __ATOMIC_SEQ_CST)) {
		(void) usleep(3000);
		release_held();
	}
	return (NULL);
}

/*
 * Queues.
 */
static nr_dev_t ND;

static void
reset(void)
{
	(void) pthread_mutex_lock(&M.m);
	M.outstanding = M.to_send = M.sent = M.done = M.failed = 0;
	M.mix = M.bad_sgls = M.rnr = M.sq_over = M.remote_read_mr = 0;
	M.wrong_inv = M.early_rsp = M.fail_at = M.hang_after = 0;
	M.executed = 0;
	M.lag = 0;
	M.slow_us = 0;
	M.bytes = 0;
	bzero(M.nop, sizeof (M.nop));
	bzero(M.cmds, sizeof (M.cmds));
	(void) pthread_mutex_unlock(&M.m);
	(void) pthread_mutex_lock(&ev_lock);
	nev = 0;
	(void) pthread_mutex_unlock(&ev_lock);
	tgt_reports = tgt_probe_range = tgt_linger_us = tgt_hold = 0;
}

/* A device of kind: 0 RoCE, 1 iWARP with READ_WITH_INV, 2 T6-like. */
static void
set_dev(int kind, int max_qp_wr)
{
	struct rdk_device_attr *a = &dev.rd_attr;

	fake_iwarp = kind != 0;
	a->max_qp_wr = max_qp_wr;
	a->max_cqe = 1 << 16;
	a->max_send_sge = 4;
	a->max_recv_sge = 4;
	a->max_sge_rd = kind == 2 ? 1 : 4;
	a->max_qp_init_rd_atom = 16;
	a->max_fast_reg_page_list_len = 64;
	a->max_mr_size = 1ULL << 32;
	a->page_size_cap = PAGESIZE;
	a->device_cap_flags = RDK_DEVICE_MEM_MGT_EXTENSIONS;
	a->kernel_cap_flags = RDK_KCAP_LOCAL_DMA_LKEY |
	    (kind == 1 ? RDK_KCAP_READ_WITH_INV : 0);
	a->max_inline_data = kind == 0 ? 64 : 0;
	a->local_dma_lkey = DMA_LKEY;
	ND.nd_iwarp = fake_iwarp;
}

static nr_queue_t *
mkq(uint32_t depth, uint32_t icd)
{
	struct rdk_rw_attr a;
	struct rdk_rw_limits l;
	nvmf_rdma_devlim_t dl;
	nvmf_rdma_sizes_t sz;
	nr_queue_t *q;
	int err = 0;

	reset();
	ND.nd_dev = &dev;
	if (ND.nd_pd == NULL)
		CHECK(rdk_alloc_pd(&dev, 0, &ND.nd_pd) == 0);
	CHECK(nr_rw_attr(&ND, &a) != 0);
	CHECK(rdk_rw_limits(&dev, &a, &l) == 0);
	dl.ndl_max_qp_wr = (uint32_t)dev.rd_attr.max_qp_wr;
	dl.ndl_max_cqe = (uint32_t)dev.rd_attr.max_cqe;
	dl.ndl_rw_wrs = l.rwl_wrs;
	CHECK(nvmf_rdma_size_queue(depth, icd, &dl, &sz) == NVMF_RDMA_OK);
	q = nr_queue_create(&ND, &sz, 1, icd, 0, &err);
	CHECK(q != NULL);
	q->nq_nq.nq_ops = &nvmf_rdma_ops;
	q->nq_nq.nq_controller = B_TRUE;
	q->nq_nq.nq_error = tgt_error;
	q->nq_nq.nq_error_arg = q;
	q->nq_nq.nq_receive = tgt_receive;
	q->nq_nq.nq_receive_arg = q;
	q->nq_adopted = B_TRUE;
	(void) pthread_mutex_lock(&M.m);
	M.icd = icd;
	(void) pthread_mutex_unlock(&M.m);
	CHECK(nr_queue_post_ring(q) == 0);
	nr_queue_established(q);
	return (q);
}

/* nvmft lets go of the queue once its commands are done. */
static void
endq(nr_queue_t *q)
{
	int i;

	taskq_wait(tgt_tq);
	release_held();
	nvmf_rdma_ops.free_qpair(&q->nq_nq);
	CHECK(pool_bufs == 0);
	(void) pthread_mutex_lock(&M.m);
	for (i = 0; i < nmrs; i++)
		CHECK(!mrs[i]->fm_valid);
	CHECK(M.qp == NULL);
	(void) pthread_mutex_unlock(&M.m);
}

/* Send total commands, depth at a time, until done or the QP fails. */
static void
run_host(nr_queue_t *q, int total, int depth)
{
	hrtime_t end = gethrtime() + SEC2NSEC(120);
	struct timespec ts = { 0, 2000000 };

	(void) pthread_mutex_lock(&M.m);
	M.depth = depth;
	M.to_send = total;
	host_fill();
	(void) pthread_mutex_unlock(&M.m);
	rdk_comp_upcall(q->nq_cq);
	(void) pthread_mutex_lock(&M.m);
	while (M.done < total && M.rnr == 0 && M.qp != NULL &&
	    !M.qp->fq_err && !M.qp->fq_hang) {
		CHECK(gethrtime() < end);
		(void) pthread_mutex_unlock(&M.m);
		(void) nanosleep(&ts, NULL);
		(void) pthread_mutex_lock(&M.m);
	}
	(void) pthread_mutex_unlock(&M.m);
}

static void
check_clean(const char *what)
{
	if (M.failed || M.rnr || M.sq_over || M.remote_read_mr ||
	    M.wrong_inv || M.early_rsp) {
		(void) fprintf(stderr, "%s: failed %d rnr %d sq_over %d "
		    "remote_read_mr %d wrong_inv %d early_rsp %d\n", what,
		    M.failed, M.rnr, M.sq_over, M.remote_read_mr, M.wrong_inv,
		    M.early_rsp);
		abort();
	}
}

/*
 * Transfers of every shape on each kind of device: each lands where its
 * SGL says with the host's key, the response only after the data, and a
 * SEND_WITH_INV only for the key the host asked to invalidate.
 */
static void
test_plan(void)
{
	nr_queue_t *q;
	int kind;

	for (kind = 0; kind < 3; kind++) {
		set_dev(kind, 4096);
		q = mkq(16, 8192);
		tgt_probe_range = 1;
		run_host(q, 300, 16);
		check_clean("plan");
		CHECK(M.done == 300);
		(void) printf("device %d: %d requests, %llu bytes: write %d "
		    "read %d read_inv %d reg %d inv %d send %d send_inv %d\n",
		    kind, M.executed, (unsigned long long)M.bytes,
		    M.nop[RDK_WR_RDMA_WRITE], M.nop[RDK_WR_RDMA_READ],
		    M.nop[RDK_WR_RDMA_READ_WITH_INV], M.nop[15],
		    M.nop[RDK_WR_LOCAL_INV], M.nop[RDK_WR_SEND],
		    M.nop[RDK_WR_SEND_WITH_INV]);
		/* Each kind must have used its own way to sink a READ. */
		CHECK(M.nop[RDK_WR_RDMA_WRITE] > 0 && M.nop[RDK_WR_SEND] > 0 &&
		    M.nop[RDK_WR_SEND_WITH_INV] > 0);
		if (kind == 0)
			CHECK(M.nop[RDK_WR_RDMA_READ] > 0 &&
			    M.nop[15] == 0);
		else if (kind == 1)
			CHECK(M.nop[RDK_WR_RDMA_READ_WITH_INV] > 0);
		else
			CHECK(M.nop[RDK_WR_LOCAL_INV] > 0);
		endq(q);

		q = mkq(8, 4096);
		M.bad_sgls = 3;
		run_host(q, 120, 8);
		check_clean("bad SGLs");
		CHECK(M.done == 120);
		endq(q);
	}
	(void) printf("PASS: transfers land only within their SGL\n");
}

/*
 * Credit: in-capsule commands whose host sends again the moment a response
 * lands; a send queue with room for two transfers; contexts held by nvmft
 * long enough that RECVs must wait for one.
 */
static void
test_credit(void)
{
	nr_queue_t *q;
	pthread_t t;
	int stop = 0;

	/* 8 entries: 16 contexts and 2 transfers of 3 requests each. */
	set_dev(2, 17 + 2 * 3);
	q = mkq(8, 4096);
	M.mix = 1;
	M.lag = 12;
	tgt_linger_us = 300;
	run_host(q, 300, 8);
	check_clean("credit");
	(void) printf("credit: send queue of %u, peak %u\n", q->nq_sz.nrs_sq,
	    M.qp->fq_sq_peak);
	CHECK(M.done == 300);
	CHECK(q->nq_sz.nrs_xfers == 2 && q->nq_rw_waits > 0);
	endq(q);

	/* Many large READs at once, and completions polled in batches. */
	set_dev(2, 4096);
	q = mkq(8, 4096);
	M.mix = 2;
	M.lag = 64;
	M.slow_us = 100;
	run_host(q, 200, 8);
	check_clean("credit, polled late");
	CHECK(M.done == 200);
	(void) printf("credit, polled late: send queue of %u, peak %u\n",
	    q->nq_sz.nrs_sq, M.qp->fq_sq_peak);
	endq(q);

	set_dev(0, 4096);
	q = mkq(4, 4096);
	M.mix = 1;
	tgt_hold = 1;
	CHECK(pthread_create(&t, NULL, releaser, &stop) == 0);
	run_host(q, 200, 4);
	__atomic_store_n(&stop, 1, __ATOMIC_SEQ_CST);
	CHECK(pthread_join(t, NULL) == 0);
	check_clean("backlog");
	CHECK(M.done == 200);
	endq(q);
	(void) printf("PASS: RECV, context and send queue credit\n");
}

static int
ev_index(int e, int first)
{
	int i;

	if (first) {
		for (i = 0; i < nev; i++) {
			if (evlog[i] == e)
				return (i);
		}
	} else {
		for (i = nev - 1; i >= 0; i--) {
			if (evlog[i] == e)
				return (i);
		}
	}
	return (-1);
}

static void
wait_reports(int n)
{
	hrtime_t end = gethrtime() + SEC2NSEC(60);

	while (__atomic_load_n(&tgt_reports, __ATOMIC_SEQ_CST) < n) {
		CHECK(gethrtime() < end);
		(void) usleep(1000);
	}
}

/*
 * Teardown: a failed transfer's callback only starts the teardown.  After
 * a drain that accounts for everything, what the device will not complete
 * is failed before the QP is destroyed; after a drain that does not, only
 * once it is.
 */
static void
test_teardown(void)
{
	nr_queue_t *q;
	int destroy, last_err;

	/* Two transfers at a time, so some wait when the error comes. */
	set_dev(0, 33 + 2);
	q = mkq(16, 0);
	(void) pthread_mutex_lock(&M.m);
	M.fail_at = 60;
	(void) pthread_mutex_unlock(&M.m);
	run_host(q, 400, 16);
	wait_reports(1);
	taskq_wait(tgt_tq);
	(void) pthread_mutex_lock(&ev_lock);
	destroy = ev_index(EV_DESTROY_QP, 1);
	last_err = ev_index(EV_XFER_ERR, 0);
	(void) pthread_mutex_unlock(&ev_lock);
	CHECK(destroy >= 0 && last_err >= 0);
	if (last_err > destroy) {
		(void) fprintf(stderr, "a transfer failed after the QP was "
		    "destroyed, with nothing outstanding\n");
		abort();
	}
	CHECK(tgt_reports == 1 && tgt_error_val == EIO);
	CHECK(q->nq_sz.nrs_xfers == 2 && q->nq_rw_waits > 0);
	endq(q);
	CHECK(tgt_reports == 1);

	/* The device stops; the drain times out. */
	rdk_drain_timeout_ms = 300;
	set_dev(0, 4096);
	q = mkq(16, 0);
	(void) pthread_mutex_lock(&M.m);
	M.hang_after = 80;
	(void) pthread_mutex_unlock(&M.m);
	run_host(q, 400, 16);
	nr_queue_fail(q, ECONNRESET);
	wait_reports(1);
	taskq_wait(tgt_tq);
	(void) pthread_mutex_lock(&ev_lock);
	destroy = ev_index(EV_DESTROY_QP, 1);
	last_err = ev_index(EV_XFER_ERR, 1);
	(void) pthread_mutex_unlock(&ev_lock);
	CHECK(destroy >= 0 && last_err >= 0);
	if (last_err < destroy) {
		(void) fprintf(stderr, "a transfer the device may still "
		    "reach failed before the QP was destroyed\n");
		abort();
	}
	CHECK(tgt_reports == 1 && tgt_error_val == 0);
	endq(q);
	rdk_drain_timeout_ms = 10000;
	(void) printf("PASS: teardown order after an error and a dead "
	    "device\n");
}

int
main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "all";

	(void) alarm(300);
	(void) strcpy(dev.rd_name, "fake0");
	dev.rd_ops = &fake_ops;
	dev.rd_num_comp_vectors = 1;
	CHECK(rdk_cq_init() == 0);
	(void) pthread_mutex_init(&M.m, NULL);
	(void) pthread_cond_init(&M.cv, NULL);
	(void) pthread_cond_init(&M.hcv, NULL);
	CHECK(pthread_create(&M.thr, NULL, device, NULL) == 0);
	nvmf_rdma_taskq = taskq_create("nvmf_rdma", 4, 60, 4, 4, 0);
	tgt_tq = taskq_create("tgt", 16, 60, 16, 16, 0);

	if (strcmp(which, "all") == 0 || strcmp(which, "plan") == 0)
		test_plan();
	if (strcmp(which, "all") == 0 || strcmp(which, "credit") == 0)
		test_credit();
	if (strcmp(which, "all") == 0 || strcmp(which, "teardown") == 0)
		test_teardown();

	(void) pthread_mutex_lock(&M.m);
	M.stop = 1;
	(void) pthread_cond_broadcast(&M.cv);
	(void) pthread_mutex_unlock(&M.m);
	CHECK(pthread_join(M.thr, NULL) == 0);
	taskq_destroy(tgt_tq);
	taskq_destroy(nvmf_rdma_taskq);
	if (ND.nd_pd != NULL)
		rdk_dealloc_pd(ND.nd_pd);
	rdk_cq_fini();
	return (0);
}

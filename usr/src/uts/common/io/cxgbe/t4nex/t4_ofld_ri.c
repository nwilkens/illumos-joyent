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
 * RDMA queues of the child: CQs and QPs made with FW_RI_RES_WR, and the
 * FW_RI_WR that binds a QP to a connection.  t4nex builds every work
 * request; the child names its queues by ID and its memory by the buffers
 * t4nex gave it.  Queue memory belongs to t4nex from create on and is freed
 * only when the firmware has let go of the queue: a CQ or a QP that never
 * ran after a RESET the firmware completed, and a QP that ran after a FINI
 * the firmware completed or once the chip reported its connection aborted.
 * Anything else stays in the quarantine until the SGE stops.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/avl.h>
#include <sys/bitmap.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "common/t4_regs_values.h"
#include "t4_ofld.h"

#define	T4_RI_MAX_IQ_SIZE	65520
#define	T4_RI_MAX_SQ_SIZE	65520
#define	T4_RI_MAX_RQ_SIZE	8192
#define	T4_RI_MIN_IQ_SIZE	64
#define	T4_RI_ENTRY		64

#define	ROF_QP		0x01
#define	ROF_BUSY	0x02	/* an operation is waiting for the firmware */
#define	ROF_INIT	0x04	/* FW_RI_WR sent */
#define	ROF_FINI_OK	0x08	/* the firmware completed FW_RI_FINI */
#define	ROF_GONE	0x10	/* the chip aborted the connection */

static int
t4_ri_cmp(const void *a, const void *b)
{
	const t4_ri_obj_t *x = a, *y = b;

	return (x->ro_id < y->ro_id ? -1 : x->ro_id > y->ro_id ? 1 : 0);
}

void
t4_ofld_ri_setup(t4_ofld_t *of)
{
	const t4_rdma_range_t *r = &of->of_vres.trv_qp;

	mutex_init(&of->of_ri_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(of->of_sc->intr_pri));
	cv_init(&of->of_ri_cv, NULL, CV_DRIVER, NULL);
	avl_create(&of->of_ri_objs, t4_ri_cmp, sizeof (t4_ri_obj_t),
	    offsetof(t4_ri_obj_t, ro_node));
	of->of_ri_nids = r->trr_size;
	of->of_ri_used = kmem_zalloc(BT_SIZEOFMAP(r->trr_size), KM_SLEEP);
}

void
t4_ofld_ri_teardown(t4_ofld_t *of)
{
	t4_ri_obj_t *ro;
	void *c = NULL;

	if (of->of_ri_used == NULL)
		return;
	while ((ro = avl_destroy_nodes(&of->of_ri_objs, &c)) != NULL)
		kmem_free(ro, sizeof (*ro));
	avl_destroy(&of->of_ri_objs);
	kmem_free(of->of_ri_used, BT_SIZEOFMAP(of->of_ri_nids));
	of->of_ri_used = NULL;
	cv_destroy(&of->of_ri_cv);
	mutex_destroy(&of->of_ri_lock);
}

static boolean_t
t4_ri_id_ok(t4_ofld_t *of, uint32_t id)
{
	const t4_rdma_range_t *r = &of->of_vres.trv_qp;

	return (id >= r->trr_start && id - r->trr_start < r->trr_size);
}

/* of_ri_lock is held. */
static boolean_t
t4_ri_id_free(t4_ofld_t *of, uint32_t id)
{
	ASSERT(MUTEX_HELD(&of->of_ri_lock));
	return (t4_ri_id_ok(of, id) &&
	    !BT_TEST(of->of_ri_used, id - of->of_vres.trv_qp.trr_start));
}

static void
t4_ri_id_set(t4_ofld_t *of, uint32_t id, boolean_t used)
{
	const uint32_t i = id - of->of_vres.trv_qp.trr_start;

	ASSERT(MUTEX_HELD(&of->of_ri_lock));
	if (used) {
		BT_SET(of->of_ri_used, i);
	} else {
		BT_CLEAR(of->of_ri_used, i);
	}
}

/* The object the client generation owns, or NULL.  of_ri_lock is held. */
static t4_ri_obj_t *
t4_ri_find(t4_ofld_t *of, uint32_t id, uint32_t gen, boolean_t qp)
{
	t4_ri_obj_t key, *ro;

	ASSERT(MUTEX_HELD(&of->of_ri_lock));
	key.ro_id = id;
	ro = avl_find(&of->of_ri_objs, &key, NULL);
	if (ro == NULL || ro->ro_gen != gen ||
	    ((ro->ro_flags & ROF_QP) != 0) != qp)
		return (NULL);
	return (ro);
}

/*
 * Claim an object for one firmware operation.  Returns with ROF_BUSY set
 * and of_ri_lock dropped.
 */
static t4_ri_obj_t *
t4_ri_busy(t4_ofld_t *of, uint32_t id, uint32_t gen, boolean_t qp, int *rcp)
{
	t4_ri_obj_t *ro;

	mutex_enter(&of->of_ri_lock);
	if ((ro = t4_ri_find(of, id, gen, qp)) == NULL) {
		mutex_exit(&of->of_ri_lock);
		*rcp = EINVAL;
		return (NULL);
	}
	if ((ro->ro_flags & ROF_BUSY) != 0) {
		mutex_exit(&of->of_ri_lock);
		*rcp = EBUSY;
		return (NULL);
	}
	ro->ro_flags |= ROF_BUSY;
	mutex_exit(&of->of_ri_lock);
	return (ro);
}

static void
t4_ri_unbusy(t4_ofld_t *of, t4_ri_obj_t *ro, uint16_t set)
{
	mutex_enter(&of->of_ri_lock);
	ro->ro_flags = (ro->ro_flags & ~ROF_BUSY) | set;
	cv_broadcast(&of->of_ri_cv);
	mutex_exit(&of->of_ri_lock);
}

static void
t4_ri_res_hdr(struct fw_ri_res_wr *wr, uint_t nres, size_t len,
    uint64_t cookie)
{
	wr->op_nres = BE_32(V_FW_WR_OP(FW_RI_RES_WR) |
	    V_FW_RI_RES_WR_NRES(nres) | F_FW_WR_COMPL);
	wr->len16_pkd = BE_32(howmany(len, 16));
	wr->cookie = BE_64(cookie);
}

/*
 * Build, send and wait for a FW_RI_RES_WR.  The cookie goes into the
 * request before it is sent.
 */
static int
t4_ri_res_send(t4_ofld_t *of, struct fw_ri_res_wr *wr, uint_t nres)
{
	const size_t len = sizeof (*wr) + nres * sizeof (struct fw_ri_res);
	t4_sge_eq_t *eq = &of->of_port[0].op_ctrlq;
	uint64_t cookie;
	int rc;

	if ((rc = t4_ofld_waiter_get(of, &cookie)) != 0)
		return (rc);
	t4_ri_res_hdr(wr, nres, len, cookie);
	if ((rc = t4_ofld_wr_send(of, eq, wr, roundup(len, 16))) != 0) {
		t4_ofld_waiter_put(of, cookie);
		return (rc);
	}
	return (t4_ofld_waiter_wait(of, cookie));
}

static t4_ri_obj_t *
t4_ri_obj_new(uint32_t id, uint32_t gen)
{
	t4_ri_obj_t *ro = kmem_zalloc(sizeof (*ro), KM_SLEEP);

	ro->ro_id = id;
	ro->ro_gen = gen;
	ro->ro_tid = T4_TID_NIL;
	ro->ro_flags = ROF_BUSY;
	return (ro);
}

/* The BAR2 registers of a queue; the ID is in the vres range. */
static int
t4_ri_db(t4_ofld_t *of, uint32_t qid, boolean_t egress, t4_rdma_db_t *db)
{
	struct adapter *sc = of->of_sc;
	uint64_t off;
	off_t bar2;
	uint_t bqid;

	if (t4_bar2_sge_qregs(sc, qid, egress ? T4_BAR2_QTYPE_EGRESS :
	    T4_BAR2_QTYPE_INGRESS, 0, &off, &bqid) != 0)
		return (ENXIO);
	if (ddi_dev_regsize(sc->dip, 2, &bar2) != DDI_SUCCESS ||
	    off + SGE_UDB_WCDOORBELL + 64 > (uint64_t)bar2)
		return (ENXIO);
	db->trdb_off = off;
	db->trdb_qid = bqid;
	return (0);
}

/* of_ri_lock is held. */
static void
t4_ri_wait_idle(t4_ofld_t *of, t4_ri_obj_t *ro)
{
	ASSERT(MUTEX_HELD(&of->of_ri_lock));
	while ((ro->ro_flags & ROF_BUSY) != 0)
		cv_wait(&of->of_ri_cv, &of->of_ri_lock);
}

int
t4_ofld_cq_create(t4_ofld_t *of, const t4_rdma_cq_res_t *c, t4_rdma_db_t *db)
{
	struct {
		struct fw_ri_res_wr	hdr;
		struct fw_ri_res	res;
	} wr;
	struct fw_ri_res_cq *cq = &wr.res.u.cq;
	t4_ofld_buf_t *ob;
	t4_ri_obj_t *ro;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if (c->trcq_size < T4_RI_MIN_IQ_SIZE ||
	    c->trcq_size > T4_RI_MAX_IQ_SIZE || (c->trcq_size % 16) != 0 ||
	    !t4_ri_id_ok(of, c->trcq_cqid))
		return (EINVAL);
	if ((rc = t4_ri_db(of, c->trcq_cqid, B_FALSE, db)) != 0)
		return (rc);
	if ((ob = t4_ofld_dma_bind(of, c->trcq_mem,
	    (size_t)c->trcq_size * T4_RI_ENTRY)) == NULL)
		return (EINVAL);

	ro = t4_ri_obj_new(c->trcq_cqid, gen);
	ro->ro_mem[0] = ob;
	mutex_enter(&of->of_ri_lock);
	if (!t4_ri_id_free(of, c->trcq_cqid)) {
		mutex_exit(&of->of_ri_lock);
		kmem_free(ro, sizeof (*ro));
		t4_ofld_dma_unbind(of, ob);
		return (EINVAL);
	}
	t4_ri_id_set(of, c->trcq_cqid, B_TRUE);
	avl_add(&of->of_ri_objs, ro);
	mutex_exit(&of->of_ri_lock);

	bzero(&wr, sizeof (wr));
	cq->restype = FW_RI_RES_TYPE_CQ;
	cq->op = FW_RI_RES_OP_WRITE;
	cq->iqid = BE_32(c->trcq_cqid);
	cq->iqandst_to_iqandstindex = BE_32(V_FW_RI_RES_WR_IQANUS(0) |
	    V_FW_RI_RES_WR_IQANUD(1) | F_FW_RI_RES_WR_IQANDST |
	    V_FW_RI_RES_WR_IQANDSTINDEX(of->of_ciq.tsi_abs_id));
	cq->iqdroprss_to_iqesize = BE_16(F_FW_RI_RES_WR_IQDROPRSS |
	    V_FW_RI_RES_WR_IQPCIECH(2) | V_FW_RI_RES_WR_IQINTCNTTHRESH(0) |
	    F_FW_RI_RES_WR_IQO | V_FW_RI_RES_WR_IQESIZE(2));
	cq->iqsize = BE_16(c->trcq_size);
	cq->iqaddr = BE_64(ob->ob_pub.trd_pa);

	rc = t4_ri_res_send(of, &wr.hdr, 1);
	if (rc == 0) {
		t4_ri_unbusy(of, ro, 0);
		return (0);
	}

	/* The firmware may or may not have taken the queue. */
	mutex_enter(&of->of_ri_lock);
	avl_remove(&of->of_ri_objs, ro);
	if (rc != ETIMEDOUT)
		t4_ri_id_set(of, c->trcq_cqid, B_FALSE);
	mutex_exit(&of->of_ri_lock);
	t4_ofld_dma_release(of, ob, rc != ETIMEDOUT);
	kmem_free(ro, sizeof (*ro));
	return (rc);
}

static int
t4_ri_reset(t4_ofld_t *of, uint_t n, const uint8_t *types,
    const uint32_t *ids)
{
	struct {
		struct fw_ri_res_wr	hdr;
		struct fw_ri_res	res[2];
	} wr;

	ASSERT3U(n, <=, 2);
	bzero(&wr, sizeof (wr));
	for (uint_t i = 0; i < n; i++) {
		if (types[i] == FW_RI_RES_TYPE_CQ) {
			wr.res[i].u.cq.restype = types[i];
			wr.res[i].u.cq.op = FW_RI_RES_OP_RESET;
			wr.res[i].u.cq.iqid = BE_32(ids[i]);
		} else {
			wr.res[i].u.sqrq.restype = types[i];
			wr.res[i].u.sqrq.op = FW_RI_RES_OP_RESET;
			wr.res[i].u.sqrq.eqid = BE_32(ids[i]);
		}
	}
	return (t4_ri_res_send(of, &wr.hdr, n));
}

int
t4_ofld_cq_destroy(t4_ofld_t *of, uint32_t cqid)
{
	const uint8_t type = FW_RI_RES_TYPE_CQ;
	t4_ri_obj_t *ro;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if ((ro = t4_ri_busy(of, cqid, gen, B_FALSE, &rc)) == NULL)
		return (rc);
	if (ro->ro_refs != 0) {
		t4_ri_unbusy(of, ro, 0);
		return (EBUSY);
	}

	rc = t4_ri_reset(of, 1, &type, &cqid);

	mutex_enter(&of->of_ri_lock);
	avl_remove(&of->of_ri_objs, ro);
	if (rc == 0)
		t4_ri_id_set(of, cqid, B_FALSE);
	mutex_exit(&of->of_ri_lock);
	t4_ofld_dma_release(of, ro->ro_mem[0], rc == 0);
	kmem_free(ro, sizeof (*ro));
	return (rc);
}

/* A CQ of the client that QPs may use; of_ri_lock is held. */
static t4_ri_obj_t *
t4_ri_cq(t4_ofld_t *of, uint32_t cqid, uint32_t gen)
{
	t4_ri_obj_t *cq = t4_ri_find(of, cqid, gen, B_FALSE);

	if (cq == NULL || (cq->ro_flags & ROF_BUSY) != 0)
		return (NULL);
	return (cq);
}

static void
t4_ri_sqrq(struct fw_ri_res_sqrq *r, uint8_t type, uint32_t eqid,
    uint32_t cqid, uint32_t size, uint64_t pa)
{
	r->restype = type;
	r->op = FW_RI_RES_OP_WRITE;
	r->fetchszm_to_iqid = BE_32(V_FW_RI_RES_WR_HOSTFCMODE(0) |
	    V_FW_RI_RES_WR_CPRIO(0) | V_FW_RI_RES_WR_PCIECHN(0) |
	    V_FW_RI_RES_WR_IQID(cqid));
	r->dcaen_to_eqsize = BE_32(V_FW_RI_RES_WR_DCAEN(0) |
	    V_FW_RI_RES_WR_DCACPU(0) | V_FW_RI_RES_WR_FBMIN(2) |
	    V_FW_RI_RES_WR_FBMAX(3) | V_FW_RI_RES_WR_CIDXFTHRESHO(0) |
	    V_FW_RI_RES_WR_CIDXFTHRESH(0) | V_FW_RI_RES_WR_EQSIZE(size));
	r->eqid = BE_32(eqid);
	r->eqaddr = BE_64(pa);
}

int
t4_ofld_qp_create(t4_ofld_t *of, const t4_rdma_qp_res_t *q, t4_rdma_db_t *sdb,
    t4_rdma_db_t *rdb)
{
	struct {
		struct fw_ri_res_wr	hdr;
		struct fw_ri_res	res[2];
	} wr;
	const uint32_t spg = of->of_sc->sge.eq_spg_len;
	t4_ofld_buf_t *sob, *rob;
	t4_ri_obj_t *ro, *scq, *rcq;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if (q->trqp_sqid == q->trqp_rqid ||
	    q->trqp_sq_size <= spg || q->trqp_sq_size > T4_RI_MAX_SQ_SIZE ||
	    q->trqp_rq_size <= spg || q->trqp_rq_size > T4_RI_MAX_RQ_SIZE ||
	    !t4_ri_id_ok(of, q->trqp_sqid) || !t4_ri_id_ok(of, q->trqp_rqid))
		return (EINVAL);
	if ((rc = t4_ri_db(of, q->trqp_sqid, B_TRUE, sdb)) != 0 ||
	    (rc = t4_ri_db(of, q->trqp_rqid, B_TRUE, rdb)) != 0)
		return (rc);
	if ((sob = t4_ofld_dma_bind(of, q->trqp_sq_mem,
	    (size_t)q->trqp_sq_size * T4_RI_ENTRY)) == NULL)
		return (EINVAL);
	if ((rob = t4_ofld_dma_bind(of, q->trqp_rq_mem,
	    (size_t)q->trqp_rq_size * T4_RI_ENTRY)) == NULL) {
		t4_ofld_dma_unbind(of, sob);
		return (EINVAL);
	}

	ro = t4_ri_obj_new(q->trqp_sqid, gen);
	ro->ro_flags |= ROF_QP;
	ro->ro_rqid = q->trqp_rqid;
	ro->ro_cq[0] = q->trqp_scqid;
	ro->ro_cq[1] = q->trqp_rcqid;
	ro->ro_mem[0] = sob;
	ro->ro_mem[1] = rob;

	mutex_enter(&of->of_ri_lock);
	scq = t4_ri_cq(of, q->trqp_scqid, gen);
	rcq = t4_ri_cq(of, q->trqp_rcqid, gen);
	if (scq == NULL || rcq == NULL || !t4_ri_id_free(of, q->trqp_sqid) ||
	    !t4_ri_id_free(of, q->trqp_rqid)) {
		mutex_exit(&of->of_ri_lock);
		kmem_free(ro, sizeof (*ro));
		t4_ofld_dma_unbind(of, sob);
		t4_ofld_dma_unbind(of, rob);
		return (EINVAL);
	}
	scq->ro_refs++;
	rcq->ro_refs++;
	t4_ri_id_set(of, q->trqp_sqid, B_TRUE);
	t4_ri_id_set(of, q->trqp_rqid, B_TRUE);
	avl_add(&of->of_ri_objs, ro);
	mutex_exit(&of->of_ri_lock);

	bzero(&wr, sizeof (wr));
	t4_ri_sqrq(&wr.res[0].u.sqrq, FW_RI_RES_TYPE_SQ, q->trqp_sqid,
	    q->trqp_scqid, q->trqp_sq_size, sob->ob_pub.trd_pa);
	t4_ri_sqrq(&wr.res[1].u.sqrq, FW_RI_RES_TYPE_RQ, q->trqp_rqid,
	    q->trqp_rcqid, q->trqp_rq_size, rob->ob_pub.trd_pa);

	rc = t4_ri_res_send(of, &wr.hdr, 2);
	if (rc == 0) {
		t4_ri_unbusy(of, ro, 0);
		return (0);
	}

	mutex_enter(&of->of_ri_lock);
	avl_remove(&of->of_ri_objs, ro);
	scq->ro_refs--;
	rcq->ro_refs--;
	if (rc != ETIMEDOUT) {
		t4_ri_id_set(of, q->trqp_sqid, B_FALSE);
		t4_ri_id_set(of, q->trqp_rqid, B_FALSE);
	}
	mutex_exit(&of->of_ri_lock);
	t4_ofld_dma_release(of, sob, rc != ETIMEDOUT);
	t4_ofld_dma_release(of, rob, rc != ETIMEDOUT);
	kmem_free(ro, sizeof (*ro));
	return (rc);
}

int
t4_ofld_qp_destroy(t4_ofld_t *of, uint32_t sqid)
{
	uint8_t types[2] = { FW_RI_RES_TYPE_SQ, FW_RI_RES_TYPE_RQ };
	uint32_t ids[2];
	t4_ri_obj_t *ro, *cq;
	boolean_t safe;
	uint32_t gen;
	int rc = 0;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if ((ro = t4_ri_busy(of, sqid, gen, B_TRUE, &rc)) == NULL)
		return (rc);

	ids[0] = ro->ro_id;
	ids[1] = ro->ro_rqid;
	if ((ro->ro_flags & ROF_INIT) == 0) {
		rc = t4_ri_reset(of, 2, types, ids);
		safe = rc == 0;
	} else {
		mutex_enter(&of->of_ri_lock);
		safe = (ro->ro_flags & (ROF_FINI_OK | ROF_GONE)) != 0;
		mutex_exit(&of->of_ri_lock);
		if (!safe)
			rc = EBUSY;
	}

	mutex_enter(&of->of_ri_lock);
	avl_remove(&of->of_ri_objs, ro);
	for (uint_t i = 0; i < 2; i++) {
		if ((cq = t4_ri_find(of, ro->ro_cq[i], gen, B_FALSE)) != NULL)
			cq->ro_refs--;
		if (safe)
			t4_ri_id_set(of, ids[i], B_FALSE);
	}
	mutex_exit(&of->of_ri_lock);
	t4_ofld_ri_untid(of, ro);
	t4_ofld_dma_release(of, ro->ro_mem[0], safe);
	t4_ofld_dma_release(of, ro->ro_mem[1], safe);
	kmem_free(ro, sizeof (*ro));
	return (rc);
}

static boolean_t
t4_ri_p2p_ok(uint8_t p2p)
{
	switch (p2p) {
	case FW_RI_INIT_P2PTYPE_RDMA_WRITE:
	case FW_RI_INIT_P2PTYPE_READ_REQ:
	case FW_RI_INIT_P2PTYPE_DISABLED:
		return (B_TRUE);
	default:
		return (B_FALSE);
	}
}

/* The ready-to-receive message the initiator's firmware sends first. */
static void
t4_ri_rtr(uint8_t p2p, struct fw_ri_init *init)
{
	bzero(&init->u, sizeof (init->u));
	switch (p2p) {
	case FW_RI_INIT_P2PTYPE_RDMA_WRITE:
		init->u.write.opcode = FW_RI_RDMA_WRITE_WR;
		init->u.write.stag_sink = BE_32(1);
		init->u.write.to_sink = BE_64(1);
		init->u.write.u.immd_src[0].op = FW_RI_DATA_IMMD;
		init->u.write.len16 = howmany(sizeof (init->u.write) +
		    sizeof (struct fw_ri_immd), 16);
		break;
	case FW_RI_INIT_P2PTYPE_READ_REQ:
		init->u.write.opcode = FW_RI_RDMA_READ_WR;
		init->u.read.stag_src = BE_32(1);
		init->u.read.to_src_lo = BE_32(1);
		init->u.read.stag_sink = BE_32(1);
		init->u.read.to_sink_lo = BE_32(1);
		init->u.read.len16 = howmany(sizeof (init->u.read), 16);
		break;
	default:
		break;
	}
}

/*
 * The connection's hwtid entry for an RDMA work request, with td_lock held
 * on success: owned, established and past FLOWC.
 */
static t4_tid_ent_t *
t4_ri_conn(t4_ofld_t *of, uint32_t tid, uint32_t gen)
{
	t4_tid_ent_t *e;

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_owned(of, T4_TID_HW, tid, gen);
	if (e == NULL || (e->te_flags & (TEF_EMBRYO | TEF_RELEASING |
	    TEF_ABORT)) != 0 || (e->te_flags & TEF_FLOWC) == 0) {
		mutex_exit(&of->of_tids.td_lock);
		return (NULL);
	}
	return (e);
}

int
t4_ofld_ri_init(t4_ofld_t *of, uint32_t tid, const t4_rdma_ri_init_t *ri)
{
	const t4_rdma_vres_t *vr = &of->of_vres;
	struct fw_ri_wr wr;
	struct fw_ri_init *init = &wr.u.init;
	t4_tid_ent_t *e;
	t4_ri_obj_t *ro;
	t4_sge_eq_t *eq;
	uint64_t cookie, rqt_end;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (servicing_interrupt())
		return (EWOULDBLOCK);
	rqt_end = (uint64_t)ri->trri_rqt_addr +
	    (uint64_t)ri->trri_rqt_size * T4_RI_ENTRY;
	if (!t4_ri_p2p_ok(ri->trri_p2p_type) ||
	    ri->trri_ord > vr->trv_max_ordird_qp ||
	    ri->trri_ird > vr->trv_max_ordird_qp ||
	    ri->trri_pdid == 0 || ri->trri_pdid > M_FW_RI_TPTE_PDID ||
	    ri->trri_nrqe > T4_RI_MAX_RQ_SIZE ||
	    ri->trri_rqt_size < 16 || !ISP2(ri->trri_rqt_size) ||
	    (ri->trri_rqt_addr % T4_RI_ENTRY) != 0 ||
	    ri->trri_rqt_addr < vr->trv_rq.trr_start ||
	    rqt_end > (uint64_t)vr->trv_rq.trr_start + vr->trv_rq.trr_size)
		return (EINVAL);
	if ((ro = t4_ri_busy(of, ri->trri_sqid, gen, B_TRUE, &rc)) == NULL)
		return (rc);
	if ((ro->ro_flags & ROF_INIT) != 0) {
		t4_ri_unbusy(of, ro, 0);
		return (EALREADY);
	}

	bzero(&wr, sizeof (wr));
	wr.op_compl = BE_32(V_FW_WR_OP(FW_RI_WR) | F_FW_WR_COMPL);
	wr.flowid_len16 = BE_32(V_FW_WR_FLOWID(tid) |
	    V_FW_WR_LEN16(howmany(sizeof (wr), 16)));
	init->type = FW_RI_TYPE_INIT;
	init->mpareqbit_p2ptype = V_FW_RI_WR_MPAREQBIT(ri->trri_initiator ?
	    1 : 0) | V_FW_RI_WR_P2PTYPE(ri->trri_p2p_type);
	init->mpa_attrs = FW_RI_MPA_IETF_ENABLE |
	    (ri->trri_crc ? FW_RI_MPA_CRC_ENABLE : 0);
	init->qp_caps = FW_RI_QP_RDMA_READ_ENABLE | FW_RI_QP_RDMA_WRITE_ENABLE |
	    FW_RI_QP_BIND_ENABLE | FW_RI_QP_FAST_REGISTER_ENABLE |
	    FW_RI_QP_STAG0_ENABLE;
	init->nrqe = BE_16(ri->trri_nrqe);
	init->pdid = BE_32(ri->trri_pdid);
	init->qpid = BE_32(ro->ro_id);
	init->sq_eqid = BE_32(ro->ro_id);
	init->rq_eqid = BE_32(ro->ro_rqid);
	init->scqid = BE_32(ro->ro_cq[0]);
	init->rcqid = BE_32(ro->ro_cq[1]);
	init->ord_max = BE_32(ri->trri_ord);
	init->ird_max = BE_32(ri->trri_ird);
	init->iss = BE_32(ri->trri_iss);
	init->irs = BE_32(ri->trri_irs);
	init->hwrqsize = BE_32(ri->trri_rqt_size);
	init->hwrqaddr = BE_32(ri->trri_rqt_addr - vr->trv_rq.trr_start);
	if (ri->trri_initiator)
		t4_ri_rtr(ri->trri_p2p_type, init);

	if ((rc = t4_ofld_waiter_get(of, &cookie)) != 0) {
		t4_ri_unbusy(of, ro, 0);
		return (rc);
	}
	wr.cookie = BE_64(cookie);
	if ((e = t4_ri_conn(of, tid, gen)) == NULL) {
		t4_ofld_waiter_put(of, cookie);
		t4_ri_unbusy(of, ro, 0);
		return (EINVAL);
	}
	eq = &of->of_port[e->te_port].op_txq;
	rc = t4_ofld_wr_send(of, eq, &wr, roundup(sizeof (wr), 16));
	if (rc == 0)
		e->te_ri = ro->ro_id;
	mutex_exit(&of->of_tids.td_lock);
	if (rc != 0) {
		t4_ofld_waiter_put(of, cookie);
		t4_ri_unbusy(of, ro, 0);
		return (rc);
	}

	mutex_enter(&of->of_ri_lock);
	ro->ro_tid = tid;
	mutex_exit(&of->of_ri_lock);
	rc = t4_ofld_waiter_wait(of, cookie);
	t4_ri_unbusy(of, ro, ROF_INIT);
	return (rc);
}

int
t4_ofld_ri_fini(t4_ofld_t *of, uint32_t tid, uint32_t sqid)
{
	struct fw_ri_wr wr;
	t4_tid_ent_t *e;
	t4_ri_obj_t *ro;
	uint64_t cookie;
	uint32_t gen;
	int rc;

	if ((rc = t4_ofld_gen(of, &gen)) != 0)
		return (rc);
	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if ((ro = t4_ri_busy(of, sqid, gen, B_TRUE, &rc)) == NULL)
		return (rc);
	if ((ro->ro_flags & ROF_INIT) == 0 || ro->ro_tid != tid) {
		t4_ri_unbusy(of, ro, 0);
		return (EINVAL);
	}

	bzero(&wr, sizeof (wr));
	wr.op_compl = BE_32(V_FW_WR_OP(FW_RI_WR) | F_FW_WR_COMPL);
	wr.flowid_len16 = BE_32(V_FW_WR_FLOWID(tid) |
	    V_FW_WR_LEN16(howmany(sizeof (wr), 16)));
	wr.u.fini.type = FW_RI_TYPE_FINI;
	if ((rc = t4_ofld_waiter_get(of, &cookie)) != 0) {
		t4_ri_unbusy(of, ro, 0);
		return (rc);
	}
	wr.cookie = BE_64(cookie);
	if ((e = t4_ri_conn(of, tid, gen)) == NULL) {
		t4_ofld_waiter_put(of, cookie);
		t4_ri_unbusy(of, ro, 0);
		return (EINVAL);
	}
	rc = t4_ofld_wr_send(of, &of->of_port[e->te_port].op_txq, &wr,
	    roundup(sizeof (wr), 16));
	mutex_exit(&of->of_tids.td_lock);
	if (rc != 0) {
		t4_ofld_waiter_put(of, cookie);
		t4_ri_unbusy(of, ro, 0);
		return (rc);
	}
	rc = t4_ofld_waiter_wait(of, cookie);
	t4_ri_unbusy(of, ro, rc == 0 ? ROF_FINI_OK : 0);
	return (rc);
}

/*
 * The chip aborted a connection: a QP bound to it has left RDMA mode.
 * Called from the CPL dispatch with no t4nex lock held.
 */
void
t4_ofld_ri_gone(t4_ofld_t *of, uint32_t tid)
{
	t4_tid_ent_t *e;
	t4_ri_obj_t key, *ro;
	uint32_t sqid;

	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_HW, tid);
	sqid = e != NULL ? e->te_ri : T4_TID_NIL;
	mutex_exit(&of->of_tids.td_lock);
	if (sqid == T4_TID_NIL || of->of_ri_used == NULL)
		return;

	mutex_enter(&of->of_ri_lock);
	key.ro_id = sqid;
	ro = avl_find(&of->of_ri_objs, &key, NULL);
	if (ro != NULL && (ro->ro_flags & ROF_QP) != 0 && ro->ro_tid == tid)
		ro->ro_flags |= ROF_GONE;
	mutex_exit(&of->of_ri_lock);
}

/* Forget the QP on its connection's hwtid entry. */
void
t4_ofld_ri_untid(t4_ofld_t *of, const t4_ri_obj_t *ro)
{
	t4_tid_ent_t *e;

	if (ro->ro_tid == T4_TID_NIL)
		return;
	mutex_enter(&of->of_tids.td_lock);
	e = t4_tid_ent(of, T4_TID_HW, ro->ro_tid);
	if (e != NULL && e->te_ri == ro->ro_id)
		e->te_ri = T4_TID_NIL;
	mutex_exit(&of->of_tids.td_lock);
}

/*
 * The client left.  Its queues may still be live in the firmware: their
 * memory goes to the quarantine and their IDs stay taken.
 */
void
t4_ofld_ri_close(t4_ofld_t *of, uint32_t gen)
{
	t4_ri_obj_t *ro, *next;

	if (of->of_ri_used == NULL)
		return;
	mutex_enter(&of->of_ri_lock);
	for (ro = avl_first(&of->of_ri_objs); ro != NULL; ro = next) {
		next = AVL_NEXT(&of->of_ri_objs, ro);
		if (ro->ro_gen != gen)
			continue;
		t4_ri_wait_idle(of, ro);
		avl_remove(&of->of_ri_objs, ro);
		mutex_exit(&of->of_ri_lock);
		t4_ofld_ri_untid(of, ro);
		for (uint_t i = 0; i < 2; i++) {
			if (ro->ro_mem[i] != NULL)
				t4_ofld_dma_release(of, ro->ro_mem[i], B_FALSE);
		}
		kmem_free(ro, sizeof (*ro));
		mutex_enter(&of->of_ri_lock);
		next = avl_first(&of->of_ri_objs);
	}
	mutex_exit(&of->of_ri_lock);
}

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
 * NVMe over Fabrics RDMA transport, controller side, written for illumos
 * from the NVMe RDMA Transport Specification.  A queue is one RC QP with
 * its own CQ.  The CM listener (nvmf_rdma_cm.c) creates it in the kernel
 * and hands it to nvmft, which owns it from then on and frees it with
 * free_qpair.  Data moves with rdk_rw (nvmf_rdma_xfer.c) through buffers
 * of the device's registered pool (nvmf_rdma_pool.c).
 *
 * Each RECV buffer holds one command capsule.  A command without in-capsule
 * data gives its buffer back at once; one with in-capsule data holds it
 * until its response is posted, and the buffer is posted again before the
 * SEND.  The host has at most depth commands without a response, so a RECV
 * is always posted for the next one.  A command context lives until nvmft
 * frees its capsule and no work request names it; a RECV that finds no free
 * context waits on the backlog.
 *
 * Every post happens with nq_lock held and the queue live, so teardown,
 * which marks the queue dying under the lock, never races a post.  Nothing
 * calls into nvmft with nq_lock held.
 *
 * Completion and event callbacks only mark the queue dying and start its
 * teardown, which runs once in an rdmak thread: destroy the CM ID, drain
 * the QP, complete the transfers still owed to nvmft, destroy the QP, CQ
 * and MRs, then free the RECV and response memory.  If the drain did not
 * account for every work request, the QP is destroyed before the transfers
 * are completed, so that their buffers go back only once the device can no
 * longer reach them; on a tainted device the pool leaks them.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/byteorder.h>
#include <sys/nvme/nvmf.h>

#include "nvmf_rdma_impl.h"

taskq_t *nvmf_rdma_taskq;

/* The largest transfer a command may name (MDTS). */
uint32_t nvmf_rdma_max_xfer = 1024 * 1024;

#define	NR_RING_CHUNK	(1024 * 1024)

static void nr_queue_teardown(void *);
static void nr_backlog_task(void *);
static void nr_recv_done(struct rdk_cq *, struct rdk_wc *);
static void nr_send_done(struct rdk_cq *, struct rdk_wc *);

static nr_cmd_t *
nr_cmd_of(struct nvmf_capsule *nc)
{
	return ((nr_cmd_t *)(void *)nc);
}

static void
nr_qp_event(struct rdk_event *ev, void *arg)
{
	nr_queue_t *q = arg;

	switch (ev->event) {
	case RDK_EVENT_COMM_EST:
	case RDK_EVENT_SQ_DRAINED:
	case RDK_EVENT_QP_LAST_WQE_REACHED:
		return;
	default:
		nr_queue_fail(q, EIO);
		return;
	}
}

/*
 * Build a queue for a CONNECT_REQUEST: CQ, QP, RECV ring, response slots,
 * contexts and transfers.  Nothing is posted yet.
 */
nr_queue_t *
nr_queue_create(nr_dev_t *nd, const nvmf_rdma_sizes_t *sz, uint16_t qid,
    uint32_t icd, uint32_t vector, int *errp)
{
	struct rdk_device *dev = nd->nd_dev;
	struct rdk_qp_init_attr init;
	struct rdk_rw_attr rwa;
	nr_queue_t *q;
	uint_t i, per, n;
	int ret;

	q = kmem_zalloc(sizeof (*q), KM_SLEEP);
	q->nq_kind = NR_KIND_QUEUE;
	mutex_init(&q->nq_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&q->nq_cv, NULL, CV_DRIVER, NULL);
	q->nq_state = NR_Q_ACCEPTING;
	q->nq_refs = 1;
	q->nq_dev = nd;
	q->nq_sz = *sz;
	q->nq_qid = qid;
	q->nq_io_icd = icd;
	q->nq_icd = qid == 0 ? 0 : icd;
	/*
	 * One keyed descriptor carries 24 bits of length, and MDTS cannot say
	 * less than 8 KiB.
	 */
	q->nq_max_xfer = MIN(MAX(nvmf_rdma_max_xfer, 8192), 0xffffff);
	q->nq_send_inv = (dev->rd_attr.device_cap_flags &
	    RDK_DEVICE_MEM_MGT_EXTENSIONS) != 0;
	q->nq_inline = dev->rd_attr.max_inline_data >= NVMF_RDMA_CQE_LEN;
	list_create(&q->nq_free_cmds, sizeof (nr_cmd_t),
	    offsetof(nr_cmd_t, nc_node));
	for (i = 0; i < NR_CIDHASH; i++) {
		list_create(&q->nq_cid[i], sizeof (nr_cmd_t),
		    offsetof(nr_cmd_t, nc_node));
	}
	list_create(&q->nq_parked, sizeof (nr_cmd_t),
	    offsetof(nr_cmd_t, nc_park));
	list_create(&q->nq_backlog, sizeof (nr_recv_t),
	    offsetof(nr_recv_t, rv_node));
	list_create(&q->nq_free_xfers, sizeof (nr_xfer_t),
	    offsetof(nr_xfer_t, xf_node));
	list_create(&q->nq_wait, sizeof (nr_xreq_t),
	    offsetof(nr_xreq_t, xr_node));
	q->nq_td = rdk_teardown_alloc(nr_queue_teardown, q);

	if ((ret = rdk_alloc_cq(dev, q, (int)sz->nrs_cq, (int)vector,
	    RDK_POLL_TASKQ, &q->nq_cq)) != 0)
		goto fail;

	(void) nr_rw_attr(nd, &rwa);
	bzero(&init, sizeof (init));
	init.event_handler = nr_qp_event;
	init.qp_context = q;
	init.send_cq = q->nq_cq;
	init.recv_cq = q->nq_cq;
	init.cap.max_send_wr = sz->nrs_sq;
	init.cap.max_recv_wr = sz->nrs_rq;
	init.cap.max_send_sge = rwa.rwa_max_sge;
	init.cap.max_recv_sge = 1;
	init.cap.max_inline_data = q->nq_inline ? NVMF_RDMA_CQE_LEN : 0;
	init.sq_sig_type = RDK_SIGNAL_REQ_WR;
	init.qp_type = RDK_QPT_RC;
	init.port_num = 1;
	if ((ret = rdk_create_qp(nd->nd_pd, &init, &q->nq_qp)) != 0)
		goto fail;

	/* RECV buffers in chunks of whole slots. */
	per = NR_RING_CHUNK / sz->nrs_slot;
	q->nq_nring = howmany(sz->nrs_depth, per);
	q->nq_ring = kmem_zalloc(sizeof (rdk_dma_buf_t) * q->nq_nring,
	    KM_SLEEP);
	q->nq_recvs = kmem_zalloc(sizeof (nr_recv_t) * sz->nrs_depth,
	    KM_SLEEP);
	for (i = 0; i < q->nq_nring; i++) {
		n = MIN(per, sz->nrs_depth - i * per);
		if ((ret = rdk_dma_buf_alloc(dev, (size_t)n * sz->nrs_slot,
		    &q->nq_ring[i])) != 0)
			goto fail;
	}
	for (i = 0; i < sz->nrs_depth; i++) {
		nr_recv_t *r = &q->nq_recvs[i];
		rdk_dma_buf_t *b = &q->nq_ring[i / per];
		size_t off = (size_t)(i % per) * sz->nrs_slot;

		r->rv_cqe.done = nr_recv_done;
		r->rv_q = q;
		r->rv_va = b->rdb_va + off;
		r->rv_pa = b->rdb_pa + off;
		r->rv_len = NVMF_RDMA_SQE_LEN + q->nq_icd;
	}

	if ((ret = rdk_dma_buf_alloc(dev, (size_t)sz->nrs_cmds *
	    NVMF_RDMA_CQE_LEN, &q->nq_cqebuf)) != 0)
		goto fail;
	q->nq_cmds = kmem_zalloc(sizeof (nr_cmd_t) * sz->nrs_cmds, KM_SLEEP);
	for (i = 0; i < sz->nrs_cmds; i++) {
		nr_cmd_t *c = &q->nq_cmds[i];

		c->nc_kind = NR_KIND_CMD;
		c->nc_q = q;
		c->nc_cqe = (nvme_cqe_t *)(void *)(q->nq_cqebuf.rdb_va +
		    (size_t)i * NVMF_RDMA_CQE_LEN);
		c->nc_cqe_pa = q->nq_cqebuf.rdb_pa +
		    (uint64_t)i * NVMF_RDMA_CQE_LEN;
		c->nc_send_cqe.done = nr_send_done;
		list_insert_tail(&q->nq_free_cmds, c);
	}

	if ((ret = nr_xfer_init(q)) != 0)
		goto fail;
	return (q);
fail:
	*errp = ret;
	nr_queue_destroy_unadopted(q);
	return (NULL);
}

/* Called with nq_lock held. */
static int
nr_recv_post_locked(nr_queue_t *q, nr_recv_t *r)
{
	struct rdk_recv_wr wr;
	struct rdk_sge sge;
	int ret;

	ASSERT(MUTEX_HELD(&q->nq_lock));
	ASSERT(!r->rv_posted);
	if (q->nq_state != NR_Q_LIVE && q->nq_state != NR_Q_ACCEPTING)
		return (ENOTCONN);
	sge.addr = r->rv_pa;
	sge.length = r->rv_len;
	sge.lkey = q->nq_dev->nd_pd->local_dma_lkey;
	bzero(&wr, sizeof (wr));
	wr.wr_cqe = &r->rv_cqe;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	if ((ret = rdk_post_recv(q->nq_qp, &wr, NULL)) == 0)
		r->rv_posted = B_TRUE;
	return (ret);
}

int
nr_queue_post_ring(nr_queue_t *q)
{
	uint_t i;
	int ret = 0;

	mutex_enter(&q->nq_lock);
	for (i = 0; i < q->nq_sz.nrs_depth && ret == 0; i++)
		ret = nr_recv_post_locked(q, &q->nq_recvs[i]);
	mutex_exit(&q->nq_lock);
	return (ret);
}

/* Start the teardown; any context, but not with nq_lock held. */
void
nr_queue_fail(nr_queue_t *q, int error)
{
	mutex_enter(&q->nq_lock);
	if (q->nq_state == NR_Q_ACCEPTING || q->nq_state == NR_Q_LIVE) {
		q->nq_state = NR_Q_DYING;
		q->nq_error = error;
	}
	mutex_exit(&q->nq_lock);
	(void) rdk_teardown_start(q->nq_td);
}

/* A failed post leaves the queue unusable. */
void
nr_queue_fail_locked(nr_queue_t *q, int error)
{
	ASSERT(MUTEX_HELD(&q->nq_lock));
	if (q->nq_state == NR_Q_ACCEPTING || q->nq_state == NR_Q_LIVE) {
		q->nq_state = NR_Q_DYING;
		q->nq_error = error;
	}
	(void) rdk_teardown_start(q->nq_td);
}

int
nr_post_locked(nr_queue_t *q, struct rdk_send_wr *wr)
{
	int ret;

	ASSERT(MUTEX_HELD(&q->nq_lock));
	if (q->nq_state != NR_Q_LIVE)
		return (ENOTCONN);
	if ((ret = rdk_post_send(q->nq_qp, wr, NULL)) != 0)
		nr_queue_fail_locked(q, EIO);
	return (ret);
}

static void
nr_cid_insert_locked(nr_queue_t *q, nr_cmd_t *c)
{
	list_insert_head(&q->nq_cid[nvmf_rdma_cid_hash(c->nc_cid)], c);
	c->nc_hashed = B_TRUE;
}

static void
nr_cid_remove_locked(nr_queue_t *q, nr_cmd_t *c)
{
	if (c->nc_hashed) {
		list_remove(&q->nq_cid[nvmf_rdma_cid_hash(c->nc_cid)], c);
		c->nc_hashed = B_FALSE;
	}
}

/*
 * The newest command with this CID that still owes a response, so that the
 * rejection of a duplicate finds the duplicate.
 */
static nr_cmd_t *
nr_cid_find_locked(nr_queue_t *q, uint16_t cid)
{
	list_t *l = &q->nq_cid[nvmf_rdma_cid_hash(cid)];
	nr_cmd_t *c;

	for (c = list_head(l); c != NULL; c = list_next(l, c)) {
		if (c->nc_cid == cid && c->nc_state == NR_C_ACTIVE)
			return (c);
	}
	return (NULL);
}

/* Give back a held RECV buffer; the command no longer needs its data. */
static void
nr_cmd_unhold_locked(nr_cmd_t *c)
{
	nr_queue_t *q = c->nc_q;
	nr_recv_t *r = c->nc_recv;

	if (r == NULL)
		return;
	c->nc_recv = NULL;
	if (nr_recv_post_locked(q, r) != 0 &&
	    (q->nq_state == NR_Q_LIVE || q->nq_state == NR_Q_ACCEPTING))
		nr_queue_fail_locked(q, EIO);
}

static void
nr_backlog_kick_locked(nr_queue_t *q)
{
	if (list_is_empty(&q->nq_backlog) || q->nq_backlog_queued ||
	    q->nq_state != NR_Q_LIVE)
		return;
	q->nq_backlog_queued = B_TRUE;
	q->nq_refs++;
	taskq_dispatch_ent(nvmf_rdma_taskq, nr_backlog_task, q, 0,
	    &q->nq_backlog_ent);
}

/* Return a context once nvmft and the device are both done with it. */
void
nr_cmd_rele_locked(nr_cmd_t *c)
{
	nr_queue_t *q = c->nc_q;

	ASSERT(MUTEX_HELD(&q->nq_lock));
	if (c->nc_capsule || c->nc_wrs != 0 || c->nc_state == NR_C_FREE)
		return;
	nr_cid_remove_locked(q, c);
	nr_cmd_unhold_locked(c);
	c->nc_state = NR_C_FREE;
	list_insert_tail(&q->nq_free_cmds, c);
	nr_backlog_kick_locked(q);
}

/*
 * Take a context for the capsule in r.  Returns the command, to be handed
 * to nvmft once nq_lock is dropped, or NULL if it parked or failed.
 */
static nr_cmd_t *
nr_cmd_start_locked(nr_queue_t *q, nr_recv_t *r, uint32_t byte_len)
{
	nr_cmd_t *c;

	ASSERT(MUTEX_HELD(&q->nq_lock));
	c = list_remove_head(&q->nq_free_cmds);
	if (c == NULL) {
		r->rv_byte_len = byte_len;
		list_insert_tail(&q->nq_backlog, r);
		return (NULL);
	}
	bzero(&c->nc_nc, sizeof (c->nc_nc));
	c->nc_nc.nc_qpair = &q->nq_nq;
	c->nc_nc.nc_qe_len = NVMF_RDMA_SQE_LEN;
	bcopy(r->rv_va, &c->nc_nc.nc_sqe, NVMF_RDMA_SQE_LEN);
	c->nc_cid = c->nc_nc.nc_sqe.sqe_cid;
	c->nc_icd = byte_len - NVMF_RDMA_SQE_LEN;
	c->nc_sgl_done = B_FALSE;
	c->nc_state = NR_C_ACTIVE;
	c->nc_final = NULL;
	nr_cid_insert_locked(q, c);

	if (!q->nq_have_connect) {
		q->nq_have_connect = B_TRUE;
		q->nq_connect_cid = c->nc_cid;
	}
	if (c->nc_icd != 0) {
		c->nc_recv = r;
	} else if (nr_recv_post_locked(q, r) != 0) {
		nr_queue_fail_locked(q, EIO);
	}
	if (q->nq_state == NR_Q_ACCEPTING) {
		list_insert_tail(&q->nq_parked, c);
		return (NULL);
	}
	c->nc_capsule = B_TRUE;
	q->nq_refs++;
	return (c);
}

static void
nr_recv_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	nr_recv_t *r = (nr_recv_t *)(void *)wc->wr_cqe;
	nr_queue_t *q = r->rv_q;
	nr_cmd_t *c = NULL;
	int error = 0;

	_NOTE(ARGUNUSED(cq));
	mutex_enter(&q->nq_lock);
	r->rv_posted = B_FALSE;
	if (wc->status != RDK_WC_SUCCESS) {
		if (wc->status != RDK_WC_WR_FLUSH_ERR)
			error = EIO;
	} else if (wc->byte_len < NVMF_RDMA_SQE_LEN ||
	    wc->byte_len > r->rv_len) {
		error = EPROTO;
	} else if (!list_is_empty(&q->nq_backlog)) {
		/* Behind the commands already waiting for a context. */
		r->rv_byte_len = wc->byte_len;
		list_insert_tail(&q->nq_backlog, r);
	} else if (q->nq_state == NR_Q_LIVE || q->nq_state == NR_Q_ACCEPTING) {
		c = nr_cmd_start_locked(q, r, wc->byte_len);
	}
	if (error != 0)
		nr_queue_fail_locked(q, error);
	mutex_exit(&q->nq_lock);
	if (c != NULL)
		nvmf_capsule_received(&q->nq_nq, &c->nc_nc);
}

static void
nr_backlog_task(void *arg)
{
	nr_queue_t *q = arg;
	nr_recv_t *r;
	nr_cmd_t *c;

	mutex_enter(&q->nq_lock);
	while (q->nq_state == NR_Q_LIVE && !list_is_empty(&q->nq_free_cmds) &&
	    (r = list_remove_head(&q->nq_backlog)) != NULL) {
		c = nr_cmd_start_locked(q, r, r->rv_byte_len);
		if (c == NULL)
			continue;
		mutex_exit(&q->nq_lock);
		nvmf_capsule_received(&q->nq_nq, &c->nc_nc);
		mutex_enter(&q->nq_lock);
	}
	q->nq_backlog_queued = B_FALSE;
	mutex_exit(&q->nq_lock);
	nr_queue_rele(q);
}

/* ESTABLISHED: hand nvmft what arrived before it, in order. */
void
nr_queue_established(nr_queue_t *q)
{
	nr_cmd_t *c;

	mutex_enter(&q->nq_lock);
	q->nq_established = B_TRUE;
	while (q->nq_state == NR_Q_ACCEPTING &&
	    (c = list_remove_head(&q->nq_parked)) != NULL) {
		c->nc_capsule = B_TRUE;
		q->nq_refs++;
		mutex_exit(&q->nq_lock);
		nvmf_capsule_received(&q->nq_nq, &c->nc_nc);
		mutex_enter(&q->nq_lock);
	}
	if (q->nq_state == NR_Q_ACCEPTING)
		q->nq_state = NR_Q_LIVE;
	nr_backlog_kick_locked(q);
	mutex_exit(&q->nq_lock);
}

/*
 * Fill in the response SEND of a command and give back its RECV buffer
 * first.  Returns the SEND to post; the caller then calls
 * nr_respond_posted_locked() or nr_respond_unposted_locked().
 */
int
nr_respond_locked(nr_cmd_t *c, const nvme_cqe_t *cqe,
    struct rdk_send_wr **wrp, boolean_t inv)
{
	nr_queue_t *q = c->nc_q;
	const uint8_t *b = (const uint8_t *)cqe;
	uint16_t status;

	ASSERT(MUTEX_HELD(&q->nq_lock));
	if (q->nq_state != NR_Q_LIVE)
		return (ENOTCONN);
	if (c->nc_state != NR_C_ACTIVE || c->nc_send_posted)
		return (EALREADY);
	nr_cmd_unhold_locked(c);
	nr_cid_remove_locked(q, c);
	c->nc_state = NR_C_DONE;

	bcopy(cqe, c->nc_cqe, NVMF_RDMA_CQE_LEN);
	c->nc_ssge.addr = c->nc_cqe_pa;
	c->nc_ssge.length = NVMF_RDMA_CQE_LEN;
	c->nc_ssge.lkey = q->nq_dev->nd_pd->local_dma_lkey;
	bzero(&c->nc_swr, sizeof (c->nc_swr));
	c->nc_swr.wr_cqe = &c->nc_send_cqe;
	c->nc_swr.sg_list = &c->nc_ssge;
	c->nc_swr.num_sge = 1;
	c->nc_swr.opcode = RDK_WR_SEND;
	c->nc_swr.send_flags = RDK_SEND_SIGNALED;
	if (q->nq_inline) {
		c->nc_ssge.addr = (uint64_t)(uintptr_t)c->nc_cqe;
		c->nc_swr.send_flags |= RDK_SEND_INLINE;
	}
	/* The host's key is ours to invalidate only if its SGL was valid. */
	if (inv && c->nc_sgl_done && c->nc_sgl_sc == 0 &&
	    c->nc_sgl.nsl_keyed && c->nc_sgl.nsl_invalidate &&
	    q->nq_send_inv) {
		c->nc_swr.opcode = RDK_WR_SEND_WITH_INV;
		c->nc_swr.ex.invalidate_rkey = c->nc_sgl.nsl_key;
	}

	status = (uint16_t)(b[14] | (b[15] << 8));
	if (!q->nq_connected && q->nq_have_connect &&
	    c->nc_cid == q->nq_connect_cid && (status >> 1) == 0) {
		q->nq_connected = B_TRUE;
		if (q->nq_qid == 0)
			q->nq_cntlid = (uint16_t)(b[0] | (b[1] << 8));
	}
	*wrp = &c->nc_swr;
	return (0);
}

void
nr_respond_posted_locked(nr_cmd_t *c)
{
	c->nc_send_posted = B_TRUE;
	c->nc_wrs++;
}

void
nr_respond_unposted_locked(nr_cmd_t *c)
{
	ASSERT(!c->nc_send_posted);
	nr_cmd_rele_locked(c);
}

/*
 * The final transfer of a read learns of its response here, while the SEND
 * still holds the context.
 */
static void
nr_send_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	nr_cmd_t *c = (nr_cmd_t *)(void *)((caddr_t)wc->wr_cqe -
	    offsetof(nr_cmd_t, nc_send_cqe));
	nr_queue_t *q = c->nc_q;
	boolean_t ok = wc->status == RDK_WC_SUCCESS;

	_NOTE(ARGUNUSED(cq));
	mutex_enter(&q->nq_lock);
	if (!c->nc_send_posted) {
		mutex_exit(&q->nq_lock);
		return;
	}
	mutex_exit(&q->nq_lock);

	nr_xreq_send_done(c, ok);

	mutex_enter(&q->nq_lock);
	c->nc_send_posted = B_FALSE;
	c->nc_wrs--;
	if (!ok && wc->status != RDK_WC_WR_FLUSH_ERR)
		nr_queue_fail_locked(q, EIO);
	nr_cmd_rele_locked(c);
	mutex_exit(&q->nq_lock);
}

/*
 * Transport operations.
 */

/* ARGSUSED */
static struct nvmf_qpair *
nr_allocate_qpair(boolean_t controller, const nvlist_t *nvl)
{
	return (NULL);
}

void
nr_queue_rele(nr_queue_t *q)
{
	uint_t i;

	mutex_enter(&q->nq_lock);
	ASSERT3U(q->nq_refs, >, 0);
	if (--q->nq_refs != 0) {
		mutex_exit(&q->nq_lock);
		return;
	}
	mutex_exit(&q->nq_lock);

	ASSERT(q->nq_state == NR_Q_DEAD);
	nr_queue_gone(q);
	rdk_teardown_free(q->nq_td);
	while (list_remove_head(&q->nq_free_cmds) != NULL)
		;
	if (q->nq_cmds != NULL)
		kmem_free(q->nq_cmds, sizeof (nr_cmd_t) * q->nq_sz.nrs_cmds);
	if (q->nq_recvs != NULL) {
		kmem_free(q->nq_recvs, sizeof (nr_recv_t) *
		    q->nq_sz.nrs_depth);
	}
	if (q->nq_ring != NULL)
		kmem_free(q->nq_ring, sizeof (rdk_dma_buf_t) * q->nq_nring);
	for (i = 0; i < NR_CIDHASH; i++)
		list_destroy(&q->nq_cid[i]);
	list_destroy(&q->nq_free_cmds);
	list_destroy(&q->nq_parked);
	list_destroy(&q->nq_backlog);
	list_destroy(&q->nq_free_xfers);
	list_destroy(&q->nq_wait);
	cv_destroy(&q->nq_cv);
	mutex_destroy(&q->nq_lock);
	kmem_free(q, sizeof (*q));
}

/*
 * The queue's teardown, run once by rdmak away from callbacks.  nvmft may
 * still call in with capsules; with the queue dying those calls fail.
 */
static void
nr_queue_teardown(void *arg)
{
	nr_queue_t *q = arg;
	struct rdk_device *dev = q->nq_dev->nd_dev;
	boolean_t drained, report;
	rdk_cm_id_t *cmid;
	nr_cmd_t *c;
	uint_t i;
	int error;

	mutex_enter(&q->nq_lock);
	if (q->nq_state == NR_Q_ACCEPTING || q->nq_state == NR_Q_LIVE) {
		q->nq_state = NR_Q_DYING;
		if (q->nq_error == 0)
			q->nq_error = ECONNABORTED;
	}
	cmid = q->nq_cmid;
	q->nq_cmid = NULL;
	/* Capsules that never reached nvmft go back now. */
	while ((c = list_remove_head(&q->nq_parked)) != NULL) {
		c->nc_state = NR_C_DONE;
		nr_cmd_rele_locked(c);
	}
	mutex_exit(&q->nq_lock);

	/* No CM event follows, and the connection is aborted. */
	if (cmid != NULL)
		(void) rdk_cm_destroy_id(cmid);

	if (q->nq_qp != NULL)
		rdk_drain_qp(q->nq_qp);

	mutex_enter(&q->nq_lock);
	drained = B_TRUE;
	for (i = 0; q->nq_recvs != NULL && i < q->nq_sz.nrs_depth; i++) {
		if (q->nq_recvs[i].rv_posted)
			drained = B_FALSE;
	}
	for (i = 0; q->nq_cmds != NULL && i < q->nq_sz.nrs_cmds; i++) {
		if (q->nq_cmds[i].nc_send_posted)
			drained = B_FALSE;
	}
	for (i = 0; q->nq_xfers != NULL && i < q->nq_sz.nrs_xfers; i++) {
		if (q->nq_xfers[i].xf_busy)
			drained = B_FALSE;
	}
	mutex_exit(&q->nq_lock);

	/*
	 * Until the QP is destroyed a request the drain missed may still
	 * reach its buffer, so its transfer is completed only after that.
	 */
	if (drained)
		nr_xfer_fail_all(q);
	if (q->nq_qp != NULL) {
		rdk_destroy_qp(q->nq_qp);
		q->nq_qp = NULL;
	}

	mutex_enter(&q->nq_lock);
	q->nq_state = NR_Q_DEAD;
	mutex_exit(&q->nq_lock);
	nr_xfer_fail_all(q);

	mutex_enter(&q->nq_lock);
	for (i = 0; q->nq_recvs != NULL && i < q->nq_sz.nrs_depth; i++)
		q->nq_recvs[i].rv_posted = B_FALSE;
	for (i = 0; q->nq_cmds != NULL && i < q->nq_sz.nrs_cmds; i++) {
		c = &q->nq_cmds[i];
		c->nc_recv = NULL;
		nr_cid_remove_locked(q, c);
	}
	while (list_remove_head(&q->nq_backlog) != NULL)
		;
	report = q->nq_adopted && !q->nq_reported;
	q->nq_reported = B_TRUE;
	error = q->nq_error;
	mutex_exit(&q->nq_lock);

	nr_xfer_fini(q);
	if (q->nq_cq != NULL) {
		rdk_free_cq(q->nq_cq);
		q->nq_cq = NULL;
	}
	for (i = 0; q->nq_ring != NULL && i < q->nq_nring; i++)
		rdk_dma_buf_free(dev, &q->nq_ring[i]);
	rdk_dma_buf_free(dev, &q->nq_cqebuf);

	nr_queue_detach(q);
	if (report)
		nvmf_qpair_error(&q->nq_nq, error == ECONNRESET ? 0 : error);
}

/*
 * A queue nvmft never took: tear it down here.  The caller holds no lock
 * and is not in a callback.
 */
void
nr_queue_destroy_unadopted(nr_queue_t *q)
{
	ASSERT(!q->nq_adopted);
	(void) rdk_teardown_start(q->nq_td);
	rdk_teardown_wait(q->nq_td);
	nr_queue_rele(q);
}

static void
nr_free_qpair(struct nvmf_qpair *nq)
{
	nr_queue_t *q = NR_Q(nq);

	mutex_enter(&q->nq_lock);
	q->nq_reported = B_TRUE;
	mutex_exit(&q->nq_lock);
	(void) rdk_teardown_start(q->nq_td);
	rdk_teardown_wait(q->nq_td);
	nr_queue_rele(q);
}

/* nvmft asks on the admin queue; the answer is for the I/O queues. */
static uint32_t
nr_max_ioccsz(struct nvmf_qpair *nq)
{
	return (NVMF_RDMA_SQE_LEN + NR_Q(nq)->nq_io_icd);
}

static uint64_t
nr_max_xfer_size(struct nvmf_qpair *nq)
{
	return (NR_Q(nq)->nq_max_xfer);
}

static uint32_t
nr_caps(struct nvmf_qpair *nq)
{
	uint32_t caps = NVMF_QP_CAP_SGL_KEYED | NVMF_QP_CAP_UNORDERED_DATA |
	    NVMF_QP_CAP_ALWAYS_RESPONSE | NVMF_QP_CAP_NOSLEEP_RECEIVE |
	    NVMF_QP_CAP_DATA_BUF;

	if (nr_max_ioccsz(nq) > NVMF_RDMA_SQE_LEN)
		caps |= NVMF_QP_CAP_SGL_OFFSET;
	return (caps);
}

static struct nvmf_capsule *
nr_allocate_capsule(struct nvmf_qpair *nq, int how)
{
	nr_queue_t *q = NR_Q(nq);
	nr_rsp_t *rs;

	rs = kmem_zalloc(sizeof (*rs), how);
	if (rs == NULL)
		return (NULL);
	rs->rs_kind = NR_KIND_RSP;
	mutex_enter(&q->nq_lock);
	q->nq_refs++;
	mutex_exit(&q->nq_lock);
	return (&rs->rs_nc);
}

static void
nr_free_capsule(struct nvmf_capsule *nc)
{
	nr_rsp_t *rs = (nr_rsp_t *)(void *)nc;
	nr_cmd_t *c = nr_cmd_of(nc);
	nr_queue_t *q;

	if (rs->rs_kind == NR_KIND_RSP) {
		q = NR_Q(nc->nc_qpair);
		kmem_free(rs, sizeof (*rs));
		nr_queue_rele(q);
		return;
	}
	VERIFY3U(c->nc_kind, ==, NR_KIND_CMD);
	q = c->nc_q;
	mutex_enter(&q->nq_lock);
	VERIFY(c->nc_capsule);
	c->nc_capsule = B_FALSE;
	if (c->nc_state == NR_C_ACTIVE)
		c->nc_state = NR_C_DONE;
	nr_cmd_rele_locked(c);
	mutex_exit(&q->nq_lock);
	nr_queue_rele(q);
}

static int
nr_transmit_capsule(struct nvmf_capsule *nc)
{
	nr_rsp_t *rs = (nr_rsp_t *)(void *)nc;
	nr_queue_t *q = NR_Q(nc->nc_qpair);
	struct rdk_send_wr *wr;
	boolean_t connected;
	nr_cmd_t *c;
	int ret;

	if (rs->rs_kind != NR_KIND_RSP || nc->nc_qe_len != NVMF_RDMA_CQE_LEN)
		return (EINVAL);
	mutex_enter(&q->nq_lock);
	connected = q->nq_connected;
	if (q->nq_state != NR_Q_LIVE) {
		ret = ENOTCONN;
	} else if ((c = nr_cid_find_locked(q, nc->nc_cqe.cqe_cid)) == NULL) {
		ret = ENOENT;
	} else if ((ret = nr_respond_locked(c, &nc->nc_cqe, &wr,
	    B_TRUE)) == 0) {
		if ((ret = nr_post_locked(q, wr)) == 0)
			nr_respond_posted_locked(c);
		else
			nr_respond_unposted_locked(c);
	}
	connected = !connected && q->nq_connected;
	mutex_exit(&q->nq_lock);
	if (connected)
		nr_queue_connected(q);
	return (ret);
}

/* Decode SGL1 once, against the in-capsule data that arrived. */
static uint8_t
nr_decode_sgl(nr_cmd_t *c)
{
	nr_queue_t *q = c->nc_q;

	if (!c->nc_sgl_done) {
		c->nc_sgl_sc = nvmf_sgl_decode(&c->nc_nc.nc_sqe, c->nc_icd,
		    q->nq_max_xfer, &c->nc_sgl);
		c->nc_sgl_done = B_TRUE;
	}
	return (c->nc_sgl_sc);
}

static uint8_t
nr_validate_command_capsule(struct nvmf_capsule *nc)
{
	nr_cmd_t *c = nr_cmd_of(nc);

	if (c->nc_kind != NR_KIND_CMD)
		return (NVME_CQE_SC_GEN_INV_FLD);
	return (nr_decode_sgl(c));
}

static size_t
nr_capsule_data_len(const struct nvmf_capsule *nc)
{
	nr_cmd_t *c = nr_cmd_of((struct nvmf_capsule *)nc);

	if (c->nc_kind != NR_KIND_CMD || nr_decode_sgl(c) != 0)
		return (0);
	return (c->nc_sgl.nsl_len);
}

/* Data goes out only through send_controller_data_io. */
/* ARGSUSED */
static uint_t
nr_send_controller_data(struct nvmf_capsule *nc, uint32_t off, mblk_t *mp,
    size_t len)
{
	freemsg(mp);
	return (NVME_CQE_SC_GEN_INTERNAL_ERR);
}

struct nvmf_transport_ops nvmf_rdma_ops = {
	.allocate_qpair = nr_allocate_qpair,
	.free_qpair = nr_free_qpair,
	.max_ioccsz = nr_max_ioccsz,
	.max_xfer_size = nr_max_xfer_size,
	.allocate_capsule = nr_allocate_capsule,
	.free_capsule = nr_free_capsule,
	.transmit_capsule = nr_transmit_capsule,
	.validate_command_capsule = nr_validate_command_capsule,
	.capsule_data_len = nr_capsule_data_len,
	.receive_controller_data = nr_receive_controller_data,
	.send_controller_data = nr_send_controller_data,
	.send_controller_data_io = nr_send_controller_data_io,
	.caps = nr_caps,
	.alloc_data_buf = nr_alloc_data_buf,
	.free_data_buf = nr_free_data_buf,
	.trtype = NVMF_TRTYPE_RDMA,
	.priority = 0
};

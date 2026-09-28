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
 * Command data for the RDMA transport.  In-capsule data is copied out of
 * the RECV buffer.  Keyed data moves by RDMA READ (host writes) or RDMA
 * WRITE (host reads) through rdk_rw, each transfer only within its
 * command's SGL and with the host's key; a read's last transfer carries
 * the response SEND in the same post.  A transfer that finds no free
 * rdk_rw context waits, in order, so a response never overtakes the data
 * before it.
 *
 * Local memory is a pool buffer with a single cookie; anything else goes
 * through a bounce buffer from the pool.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/nvme/nvmf.h>

#include "nvmf_rdma_impl.h"

static void nr_rw_done(struct rdk_cq *, struct rdk_wc *);

/*
 * The rdk_rw attributes of a device's transfers; returns the most bytes
 * one transfer moves.  On iWARP each READ sink is an MR over the transfer,
 * which bounds it by the page list.
 */
uint_t
nr_rw_attr(nr_dev_t *nd, struct rdk_rw_attr *a)
{
	const struct rdk_device_attr *da = &nd->nd_dev->rd_attr;
	uint64_t max = nvmf_rdma_dbuf_max;
	uint32_t pages;

	bzero(a, sizeof (*a));
	a->rwa_max_sge = (uint32_t)MAX(1, MIN(2, da->max_send_sge));
	a->rwa_max_cookies = 1;
	a->rwa_max_segs = 1;
	if (nd->nd_iwarp) {
		pages = MIN(da->max_fast_reg_page_list_len,
		    (uint32_t)(max / PAGESIZE) + 1);
		if (pages < 2)
			return (0);
		max = MIN(max, (uint64_t)(pages - 1) * PAGESIZE);
		a->rwa_mr_pages = pages;
	}
	if (da->max_mr_size != 0)
		max = MIN(max, da->max_mr_size);
	a->rwa_max_len = (uint32_t)max;
	return ((uint_t)max);
}

int
nr_xfer_init(nr_queue_t *q)
{
	nr_dev_t *nd = q->nq_dev;
	struct rdk_rw_attr a;
	struct rdk_rw_limits l;
	nr_xfer_t *xf;
	uint_t i, j;
	int ret;

	if (nr_rw_attr(nd, &a) == 0)
		return (ENOTSUP);
	if ((ret = rdk_rw_limits(nd->nd_dev, &a, &l)) != 0)
		return (ret);
	q->nq_xfer_len = a.rwa_max_len;
	q->nq_xfers = kmem_zalloc(sizeof (nr_xfer_t) * q->nq_sz.nrs_xfers,
	    KM_SLEEP);
	for (i = 0; i < q->nq_sz.nrs_xfers; i++) {
		xf = &q->nq_xfers[i];
		xf->xf_q = q;
		xf->xf_cqe.done = nr_rw_done;
		if ((ret = rdk_rw_ctx_alloc(nd->nd_dev, &a, &xf->xf_rw)) != 0)
			return (ret);
		if (l.rwl_mrs != 0) {
			xf->xf_mrs = kmem_zalloc(sizeof (struct rdk_mr *) *
			    l.rwl_mrs, KM_SLEEP);
			xf->xf_mrs_len = l.rwl_mrs;
		}
		for (j = 0; j < l.rwl_mrs; j++) {
			if ((ret = rdk_alloc_mr(nd->nd_pd, RDK_MR_TYPE_MEM_REG,
			    a.rwa_mr_pages, &xf->xf_mrs[j])) != 0)
				return (ret);
			xf->xf_nmrs++;
		}
		list_insert_tail(&q->nq_free_xfers, xf);
	}
	return (0);
}

/* After the QP is destroyed; an MR that fails to go taints the device. */
void
nr_xfer_fini(nr_queue_t *q)
{
	nr_xfer_t *xf;
	uint_t i, j;

	if (q->nq_xfers == NULL)
		return;
	for (i = 0; i < q->nq_sz.nrs_xfers; i++) {
		xf = &q->nq_xfers[i];
		for (j = 0; j < xf->xf_nmrs; j++)
			(void) rdk_dereg_mr(xf->xf_mrs[j]);
		if (xf->xf_mrs != NULL) {
			kmem_free(xf->xf_mrs, sizeof (struct rdk_mr *) *
			    xf->xf_mrs_len);
		}
		if (xf->xf_rw != NULL)
			rdk_rw_ctx_free(xf->xf_rw);
	}
	while (list_remove_head(&q->nq_free_xfers) != NULL)
		;
	kmem_free(q->nq_xfers, sizeof (nr_xfer_t) * q->nq_sz.nrs_xfers);
	q->nq_xfers = NULL;
}

/* A transfer is over: its callback runs once nq_lock is dropped. */
static void
nr_xreq_finish_locked(nr_xreq_t *xr, uint_t status, list_t *done)
{
	nr_cmd_t *c = xr->xr_cmd;

	ASSERT(xr->xr_state != NR_X_FREE && xr->xr_state != NR_X_DONE);
	if (c->nc_final == xr)
		c->nc_final = NULL;
	xr->xr_status = status;
	xr->xr_state = NR_X_DONE;
	list_insert_tail(done, xr);
}

static void
nr_xreq_fail_locked(nr_xreq_t *xr, list_t *done)
{
	nr_xreq_finish_locked(xr, xr->xr_read ? EIO :
	    NVME_CQE_SC_GEN_DATA_XFR_ERR, done);
}

static void
nr_xfer_put_locked(nr_queue_t *q, nr_xfer_t *xf)
{
	xf->xf_busy = B_FALSE;
	xf->xf_req = NULL;
	list_insert_tail(&q->nq_free_xfers, xf);
}

/*
 * Post a transfer, and for a read's last one the response after it.  A
 * post that fails may leave part of the chain queued, so the transfer then
 * stays posted and the teardown completes it.
 */
static void
nr_xreq_post_locked(nr_queue_t *q, nr_xreq_t *xr, nr_xfer_t *xf,
    list_t *done)
{
	nr_cmd_t *c = xr->xr_cmd;
	struct rdk_send_wr *next = NULL;
	struct rdk_rw_seg seg;
	int ret;

	ASSERT(MUTEX_HELD(&q->nq_lock));
	seg.rs_addr = c->nc_sgl.nsl_addr + xr->xr_off;
	seg.rs_len = xr->xr_len;
	seg.rs_key = c->nc_sgl.nsl_key;
	ret = rdk_rw_init(xf->xf_rw, q->nq_qp, xr->xr_read ? RDK_RW_READ :
	    RDK_RW_WRITE, &xr->xr_ck, 1, 0, xr->xr_len, &seg, 1, xf->xf_mrs,
	    xf->xf_nmrs);
	if (ret == 0 && xr->xr_final) {
		ret = nr_respond_locked(c, &xr->xr_cqe, &next, B_FALSE);
		if (ret == 0 && c->nc_sgl.nsl_invalidate)
			(void) rdk_rw_send_inv(xf->xf_rw, next);
	}
	if (ret != 0) {
		nr_xfer_put_locked(q, xf);
		nr_xreq_fail_locked(xr, done);
		return;
	}

	xf->xf_busy = B_TRUE;
	xf->xf_req = xr;
	xr->xr_xfer = xf;
	xr->xr_state = NR_X_POSTED;
	if (xr->xr_final) {
		c->nc_final = xr;
		nr_respond_posted_locked(c);
	}
	if (rdk_rw_post(xf->xf_rw, &xf->xf_cqe, next) != 0)
		nr_queue_fail_locked(q, EIO);
}

/* Start what waits, in order, while contexts are free. */
static void
nr_xreq_run_locked(nr_queue_t *q, list_t *done)
{
	nr_xreq_t *xr;
	nr_xfer_t *xf;

	while (q->nq_state == NR_Q_LIVE && !list_is_empty(&q->nq_wait) &&
	    (xf = list_remove_head(&q->nq_free_xfers)) != NULL) {
		xr = list_remove_head(&q->nq_wait);
		nr_xreq_post_locked(q, xr, xf, done);
	}
}

/*
 * Run the callbacks of finished transfers; nq_lock is not held.  A slot is
 * free again before its callback, which may start the next transfer; the
 * command stays held until the callback returns.
 */
static void
nr_xreq_callbacks(nr_queue_t *q, list_t *done)
{
	nvmf_io_complete_t *io_cb;
	nvmf_send_complete_t *send_cb;
	nr_xreq_t *xr;
	nr_cmd_t *c;
	void *arg;
	uint_t status;
	uint32_t len;
	boolean_t read;

	while ((xr = list_remove_head(done)) != NULL) {
		c = xr->xr_cmd;
		read = xr->xr_read;
		status = xr->xr_status;
		len = xr->xr_len;
		io_cb = xr->xr_io_cb;
		send_cb = xr->xr_send_cb;
		arg = xr->xr_cb_arg;
		if (read && status == 0 && xr->xr_bounce != NULL) {
			nvmf_memdesc_copyin(&xr->xr_mem, 0,
			    xr->xr_bounce->nb_va, len);
		}
		if (xr->xr_bounce != NULL) {
			nr_buf_free(xr->xr_bounce);
			xr->xr_bounce = NULL;
		}
		mutex_enter(&q->nq_lock);
		xr->xr_state = NR_X_FREE;
		mutex_exit(&q->nq_lock);

		if (read)
			io_cb(arg, status == 0 ? len : 0, (int)status);
		else
			send_cb(arg, status);

		mutex_enter(&q->nq_lock);
		c->nc_wrs--;
		nr_cmd_rele_locked(c);
		mutex_exit(&q->nq_lock);
	}
}

static void
nr_rw_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	nr_xfer_t *xf = (nr_xfer_t *)(void *)wc->wr_cqe;
	nr_queue_t *q = xf->xf_q;
	nr_xreq_t *xr;
	nr_cmd_t *c;
	list_t done;

	_NOTE(ARGUNUSED(cq));
	list_create(&done, sizeof (nr_xreq_t), offsetof(nr_xreq_t, xr_node));
	mutex_enter(&q->nq_lock);
	xr = xf->xf_req;
	/* After an error each flushed request of the chain completes. */
	if (!xf->xf_busy || xr == NULL) {
		mutex_exit(&q->nq_lock);
		list_destroy(&done);
		return;
	}
	c = xr->xr_cmd;
	nr_xfer_put_locked(q, xf);
	xr->xr_xfer = NULL;
	if (wc->status != RDK_WC_SUCCESS) {
		if (wc->status != RDK_WC_WR_FLUSH_ERR)
			nr_queue_fail_locked(q, EIO);
		nr_xreq_fail_locked(xr, &done);
	} else if (xr->xr_read) {
		nr_xreq_finish_locked(xr, 0, &done);
	} else if (!xr->xr_final) {
		nr_xreq_finish_locked(xr, xr->xr_off + xr->xr_len ==
		    c->nc_sgl.nsl_len ? NVME_CQE_SC_GEN_SUCCESS : NVMF_MORE,
		    &done);
	} else if (xr->xr_sent) {
		nr_xreq_finish_locked(xr, xr->xr_send_ok ? NVMF_SUCCESS_SENT :
		    NVME_CQE_SC_GEN_DATA_XFR_ERR, &done);
	} else {
		xr->xr_state = NR_X_SENDING;
	}
	nr_xreq_run_locked(q, &done);
	mutex_exit(&q->nq_lock);
	nr_xreq_callbacks(q, &done);
	list_destroy(&done);
}

/* The response SEND of a command completed; ok if it went out. */
void
nr_xreq_send_done(nr_cmd_t *c, boolean_t ok)
{
	nr_queue_t *q = c->nc_q;
	nr_xreq_t *xr;
	list_t done;

	list_create(&done, sizeof (nr_xreq_t), offsetof(nr_xreq_t, xr_node));
	mutex_enter(&q->nq_lock);
	if ((xr = c->nc_final) != NULL) {
		if (xr->xr_state == NR_X_SENDING) {
			nr_xreq_finish_locked(xr, ok ? NVMF_SUCCESS_SENT :
			    NVME_CQE_SC_GEN_DATA_XFR_ERR, &done);
		} else {
			xr->xr_sent = B_TRUE;
			xr->xr_send_ok = ok;
		}
	}
	mutex_exit(&q->nq_lock);
	nr_xreq_callbacks(q, &done);
	list_destroy(&done);
}

/*
 * Queue a transfer of len bytes at off in the command's SGL, between the
 * host and mem.  Returns 0 if the callback will run, once.
 */
static int
nr_xreq_submit(nr_cmd_t *c, boolean_t read, uint32_t off, size_t len,
    const nvmf_memdesc_t *mem, nvmf_io_complete_t *io_cb,
    nvmf_send_complete_t *send_cb, void *arg, const nvme_cqe_t *final)
{
	nr_queue_t *q = c->nc_q;
	const ddi_dma_cookie_t *ck = NULL;
	nr_buf_t *bounce = NULL;
	nr_xreq_t *xr = NULL;
	nr_xfer_t *xf;
	list_t done;
	uint_t i;

	if (len == 0 || len > q->nq_xfer_len || len > mem->nmd_len ||
	    !nvmf_rdma_range_ok(off, (uint32_t)len, c->nc_sgl.nsl_len))
		return (EFBIG);
	if (mem->nmd_type == NVMF_MEMDESC_SGL &&
	    mem->nmd_u.nmd_sgl.nmd_ncookies == 1 &&
	    mem->nmd_u.nmd_sgl.nmd_cookies != NULL &&
	    mem->nmd_u.nmd_sgl.nmd_cookies[0].dmac_size >= len) {
		ck = &mem->nmd_u.nmd_sgl.nmd_cookies[0];
	} else {
		if ((bounce = nr_buf_alloc(q->nq_dev, len, len)) == NULL)
			return (ENOMEM);
		if (!read)
			nvmf_memdesc_copyout(mem, 0, bounce->nb_va, len);
		ck = &bounce->nb_ck;
	}

	list_create(&done, sizeof (nr_xreq_t), offsetof(nr_xreq_t, xr_node));
	mutex_enter(&q->nq_lock);
	if (q->nq_state != NR_Q_LIVE || c->nc_state != NR_C_ACTIVE ||
	    (final != NULL && c->nc_final != NULL)) {
		mutex_exit(&q->nq_lock);
		list_destroy(&done);
		if (bounce != NULL)
			nr_buf_free(bounce);
		return (ENOTCONN);
	}
	for (i = 0; i < NR_CMD_XREQS; i++) {
		if (c->nc_xreq[i].xr_state == NR_X_FREE) {
			xr = &c->nc_xreq[i];
			break;
		}
	}
	if (xr == NULL) {
		mutex_exit(&q->nq_lock);
		list_destroy(&done);
		if (bounce != NULL)
			nr_buf_free(bounce);
		return (EBUSY);
	}
	bzero(xr, sizeof (*xr));
	xr->xr_cmd = c;
	xr->xr_read = read;
	xr->xr_off = off;
	xr->xr_len = (uint32_t)len;
	xr->xr_mem = *mem;
	xr->xr_bounce = bounce;
	xr->xr_ck = *ck;
	xr->xr_ck.dmac_size = len;
	xr->xr_io_cb = io_cb;
	xr->xr_send_cb = send_cb;
	xr->xr_cb_arg = arg;
	if (final != NULL) {
		xr->xr_final = B_TRUE;
		bcopy(final, &xr->xr_cqe, sizeof (xr->xr_cqe));
		c->nc_final = xr;
	}
	c->nc_wrs++;
	if (!list_is_empty(&q->nq_wait) ||
	    (xf = list_remove_head(&q->nq_free_xfers)) == NULL) {
		xr->xr_state = NR_X_WAIT;
		list_insert_tail(&q->nq_wait, xr);
		q->nq_rw_waits++;
	} else {
		xr->xr_state = NR_X_WAIT;
		nr_xreq_post_locked(q, xr, xf, &done);
	}
	mutex_exit(&q->nq_lock);
	nr_xreq_callbacks(q, &done);
	list_destroy(&done);
	return (0);
}

static nr_cmd_t *
nr_keyed_cmd(struct nvmf_capsule *nc)
{
	nr_cmd_t *c = (nr_cmd_t *)(void *)nc;

	if (c->nc_kind != NR_KIND_CMD || !c->nc_sgl_done || c->nc_sgl_sc != 0)
		return (NULL);
	return (c);
}

int
nr_receive_controller_data(struct nvmf_capsule *nc, uint32_t off,
    struct nvmf_io_request *io)
{
	nr_cmd_t *c = nr_keyed_cmd(nc);
	nr_queue_t *q;
	caddr_t src;

	if (c == NULL)
		return (EINVAL);
	q = c->nc_q;
	if (io->io_len == 0 || io->io_len > UINT32_MAX ||
	    io->io_len > io->io_mem.nmd_len ||
	    !nvmf_rdma_range_ok(off, (uint32_t)io->io_len, c->nc_sgl.nsl_len))
		return (EFBIG);
	if (c->nc_sgl.nsl_keyed) {
		return (nr_xreq_submit(c, B_TRUE, off, io->io_len, &io->io_mem,
		    io->io_complete, NULL, io->io_complete_arg, NULL));
	}

	/* The SGL was checked against the bytes that arrived. */
	mutex_enter(&q->nq_lock);
	if (c->nc_recv == NULL || q->nq_state != NR_Q_LIVE) {
		mutex_exit(&q->nq_lock);
		return (ENOTCONN);
	}
	src = c->nc_recv->rv_va + NVMF_RDMA_SQE_LEN + c->nc_sgl.nsl_addr + off;
	nvmf_memdesc_copyin(&io->io_mem, 0, src, io->io_len);
	mutex_exit(&q->nq_lock);
	nvmf_complete_io_request(io, io->io_len, 0);
	return (0);
}

int
nr_send_controller_data_io(struct nvmf_capsule *nc, uint32_t off,
    const struct nvmf_send_request *req, const nvme_cqe_t *final_cqe)
{
	nr_cmd_t *c = nr_keyed_cmd(nc);

	if (c == NULL || !c->nc_sgl.nsl_keyed)
		return (EINVAL);
	return (nr_xreq_submit(c, B_FALSE, off, req->nsr_len, &req->nsr_mem,
	    NULL, req->nsr_complete, req->nsr_complete_arg, final_cqe));
}

/*
 * Complete what the device will not: after the drain, and again once the
 * QP is gone.
 */
void
nr_xfer_fail_all(nr_queue_t *q)
{
	nr_xreq_t *xr;
	nr_xfer_t *xf;
	nr_cmd_t *c;
	list_t done;
	uint_t i, j;

	list_create(&done, sizeof (nr_xreq_t), offsetof(nr_xreq_t, xr_node));
	mutex_enter(&q->nq_lock);
	ASSERT(q->nq_state == NR_Q_DYING || q->nq_state == NR_Q_DEAD);
	while ((xr = list_remove_head(&q->nq_wait)) != NULL)
		nr_xreq_fail_locked(xr, &done);
	for (i = 0; q->nq_xfers != NULL && i < q->nq_sz.nrs_xfers; i++) {
		xf = &q->nq_xfers[i];
		if (!xf->xf_busy)
			continue;
		xr = xf->xf_req;
		nr_xfer_put_locked(q, xf);
		xr->xr_xfer = NULL;
		nr_xreq_fail_locked(xr, &done);
	}
	for (i = 0; q->nq_cmds != NULL && i < q->nq_sz.nrs_cmds; i++) {
		c = &q->nq_cmds[i];
		for (j = 0; j < NR_CMD_XREQS; j++) {
			if (c->nc_xreq[j].xr_state == NR_X_SENDING)
				nr_xreq_fail_locked(&c->nc_xreq[j], &done);
		}
		if (q->nq_state == NR_Q_DEAD && c->nc_send_posted) {
			c->nc_send_posted = B_FALSE;
			c->nc_wrs--;
			nr_cmd_rele_locked(c);
		}
	}
	mutex_exit(&q->nq_lock);
	nr_xreq_callbacks(q, &done);
	list_destroy(&done);
}

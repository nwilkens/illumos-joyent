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
 * The data path runs of rdmat: windowed streams of sends, writes and reads
 * posted in chains, a receive stream that keeps its receives posted, send
 * and write ping-pong, and the latency of one request at a time.
 *
 * A stream keeps at most rr_depth requests on the send queue.  It posts
 * them in chains of up to rr_batch, so one post call rings the doorbell
 * once, and signals every rr_signal'th request and the last.  A request
 * before a signaled one that completed is complete, so the window moves
 * by rr_signal at a time.
 *
 * Write ping-pong polls the last byte of the region the peer writes, as
 * ib_write_lat does; each side writes the round's sequence number there.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "rdmat_impl.h"

/* How long a timed receive stream waits for a straggler. */
#define	RDMAT_IDLE_MS	500

/*
 * The local SGE for [off, off + len) of the buffer.  With the DMA lkey the
 * range must lie in one chunk.
 */
static int
rdmat_sge(rdmat_sess_t *ts, rdmat_qp_t *tq, uint64_t off, uint32_t len,
    boolean_t dma_lkey, struct rdk_sge *sge)
{
	uint64_t c;

	if (off >= tq->tq_len || len > tq->tq_len - off)
		return (EINVAL);
	if (dma_lkey || tq->tq_lmr == NULL) {
		c = off / RDMAT_CHUNK;
		if ((off % RDMAT_CHUNK) + len > RDMAT_CHUNK)
			return (EINVAL);
		sge->addr = tq->tq_chunks[c].rdb_pa + (off % RDMAT_CHUNK);
		sge->lkey = ts->ts_pd->local_dma_lkey;
	} else {
		if (!tq->tq_lmr_bound)
			return (ENXIO);
		sge->addr = tq->tq_lmr->iova + off;
		sge->lkey = tq->tq_lmr->lkey;
	}
	sge->length = len;
	return (0);
}

static volatile uint8_t *
rdmat_byte(rdmat_qp_t *tq, uint64_t off)
{
	ASSERT3U(off, <, tq->tq_len);
	return ((volatile uint8_t *)tq->tq_chunks[off / RDMAT_CHUNK].rdb_va +
	    (off % RDMAT_CHUNK));
}

/* Post n receives in chains; *posted counts the ones the QP took. */
int
rdmat_post_recvs(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    uint32_t n, uint64_t *posted)
{
	const struct rdk_recv_wr *bad;
	uint32_t size = rr->rr_size, k, i;
	uint64_t off;
	int ret;

	if (ts->ts_qpt == RDMAT_QPT_UD)
		size += RDMAT_GRH_LEN;
	while (n > 0) {
		k = MIN(n, RDMAT_MAX_BATCH);
		for (i = 0; i < k; i++) {
			rdmat_rwr_t *rw = &tq->tq_rwr[i];

			off = rr->rr_offset + ((*posted + i) % ts->ts_depth) *
			    size;
			if ((ret = rdmat_sge(ts, tq, off, size,
			    (rr->rr_flags & RDMAT_F_DMA_LKEY) != 0,
			    &rw->rw_sge)) != 0)
				return (ret);
			bzero(&rw->rw_wr, sizeof (rw->rw_wr));
			rw->rw_wr.wr_cqe = &tq->tq_recv_cqe;
			rw->rw_wr.sg_list = &rw->rw_sge;
			rw->rw_wr.num_sge = 1;
			rw->rw_wr.next = i + 1 < k ? &tq->tq_rwr[i + 1].rw_wr :
			    NULL;
		}
		if ((ret = rdk_post_recv(tq->tq_qp, &tq->tq_rwr[0].rw_wr,
		    &bad)) != 0) {
			for (i = 0; i < k && &tq->tq_rwr[i].rw_wr != bad; i++)
				(*posted)++;
			return (ret);
		}
		*posted += k;
		n -= k;
	}
	return (0);
}

/* Build send-queue work request i of a run in sw. */
static int
rdmat_swr(rdmat_sess_t *ts, rdmat_qp_t *tq, const rdmat_run_t *rr,
    uint64_t i, boolean_t signal, rdmat_swr_t *sw)
{
	struct rdk_send_wr *wr = &sw->sw_wr;
	uint64_t off, roff = 0;
	int ret;

	off = rr->rr_offset;
	if (rr->rr_op == RDMAT_OP_WRITE || rr->rr_op == RDMAT_OP_READ) {
		/*
		 * Successive transfers walk the remote window, and the same
		 * offsets of the local buffer while they fit.
		 */
		uint64_t span = MAX(rr->rr_rlen / rr->rr_size, 1);

		roff = (i % span) * rr->rr_size;
		off = rr->rr_offset + roff %
		    MAX(tq->tq_len - rr->rr_offset - rr->rr_size + 1, 1);
	}
	if ((ret = rdmat_sge(ts, tq, off, rr->rr_size,
	    (rr->rr_flags & RDMAT_F_DMA_LKEY) != 0, &sw->sw_sge)) != 0)
		return (ret);

	bzero(sw, offsetof(rdmat_swr_t, sw_sge));
	wr->wr_cqe = &tq->tq_send_cqe;
	wr->sg_list = &sw->sw_sge;
	wr->num_sge = rr->rr_size != 0 ? 1 : 0;
	wr->send_flags = signal ? RDK_SEND_SIGNALED : 0;
	switch (rr->rr_op) {
	case RDMAT_OP_SEND:
	case RDMAT_OP_PING:
	case RDMAT_OP_PONG:
		wr->opcode = RDK_WR_SEND;
		break;
	case RDMAT_OP_SEND_INV:
		wr->opcode = RDK_WR_SEND_WITH_INV;
		wr->ex.invalidate_rkey = rr->rr_rkey;
		break;
	case RDMAT_OP_WRITE:
		wr->opcode = RDK_WR_RDMA_WRITE;
		sw->sw_rdma.remote_addr = rr->rr_raddr + roff;
		sw->sw_rdma.rkey = rr->rr_rkey;
		break;
	case RDMAT_OP_READ:
		wr->opcode = RDK_WR_RDMA_READ;
		sw->sw_rdma.remote_addr = rr->rr_raddr + roff;
		sw->sw_rdma.rkey = rr->rr_rkey;
		break;
	default:
		return (EINVAL);
	}
	if ((rr->rr_flags & RDMAT_F_INLINE) != 0 &&
	    wr->opcode != RDK_WR_RDMA_READ)
		wr->send_flags |= RDK_SEND_INLINE;
	if (ts->ts_qpt == RDMAT_QPT_UD) {
		sw->sw_ud.ah = tq->tq_ah;
		sw->sw_ud.remote_qpn = tq->tq_rqpn;
		sw->sw_ud.remote_qkey = tq->tq_rqkey;
	}
	return (0);
}

static int
rdmat_post_one(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    uint64_t i, boolean_t signal)
{
	int ret;

	if ((ret = rdmat_swr(ts, tq, rr, i, signal, &tq->tq_swr[0])) != 0)
		return (ret);
	tq->tq_post_calls++;
	if ((ret = rdk_post_send(tq->tq_qp, &tq->tq_swr[0].sw_wr, NULL)) == 0)
		tq->tq_posted++;
	return (ret);
}

static uint64_t
rdmat_count(rdmat_qp_t *tq, uint64_t *ctr)
{
	uint64_t v;

	mutex_enter(&tq->tq_lock);
	v = *ctr;
	mutex_exit(&tq->tq_lock);
	return (v);
}

/*
 * A windowed stream of rr_count send-queue requests, or fewer if rr_run_ms
 * passes first; the stream then ends with one signaled request.
 */
int
rdmat_stream(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	const struct rdk_send_wr *bad;
	uint64_t count = rr->rr_count, posted = 0, sig = 0, sig_done;
	uint64_t inflight, room, want, n, k;
	uint32_t depth = rr->rr_depth, every;
	hrtime_t stop = 0;
	boolean_t signal;
	int ret;

	every = rr->rr_signal;
	if (every == 0)
		every = (rr->rr_flags & RDMAT_F_UNSIGNALED) != 0 ? depth : 1;
	every = MIN(every, depth);
	if (rr->rr_run_ms != 0)
		stop = gethrtime() + MSEC2NSEC(rr->rr_run_ms);

	while (posted < count) {
		sig_done = rdmat_count(tq, &tq->tq_send_done);
		inflight = posted - MIN(sig_done * every, posted);
		room = depth - MIN(inflight, depth);
		want = MIN(rr->rr_batch, count - posted);
		if (room == 0 || (room < want && sig > sig_done)) {
			if ((ret = rdmat_wait(tq, &tq->tq_send_done,
			    sig_done + 1, deadline)) != 0)
				goto out;
			continue;
		}
		if (stop != 0 && gethrtime() >= stop)
			count = posted + 1;
		n = MIN(MIN(want, room), count - posted);
		for (k = 0; k < n; k++) {
			uint64_t j = posted + k;

			signal = ((j + 1) % every) == 0 || j + 1 == count;
			if ((ret = rdmat_swr(ts, tq, rr, j, signal,
			    &tq->tq_swr[k])) != 0)
				goto out;
			tq->tq_swr[k].sw_wr.next = k + 1 < n ?
			    &tq->tq_swr[k + 1].sw_wr : NULL;
		}
		tq->tq_post_calls++;
		ret = rdk_post_send(tq->tq_qp, &tq->tq_swr[0].sw_wr, &bad);
		for (k = 0; k < n; k++) {
			if (ret != 0 && &tq->tq_swr[k].sw_wr == bad)
				break;
			if ((tq->tq_swr[k].sw_wr.send_flags &
			    RDK_SEND_SIGNALED) != 0)
				sig++;
			posted++;
		}
		if (ret != 0)
			goto out;
	}
	ret = rdmat_wait(tq, &tq->tq_send_done, sig, deadline);
out:
	tq->tq_posted = posted;
	return (ret);
}

/*
 * Receive rr_count messages, keeping up to rr_depth receives posted and
 * posting again in chains of at least rr_batch.  With rr_run_ms the stream
 * also ends once that has passed and no message came for RDMAT_IDLE_MS.
 */
int
rdmat_recv_stream(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	uint64_t count = rr->rr_count, posted = 0, seen, room, want;
	hrtime_t stop = 0, wd;
	int ret;

	if (rr->rr_run_ms != 0)
		stop = gethrtime() + MSEC2NSEC(rr->rr_run_ms);

	if ((ret = rdmat_post_recvs(ts, tq, rr,
	    (uint32_t)MIN(count, rr->rr_depth), &posted)) != 0)
		goto out;
	for (;;) {
		seen = rdmat_count(tq, &tq->tq_recv_done);
		if (seen >= count)
			break;
		room = rr->rr_depth - MIN(posted - seen, rr->rr_depth);
		want = MIN(room, count - posted);
		if (want > 0 && want >= MIN(rr->rr_batch, count - posted)) {
			tq->tq_post_calls++;
			if ((ret = rdmat_post_recvs(ts, tq, rr, (uint32_t)want,
			    &posted)) != 0)
				goto out;
			continue;
		}
		wd = deadline;
		if (stop != 0 && gethrtime() >= stop)
			wd = MIN(deadline, gethrtime() +
			    MSEC2NSEC(RDMAT_IDLE_MS));
		ret = rdmat_wait(tq, &tq->tq_recv_done, seen + 1, wd);
		if (ret == ETIMEDOUT && wd < deadline) {
			ret = 0;
			break;
		}
		if (ret != 0)
			goto out;
	}
out:
	tq->tq_posted = posted;
	return (ret);
}

/* Heap sort; the kernel has no public qsort(). */
static void
rdmat_sift(uint64_t *v, uint64_t i, uint64_t n)
{
	uint64_t c, x = v[i];

	while ((c = 2 * i + 1) < n) {
		if (c + 1 < n && v[c + 1] > v[c])
			c++;
		if (v[c] <= x)
			break;
		v[i] = v[c];
		i = c;
	}
	v[i] = x;
}

static void
rdmat_sort(uint64_t *v, uint64_t n)
{
	uint64_t i, x;

	for (i = n / 2; i-- > 0; )
		rdmat_sift(v, i, n);
	for (i = n; i-- > 1; ) {
		x = v[0];
		v[0] = v[i];
		v[i] = x;
		rdmat_sift(v, 0, i);
	}
}

static void
rdmat_lat_stats(rdmat_run_t *rr, uint64_t *lat, uint64_t n, uint64_t sum)
{
	if (n == 0)
		return;
	rdmat_sort(lat, n);
	rr->rr_lat_min = lat[0];
	rr->rr_lat_max = lat[n - 1];
	rr->rr_lat_avg = sum / n;
	rr->rr_lat_p50 = lat[n / 2];
	rr->rr_lat_p99 = lat[MIN(n - 1, (n * 99) / 100)];
	rr->rr_lat_p999 = lat[MIN(n - 1, (n * 999) / 1000)];
}

static uint64_t *
rdmat_lat_alloc(const rdmat_run_t *rr, uint64_t *np)
{
	*np = MIN(rr->rr_count, RDMAT_LAT_SAMPLES);
	return (kmem_zalloc(sizeof (uint64_t) * MAX(*np, 1), KM_SLEEP));
}

static void
rdmat_lat_free(uint64_t *lat, uint64_t n)
{
	kmem_free(lat, sizeof (uint64_t) * MAX(n, 1));
}

int
rdmat_pingpong(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	uint64_t *lat, n, i, rposted = 0, sum = 0;
	boolean_t ping = rr->rr_op == RDMAT_OP_PING;
	hrtime_t t0;
	int ret;

	lat = rdmat_lat_alloc(rr, &n);
	if ((ret = rdmat_post_recvs(ts, tq, rr,
	    (uint32_t)MIN(rr->rr_count, rr->rr_depth), &rposted)) != 0)
		goto out;

	for (i = 0; i < rr->rr_count; i++) {
		t0 = gethrtime();
		if (!ping && (ret = rdmat_wait(tq, &tq->tq_recv_done, i + 1,
		    deadline)) != 0)
			break;
		if ((ret = rdmat_post_one(ts, tq, rr, i, B_TRUE)) != 0)
			break;
		if (ping && (ret = rdmat_wait(tq, &tq->tq_recv_done, i + 1,
		    deadline)) != 0)
			break;
		if ((ret = rdmat_wait(tq, &tq->tq_send_done, i + 1,
		    deadline)) != 0)
			break;
		if (ping && i < n) {
			lat[i] = (uint64_t)(gethrtime() - t0);
			sum += lat[i];
		}
		if (rposted < rr->rr_count && (ret = rdmat_post_recvs(ts, tq,
		    rr, 1, &rposted)) != 0)
			break;
	}
	if (ping && ret == 0)
		rdmat_lat_stats(rr, lat, n, sum);
out:
	rdmat_lat_free(lat, n);
	return (ret);
}

/*
 * Spin until the byte holds want.  The peer's write lands in order, so its
 * last byte arrives last.
 */
static int
rdmat_wait_byte(rdmat_qp_t *tq, volatile uint8_t *p, uint8_t want,
    hrtime_t deadline)
{
	uint_t spins = 0;

	while (*p != want) {
		if ((++spins & 0xfff) != 0)
			continue;
		if (tq->tq_sess->ts_dying)
			return (ENXIO);
		if (tq->tq_errors != 0)
			return (EIO);
		if (gethrtime() >= deadline)
			return (ETIMEDOUT);
		if (ISSIG(curthread, JUSTLOOKING))
			return (EINTR);
		if (tq->tq_sess->ts_poll == RDMAT_POLL_DIRECT)
			(void) rdk_process_cq_direct(tq->tq_scq, -1);
	}
	return (0);
}

/*
 * The send region is at rr_offset and the region the peer writes follows
 * it at a 64 byte boundary; rr_raddr is the peer's buffer at rr_offset.
 */
int
rdmat_write_pingpong(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	boolean_t ping = rr->rr_op == RDMAT_OP_WRITE_PING;
	uint64_t slot = P2ROUNDUP((uint64_t)rr->rr_size, 64);
	uint64_t *lat, n, i, sig = 0, sum = 0;
	volatile uint8_t *tx, *rx;
	uint32_t every;
	rdmat_run_t w;
	boolean_t signal;
	hrtime_t t0;
	uint8_t seq;
	int ret = 0;

	every = rr->rr_signal != 0 ? rr->rr_signal : 16;
	every = MAX(MIN(every, rr->rr_depth / 2), 1);
	tx = rdmat_byte(tq, rr->rr_offset + rr->rr_size - 1);
	rx = rdmat_byte(tq, rr->rr_offset + slot + rr->rr_size - 1);
	w = *rr;
	w.rr_op = RDMAT_OP_WRITE;
	w.rr_raddr = rr->rr_raddr + slot;
	w.rr_rlen = rr->rr_size;
	lat = rdmat_lat_alloc(rr, &n);

	for (i = 0; i < rr->rr_count; i++) {
		seq = (uint8_t)(i % 255 + 1);
		if (!ping && (ret = rdmat_wait_byte(tq, rx, seq,
		    deadline)) != 0)
			break;
		t0 = gethrtime();
		*tx = seq;
		signal = ((i + 1) % every) == 0 || i + 1 == rr->rr_count;
		/* At most two signal intervals are on the send queue. */
		if (signal && sig > 0 && (ret = rdmat_wait(tq,
		    &tq->tq_send_done, sig, deadline)) != 0)
			break;
		if ((ret = rdmat_post_one(ts, tq, &w, 0, signal)) != 0)
			break;
		if (signal)
			sig++;
		if (ping) {
			if ((ret = rdmat_wait_byte(tq, rx, seq,
			    deadline)) != 0)
				break;
			if (i < n) {
				lat[i] = (uint64_t)(gethrtime() - t0);
				sum += lat[i];
			}
		}
	}
	if (ret == 0)
		ret = rdmat_wait(tq, &tq->tq_send_done, sig, deadline);
	if (ping && ret == 0)
		rdmat_lat_stats(rr, lat, n, sum);
	rdmat_lat_free(lat, n);
	return (ret);
}

/* One signaled READ or WRITE at a time, each timed to its completion. */
int
rdmat_one_lat(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	uint64_t *lat, n, i, sum = 0;
	hrtime_t t0;
	int ret = 0;

	lat = rdmat_lat_alloc(rr, &n);
	for (i = 0; i < rr->rr_count; i++) {
		t0 = gethrtime();
		if ((ret = rdmat_post_one(ts, tq, rr, i, B_TRUE)) != 0 ||
		    (ret = rdmat_wait(tq, &tq->tq_send_done, i + 1,
		    deadline)) != 0)
			break;
		if (i < n) {
			lat[i] = (uint64_t)(gethrtime() - t0);
			sum += lat[i];
		}
	}
	if (ret == 0)
		rdmat_lat_stats(rr, lat, n, sum);
	rdmat_lat_free(lat, n);
	return (ret);
}

/*
 * The cost of memory registration, per cycle.  RDMAT_OP_MR_ALLOC allocates
 * and frees an MR for rr_size bytes, which takes two control commands.
 * RDMAT_OP_FRWR binds one MR over the first rr_size bytes of the buffer
 * with REG_MR and unbinds it with LOCAL_INV, on the send queue.
 */
int
rdmat_mr_cost(rdmat_sess_t *ts, rdmat_qp_t *tq, rdmat_run_t *rr,
    hrtime_t deadline)
{
	uint32_t pages = rr->rr_size / PAGESIZE;
	uint_t nc = (rr->rr_size + RDMAT_CHUNK - 1) / RDMAT_CHUNK, k;
	boolean_t frwr = rr->rr_op == RDMAT_OP_FRWR;
	ddi_dma_cookie_t *ck = NULL;
	struct rdk_reg_wr reg;
	struct rdk_send_wr inv;
	struct rdk_mr *mr = NULL, *m;
	uint64_t *lat, n, i, sum = 0, off;
	hrtime_t t0;
	int ret = 0;

	lat = rdmat_lat_alloc(rr, &n);
	if (frwr) {
		if ((ret = rdk_alloc_mr(ts->ts_pd, RDK_MR_TYPE_MEM_REG, pages,
		    &mr)) != 0)
			goto out;
		ck = kmem_alloc(sizeof (*ck) * nc, KM_SLEEP);
		for (k = 0; k < nc; k++) {
			ck[k] = tq->tq_cookies[k];
			ck[k].dmac_size = MIN(RDMAT_CHUNK,
			    rr->rr_size - k * RDMAT_CHUNK);
		}
	}
	for (i = 0; i < rr->rr_count; i++) {
		t0 = gethrtime();
		if (!frwr) {
			if ((ret = rdk_alloc_mr(ts->ts_pd, RDK_MR_TYPE_MEM_REG,
			    pages, &m)) != 0 || (ret = rdk_dereg_mr(m)) != 0)
				break;
		} else {
			rdk_update_fast_reg_key(mr, (uint8_t)(mr->rkey + 1));
			off = 0;
			if (rdk_map_mr_sg(mr, ck, nc, &off, PAGESIZE) !=
			    (int)nc || mr->length != rr->rr_size) {
				ret = EIO;
				break;
			}
			bzero(&reg, sizeof (reg));
			reg.wr.wr_cqe = &tq->tq_reg_cqe;
			reg.wr.opcode = RDK_WR_REG_MR;
			reg.wr.send_flags = RDK_SEND_SIGNALED;
			reg.mr = mr;
			reg.key = mr->rkey;
			reg.access = RDK_ACCESS_LOCAL_WRITE |
			    RDK_ACCESS_REMOTE_WRITE | RDK_ACCESS_REMOTE_READ;
			bzero(&inv, sizeof (inv));
			inv.wr_cqe = &tq->tq_reg_cqe;
			inv.opcode = RDK_WR_LOCAL_INV;
			inv.send_flags = RDK_SEND_SIGNALED;
			inv.ex.invalidate_rkey = mr->rkey;
			if ((ret = rdk_post_send(tq->tq_qp, &reg.wr,
			    NULL)) != 0 || (ret = rdmat_wait(tq,
			    &tq->tq_reg_done, 2 * i + 1, deadline)) != 0 ||
			    (ret = rdk_post_send(tq->tq_qp, &inv, NULL)) != 0 ||
			    (ret = rdmat_wait(tq, &tq->tq_reg_done, 2 * i + 2,
			    deadline)) != 0)
				break;
		}
		if (i < n) {
			lat[i] = (uint64_t)(gethrtime() - t0);
			sum += lat[i];
		}
	}
	if (ret == 0)
		rdmat_lat_stats(rr, lat, n, sum);
out:
	if (mr != NULL)
		(void) rdk_dereg_mr(mr);
	if (ck != NULL)
		kmem_free(ck, sizeof (*ck) * nc);
	rdmat_lat_free(lat, n);
	return (ret);
}

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
 * rdmatool bench: perftest-style benchmarks of the kernel verbs through
 * rdmat(4D).
 *
 *	rdmatool -i ip [-d dev] [-p port] bench {loop | host} test
 *	    [key=value...]
 *
 * test is write_bw, read_bw, send_bw, write_lat, read_lat, send_lat, or
 * mr_alloc and frwr, which time registering size bytes: an MR allocated
 * and freed, or bound with REG_MR and unbound with LOCAL_INV.  In
 * loop both sides are sessions on this host; otherwise the other side is
 * "rdmatool -i ip server" on host, one connection and server process per
 * QP.  Each key takes a comma-separated list and every combination runs:
 *
 *	size=4096	message bytes
 *	qps=1		QPs, each with its own sessions and threads
 *	depth=64	requests in flight per QP (1 to 256)
 *	batch=1		requests per post call (1 to 32)
 *	signal=0	signal every n'th request; 0 is every depth/2
 *	inline=0	1: sends and writes carry their data in the WQE
 *	mode=intr	intr: the posting thread sleeps until a completion
 *			interrupt; poll: it polls the CQ and never sleeps;
 *			adapt: it polls up to spin microseconds, then sleeps
 *	spin=50		adapt: microseconds of polling per wait
 *	modc=0		CQ moderation count (intr and adapt)
 *	modus=0		CQ moderation time in microseconds
 *	secs=5		length of a bandwidth run
 *	iters=20000	round trips of a latency run
 *	verify=1	check every byte of the destination after a run
 *	vec=spread	completion vector of QP i: i modulo the vectors both
 *			hosts have, or one number for every QP
 *
 * Each run prints one "BENCH key=value ..." line.  Bandwidth is the bytes
 * of every completed request over the time of the slowest QP, from its
 * first post to its last completion.  A send_bw receiver waits up to half
 * a second after the senders stop.
 * Build with -pthread, so errno is per thread.
 *
 * CPU is in CPU seconds per GiB moved (and microseconds per operation), two
 * ways for each host.  host: the user, kernel and interrupt time of every
 * CPU from the cpu:*:sys kstats over the run, less the rate measured with
 * the network idle before the sweep.  attr: the time of the rdmatool
 * processes (getrusage, which holds the posting and polling done in their
 * ioctls), the task time of the rdmak and irdma taskqs, the irdma
 * completion threads (the ceq_busy_ns kstat) and interrupt time above the
 * idle rate.  In loop the host is both endpoints.  Interrupts, doorbells
 * and CQ arms per operation come from the irdma kstats.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stropts.h>

#include "rdmatool.h"

#define	B_MAXQP		32
#define	B_MAXLIST	16
#define	MB		(1ULL << 20)

enum {
	T_WRITE_BW, T_READ_BW, T_SEND_BW, T_WRITE_LAT, T_READ_LAT, T_SEND_LAT,
	T_MR_ALLOC, T_FRWR
};
enum { M_INTR, M_POLL, M_ADAPT };
static const char *const mnames[] = { "intr", "poll", "adapt" };

static const char *const tnames[] = {
	"write_bw", "read_bw", "send_bw", "write_lat", "read_lat", "send_lat",
	"mr_alloc", "frwr", NULL
};

typedef struct blist {
	uint32_t	bl_v[B_MAXLIST];
	uint_t		bl_n;
} blist_t;

typedef struct bconf {
	int		c_test;
	uint32_t	c_size;
	uint32_t	c_qps;
	uint32_t	c_depth;
	uint32_t	c_batch;
	uint32_t	c_signal;
	uint32_t	c_inline;
	uint32_t	c_poll;		/* M_* */
	uint32_t	c_spin;
	uint32_t	c_modc;
	uint32_t	c_modus;
	uint32_t	c_secs;
	uint32_t	c_iters;
	uint32_t	c_verify;
} bconf_t;

/* One side's run on a thread. */
typedef struct bside {
	peer_t		*s_peer;
	rdmat_run_t	s_rr;
	int		s_ret;
	int		s_used;
	pthread_t	s_thr;
} bside_t;

typedef struct bpair {
	peer_t		bp_a;
	peer_t		bp_b;
	bside_t		bp_sa;
	bside_t		bp_sb;
	uint64_t	bp_seed;
} bpair_t;

static bpair_t pairs[B_MAXQP];
static int remote;
static uint32_t ncomp = 1;
static int vec_fixed = -1;
static double idle_busy_a, idle_intr_a, idle_busy_b, idle_intr_b;
static pthread_mutex_t go_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t go_cv = PTHREAD_COND_INITIALIZER;
static int go;

static void
blist_parse(blist_t *l, const char *v, const char *key)
{
	char *copy = strdup(v), *tok, *last;

	l->bl_n = 0;
	for (tok = strtok_r(copy, ",", &last); tok != NULL;
	    tok = strtok_r(NULL, ",", &last)) {
		if (l->bl_n == B_MAXLIST)
			fatal("%s: at most %d values", key, B_MAXLIST);
		if (strcmp(key, "mode") == 0) {
			if (strcmp(tok, "poll") == 0)
				l->bl_v[l->bl_n++] = M_POLL;
			else if (strcmp(tok, "intr") == 0)
				l->bl_v[l->bl_n++] = M_INTR;
			else if (strcmp(tok, "adapt") == 0)
				l->bl_v[l->bl_n++] = M_ADAPT;
			else
				fatal("mode is poll, intr or adapt");
		} else {
			l->bl_v[l->bl_n++] = (uint32_t)strtoul(tok, NULL, 0);
		}
	}
	free(copy);
	if (l->bl_n == 0)
		fatal("%s: no value", key);
}

static void *
side_thr(void *arg)
{
	bside_t *s = arg;

	(void) pthread_mutex_lock(&go_lock);
	while (!go)
		(void) pthread_cond_wait(&go_cv, &go_lock);
	(void) pthread_mutex_unlock(&go_lock);
	s->s_ret = ioctl(s->s_peer->p_fd, RDMAT_IOC_RUN, &s->s_rr) == 0 ? 0 :
	    errno;
	return (NULL);
}

/* The rates of busy and interrupt time with the network idle. */
static void
idle_rates(void)
{
	host_stats_t a0, a1, b0, b1;

	host_stats(&a0);
	if (remote)
		(void) rpc(&pairs[0].bp_b, C_STATS, &b0, 0, K_RESULT);
	(void) sleep(3);
	host_stats(&a1);
	if (remote)
		(void) rpc(&pairs[0].bp_b, C_STATS, &b1, 0, K_RESULT);
	idle_busy_a = (double)(a1.hs_busy_ns - a0.hs_busy_ns) /
	    (double)(a1.hs_now_ns - a0.hs_now_ns);
	idle_intr_a = (double)(a1.hs_intr_ns - a0.hs_intr_ns) /
	    (double)(a1.hs_now_ns - a0.hs_now_ns);
	if (remote && b1.hs_now_ns <= b0.hs_now_ns)
		fatal("the peer's CPU counters are not available");
	if (remote) {
		idle_busy_b = (double)(b1.hs_busy_ns - b0.hs_busy_ns) /
		    (double)(b1.hs_now_ns - b0.hs_now_ns);
		idle_intr_b = (double)(b1.hs_intr_ns - b0.hs_intr_ns) /
		    (double)(b1.hs_now_ns - b0.hs_now_ns);
	}
	(void) printf("# idle CPUs busy: %.2f here (%.3f interrupt), %.2f on "
	    "the peer (%.3f interrupt), subtracted\n", idle_busy_a,
	    idle_intr_a, idle_busy_b, idle_intr_b);
}

/* Host stats of the peer: host-wide from pair 0, processes summed. */
static void
peer_stats(uint32_t qps, host_stats_t *hs)
{
	host_stats_t one;
	uint32_t i;

	bzero(hs, sizeof (*hs));
	for (i = 0; i < qps; i++) {
		if (rpc(&pairs[i].bp_b, C_STATS, &one, 0, K_RESULT) != 0)
			fatal("peer stats failed");
		if (i == 0) {
			*hs = one;
		} else {
			hs->hs_proc_ns += one.hs_proc_ns;
		}
	}
}

static uint64_t
bufneed(const bconf_t *c)
{
	uint64_t need;

	switch (c->c_test) {
	case T_WRITE_BW:
	case T_READ_BW:
		need = (uint64_t)c->c_depth * c->c_size;
		need = need < MB ? MB : need;
		break;
	case T_SEND_BW:
	case T_SEND_LAT:
		need = (uint64_t)c->c_depth * c->c_size;
		break;
	default:
		need = 2 * (((uint64_t)c->c_size + 63) & ~63ULL);
		break;
	}
	return ((need + MB - 1) & ~(MB - 1));
}

/* The remote window of a bandwidth run: whole messages in the buffer. */
static uint64_t
window(const bconf_t *c)
{
	uint64_t len = o_buf_mb * MB;

	return (len - len % c->c_size);
}

static void
side_init(bside_t *s, peer_t *p, uint32_t op, const bconf_t *c,
    uint32_t count)
{
	bzero(s, sizeof (*s));
	s->s_peer = p;
	run_init(&s->s_rr, op, c->c_size, count);
	s->s_rr.rr_depth = c->c_depth;
	s->s_rr.rr_batch = c->c_batch;
	s->s_rr.rr_signal = c->c_signal;
	s->s_rr.rr_flags = (c->c_poll == M_POLL ? RDMAT_F_BUSY : 0) |
	    (c->c_poll == M_ADAPT ? RDMAT_F_ADAPT : 0) |
	    (c->c_inline ? RDMAT_F_INLINE : 0);
	s->s_rr.rr_spin_us = c->c_poll == M_ADAPT ? c->c_spin : 0;
	s->s_rr.rr_timeout_ms = RDMAT_MAX_TIMEOUT_MS;
	s->s_used = 1;
}

static void
set_remote(rdmat_run_t *rr, const peer_t *target, uint64_t rlen)
{
	rr->rr_raddr = target->p_setup.rs_qp[0].rqi_addr;
	rr->rr_rkey = target->p_setup.rs_qp[0].rqi_rkey;
	rr->rr_rlen = rlen;
}

/* Fill in the runs and the data each side starts with. */
static int
prepare(const bconf_t *c, bpair_t *bp)
{
	uint64_t rlen = window(c);
	bside_t *sa = &bp->bp_sa, *sb = &bp->bp_sb;
	peer_t *a = &bp->bp_a, *b = &bp->bp_b;

	bp->bp_seed = ((uint64_t)getpid() << 32) ^ now_ns();
	switch (c->c_test) {
	case T_WRITE_BW:
	case T_READ_BW:
		side_init(sa, a, c->c_test == T_WRITE_BW ? RDMAT_OP_WRITE :
		    RDMAT_OP_READ, c, RDMAT_MAX_COUNT);
		sa->s_rr.rr_run_ms = c->c_secs * 1000;
		set_remote(&sa->s_rr, b, rlen);
		if (c->c_verify) {
			peer_t *src = c->c_test == T_WRITE_BW ? a : b;
			peer_t *dst = c->c_test == T_WRITE_BW ? b : a;

			if (buf(src, RDMAT_BUF_FILL, 0, rlen, bp->bp_seed, 0,
			    NULL) != 0 ||
			    buf(dst, RDMAT_BUF_ZERO, 0, rlen, 0, 0, NULL) != 0)
				return (-1);
		}
		break;
	case T_SEND_BW:
		/* The receiver ends once the sends stop coming. */
		side_init(sa, a, RDMAT_OP_SEND, c, RDMAT_MAX_COUNT);
		side_init(sb, b, RDMAT_OP_RECV_STREAM, c, RDMAT_MAX_COUNT);
		sa->s_rr.rr_run_ms = sb->s_rr.rr_run_ms = c->c_secs * 1000;
		sb->s_rr.rr_flags &= ~RDMAT_F_INLINE;
		if (c->c_verify && (buf(a, RDMAT_BUF_FILL, 0, c->c_size,
		    bp->bp_seed, 0, NULL) != 0 || buf(b, RDMAT_BUF_ZERO, 0,
		    (uint64_t)c->c_depth * c->c_size, 0, 0, NULL) != 0))
			return (-1);
		break;
	case T_WRITE_LAT:
		side_init(sa, a, RDMAT_OP_WRITE_PING, c, c->c_iters);
		side_init(sb, b, RDMAT_OP_WRITE_PONG, c, c->c_iters);
		set_remote(&sa->s_rr, b, 0);
		set_remote(&sb->s_rr, a, 0);
		if (buf(a, RDMAT_BUF_ZERO, 0, bufneed(c), 0, 0, NULL) != 0 ||
		    buf(b, RDMAT_BUF_ZERO, 0, bufneed(c), 0, 0, NULL) != 0)
			return (-1);
		break;
	case T_READ_LAT:
		side_init(sa, a, RDMAT_OP_READ, c, c->c_iters);
		sa->s_rr.rr_flags |= RDMAT_F_LAT;
		sa->s_rr.rr_depth = 1;
		set_remote(&sa->s_rr, b, c->c_size);
		break;
	case T_SEND_LAT:
		side_init(sa, a, RDMAT_OP_PING, c, c->c_iters);
		side_init(sb, b, RDMAT_OP_PONG, c, c->c_iters);
		break;
	case T_MR_ALLOC:
	case T_FRWR:
		side_init(sa, a, c->c_test == T_MR_ALLOC ? RDMAT_OP_MR_ALLOC :
		    RDMAT_OP_FRWR, c, c->c_iters);
		break;
	}
	return (0);
}

/* Check the destination; returns the first bad offset or -1. */
static int64_t
check(const bconf_t *c, bpair_t *bp)
{
	uint64_t rlen = window(c), k, n;
	int64_t bad = -1;

	switch (c->c_test) {
	case T_WRITE_BW:
		(void) buf(&bp->bp_b, RDMAT_BUF_VERIFY, 0,
		    MIN(rlen, bp->bp_sa.s_rr.rr_posted * c->c_size),
		    bp->bp_seed, 0, &bad);
		break;
	case T_READ_BW:
		(void) buf(&bp->bp_a, RDMAT_BUF_VERIFY, 0,
		    MIN(rlen, bp->bp_sa.s_rr.rr_posted * c->c_size),
		    bp->bp_seed, 0, &bad);
		break;
	case T_SEND_BW:
		n = MIN(c->c_depth, bp->bp_sb.s_rr.rr_done);
		for (k = 0; k < n && bad == -1; k++) {
			(void) buf(&bp->bp_b, RDMAT_BUF_VERIFY,
			    k * c->c_size, c->c_size, bp->bp_seed, 0, &bad);
		}
		break;
	default:
		break;
	}
	return (bad);
}

static double
cpu_per_gib(double ns, double bytes)
{
	if (ns < 0)
		ns = 0;
	return (bytes > 0 ? ns / 1e9 / (bytes / (1024.0 * 1024 * 1024)) : 0);
}

static void
report(const bconf_t *c, uint64_t wall, uint64_t wall_b,
    const host_stats_t *a0, const host_stats_t *a1, const host_stats_t *b0,
    const host_stats_t *b1)
{
	double bytes = 0, ops = 0, qmin = 1e30, qmax = 0, gbps;
	double busy, attr, intr;
	uint64_t span = 0;
	char vecs[256];
	int64_t bad = -1, b;
	uint32_t i, v;
	int err = 0, off;

	for (i = 0; i < c->c_qps; i++) {
		bpair_t *bp = &pairs[i];
		rdmat_run_t *ra = &bp->bp_sa.s_rr;
		double q;

		if (bp->bp_sa.s_ret != 0 || (bp->bp_sb.s_used &&
		    bp->bp_sb.s_ret != 0)) {
			err = bp->bp_sa.s_ret != 0 ? bp->bp_sa.s_ret :
			    bp->bp_sb.s_ret;
			(void) printf("# QP %u: %s: A %s status %u done %llu, "
			    "B %s status %u done %llu\n", i, tnames[c->c_test],
			    strerror(bp->bp_sa.s_ret), ra->rr_status,
			    (unsigned long long)ra->rr_done,
			    strerror(bp->bp_sb.s_ret),
			    bp->bp_sb.s_rr.rr_status,
			    (unsigned long long)bp->bp_sb.s_rr.rr_done);
			continue;
		}
		if (c->c_test <= T_SEND_BW) {
			bytes += (double)ra->rr_bytes;
			ops += (double)ra->rr_done;
			q = ra->rr_ns > 0 ? (double)ra->rr_bytes * 8 /
			    ra->rr_ns : 0;
			qmin = q < qmin ? q : qmin;
			qmax = q > qmax ? q : qmax;
			span = ra->rr_ns > span ? ra->rr_ns : span;
		} else {
			ops += (double)c->c_iters;
			bytes += (double)c->c_iters * c->c_size;
		}
		if (c->c_verify && (b = check(c, bp)) != -1 && bad == -1)
			bad = b;
	}
	gbps = span > 0 ? bytes * 8 / (double)span : 0;

	(void) printf("BENCH test=%s where=%s mode=%s size=%u qps=%u "
	    "depth=%u batch=%u signal=%u inline=%u vecs=%u", tnames[c->c_test],
	    remote ? "hosts" : "loop", mnames[c->c_poll],
	    c->c_size, c->c_qps, c->c_depth, c->c_batch, c->c_signal,
	    c->c_inline, vec_fixed >= 0 ? 1 : MIN(ncomp, c->c_qps));
	if (c->c_poll == M_ADAPT)
		(void) printf(" spin=%u", c->c_spin);
	if (c->c_modc != 0 || c->c_modus != 0)
		(void) printf(" modc=%u modus=%u", c->c_modc, c->c_modus);
	if (err != 0) {
		(void) printf(" result=FAIL error=%s\n", strerror(err));
		(void) fflush(stdout);
		return;
	}
	if (c->c_test <= T_SEND_BW) {
		(void) printf(" gbps=%.3f mops=%.4f qp_gbps_min=%.3f "
		    "qp_gbps_max=%.3f", gbps, span > 0 ? ops / (double)span *
		    1e3 : 0,
		    qmin, qmax);
	} else {
		rdmat_run_t *ra = &pairs[0].bp_sa.s_rr;

		(void) printf(" p50_us=%.2f p99_us=%.2f p999_us=%.2f "
		    "min_us=%.2f avg_us=%.2f max_us=%.2f mops=%.4f",
		    ra->rr_lat_p50 / 1e3, ra->rr_lat_p99 / 1e3,
		    ra->rr_lat_p999 / 1e3, ra->rr_lat_min / 1e3,
		    ra->rr_lat_avg / 1e3, ra->rr_lat_max / 1e3,
		    ops / (double)wall * 1e3);
	}

	busy = (double)(a1->hs_busy_ns - a0->hs_busy_ns) -
	    idle_busy_a * (double)wall;
	intr = (double)(a1->hs_intr_ns - a0->hs_intr_ns) -
	    idle_intr_a * (double)wall;
	attr = (double)(a1->hs_proc_ns - a0->hs_proc_ns) +
	    (double)(a1->hs_taskq_ns - a0->hs_taskq_ns) +
	    (double)(a1->hs_ceq_ns - a0->hs_ceq_ns) + (intr > 0 ? intr : 0);
	(void) printf(" cpuA_host=%.4f cpuA_attr=%.4f usA_op=%.3f",
	    cpu_per_gib(busy, bytes), cpu_per_gib(attr, bytes),
	    ops > 0 ? attr / 1e3 / ops : 0);
	(void) printf(" intrA_op=%.4f dbA_op=%.4f armA_op=%.4f",
	    (double)(a1->hs_ceq_intrs + a1->hs_aeq_intrs - a0->hs_ceq_intrs -
	    a0->hs_aeq_intrs) / ops,
	    (double)(a1->hs_sq_doorbells - a0->hs_sq_doorbells) / ops,
	    (double)(a1->hs_cq_arms - a0->hs_cq_arms) / ops);
	if (remote) {
		busy = (double)(b1->hs_busy_ns - b0->hs_busy_ns) -
		    idle_busy_b * (double)wall_b;
		intr = (double)(b1->hs_intr_ns - b0->hs_intr_ns) -
		    idle_intr_b * (double)wall_b;
		attr = (double)(b1->hs_proc_ns - b0->hs_proc_ns) +
		    (double)(b1->hs_taskq_ns - b0->hs_taskq_ns) +
		    (double)(b1->hs_ceq_ns - b0->hs_ceq_ns) +
		    (intr > 0 ? intr : 0);
		(void) printf(" cpuB_host=%.4f cpuB_attr=%.4f usB_op=%.3f "
		    "intrB_op=%.4f dbB_op=%.4f armB_op=%.4f",
		    cpu_per_gib(busy, bytes), cpu_per_gib(attr, bytes),
		    ops > 0 ? attr / 1e3 / ops : 0,
		    (double)(b1->hs_ceq_intrs + b1->hs_aeq_intrs -
		    b0->hs_ceq_intrs - b0->hs_aeq_intrs) / ops,
		    (double)(b1->hs_sq_doorbells - b0->hs_sq_doorbells) / ops,
		    (double)(b1->hs_cq_arms - b0->hs_cq_arms) / ops);
	}
	vecs[0] = '\0';
	for (v = 0, off = 0; v < a1->hs_nvec && v < 16 &&
	    off < (int)sizeof (vecs) - 24; v++) {
		off += snprintf(vecs + off, sizeof (vecs) - off, "%s%llu",
		    v == 0 ? "" : ",", (unsigned long long)
		    (a1->hs_vec_intrs[v] - a0->hs_vec_intrs[v]));
	}
	if (vecs[0] != '\0')
		(void) printf(" vec_intrsA=%s", vecs);
	(void) printf(" wall_ms=%.1f verify=%s", wall / 1e6, !c->c_verify ?
	    "off" : bad == -1 ? "ok" : "BAD");
	if (bad != -1)
		(void) printf(" bad_offset=%lld", (long long)bad);
	(void) printf(" result=%s\n", bad == -1 ? "ok" : "FAIL");
	(void) fflush(stdout);
}

static void
bench_one(const bconf_t *c)
{
	host_stats_t a0, a1, b0, b1;
	uint32_t i;
	uint64_t t0, t1;
	int ret;

	if (c->c_inline && (c->c_size > local_dev.rdi_max_inline ||
	    c->c_test == T_READ_BW || c->c_test == T_READ_LAT)) {
		(void) printf("# skip %s size %u inline: at most %u bytes, "
		    "and not for reads\n", tnames[c->c_test], c->c_size,
		    local_dev.rdi_max_inline);
		return;
	}
	o_depth = c->c_depth;
	o_buf_mb = bufneed(c) / MB;
	if (o_buf_mb > RDMAT_MAX_BUF / MB) {
		(void) printf("# skip %s size %u depth %u: the buffer would "
		    "be %llu MB\n", tnames[c->c_test], c->c_size, c->c_depth,
		    (unsigned long long)o_buf_mb);
		return;
	}
	fresh_inline = c->c_inline ? c->c_size : 0;
	for (i = 0; i < c->c_qps; i++) {
		fresh_vector = vec_fixed >= 0 ? (uint32_t)vec_fixed : i % ncomp;
		fresh_mod_count = c->c_poll == M_POLL ? 0 : (uint16_t)c->c_modc;
		fresh_mod_us = c->c_poll == M_POLL ? 0 : (uint16_t)c->c_modus;
		if ((ret = fresh(&pairs[i].bp_a, &pairs[i].bp_b, RDMAT_QPT_RC,
		    c->c_poll == M_POLL ? RDMAT_POLL_DIRECT :
		    RDMAT_POLL_TASKQ)) != 0 ||
		    prepare(c, &pairs[i]) != 0) {
			(void) printf("BENCH test=%s qps=%u size=%u "
			    "result=FAIL error=setup:%s\n", tnames[c->c_test],
			    c->c_qps, c->c_size, strerror(ret));
			return;
		}
	}

	/*
	 * A server answers nothing else while a passive run is in progress,
	 * so its counters are read before the passive runs start and after
	 * they end.
	 */
	if (remote)
		peer_stats(c->c_qps, &b0);
	/* Passive sides first, so the active ones find them waiting. */
	for (i = 0; i < c->c_qps; i++) {
		bside_t *sb = &pairs[i].bp_sb;

		if (sb->s_used)
			run_start(sb->s_peer, &sb->s_rr);
	}
	go = 0;
	for (i = 0; i < c->c_qps; i++) {
		if (pthread_create(&pairs[i].bp_sa.s_thr, NULL, side_thr,
		    &pairs[i].bp_sa) != 0)
			fatal("pthread_create failed");
	}
	host_stats(&a0);
	t0 = now_ns();
	(void) pthread_mutex_lock(&go_lock);
	go = 1;
	(void) pthread_cond_broadcast(&go_cv);
	(void) pthread_mutex_unlock(&go_lock);
	for (i = 0; i < c->c_qps; i++)
		(void) pthread_join(pairs[i].bp_sa.s_thr, NULL);
	for (i = 0; i < c->c_qps; i++) {
		bside_t *sb = &pairs[i].bp_sb;

		if (sb->s_used)
			sb->s_ret = run_finish(sb->s_peer, &sb->s_rr);
	}
	t1 = now_ns();
	host_stats(&a1);
	if (remote) {
		peer_stats(c->c_qps, &b1);
		if (b1.hs_now_ns <= b0.hs_now_ns)
			fatal("the peer's CPU counters are not available");
	}
	report(c, t1 - t0, remote ? b1.hs_now_ns - b0.hs_now_ns : 0, &a0,
	    &a1, &b0, &b1);
}

static void
usage_bench(void)
{
	(void) fprintf(stderr, "usage: rdmatool -i ip bench {loop | host} "
	    "{write_bw|read_bw|send_bw|write_lat|read_lat|send_lat|mr_alloc|"
	    "frwr}\n"
	    "\t[size=] [qps=] [depth=] [batch=] [signal=] [inline=]\n"
	    "\t[mode=poll,intr,adapt] [spin=] [modc=] [modus=] [secs=] "
	    "[iters=] [verify=] [vec=]\n");
	exit(2);
}

int
bench_main(peer_t *a, peer_t *b, int argc, char **argv)
{
	blist_t size, qps, depth, batch, signal, inl, mode, spin, modc, modus;
	bconf_t c;
	uint32_t secs = (uint32_t)o_secs, iters = 20000, verify = 1, maxqp;
	uint_t im, iq, is, id, ib, ig, ii, ip, ic, iu;
	int i, test = -1;

	(void) a;
	(void) b;
	if (argc < 2)
		usage_bench();
	remote = strcmp(argv[0], "loop") != 0;
	for (i = 0; tnames[i] != NULL; i++) {
		if (strcmp(argv[1], tnames[i]) == 0)
			test = i;
	}
	if (test < 0)
		usage_bench();

	blist_parse(&size, "4096", "size");
	blist_parse(&qps, "1", "qps");
	blist_parse(&depth, "64", "depth");
	blist_parse(&batch, "1", "batch");
	blist_parse(&signal, "0", "signal");
	blist_parse(&inl, "0", "inline");
	blist_parse(&mode, "intr", "mode");
	blist_parse(&spin, "50", "spin");
	blist_parse(&modc, "0", "modc");
	blist_parse(&modus, "0", "modus");
	for (i = 2; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		const char *v;

		if (eq == NULL)
			usage_bench();
		*eq = '\0';
		v = eq + 1;
		if (strcmp(argv[i], "size") == 0)
			blist_parse(&size, v, "size");
		else if (strcmp(argv[i], "qps") == 0)
			blist_parse(&qps, v, "qps");
		else if (strcmp(argv[i], "depth") == 0)
			blist_parse(&depth, v, "depth");
		else if (strcmp(argv[i], "batch") == 0)
			blist_parse(&batch, v, "batch");
		else if (strcmp(argv[i], "signal") == 0)
			blist_parse(&signal, v, "signal");
		else if (strcmp(argv[i], "inline") == 0)
			blist_parse(&inl, v, "inline");
		else if (strcmp(argv[i], "mode") == 0)
			blist_parse(&mode, v, "mode");
		else if (strcmp(argv[i], "spin") == 0)
			blist_parse(&spin, v, "spin");
		else if (strcmp(argv[i], "modc") == 0)
			blist_parse(&modc, v, "modc");
		else if (strcmp(argv[i], "modus") == 0)
			blist_parse(&modus, v, "modus");
		else if (strcmp(argv[i], "secs") == 0)
			secs = (uint32_t)strtoul(v, NULL, 0);
		else if (strcmp(argv[i], "iters") == 0)
			iters = (uint32_t)strtoul(v, NULL, 0);
		else if (strcmp(argv[i], "verify") == 0)
			verify = (uint32_t)strtoul(v, NULL, 0);
		else if (strcmp(argv[i], "vec") == 0)
			vec_fixed = strcmp(v, "spread") == 0 ? -1 :
			    (int)strtoul(v, NULL, 0);
		else
			usage_bench();
	}
	if (secs == 0 || secs > 100 || iters == 0 ||
	    iters > RDMAT_MAX_COUNT)
		fatal("secs is 1 to 100 and iters 1 to %u", RDMAT_MAX_COUNT);

	maxqp = 0;
	for (iq = 0; iq < qps.bl_n; iq++) {
		if (qps.bl_v[iq] == 0 || qps.bl_v[iq] > B_MAXQP)
			fatal("qps is 1 to %d", B_MAXQP);
		maxqp = qps.bl_v[iq] > maxqp ? qps.bl_v[iq] : maxqp;
	}
	for (i = 0; i < (int)maxqp; i++) {
		pairs[i].bp_a.p_name = "A";
		pairs[i].bp_b.p_name = "B";
		pairs[i].bp_a.p_sock = pairs[i].bp_b.p_sock = -1;
		pairs[i].bp_a.p_fd = pairs[i].bp_b.p_fd = -1;
		if (remote) {
			pairs[i].bp_b.p_sock = connect_to(argv[0]);
			o_server = argv[0];
		}
	}
	if (remote) {
		rdmat_devices_t d;

		bzero(&d, sizeof (d));
		if (rpc(&pairs[0].bp_b, RDMAT_IOC_DEVICES, &d, 0,
		    K_RESULT) != 0 || d.rdd_count == 0)
			fatal("the server has no RDMA device");
		if (d.rdd_devs[0].rdi_active_mtu < path_mtu)
			path_mtu = d.rdd_devs[0].rdi_active_mtu;
		ncomp = MIN(local_dev.rdi_comp_vectors,
		    d.rdd_devs[0].rdi_comp_vectors);
	} else {
		ncomp = local_dev.rdi_comp_vectors;
	}
	ncomp = MAX(ncomp, 1);
	if (vec_fixed >= (int)ncomp)
		fatal("vec is below %u", ncomp);
	(void) printf("# %s %s against %s, path MTU %u, link %llu Mb/s, %u "
	    "completion vectors\n", o_dev, tnames[test], remote ? argv[0] :
	    "itself", path_mtu, (unsigned long long)(local_dev.rdi_speed /
	    1000000), ncomp);
	idle_rates();

	for (im = 0; im < mode.bl_n; im++)
	for (iq = 0; iq < qps.bl_n; iq++)
	for (is = 0; is < size.bl_n; is++)
	for (id = 0; id < depth.bl_n; id++)
	for (ib = 0; ib < batch.bl_n; ib++)
	for (ig = 0; ig < signal.bl_n; ig++)
	for (ii = 0; ii < inl.bl_n; ii++)
	for (ip = 0; ip < spin.bl_n; ip++)
	for (ic = 0; ic < modc.bl_n; ic++)
	for (iu = 0; iu < modus.bl_n; iu++) {
		bzero(&c, sizeof (c));
		c.c_spin = spin.bl_v[ip];
		c.c_modc = modc.bl_v[ic];
		c.c_modus = modus.bl_v[iu];
		c.c_test = test;
		c.c_poll = mode.bl_v[im];
		c.c_qps = qps.bl_v[iq];
		c.c_size = size.bl_v[is];
		c.c_depth = depth.bl_v[id];
		c.c_batch = batch.bl_v[ib];
		c.c_signal = signal.bl_v[ig];
		c.c_inline = inl.bl_v[ii];
		c.c_secs = secs;
		c.c_iters = iters;
		c.c_verify = verify;
		if (c.c_size == 0 || c.c_depth == 0 ||
		    c.c_depth > RDMAT_MAX_DEPTH || c.c_batch == 0 ||
		    c.c_batch > RDMAT_MAX_BATCH || c.c_signal > c.c_depth)
			fatal("size > 0, depth 1 to %d, batch 1 to %d, signal "
			    "at most depth", RDMAT_MAX_DEPTH, RDMAT_MAX_BATCH);
		if (c.c_signal == 0)
			c.c_signal = c.c_depth / 2 > 0 ? c.c_depth / 2 : 1;
		bench_one(&c);
	}

	for (i = 0; i < (int)maxqp; i++) {
		if (pairs[i].bp_a.p_fd >= 0)
			(void) close(pairs[i].bp_a.p_fd);
		if (remote)
			(void) rpc(&pairs[i].bp_b, C_BYE, NULL, 0, K_ACK);
		else if (pairs[i].bp_b.p_fd >= 0)
			(void) close(pairs[i].bp_b.p_fd);
	}
	return (0);
}

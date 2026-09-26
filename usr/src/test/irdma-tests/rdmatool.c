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
 * rdmatool: RDMA verbs acceptance tests through rdmat(4D).
 *
 *	rdmatool [opts] info
 *	rdmatool [opts] loop [test...]		two sessions on one device
 *	rdmatool [opts] server			serve one client at a time
 *	rdmatool [opts] client host [test...]
 *	rdmatool [opts] bench {loop | host} test [key=value...]
 *
 * Options: -d device, -i local IPv4 (required but for info), -p TCP port,
 * -b buffer MB per QP, -q queue depth, -t seconds per bandwidth run.
 * rdmabench.c describes the benchmark.
 *
 * The client (or loop) side A runs each test against side B, which is a
 * second local session or the server's session.  The server only executes
 * the ioctls the client sends it, over an ordinary TCP connection, and
 * answers with their results; QP numbers, PSNs, GIDs, MACs, rkeys and
 * addresses travel the same way.  Every data test fills the source with a
 * seeded pattern and checks every byte at the destination in the kernel.
 *
 * Tests: send write read frwr localinv badkey zerokey bounds access
 * qpaccess ud pingpong bw inflight, and tcp for a TCP baseline between two
 * hosts.
 * Each prints PASS or FAIL with its numbers; the exit status is 0 only if
 * all pass.
 *
 * Build: gcc -m64 -pthread -o rdmatool rdmatool.c rdmabench.c \
 *	-lkstat -lsocket -lnsl
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <kstat.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>

#include "rdmatool.h"

#define	ST_REM_ACCESS	10	/* RDK_WC_REM_ACCESS_ERR */
#define	QPS_ERR		6	/* RDK_QPS_ERR */

typedef struct tcpreq {
	uint64_t	tr_count;	/* ping-pong round trips */
	uint64_t	tr_bulk;	/* bytes the client then sends */
	uint32_t	tr_size;	/* ping-pong message size */
	uint32_t	tr_pad;
	uint64_t	tr_srv_cpu_ns;	/* out: server busy time in the bulk */
	uint64_t	tr_srv_ns;	/* out: server time from first byte */
} tcpreq_t;

char *o_dev;
uint32_t o_ip;
int o_port = 18515;
uint64_t o_buf_mb = 16;
uint32_t o_depth = 64;
int o_secs = 5;
static int failures;
uint32_t path_mtu = 1024;
char *o_server;

static uint64_t cpu_busy_ns(void);

void
fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void) fputc('\n', stderr);
	exit(2);
}

static void
result(int ok, const char *name, const char *fmt, ...)
{
	va_list ap;

	(void) printf("%s %s: ", ok ? "PASS" : "FAIL", name);
	va_start(ap, fmt);
	(void) vprintf(fmt, ap);
	va_end(ap);
	(void) printf("\n");
	(void) fflush(stdout);
	if (!ok)
		failures++;
}

static size_t
cmd_len(uint32_t cmd)
{
	switch (cmd) {
	case RDMAT_IOC_DEVICES:
		return (sizeof (rdmat_devices_t));
	case RDMAT_IOC_SETUP:
		return (sizeof (rdmat_setup_t));
	case RDMAT_IOC_CONNECT:
		return (sizeof (rdmat_connect_t));
	case RDMAT_IOC_RUN:
		return (sizeof (rdmat_run_t));
	case RDMAT_IOC_BUF:
		return (sizeof (rdmat_buf_t));
	case RDMAT_IOC_QUERY:
		return (sizeof (rdmat_query_t));
	case C_FRESH:
	case C_BYE:
		return (0);
	case C_CPU:
		return (sizeof (uint64_t));
	case C_TCP:
		return (sizeof (tcpreq_t));
	case C_STATS:
		return (sizeof (host_stats_t));
	default:
		return ((size_t)-1);
	}
}

static int
xfer(int s, void *buf, size_t len, int out)
{
	char *p = buf;
	ssize_t n;

	while (len > 0) {
		n = out ? write(s, p, len) : read(s, p, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return (-1);
		}
		p += n;
		len -= (size_t)n;
	}
	return (0);
}

int
open_session(void)
{
	int fd = open(DEVPATH, O_RDWR);

	if (fd < 0)
		fatal("open %s: %s", DEVPATH, strerror(errno));
	return (fd);
}

/* Send a request and read one reply of the given kind. */
int
rpc(peer_t *p, uint32_t cmd, void *arg, int async, int kind)
{
	msg_t m;
	size_t len = cmd_len(cmd);

	if (cmd == C_BYE) {
		m.m_magic = MAGIC;
		m.m_cmd = cmd;
		m.m_len = 0;
		m.m_err = 0;
		(void) xfer(p->p_sock, &m, sizeof (m), 1);
		return (0);
	}
	if (kind != K_RESULT || !async) {
		m.m_magic = MAGIC;
		m.m_cmd = cmd;
		m.m_len = (uint32_t)len;
		m.m_err = async;
		if (xfer(p->p_sock, &m, sizeof (m), 1) != 0 ||
		    (len != 0 && xfer(p->p_sock, arg, len, 1) != 0))
			fatal("%s: connection lost", p->p_name);
	}
	for (;;) {
		if (xfer(p->p_sock, &m, sizeof (m), 0) != 0 ||
		    m.m_magic != MAGIC)
			fatal("%s: bad reply", p->p_name);
		if (m.m_cmd == K_ACK && kind == K_ACK)
			return (0);
		if (m.m_cmd != K_RESULT || m.m_len != len)
			fatal("%s: unexpected reply", p->p_name);
		if (len != 0 && xfer(p->p_sock, arg, len, 0) != 0)
			fatal("%s: connection lost", p->p_name);
		return (m.m_err);
	}
}

int
pio(peer_t *p, uint32_t cmd, void *arg)
{
	if (p->p_sock >= 0)
		return (rpc(p, cmd, arg, 0, K_RESULT));
	return (ioctl(p->p_fd, cmd, arg) == 0 ? 0 : errno);
}

static void *
async_thr(void *arg)
{
	peer_t *p = arg;

	p->p_aret = ioctl(p->p_fd, RDMAT_IOC_RUN, &p->p_arun) == 0 ? 0 :
	    errno;
	return (NULL);
}

/* Start a run that the other side's work completes. */
void
run_start(peer_t *p, const rdmat_run_t *rr)
{
	p->p_arun = *rr;
	p->p_async = 1;
	if (p->p_sock >= 0) {
		(void) rpc(p, RDMAT_IOC_RUN, &p->p_arun, 1, K_ACK);
	} else if (pthread_create(&p->p_thr, NULL, async_thr, p) != 0) {
		fatal("pthread_create failed");
	}
	/* Give a local runner time to post its receives. */
	(void) usleep(50000);
}

int
run_finish(peer_t *p, rdmat_run_t *rr)
{
	int ret;

	if (p->p_sock >= 0) {
		ret = rpc(p, RDMAT_IOC_RUN, &p->p_arun, 1, K_RESULT);
	} else {
		(void) pthread_join(p->p_thr, NULL);
		ret = p->p_aret;
	}
	p->p_async = 0;
	*rr = p->p_arun;
	return (ret);
}

int
run(peer_t *p, rdmat_run_t *rr)
{
	return (pio(p, RDMAT_IOC_RUN, rr));
}

void
run_init(rdmat_run_t *rr, uint32_t op, uint32_t size, uint32_t count)
{
	bzero(rr, sizeof (*rr));
	rr->rr_op = op;
	rr->rr_size = size;
	rr->rr_count = count;
	rr->rr_depth = 1;
	rr->rr_timeout_ms = 10000;
}

int
buf(peer_t *p, uint32_t op, uint64_t off, uint64_t len, uint64_t seed,
    uint64_t base, int64_t *mismatch)
{
	rdmat_buf_t rb;
	int ret;

	bzero(&rb, sizeof (rb));
	rb.rb_op = op;
	rb.rb_offset = off;
	rb.rb_len = len;
	rb.rb_seed = seed;
	rb.rb_pattern_base = base;
	ret = pio(p, RDMAT_IOC_BUF, &rb);
	if (mismatch != NULL)
		*mismatch = ret == 0 ? rb.rb_mismatch : -2;
	return (ret);
}

static uint32_t
qp_state(peer_t *p)
{
	rdmat_query_t q;

	bzero(&q, sizeof (q));
	return (pio(p, RDMAT_IOC_QUERY, &q) == 0 ? q.rq_state : 99);
}

/*
 * Fresh sessions on both sides, set up and connected to each other.
 */
/* The QP access B's next fresh() connection grants (rc_qp_access). */
uint32_t fresh_b_access;
/* The inline size of the next fresh() QPs. */
uint32_t fresh_inline;

int
fresh(peer_t *a, peer_t *b, uint32_t qpt, uint32_t poll)
{
	rdmat_connect_t rc;
	peer_t *ps[2] = { a, b };
	int i, ret;

	for (i = 0; i < 2; i++) {
		peer_t *p = ps[i];

		if (p->p_sock >= 0) {
			if ((ret = rpc(p, C_FRESH, NULL, 0, K_RESULT)) != 0)
				return (ret);
		} else {
			(void) close(p->p_fd);
			p->p_fd = open_session();
		}
		bzero(&p->p_setup, sizeof (p->p_setup));
		(void) strlcpy(p->p_setup.rs_dev, o_dev,
		    sizeof (p->p_setup.rs_dev));
		p->p_setup.rs_ipv4 = o_ip;
		p->p_setup.rs_qpt = qpt;
		p->p_setup.rs_nqp = 1;
		p->p_setup.rs_poll = poll;
		p->p_setup.rs_buf_len = o_buf_mb << 20;
		p->p_setup.rs_depth = o_depth;
		p->p_setup.rs_inline = fresh_inline;
		/* The server supplies its own device and address. */
		if ((ret = pio(p, RDMAT_IOC_SETUP, &p->p_setup)) != 0) {
			(void) fprintf(stderr, "%s: setup: %s\n", p->p_name,
			    strerror(ret));
			return (ret);
		}
	}
	for (i = 0; i < 2; i++) {
		peer_t *p = ps[i], *o = ps[1 - i];

		bzero(&rc, sizeof (rc));
		rc.rc_rqpn = o->p_setup.rs_qp[0].rqi_qpn;
		rc.rc_rpsn = o->p_setup.rs_qp[0].rqi_psn;
		rc.rc_rqkey = o->p_setup.rs_qp[0].rqi_qkey;
		rc.rc_path_mtu = path_mtu;
		bcopy(o->p_setup.rs_gid, rc.rc_dgid, sizeof (rc.rc_dgid));
		bcopy(o->p_setup.rs_mac, rc.rc_dmac, sizeof (rc.rc_dmac));
		rc.rc_retry = 7;
		rc.rc_rnr_retry = 7;
		if (p == b)
			rc.rc_qp_access = fresh_b_access;
		if ((ret = pio(p, RDMAT_IOC_CONNECT, &rc)) != 0) {
			(void) fprintf(stderr, "%s: connect: %s\n", p->p_name,
			    strerror(ret));
			return (ret);
		}
	}
	return (0);
}

static uint64_t
seed_of(const char *tag, uint64_t n)
{
	uint64_t s = 0xcbf29ce484222325ULL ^ n ^ (uint64_t)time(NULL);

	while (*tag != '\0')
		s = (s ^ (uint8_t)*tag++) * 0x100000001b3ULL;
	return (s);
}

/*
 * The tests.
 */
static void
t_send(peer_t *a, peer_t *b)
{
	static const uint32_t sizes[] = { 1, 64, 4096, 65536 };
	const uint32_t n = 16;
	rdmat_run_t rr, rw;
	int64_t bad;
	uint64_t seed;
	uint_t i, k;
	int ret;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0) {
		result(0, "send", "setup failed");
		return;
	}
	for (i = 0; i < sizeof (sizes) / sizeof (sizes[0]); i++) {
		uint32_t s = sizes[i];

		seed = seed_of("send", s);
		(void) buf(b, RDMAT_BUF_ZERO, 0, (uint64_t)n * s, 0, 0, NULL);
		(void) buf(a, RDMAT_BUF_FILL, 0, s, seed, 0, NULL);
		run_init(&rr, RDMAT_OP_POST_RECV, s, n);
		rr.rr_flags = RDMAT_F_DMA_LKEY;
		if ((ret = run(b, &rr)) != 0) {
			result(0, "send", "post recv: %s", strerror(ret));
			return;
		}
		run_init(&rw, RDMAT_OP_WAIT_RECV, s, n);
		run_start(b, &rw);
		run_init(&rr, RDMAT_OP_SEND, s, n);
		rr.rr_depth = 8;
		rr.rr_flags = RDMAT_F_DMA_LKEY;
		ret = run(a, &rr);
		if (run_finish(b, &rw) != 0 || ret != 0 || rw.rr_done != n ||
		    rw.rr_len_mismatch != 0 || rw.rr_last_len != s) {
			result(0, "send", "size %u: send %s done %llu, recv "
			    "done %llu status %u len %u mismatched %u", s,
			    strerror(ret), (u_longlong_t)rr.rr_done,
			    (u_longlong_t)rw.rr_done, rw.rr_status,
			    rw.rr_last_len, rw.rr_len_mismatch);
			return;
		}
		for (k = 0; k < n; k++) {
			(void) buf(b, RDMAT_BUF_VERIFY, (uint64_t)k * s, s,
			    seed, 0, &bad);
			if (bad != -1) {
				result(0, "send", "size %u message %u differs "
				    "at %lld", s, k, (long long)bad);
				return;
			}
		}
	}
	result(1, "send", "%u messages each of 1, 64, 4096 and 65536 bytes "
	    "with the local DMA lkey, every byte checked", n);
}

static int
xfer_check(peer_t *a, peer_t *b, uint32_t op, uint64_t len, uint64_t *ns)
{
	peer_t *src = op == RDMAT_OP_WRITE ? a : b;
	peer_t *dst = op == RDMAT_OP_WRITE ? b : a;
	uint64_t seed = seed_of(op == RDMAT_OP_WRITE ? "write" : "read", len);
	rdmat_run_t rr;
	int64_t bad;
	int ret;

	(void) buf(dst, RDMAT_BUF_ZERO, 0, len, 0, 0, NULL);
	(void) buf(src, RDMAT_BUF_FILL, 0, len, seed, 0, NULL);
	run_init(&rr, op, (uint32_t)len, 1);
	rr.rr_raddr = b->p_setup.rs_qp[0].rqi_addr;
	rr.rr_rkey = b->p_setup.rs_qp[0].rqi_rkey;
	rr.rr_rlen = len;
	if ((ret = run(a, &rr)) != 0) {
		(void) printf("  %s %llu: %s status %u\n",
		    op == RDMAT_OP_WRITE ? "write" : "read",
		    (u_longlong_t)len, strerror(ret), rr.rr_status);
		return (-1);
	}
	*ns = rr.rr_ns;
	(void) buf(dst, RDMAT_BUF_VERIFY, 0, len, seed, 0, &bad);
	if (bad != -1) {
		(void) printf("  %s %llu: differs at %lld\n",
		    op == RDMAT_OP_WRITE ? "write" : "read",
		    (u_longlong_t)len, (long long)bad);
		return (-1);
	}
	return (0);
}

static void
t_rw(peer_t *a, peer_t *b, uint32_t op)
{
	uint64_t lens[] = { 1, 4096, 65537, 1 << 20, 8 << 20 };
	const char *name = op == RDMAT_OP_WRITE ? "write" : "read";
	uint64_t ns = 0;
	uint_t i;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_DIRECT) != 0) {
		result(0, name, "setup failed");
		return;
	}
	for (i = 0; i < sizeof (lens) / sizeof (lens[0]); i++) {
		if (lens[i] > (o_buf_mb << 20))
			continue;
		if (xfer_check(a, b, op, lens[i], &ns) != 0) {
			result(0, name, "length %llu failed",
			    (u_longlong_t)lens[i]);
			return;
		}
	}
	result(1, name, "1 B to 8 MB through FRWR MRs over 1 MB chunks, every "
	    "byte checked; 8 MB in %.2f ms", ns / 1e6);
}

/* Expect a remote access error on A with A's QP in error. */
static int
expect_rae(peer_t *a, rdmat_run_t *rr, const char *name, const char *what)
{
	int ret = run(a, rr);
	uint32_t st = qp_state(a);

	if (ret == EIO && rr->rr_status == ST_REM_ACCESS && st == QPS_ERR)
		return (0);
	result(0, name, "%s: ret %s status %u opcode %u vendor 0x%x qp "
	    "state %u", what, strerror(ret), rr->rr_status, rr->rr_err_opcode,
	    rr->rr_vendor_err, st);
	return (-1);
}

static void
t_frwr(peer_t *a, peer_t *b)
{
	rdmat_run_t rr, rw;
	uint32_t key;
	uint64_t ns;
	int ret;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0) {
		result(0, "frwr", "setup failed");
		return;
	}
	/* Rebind B's MR with a new key and use it. */
	run_init(&rr, RDMAT_OP_REG, 0, 1);
	rr.rr_access = RDMAT_ACC_REMOTE_WRITE | RDMAT_ACC_REMOTE_READ;
	if ((ret = run(b, &rr)) != 0) {
		result(0, "frwr", "REG: %s status %u", strerror(ret),
		    rr.rr_status);
		return;
	}
	key = rr.rr_new_rkey;
	if (key == b->p_setup.rs_qp[0].rqi_rkey) {
		result(0, "frwr", "REG kept the key 0x%x", key);
		return;
	}
	b->p_setup.rs_qp[0].rqi_rkey = key;
	if (xfer_check(a, b, RDMAT_OP_WRITE, 1 << 20, &ns) != 0) {
		result(0, "frwr", "write with the new key 0x%x failed", key);
		return;
	}

	/* A send with invalidate unbinds it on B. */
	run_init(&rr, RDMAT_OP_POST_RECV, 64, 1);
	(void) run(b, &rr);
	run_init(&rw, RDMAT_OP_WAIT_RECV, 64, 1);
	run_start(b, &rw);
	run_init(&rr, RDMAT_OP_SEND_INV, 64, 1);
	rr.rr_rkey = key;
	ret = run(a, &rr);
	if (run_finish(b, &rw) != 0 || ret != 0 ||
	    (rw.rr_wc_flags & 0x4) == 0 || rw.rr_inv_rkey != key) {
		result(0, "frwr", "send with invalidate: %s, recv flags 0x%x "
		    "rkey 0x%x want 0x%x", strerror(ret), rw.rr_wc_flags,
		    rw.rr_inv_rkey, key);
		return;
	}

	/* The invalidated key must now be refused. */
	run_init(&rr, RDMAT_OP_WRITE, 4096, 1);
	rr.rr_raddr = b->p_setup.rs_qp[0].rqi_addr;
	rr.rr_rkey = key;
	rr.rr_rlen = 4096;
	if (expect_rae(a, &rr, "frwr", "write after invalidate") != 0)
		return;
	result(1, "frwr", "REG to key 0x%x, 1 MB write checked, send with "
	    "invalidate reported rkey 0x%x on B, the next write got remote "
	    "access error and A's QP is in error (B state %u)", key,
	    rw.rr_inv_rkey, qp_state(b));
}

static void
t_localinv(peer_t *a, peer_t *b)
{
	rdmat_run_t rr;
	uint32_t key;
	int ret;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_DIRECT) != 0) {
		result(0, "localinv", "setup failed");
		return;
	}
	key = b->p_setup.rs_qp[0].rqi_rkey;
	run_init(&rr, RDMAT_OP_LOCAL_INV, 0, 1);
	if ((ret = run(b, &rr)) != 0) {
		result(0, "localinv", "LOCAL_INV: %s status %u",
		    strerror(ret), rr.rr_status);
		return;
	}
	run_init(&rr, RDMAT_OP_READ, 4096, 1);
	rr.rr_raddr = b->p_setup.rs_qp[0].rqi_addr;
	rr.rr_rkey = key;
	rr.rr_rlen = 4096;
	if (expect_rae(a, &rr, "localinv", "read after local invalidate") != 0)
		return;
	result(1, "localinv", "B invalidated rkey 0x%x locally; A's read got "
	    "remote access error, A's QP in error", key);
}

/* A write with a bad rkey, address or right: remote access error. */
static void
t_reject(peer_t *a, peer_t *b, const char *name)
{
	rdmat_run_t rr;
	uint64_t addr, len;
	uint32_t key;
	uint64_t ns;
	int ret;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0) {
		result(0, name, "setup failed");
		return;
	}
	addr = b->p_setup.rs_qp[0].rqi_addr;
	len = b->p_setup.rs_qp[0].rqi_len;
	key = b->p_setup.rs_qp[0].rqi_rkey;
	run_init(&rr, RDMAT_OP_WRITE, 4096, 1);
	rr.rr_rlen = 4096;
	if (strcmp(name, "badkey") == 0) {
		rr.rr_raddr = addr;
		rr.rr_rkey = key ^ 0x00a5a500;
	} else if (strcmp(name, "zerokey") == 0) {
		/* STAG 0 is the local DMA key; it must not work remotely. */
		rr.rr_raddr = addr;
		rr.rr_rkey = 0;
	} else if (strcmp(name, "bounds") == 0) {
		rr.rr_raddr = addr + len - 100;
		rr.rr_rkey = key;
	} else {
		/* Rebind B's MR read only, then write to it. */
		rdmat_run_t rg;

		run_init(&rg, RDMAT_OP_REG, 0, 1);
		rg.rr_access = RDMAT_ACC_REMOTE_READ;
		if ((ret = run(b, &rg)) != 0) {
			result(0, name, "REG read only: %s", strerror(ret));
			return;
		}
		b->p_setup.rs_qp[0].rqi_rkey = key = rg.rr_new_rkey;
		if (xfer_check(a, b, RDMAT_OP_READ, 4096, &ns) != 0) {
			result(0, name, "read of the read-only MR failed");
			return;
		}
		rr.rr_raddr = addr;
		rr.rr_rkey = key;
	}
	if (expect_rae(a, &rr, name, "write") != 0)
		return;
	result(1, name, "remote access error (status %u, vendor 0x%x) on A's "
	    "send CQ, A's QP in error; B's QP state %u", rr.rr_status,
	    rr.rr_vendor_err, qp_state(b));
}

/*
 * B's QP grants less than B's MR.  The device has one right for inbound
 * writes and read responses, which LOCAL_WRITE also turns on.
 */
static int
qpacc_case(peer_t *a, peer_t *b, uint32_t acc, uint32_t ok_op,
    uint32_t bad_op, char *out, size_t outlen)
{
	rdmat_run_t rr;
	uint32_t st;
	uint64_t ns;
	int ret;

	fresh_b_access = RDMAT_QPACC_SET | acc;
	ret = fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ);
	fresh_b_access = 0;
	if (ret != 0) {
		(void) snprintf(out, outlen, "setup with access 0x%x failed",
		    acc);
		return (-1);
	}
	if (ok_op != 0 && xfer_check(a, b, ok_op, 4096, &ns) != 0) {
		(void) snprintf(out, outlen, "access 0x%x: the allowed %s "
		    "failed", acc, ok_op == RDMAT_OP_READ ? "read" : "write");
		return (-1);
	}
	run_init(&rr, bad_op, 4096, 1);
	rr.rr_raddr = b->p_setup.rs_qp[0].rqi_addr;
	rr.rr_rlen = 4096;
	rr.rr_rkey = b->p_setup.rs_qp[0].rqi_rkey;
	ret = run(a, &rr);
	st = qp_state(a);
	if (ret != EIO || rr.rr_status == 0 || st != QPS_ERR) {
		(void) snprintf(out, outlen, "access 0x%x: %s got ret %s "
		    "status %u qp state %u", acc, bad_op == RDMAT_OP_READ ?
		    "read" : "write", strerror(ret), rr.rr_status, st);
		return (-1);
	}
	(void) snprintf(out, outlen, "access 0x%x: %s refused, status %u",
	    acc, bad_op == RDMAT_OP_READ ? "read" : "write", rr.rr_status);
	return (0);
}

static void
t_qpaccess(peer_t *a, peer_t *b)
{
	char r1[96], r2[96], r3[96];
	int bad = 0;

	bad |= qpacc_case(a, b, RDMAT_ACC_REMOTE_READ, RDMAT_OP_READ,
	    RDMAT_OP_WRITE, r1, sizeof (r1));
	bad |= qpacc_case(a, b, RDMAT_ACC_LOCAL_WRITE | RDMAT_ACC_REMOTE_WRITE,
	    RDMAT_OP_WRITE, RDMAT_OP_READ, r2, sizeof (r2));
	bad |= qpacc_case(a, b, RDMAT_ACC_LOCAL_WRITE | RDMAT_ACC_REMOTE_WRITE |
	    RDMAT_ACC_REMOTE_READ | RDMAT_QPACC_NO_IRD, RDMAT_OP_WRITE,
	    RDMAT_OP_READ, r3, sizeof (r3));
	result(bad == 0, "qpaccess", "B's QP rights under a full MR: %s; %s; "
	    "%s, no inbound read resources", r1, r2, r3);
}

static void
t_ud(peer_t *a, peer_t *b)
{
	const uint32_t n = 16, s = 1024;
	rdmat_run_t rr, rw;
	uint64_t seed = seed_of("ud", s);
	int64_t bad;
	uint_t k;
	int ret;

	if (fresh(a, b, RDMAT_QPT_UD, RDMAT_POLL_TASKQ) != 0) {
		result(0, "ud", "setup failed");
		return;
	}
	(void) buf(b, RDMAT_BUF_ZERO, 0, (uint64_t)n * (s + RDMAT_GRH_LEN), 0,
	    0, NULL);
	(void) buf(a, RDMAT_BUF_FILL, 0, s, seed, 0, NULL);
	run_init(&rr, RDMAT_OP_POST_RECV, s, n);
	rr.rr_flags = RDMAT_F_DMA_LKEY;
	(void) run(b, &rr);
	run_init(&rw, RDMAT_OP_WAIT_RECV, s, n);
	run_start(b, &rw);
	run_init(&rr, RDMAT_OP_SEND, s, n);
	rr.rr_depth = 8;
	rr.rr_flags = RDMAT_F_DMA_LKEY;
	ret = run(a, &rr);
	if (run_finish(b, &rw) != 0 || ret != 0 || rw.rr_done != n ||
	    rw.rr_last_len != s + RDMAT_GRH_LEN) {
		result(0, "ud", "send %s, recv done %llu status %u len %u",
		    strerror(ret), (u_longlong_t)rw.rr_done, rw.rr_status,
		    rw.rr_last_len);
		return;
	}
	for (k = 0; k < n; k++) {
		(void) buf(b, RDMAT_BUF_VERIFY,
		    (uint64_t)k * (s + RDMAT_GRH_LEN) + RDMAT_GRH_LEN, s, seed,
		    0, &bad);
		if (bad != -1) {
			result(0, "ud", "datagram %u differs at %lld", k,
			    (long long)bad);
			return;
		}
	}
	result(1, "ud", "%u datagrams of %u bytes through an AH, GRH plus "
	    "payload length reported, every byte checked", n, s);
}

static void
t_pingpong(peer_t *a, peer_t *b)
{
	static const uint32_t polls[] = { RDMAT_POLL_DIRECT, RDMAT_POLL_TASKQ };
	const uint32_t n = 20000;
	rdmat_run_t rr, rp;
	uint_t i;
	int ret;

	for (i = 0; i < 2; i++) {
		if (fresh(a, b, RDMAT_QPT_RC, polls[i]) != 0) {
			result(0, "pingpong", "setup failed");
			return;
		}
		run_init(&rp, RDMAT_OP_PONG, 64, n);
		rp.rr_depth = 16;
		rp.rr_flags = RDMAT_F_DMA_LKEY;
		rp.rr_timeout_ms = 60000;
		run_start(b, &rp);
		run_init(&rr, RDMAT_OP_PING, 64, n);
		rr.rr_depth = 16;
		rr.rr_flags = RDMAT_F_DMA_LKEY;
		rr.rr_timeout_ms = 60000;
		ret = run(a, &rr);
		if (run_finish(b, &rp) != 0 || ret != 0) {
			result(0, "pingpong", "%s: ping %s status %u done %llu",
			    i == 0 ? "direct" : "taskq", strerror(ret),
			    rr.rr_status, (u_longlong_t)rr.rr_done);
			return;
		}
		result(1, "pingpong", "%s poll, 64 B send round trip over %u: "
		    "min %.1f us, p50 %.1f us, avg %.1f us, p99 %.1f us, "
		    "max %.1f us", i == 0 ? "direct" : "taskq", n,
		    rr.rr_lat_min / 1e3, rr.rr_lat_p50 / 1e3,
		    rr.rr_lat_avg / 1e3, rr.rr_lat_p99 / 1e3,
		    rr.rr_lat_max / 1e3);
	}
}

/* The busy (user plus kernel) nanoseconds of every CPU. */
static uint64_t
cpu_busy_ns(void)
{
	kstat_ctl_t *kc;
	kstat_t *ks;
	kstat_named_t *kn;
	uint64_t sum = 0;

	if ((kc = kstat_open()) == NULL)
		return (0);
	for (ks = kc->kc_chain; ks != NULL; ks = ks->ks_next) {
		if (strcmp(ks->ks_module, "cpu") != 0 ||
		    strcmp(ks->ks_name, "sys") != 0 ||
		    kstat_read(kc, ks, NULL) == -1)
			continue;
		if ((kn = kstat_data_lookup(ks, "cpu_nsec_kernel")) != NULL)
			sum += kn->value.ui64;
		if ((kn = kstat_data_lookup(ks, "cpu_nsec_user")) != NULL)
			sum += kn->value.ui64;
	}
	(void) kstat_close(kc);
	return (sum);
}

/* A named value of a kstat, or 0. */
static uint64_t
kval(kstat_ctl_t *kc, kstat_t *ks, const char *name)
{
	kstat_named_t *kn;

	if (ks == NULL || kstat_read(kc, ks, NULL) == -1 ||
	    (kn = kstat_data_lookup(ks, (char *)name)) == NULL)
		return (0);
	switch (kn->data_type) {
	case KSTAT_DATA_UINT32:
		return (kn->value.ui32);
	case KSTAT_DATA_INT32:
		return ((uint64_t)kn->value.i32);
	default:
		return (kn->value.ui64);
	}
}

void
host_stats(host_stats_t *hs)
{
	struct rusage ru;
	kstat_ctl_t *kc;
	kstat_t *ks;
	char name[32];
	uint_t v;

	bzero(hs, sizeof (*hs));
	if (getrusage(RUSAGE_SELF, &ru) == 0) {
		hs->hs_proc_ns = (uint64_t)ru.ru_utime.tv_sec * 1000000000ULL +
		    (uint64_t)ru.ru_utime.tv_usec * 1000ULL +
		    (uint64_t)ru.ru_stime.tv_sec * 1000000000ULL +
		    (uint64_t)ru.ru_stime.tv_usec * 1000ULL;
	}
	if ((kc = kstat_open()) == NULL)
		return;
	for (ks = kc->kc_chain; ks != NULL; ks = ks->ks_next) {
		if (strcmp(ks->ks_module, "cpu") == 0 &&
		    strcmp(ks->ks_name, "sys") == 0) {
			uint64_t intr = kval(kc, ks, "cpu_nsec_intr");

			hs->hs_busy_ns += kval(kc, ks, "cpu_nsec_user") +
			    kval(kc, ks, "cpu_nsec_kernel") + intr;
			hs->hs_intr_ns += intr;
			hs->hs_ncpu++;
		} else if (strcmp(ks->ks_module, "unix") == 0 &&
		    ks->ks_type == KSTAT_TYPE_NAMED &&
		    (strcmp(ks->ks_name, "rdk_cq") == 0 ||
		    strncmp(ks->ks_name, "irdma_", 6) == 0)) {
			hs->hs_taskq_ns += kval(kc, ks, "totaltime");
		}
	}
	ks = kstat_lookup(kc, "irdma", 0, "ctl");
	hs->hs_ceq_intrs = kval(kc, ks, "ceq_intrs");
	hs->hs_aeq_intrs = kval(kc, ks, "aeq_intrs");
	hs->hs_sq_doorbells = kval(kc, ks, "sq_doorbells");
	hs->hs_cq_arms = kval(kc, ks, "cq_arms");
	hs->hs_ceq_ns = kval(kc, ks, "ceq_busy_ns");
	hs->hs_nvec = (uint32_t)kval(kc, ks, "comp_vectors");
	for (v = 0; v < hs->hs_nvec && v < 16; v++) {
		(void) snprintf(name, sizeof (name), "ceq%u_intrs", v + 1);
		hs->hs_vec_intrs[v] = kval(kc, ks, name);
	}
	(void) kstat_close(kc);
	hs->hs_now_ns = now_ns();
}

uint64_t
now_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

/* A host's busy CPU time with the network idle, per wall second. */
static double
idle_rate(peer_t *p)
{
	uint64_t c0, c1, t0, t1;

	t0 = now_ns();
	if (p != NULL)
		(void) rpc(p, C_CPU, &c0, 0, K_RESULT);
	else
		c0 = cpu_busy_ns();
	(void) sleep(3);
	if (p != NULL)
		(void) rpc(p, C_CPU, &c1, 0, K_RESULT);
	else
		c1 = cpu_busy_ns();
	t1 = now_ns();
	return ((double)(c1 - c0) / (double)(t1 - t0));
}

/* CPU seconds per GB net of the idle rate. */
static double
cpu_per_gb(uint64_t busy, double idle, uint64_t wall, uint64_t bytes)
{
	double net = (double)busy - idle * (double)wall;

	if (net < 0)
		net = 0;
	return (net / 1e9 / ((double)bytes / 1e9));
}

static void
t_bw(peer_t *a, peer_t *b)
{
	static const uint32_t sizes[] = { 4096, 65536, 1 << 20 };
	static const uint32_t ops[] = { RDMAT_OP_WRITE, RDMAT_OP_READ };
	rdmat_run_t rr;
	uint64_t c0, c1, r0 = 0, r1 = 0, w0, w1, count;
	uint_t i, j;
	double gbps, cpu, idle_a, idle_b = 0;
	char rcpu[64];
	int ret;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0) {
		result(0, "bw", "setup failed");
		return;
	}
	idle_a = idle_rate(NULL);
	if (b->p_sock >= 0)
		idle_b = idle_rate(b);
	(void) printf("  idle CPU: %.2f on A, %.2f on B, subtracted below\n",
	    idle_a, idle_b);
	for (j = 0; j < 2; j++) {
		for (i = 0; i < 3; i++) {
			uint32_t s = sizes[i];

			/* About o_secs seconds at 90 Gb/s. */
			count = (uint64_t)o_secs * 11250000000ULL / s;
			count = count > RDMAT_MAX_COUNT ? RDMAT_MAX_COUNT :
			    count;
			run_init(&rr, ops[j], s, (uint32_t)count);
			rr.rr_depth = s >= (1 << 20) ? 16 : 64;
			if (rr.rr_depth > o_depth)
				rr.rr_depth = o_depth;
			rr.rr_raddr = b->p_setup.rs_qp[0].rqi_addr;
			rr.rr_rkey = b->p_setup.rs_qp[0].rqi_rkey;
			rr.rr_rlen = b->p_setup.rs_qp[0].rqi_len;
			rr.rr_flags = RDMAT_F_UNSIGNALED;
			rr.rr_timeout_ms = (uint32_t)o_secs * 20000;
			if (rr.rr_timeout_ms > RDMAT_MAX_TIMEOUT_MS)
				rr.rr_timeout_ms = RDMAT_MAX_TIMEOUT_MS;
			if (b->p_sock >= 0)
				(void) rpc(b, C_CPU, &r0, 0, K_RESULT);
			c0 = cpu_busy_ns();
			w0 = now_ns();
			ret = run(a, &rr);
			w1 = now_ns();
			c1 = cpu_busy_ns();
			if (b->p_sock >= 0)
				(void) rpc(b, C_CPU, &r1, 0, K_RESULT);
			if (ret != 0 || rr.rr_ns == 0) {
				result(0, "bw", "%s %u: %s status %u done %llu",
				    j == 0 ? "write" : "read", s, strerror(ret),
				    rr.rr_status, (u_longlong_t)rr.rr_done);
				return;
			}
			gbps = (double)rr.rr_bytes * 8 / rr.rr_ns;
			cpu = cpu_per_gb(c1 - c0, idle_a, w1 - w0,
			    rr.rr_bytes);
			rcpu[0] = '\0';
			if (b->p_sock >= 0) {
				double bc = cpu_per_gb(r1 - r0, idle_b,
				    w1 - w0, rr.rr_bytes);

				(void) snprintf(rcpu, sizeof (rcpu),
				    ", %.3f on B", bc);
			}
			result(1, "bw", "%s %7u B x %llu, depth %u: %.2f Gb/s, "
			    "%.3f CPU-s per GB on A%s",
			    j == 0 ? "write" : "read", s, (u_longlong_t)count,
			    rr.rr_depth, gbps, cpu, rcpu);
		}
	}
}

/*
 * Kill a child in the middle of a long write stream, so its session is
 * closed with work in flight, then check that fresh sessions still work.
 */
static void
t_inflight(peer_t *a, peer_t *b)
{
	rdmat_run_t rr;
	pid_t pid;
	uint64_t ns;
	int status;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0) {
		result(0, "inflight", "setup failed");
		return;
	}
	if ((pid = fork()) == 0) {
		run_init(&rr, RDMAT_OP_WRITE, 65536, RDMAT_MAX_COUNT);
		rr.rr_depth = o_depth;
		rr.rr_raddr = b->p_setup.rs_qp[0].rqi_addr;
		rr.rr_rkey = b->p_setup.rs_qp[0].rqi_rkey;
		rr.rr_rlen = b->p_setup.rs_qp[0].rqi_len;
		rr.rr_timeout_ms = 60000;
		(void) ioctl(a->p_fd, RDMAT_IOC_RUN, &rr);
		_exit(0);
	}
	(void) sleep(2);
	(void) kill(pid, SIGKILL);
	(void) waitpid(pid, &status, 0);
	/* The parent's descriptor is the last one; closing it tears down. */
	(void) close(a->p_fd);
	a->p_fd = open_session();
	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0 ||
	    xfer_check(a, b, RDMAT_OP_WRITE, 1 << 20, &ns) != 0) {
		result(0, "inflight", "fresh sessions failed after the kill");
		return;
	}
	result(1, "inflight", "session closed with a 64 KB write stream in "
	    "flight, then a fresh 1 MB write checked");
}

static int
xsock(const char *host, int port)
{
	struct sockaddr_in sin;
	int s, one = 1;

	bzero(&sin, sizeof (sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &sin.sin_addr) != 1 ||
	    (s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return (-1);
	if (connect(s, (struct sockaddr *)&sin, sizeof (sin)) != 0) {
		(void) close(s);
		return (-1);
	}
	(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof (one));
	return (s);
}

static int
u64cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y ? -1 : x > y ? 1 : 0);
}

/*
 * The same exchanges over TCP between the same hosts, for comparison: a
 * 64 byte ping-pong and a bulk transfer, with the CPU both hosts use.
 */
static void
t_tcp(peer_t *a, peer_t *b)
{
	const uint64_t n = 20000, bulk = 8ULL << 30;
	static char data[1 << 20];
	uint64_t *lat, i, sent, c0, c1, sum = 0;
	struct timespec t0, t1;
	double idle_a, idle_b;
	tcpreq_t tr;
	char msg[64];
	int s, ret;

	(void) a;
	if (b->p_sock < 0) {
		result(0, "tcp", "needs two hosts");
		return;
	}
	idle_a = idle_rate(NULL);
	idle_b = idle_rate(b);
	bzero(&tr, sizeof (tr));
	tr.tr_count = n;
	tr.tr_size = sizeof (msg);
	tr.tr_bulk = bulk;
	(void) rpc(b, C_TCP, &tr, 1, K_ACK);
	(void) usleep(200000);
	if ((s = xsock(o_server, o_port + 1)) < 0) {
		result(0, "tcp", "connect: %s", strerror(errno));
		return;
	}
	lat = calloc(n, sizeof (uint64_t));
	bzero(msg, sizeof (msg));
	for (i = 0; i < n; i++) {
		(void) clock_gettime(CLOCK_MONOTONIC, &t0);
		if (xfer(s, msg, sizeof (msg), 1) != 0 ||
		    xfer(s, msg, sizeof (msg), 0) != 0)
			break;
		(void) clock_gettime(CLOCK_MONOTONIC, &t1);
		lat[i] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
		    (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
		sum += lat[i];
	}
	c0 = cpu_busy_ns();
	(void) clock_gettime(CLOCK_MONOTONIC, &t0);
	for (sent = 0; i == n && sent < bulk; sent += sizeof (data)) {
		if (xfer(s, data, sizeof (data), 1) != 0)
			break;
	}
	/* The server answers once it has every byte. */
	ret = rpc(b, C_TCP, &tr, 1, K_RESULT);
	(void) clock_gettime(CLOCK_MONOTONIC, &t1);
	c1 = cpu_busy_ns();
	(void) close(s);
	if (i != n || sent < bulk || ret != 0) {
		result(0, "tcp", "round trips %llu of %llu, bulk %llu of %llu",
		    (u_longlong_t)i, (u_longlong_t)n, (u_longlong_t)sent,
		    (u_longlong_t)bulk);
		free(lat);
		return;
	}
	qsort(lat, n, sizeof (uint64_t), u64cmp);
	result(1, "tcp", "64 B round trip over %llu: min %.1f us, p50 %.1f "
	    "us, avg %.1f us, p99 %.1f us", (u_longlong_t)n, lat[0] / 1e3,
	    lat[n / 2] / 1e3, sum / 1e3 / n, lat[n * 99 / 100] / 1e3);
	{
		double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 +
		    (t1.tv_nsec - t0.tv_nsec);

		result(1, "tcp", "bulk %llu GB in 1 MB writes: %.2f Gb/s, "
		    "%.3f CPU-s per GB on A, %.3f on B (idle %.2f and %.2f "
		    "subtracted)", (u_longlong_t)(bulk >> 30),
		    (double)bulk * 8 / ns,
		    cpu_per_gb(c1 - c0, idle_a, (uint64_t)ns, bulk),
		    cpu_per_gb(tr.tr_srv_cpu_ns, idle_b, tr.tr_srv_ns, bulk),
		    idle_a, idle_b);
	}
	free(lat);
}

/* The server side of t_tcp(), on a second port. */
static int
serve_tcp(tcpreq_t *tr)
{
	static char data[1 << 20];
	struct sockaddr_in sin;
	struct timespec t0, t1;
	uint64_t i, got, c0;
	char msg[256];
	int l, s, one = 1;
	ssize_t r;

	if (tr->tr_size == 0 || tr->tr_size > sizeof (msg) ||
	    tr->tr_count > 10000000 || tr->tr_bulk > (1ULL << 40))
		return (EINVAL);
	if ((l = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return (errno);
	(void) setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
	bzero(&sin, sizeof (sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)(o_port + 1));
	sin.sin_addr.s_addr = o_ip;
	if (bind(l, (struct sockaddr *)&sin, sizeof (sin)) != 0 ||
	    listen(l, 1) != 0 || (s = accept(l, NULL, NULL)) < 0) {
		(void) close(l);
		return (EIO);
	}
	(void) close(l);
	(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof (one));
	for (i = 0; i < tr->tr_count; i++) {
		if (xfer(s, msg, tr->tr_size, 0) != 0 ||
		    xfer(s, msg, tr->tr_size, 1) != 0) {
			(void) close(s);
			return (EIO);
		}
	}
	c0 = 0;
	for (got = 0; got < tr->tr_bulk; got += (uint64_t)r) {
		r = read(s, data, sizeof (data));
		if (r <= 0)
			break;
		if (got == 0) {
			c0 = cpu_busy_ns();
			(void) clock_gettime(CLOCK_MONOTONIC, &t0);
		}
	}
	(void) clock_gettime(CLOCK_MONOTONIC, &t1);
	tr->tr_srv_cpu_ns = cpu_busy_ns() - c0;
	tr->tr_srv_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
	    (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
	(void) close(s);
	return (got == tr->tr_bulk ? 0 : EIO);
}

static void
run_tests(peer_t *a, peer_t *b, int argc, char **argv)
{
	static const char *all[] = { "send", "write", "read", "frwr",
	    "localinv", "badkey", "zerokey", "bounds", "access", "qpaccess",
	    "ud", "pingpong", "bw", "inflight", NULL };
	const char **list = (const char **)argv;
	int i, n = argc;

	if (n == 0) {
		list = all;
		for (n = 0; all[n] != NULL; n++)
			;
	}
	for (i = 0; i < n; i++) {
		const char *t = list[i];

		if (strcmp(t, "send") == 0)
			t_send(a, b);
		else if (strcmp(t, "write") == 0)
			t_rw(a, b, RDMAT_OP_WRITE);
		else if (strcmp(t, "read") == 0)
			t_rw(a, b, RDMAT_OP_READ);
		else if (strcmp(t, "frwr") == 0)
			t_frwr(a, b);
		else if (strcmp(t, "localinv") == 0)
			t_localinv(a, b);
		else if (strcmp(t, "badkey") == 0 ||
		    strcmp(t, "zerokey") == 0 || strcmp(t, "bounds") == 0 ||
		    strcmp(t, "access") == 0)
			t_reject(a, b, t);
		else if (strcmp(t, "qpaccess") == 0)
			t_qpaccess(a, b);
		else if (strcmp(t, "ud") == 0)
			t_ud(a, b);
		else if (strcmp(t, "pingpong") == 0)
			t_pingpong(a, b);
		else if (strcmp(t, "bw") == 0)
			t_bw(a, b);
		else if (strcmp(t, "inflight") == 0)
			t_inflight(a, b);
		else if (strcmp(t, "tcp") == 0)
			t_tcp(a, b);
		else
			fatal("unknown test %s", t);
	}
}

static rdmat_devinfo_t *
find_dev(rdmat_devices_t *d)
{
	uint32_t i;

	for (i = 0; i < d->rdd_count; i++) {
		if (o_dev == NULL ||
		    strcmp(o_dev, d->rdd_devs[i].rdi_name) == 0)
			return (&d->rdd_devs[i]);
	}
	return (NULL);
}

rdmat_devinfo_t local_dev;

static void
load_devices(void)
{
	rdmat_devices_t d;
	rdmat_devinfo_t *di;
	int fd = open_session();

	bzero(&d, sizeof (d));
	if (ioctl(fd, RDMAT_IOC_DEVICES, &d) != 0)
		fatal("devices: %s", strerror(errno));
	(void) close(fd);
	if ((di = find_dev(&d)) == NULL)
		fatal("no RDMA device%s%s", o_dev != NULL ? " " : "",
		    o_dev != NULL ? o_dev : "");
	local_dev = *di;
	o_dev = local_dev.rdi_name;
}

static void
serve(int s)
{
	union {
		rdmat_setup_t a;
		rdmat_connect_t b;
		rdmat_run_t c;
		rdmat_buf_t d;
		rdmat_query_t e;
		rdmat_devices_t f;
		tcpreq_t g;
		uint64_t h;
		host_stats_t i;
	} u;
	int fd = open_session();
	msg_t m;
	size_t len;

	for (;;) {
		if (xfer(s, &m, sizeof (m), 0) != 0 || m.m_magic != MAGIC)
			break;
		len = cmd_len(m.m_cmd);
		if (len == (size_t)-1 || m.m_len != len ||
		    xfer(s, &u, len, 0) != 0)
			break;
		if (m.m_cmd == C_BYE)
			break;
		if ((m.m_cmd == RDMAT_IOC_RUN || m.m_cmd == C_TCP) &&
		    m.m_err != 0) {
			msg_t ack = { MAGIC, K_ACK, 0, 0 };

			if (xfer(s, &ack, sizeof (ack), 1) != 0)
				break;
		}
		if (m.m_cmd == C_FRESH) {
			(void) close(fd);
			fd = open_session();
			m.m_err = 0;
		} else if (m.m_cmd == C_CPU) {
			u.h = cpu_busy_ns();
			m.m_err = 0;
		} else if (m.m_cmd == C_STATS) {
			host_stats(&u.i);
			m.m_err = 0;
		} else if (m.m_cmd == C_TCP) {
			m.m_err = serve_tcp(&u.g);
		} else {
			if (m.m_cmd == RDMAT_IOC_SETUP) {
				(void) strlcpy(u.a.rs_dev, o_dev,
				    sizeof (u.a.rs_dev));
				u.a.rs_ipv4 = o_ip;
			}
			m.m_err = ioctl(fd, m.m_cmd, &u) == 0 ? 0 : errno;
		}
		m.m_cmd = K_RESULT;
		if (xfer(s, &m, sizeof (m), 1) != 0 ||
		    (len != 0 && xfer(s, &u, len, 1) != 0))
			break;
	}
	(void) close(fd);
}

static int
listen_on(void)
{
	struct sockaddr_in sin;
	int s, one = 1;

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		fatal("socket: %s", strerror(errno));
	(void) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
	bzero(&sin, sizeof (sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)o_port);
	sin.sin_addr.s_addr = o_ip;
	if (bind(s, (struct sockaddr *)&sin, sizeof (sin)) != 0 ||
	    listen(s, 64) != 0)
		fatal("listen on port %d: %s", o_port, strerror(errno));
	return (s);
}

int
connect_to(const char *host)
{
	struct sockaddr_in sin;
	int s, one = 1, i;

	bzero(&sin, sizeof (sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)o_port);
	if (inet_pton(AF_INET, host, &sin.sin_addr) != 1)
		fatal("bad address %s", host);
	for (i = 0; i < 50; i++) {
		if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
			fatal("socket: %s", strerror(errno));
		if (connect(s, (struct sockaddr *)&sin, sizeof (sin)) == 0) {
			(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one,
			    sizeof (one));
			return (s);
		}
		(void) close(s);
		(void) usleep(200000);
	}
	fatal("connect to %s port %d: %s", host, o_port, strerror(errno));
	return (-1);
}

static void
usage(void)
{
	(void) fprintf(stderr, "usage: rdmatool [-d dev] [-i ipv4] [-p port] "
	    "[-b MB] [-q depth] [-t secs]\n"
	    "\t{info | loop [test...] | server | client host [test...] |\n"
	    "\tbench {loop | host} test [key=value...]}\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	peer_t a, b;
	int c, s;

	while ((c = getopt(argc, argv, "d:i:p:b:q:t:")) != -1) {
		switch (c) {
		case 'd':
			o_dev = optarg;
			break;
		case 'i':
			if (inet_pton(AF_INET, optarg, &o_ip) != 1)
				fatal("bad address %s", optarg);
			break;
		case 'p':
			o_port = atoi(optarg);
			break;
		case 'b':
			o_buf_mb = strtoull(optarg, NULL, 10);
			break;
		case 'q':
			o_depth = (uint32_t)atoi(optarg);
			break;
		case 't':
			o_secs = atoi(optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		usage();
	(void) signal(SIGPIPE, SIG_IGN);
	load_devices();

	if (strcmp(argv[0], "info") == 0) {
		(void) printf("%s port state %u active MTU %u (link %u) speed "
		    "%llu MAC %02x:%02x:%02x:%02x:%02x:%02x max QP %u max WR "
		    "%u max SGE %u max MR pages %u\n", local_dev.rdi_name,
		    local_dev.rdi_port_state, local_dev.rdi_active_mtu,
		    local_dev.rdi_phys_mtu,
		    (u_longlong_t)local_dev.rdi_speed, local_dev.rdi_mac[0],
		    local_dev.rdi_mac[1], local_dev.rdi_mac[2],
		    local_dev.rdi_mac[3], local_dev.rdi_mac[4],
		    local_dev.rdi_mac[5], local_dev.rdi_max_qp,
		    local_dev.rdi_max_qp_wr, local_dev.rdi_max_sge,
		    local_dev.rdi_max_mr_pages);
		return (0);
	}
	if (o_ip == 0)
		fatal("-i is required");
	path_mtu = local_dev.rdi_active_mtu > 4096 ? 4096 :
	    local_dev.rdi_active_mtu;

	bzero(&a, sizeof (a));
	bzero(&b, sizeof (b));
	a.p_name = "A";
	b.p_name = "B";
	a.p_sock = b.p_sock = -1;
	a.p_fd = open_session();

	if (strcmp(argv[0], "loop") == 0) {
		b.p_fd = open_session();
		run_tests(&a, &b, argc - 1, argv + 1);
	} else if (strcmp(argv[0], "server") == 0) {
		int l = listen_on();

		/* A child per connection, so a benchmark's QPs run at once. */
		(void) close(a.p_fd);
		(void) signal(SIGCHLD, SIG_IGN);
		for (;;) {
			if ((s = accept(l, NULL, NULL)) < 0)
				continue;
			if (fork() == 0) {
				(void) close(l);
				serve(s);
				_exit(0);
			}
			(void) close(s);
		}
	} else if (strcmp(argv[0], "bench") == 0) {
		(void) close(a.p_fd);
		return (bench_main(&a, &b, argc - 1, argv + 1));
	} else if (strcmp(argv[0], "client") == 0) {
		rdmat_devices_t d;
		rdmat_devinfo_t *di;

		if (argc < 2)
			usage();
		o_server = argv[1];
		b.p_sock = connect_to(argv[1]);
		bzero(&d, sizeof (d));
		if (rpc(&b, RDMAT_IOC_DEVICES, &d, 0, K_RESULT) != 0 ||
		    d.rdd_count == 0)
			fatal("the server has no RDMA device");
		/* The server uses its own -d; take its first device's MTU. */
		di = &d.rdd_devs[0];
		if (di->rdi_active_mtu < path_mtu)
			path_mtu = di->rdi_active_mtu;
		run_tests(&a, &b, argc - 2, argv + 2);
		(void) rpc(&b, C_BYE, NULL, 0, K_ACK);
	} else {
		usage();
	}
	(void) printf("=== %d failed, path MTU %u, device %s\n", failures,
	    path_mtu, o_dev);
	return (failures == 0 ? 0 : 1);
}

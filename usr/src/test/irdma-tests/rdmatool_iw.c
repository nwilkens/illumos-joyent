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
 * rdmatool over the rdmak connection manager (-w).  Side B listens with an
 * allow-list naming side A only, and side A connects; the QPs exchange their
 * buffer addresses and rkeys as private data.  The iWARP tests:
 *
 *	iwreject	a listener refuses a peer not on its allow-list
 *	iwports		host TCP port ownership against rdk_cm reservations
 *	iwcycle		connect/teardown cycles under light traffic
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "rdmatool.h"

#define	IW_PORT_BASE	1000
#define	IW_PORT_SPAN	2000
#define	IW_CM_MS	10000
/* An address no host answers for: TEST-NET-1. */
#define	IW_STRANGER	"192.0.2.77"

static uint_t iw_seq;
static int iw_no_pair;
static uint16_t iw_last_bound;

static uint16_t
iw_port(void)
{
	return (htons((uint16_t)(o_port + IW_PORT_BASE +
	    (iw_seq++ % IW_PORT_SPAN))));
}

static void
iw_cm_init(rdmat_cm_t *c, uint32_t op, uint32_t qp)
{
	bzero(c, sizeof (*c));
	c->rcm_op = op;
	c->rcm_qp = qp;
	c->rcm_timeout_ms = IW_CM_MS;
}

/* B listens for A; returns B's address in *baddr and the slot in *slot. */
static int
iw_listen(peer_t *b, uint16_t port, uint32_t peer, uint32_t flags,
    uint32_t *baddr, uint32_t *slot)
{
	rdmat_cm_t c;
	int ret;

	iw_cm_init(&c, RDMAT_CM_LISTEN, 0);
	c.rcm_laddr = b->p_sock >= 0 ? 0 : o_ip;
	c.rcm_lport = port;
	c.rcm_backlog = 8;
	c.rcm_npeers = 1;
	c.rcm_peers[0] = peer;
	c.rcm_flags = flags;
	if ((ret = pio(b, RDMAT_IOC_CM, &c)) != 0) {
		(void) fprintf(stderr, "%s: listen: %s\n", b->p_name,
		    strerror(ret));
		return (ret);
	}
	*baddr = c.rcm_laddr;
	if (slot != NULL)
		*slot = c.rcm_qp;
	return (0);
}

/* Connect A's QP 0 to B's through the connection manager. */
int
iw_pair(peer_t *a, peer_t *b)
{
	const uint16_t port = iw_port();
	rdmat_cm_t c;
	uint32_t baddr;
	int ret;

	if (iw_no_pair)
		return (0);
	if ((ret = iw_listen(b, port, o_ip, 0, &baddr, NULL)) != 0)
		return (ret);
	iw_cm_init(&c, RDMAT_CM_CONNECT, 0);
	c.rcm_laddr = o_ip;
	c.rcm_raddr = baddr;
	c.rcm_rport = port;
	if ((ret = pio(a, RDMAT_IOC_CM, &c)) != 0) {
		(void) fprintf(stderr, "%s: connect: %s (event %u status "
		    "%d)\n", a->p_name, strerror(ret), c.rcm_event,
		    c.rcm_status);
		return (ret);
	}
	iw_last_bound = c.rcm_bound;
	if (c.rcm_peer.rqi_rkey != b->p_setup.rs_qp[0].rqi_rkey ||
	    c.rcm_peer.rqi_addr != b->p_setup.rs_qp[0].rqi_addr) {
		(void) fprintf(stderr, "%s: the peer's private data does not "
		    "match its setup\n", a->p_name);
		return (EPROTO);
	}
	iw_cm_init(&c, RDMAT_CM_ACCEPT, 0);
	if ((ret = pio(b, RDMAT_IOC_CM, &c)) != 0) {
		(void) fprintf(stderr, "%s: accept: %s (event %u status "
		    "%d)\n", b->p_name, strerror(ret), c.rcm_event,
		    c.rcm_status);
		return (ret);
	}
	if (c.rcm_peer.rqi_rkey != a->p_setup.rs_qp[0].rqi_rkey)
		return (EPROTO);
	return (0);
}

/* A peer not on the allow-list gets no connection, and B never sees it. */
static void
t_iwreject(peer_t *a, peer_t *b)
{
	struct in_addr stranger;
	rdmat_cm_t c;
	uint32_t baddr, slot, ev;
	uint16_t port = iw_port();
	uint64_t t0, ms;
	int ret, ok, st;

	iw_no_pair = 1;
	ret = fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ);
	iw_no_pair = 0;
	(void) inet_pton(AF_INET, IW_STRANGER, &stranger);
	if (ret != 0 || iw_listen(b, port, stranger.s_addr, 0, &baddr,
	    &slot) != 0) {
		result(0, "iwreject", "setup failed");
		return;
	}
	iw_cm_init(&c, RDMAT_CM_CONNECT, 0);
	c.rcm_laddr = o_ip;
	c.rcm_raddr = baddr;
	c.rcm_rport = port;
	c.rcm_timeout_ms = 4000;
	t0 = now_ns();
	ret = pio(a, RDMAT_IOC_CM, &c);
	ok = ret != 0;
	ev = c.rcm_event;
	st = c.rcm_status;
	ms = (now_ns() - t0) / 1000000;

	iw_cm_init(&c, RDMAT_CM_STATUS, slot);
	if (pio(b, RDMAT_IOC_CM, &c) != 0 || c.rcm_reqs != 0)
		ok = 0;
	result(ok, "iwreject", "connect from a peer off the allow-list: %s "
	    "(event %u status %d after %llu ms); requests B's consumer saw: %u",
	    ret == 0 ? "connected" : strerror(ret), ev, st,
	    (unsigned long long)ms, c.rcm_reqs);
}

/* bind(2) the address and port, as a host application would. */
static int
host_bind(uint32_t addr, uint16_t port, int reuse, int listening, int *fdp)
{
	struct sockaddr_in sin;
	int s, one = 1, err = 0;

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return (errno);
	if (reuse)
		(void) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one,
		    sizeof (one));
	bzero(&sin, sizeof (sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = addr;
	sin.sin_port = port;
	if (bind(s, (struct sockaddr *)&sin, sizeof (sin)) != 0 ||
	    (listening && listen(s, 1) != 0))
		err = errno;
	if (err != 0 || fdp == NULL)
		(void) close(s);
	else
		*fdp = s;
	return (err);
}

static int
cm_listen_local(peer_t *a, uint32_t addr, uint16_t port, uint32_t *slot)
{
	rdmat_cm_t c;
	int ret;

	iw_cm_init(&c, RDMAT_CM_LISTEN, 0);
	c.rcm_laddr = addr;
	c.rcm_lport = port;
	c.rcm_backlog = 1;
	c.rcm_npeers = 1;
	(void) inet_pton(AF_INET, IW_STRANGER, &c.rcm_peers[0]);
	ret = pio(a, RDMAT_IOC_CM, &c);
	if (slot != NULL)
		*slot = c.rcm_qp;
	return (ret);
}

/* Host sockets and rdk_cm never share a port. */
static void
t_iwports(peer_t *a, peer_t *b)
{
	const uint16_t p1 = iw_port(), p2 = iw_port();
	rdmat_cm_t c;
	uint32_t slot;
	int hfd = -1, r1, r2, r3, r4, r5, r6, r7, r8;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0) {
		result(0, "iwports", "setup failed");
		return;
	}
	/* 1: a port a host listener owns cannot be offloaded. */
	r1 = host_bind(o_ip, p1, 0, 1, &hfd);
	r2 = cm_listen_local(a, o_ip, p1, NULL);
	if (hfd >= 0)
		(void) close(hfd);
	result(r1 == 0 && r2 == EADDRINUSE, "iwports", "host listener on %u, "
	    "then rdk_cm listen: %s", ntohs(p1), strerror(r2));

	/* 2: an offloaded port refuses host binds, SO_REUSEADDR or not. */
	r3 = cm_listen_local(a, o_ip, p2, &slot);
	r4 = host_bind(o_ip, p2, 0, 0, NULL);
	r5 = host_bind(o_ip, p2, 1, 0, NULL);
	r6 = host_bind(INADDR_ANY, p2, 1, 1, NULL);
	result(r3 == 0 && r4 == EADDRINUSE && r5 == EADDRINUSE &&
	    r6 == EADDRINUSE, "iwports", "rdk_cm listen on %u: %s; host bind "
	    "%s, with SO_REUSEADDR %s, wildcard %s", ntohs(p2), strerror(r3),
	    strerror(r4), strerror(r5), strerror(r6));

	/* 3: the port stays reserved after the listener goes. */
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(a, RDMAT_IOC_CM, &c);
	r7 = host_bind(o_ip, p2, 1, 0, NULL);
	result(r7 == EADDRINUSE, "iwports", "host bind after the listener "
	    "closed: %s (TIME_WAIT hold)", strerror(r7));

	/* 4: no wildcard listener, and the active side's port is held. */
	r8 = cm_listen_local(a, INADDR_ANY, iw_port(), NULL);
	r1 = iw_last_bound != 0 ? host_bind(o_ip, iw_last_bound, 1, 0, NULL) :
	    -1;
	result(r8 != 0 && r1 == EADDRINUSE, "iwports", "wildcard listen: %s; "
	    "host bind on the connection's port %u: %s", strerror(r8),
	    ntohs(iw_last_bound), r1 < 0 ? "no port" : strerror(r1));
}

typedef struct iw_load {
	peer_t		*il_peer;
	peer_t		*il_b;
	volatile int	il_stop;
	uint64_t	il_runs;
	uint64_t	il_bytes;
	int		il_err;
} iw_load_t;

/* Light traffic: bursts of 4 KB writes, 20 a second. */
static void *
iw_load_thr(void *arg)
{
	iw_load_t *il = arg;
	rdmat_run_t rr;
	int ret;

	while (!il->il_stop) {
		run_init(&rr, RDMAT_OP_WRITE, 4096, 64);
		rr.rr_depth = 8;
		rr.rr_raddr = il->il_b->p_setup.rs_qp[0].rqi_addr;
		rr.rr_rkey = il->il_b->p_setup.rs_qp[0].rqi_rkey;
		rr.rr_rlen = il->il_b->p_setup.rs_qp[0].rqi_len;
		if ((ret = run(il->il_peer, &rr)) != 0) {
			il->il_err = ret;
			break;
		}
		il->il_runs++;
		il->il_bytes += rr.rr_bytes;
		(void) usleep(50000);
	}
	return (NULL);
}

static void
t_iwcycle(peer_t *a, peer_t *b, uint32_t n)
{
	const uint16_t port = iw_port();
	rdmat_cm_t c;
	peer_t a2;
	iw_load_t il;
	pthread_t thr;
	uint32_t baddr, slot;
	int ret;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0 ||
	    iw_listen(b, port, o_ip, RDMAT_CM_AUTO, &baddr, &slot) != 0) {
		result(0, "iwcycle", "setup failed");
		return;
	}
	bzero(&a2, sizeof (a2));
	a2.p_name = "A2";
	a2.p_sock = -1;
	a2.p_fd = open_session();
	a2.p_setup = a->p_setup;
	a2.p_setup.rs_nqp = 1;
	a2.p_setup.rs_buf_len = 1 << 20;
	a2.p_setup.rs_depth = 8;
	if ((ret = pio(&a2, RDMAT_IOC_SETUP, &a2.p_setup)) != 0) {
		result(0, "iwcycle", "second session: %s", strerror(ret));
		(void) close(a2.p_fd);
		return;
	}

	bzero(&il, sizeof (il));
	il.il_peer = a;
	il.il_b = b;
	if (pthread_create(&thr, NULL, iw_load_thr, &il) != 0)
		fatal("pthread_create failed");

	iw_cm_init(&c, RDMAT_CM_CYCLE, 0);
	c.rcm_laddr = o_ip;
	c.rcm_raddr = baddr;
	c.rcm_rport = port;
	c.rcm_count = n;
	ret = pio(&a2, RDMAT_IOC_CM, &c);
	il.il_stop = 1;
	(void) pthread_join(thr, NULL);
	(void) close(a2.p_fd);

	result(ret == 0 && c.rcm_done == n && il.il_err == 0, "iwcycle",
	    "%u of %u connect/teardown cycles in %.1f s (%.1f ms each), "
	    "last event %u status %d; background writes: %llu runs, %llu "
	    "bytes, %s", c.rcm_done, n, c.rcm_ns / 1e9,
	    c.rcm_done != 0 ? c.rcm_ns / 1e6 / c.rcm_done : 0.0, c.rcm_event,
	    c.rcm_status, (unsigned long long)il.il_runs,
	    (unsigned long long)il.il_bytes, il.il_err == 0 ? "no errors" :
	    strerror(il.il_err));

	/* B freed every automatic QP once its connection ended. */
	(void) usleep(500000);
	iw_cm_init(&c, RDMAT_CM_STATUS, slot);
	if (pio(b, RDMAT_IOC_CM, &c) == 0) {
		result(c.rcm_live == 0 && c.rcm_accepts == n, "iwcycle",
		    "B: %u requests, %u accepted, %u rejected, %u still "
		    "live", c.rcm_reqs, c.rcm_accepts, c.rcm_rejects,
		    c.rcm_live);
	}
}

/* Returns nonzero for a name that is not an iWARP test. */
int
iw_test(peer_t *a, peer_t *b, const char *t)
{
	if (strcmp(t, "iwreject") == 0)
		t_iwreject(a, b);
	else if (strcmp(t, "iwports") == 0)
		t_iwports(a, b);
	else if (strncmp(t, "iwcycle", 7) == 0)
		t_iwcycle(a, b, t[7] == '=' ? (uint32_t)atoi(t + 8) : 1000);
	else
		return (-1);
	return (0);
}

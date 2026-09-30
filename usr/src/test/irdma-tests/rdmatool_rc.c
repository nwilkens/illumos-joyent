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
 * rdmatool's RoCE connection manager tests (-w).  With -I, side B has a
 * second address of the same port, so the MADs and the RC traffic loop
 * through the device between two IPs.
 *
 *	rcerrors	no listener, an allow-list miss, a consumer reject,
 *			an unreachable address and a second listener
 *	rcports		the RoCE port space and host TCP ports do not meet
 *	rccycle[=n]	n connect/teardown cycles, with the time each
 *			connection took to come up and to go down
 *	rcgsi		the GSI agent's counters: nothing was dropped
 *	rcearly		a REJ sent before the way back is resolved reaches
 *			A (run with rdk_cm_roce_max_resolving set to 0)
 *	rcrejto		A gives up while B decides: A's REJ (Timeout, no
 *			remote ID) ends B's side at once
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <kstat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "rdmatool.h"

/* Nobody answers ARP for these: TEST-NET-1 and TEST-NET-2. */
#define	RC_STRANGER	"192.0.2.77"
#define	RC_NOHOST	"198.51.100.77"

/* IB CM REJ reasons (IBTA vol 1, table 104). */
#define	RC_REJ_INVALID_SID	8
#define	RC_REJ_CONSUMER		28

static int
rc_connect(peer_t *a, uint32_t raddr, uint16_t port, uint32_t ms,
    rdmat_cm_t *c)
{
	iw_cm_init(c, RDMAT_CM_CONNECT, 0);
	c->rcm_laddr = o_ip;
	c->rcm_raddr = raddr;
	c->rcm_rport = port;
	c->rcm_timeout_ms = ms;
	return (pio(a, RDMAT_IOC_CM, c));
}

static void
t_rcerrors(peer_t *a, peer_t *b)
{
	struct in_addr stranger, nohost;
	rdmat_cm_t c;
	uint32_t baddr = 0, slot = 0;
	uint16_t port;
	uint64_t t0, ms;
	int ret;

	iw_no_pair = 1;
	ret = fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ);
	iw_no_pair = 0;
	if (ret != 0) {
		result(0, "rcerrors", "setup failed");
		return;
	}
	(void) inet_pton(AF_INET, RC_STRANGER, &stranger);
	(void) inet_pton(AF_INET, RC_NOHOST, &nohost);

	/* 1: no listener: a REJ with an invalid service ID, at once. */
	port = iw_port();
	if (iw_listen(b, port, o_ip, RDMAT_CM_REJECT, &baddr, &slot) != 0) {
		result(0, "rcerrors", "listen failed");
		return;
	}
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(b, RDMAT_IOC_CM, &c);
	t0 = now_ns();
	ret = rc_connect(a, baddr, port, 5000, &c);
	ms = (now_ns() - t0) / 1000000;
	result(ret == ECONNREFUSED && c.rcm_reason == RC_REJ_INVALID_SID &&
	    ms < 1000, "rcerrors", "no listener: %s, event %u reason %u "
	    "after %llu ms", strerror(ret), c.rcm_event, c.rcm_reason,
	    (unsigned long long)ms);

	/* 2: a listener whose allow-list lacks A says nothing. */
	port = iw_port();
	if (iw_listen(b, port, stranger.s_addr, 0, &baddr, &slot) == 0) {
		t0 = now_ns();
		ret = rc_connect(a, baddr, port, 3000, &c);
		ms = (now_ns() - t0) / 1000000;
		iw_cm_init(&c, RDMAT_CM_STATUS, slot);
		(void) pio(b, RDMAT_IOC_CM, &c);
		result(ret != 0 && c.rcm_reqs == 0, "rcerrors", "allow-list "
		    "miss: %s after %llu ms; requests B saw: %u", strerror(ret),
		    (unsigned long long)ms, c.rcm_reqs);
		iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
		(void) pio(b, RDMAT_IOC_CM, &c);
	} else {
		result(0, "rcerrors", "allow-list listener failed");
	}

	/* 3: the consumer refuses: REJ, reason and private data. */
	port = iw_port();
	if (iw_listen(b, port, o_ip, RDMAT_CM_REJECT, &baddr, &slot) == 0) {
		ret = rc_connect(a, baddr, port, 5000, &c);
		result(ret == ECONNREFUSED && c.rcm_reason == RC_REJ_CONSUMER &&
		    c.rcm_rej_len == RDMAT_CM_REJ_LEN &&
		    bcmp(c.rcm_rej_data, RDMAT_CM_REJ_DATA,
		    RDMAT_CM_REJ_LEN) == 0, "rcerrors", "consumer reject: %s, "
		    "reason %u, %u bytes of private data %s", strerror(ret),
		    c.rcm_reason, c.rcm_rej_len, bcmp(c.rcm_rej_data,
		    RDMAT_CM_REJ_DATA, RDMAT_CM_REJ_LEN) == 0 ? "intact" :
		    "wrong");

		/* 4: a second listener on the same address and port. */
		iw_cm_init(&c, RDMAT_CM_LISTEN, 0);
		c.rcm_laddr = baddr;
		c.rcm_lport = port;
		c.rcm_backlog = 1;
		c.rcm_npeers = 1;
		c.rcm_peers[0] = o_ip;
		c.rcm_flags = RDMAT_CM_REJECT;
		ret = pio(b, RDMAT_IOC_CM, &c);
		result(ret == EADDRINUSE, "rcerrors", "second listener on "
		    "the port: %s", strerror(ret));
		iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
		(void) pio(b, RDMAT_IOC_CM, &c);
	} else {
		result(0, "rcerrors", "rejecting listener failed");
	}

	/* 5: an address no neighbor answers for. */
	if (o_ip2 != 0) {
		t0 = now_ns();
		ret = rc_connect(a, nohost.s_addr, iw_port(), 8000, &c);
		ms = (now_ns() - t0) / 1000000;
		result(ret != 0, "rcerrors", "unreachable %s: %s (event %u "
		    "status %d) after %llu ms", RC_NOHOST, strerror(ret),
		    c.rcm_event, c.rcm_status, (unsigned long long)ms);
	}
}

/* bind(2) the address and port as a host application would. */
static int
rc_host_bind(uint32_t addr, uint16_t port, int listening, int *fdp)
{
	struct sockaddr_in sin;
	int s, err = 0;

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return (errno);
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
rc_listen(peer_t *p, uint32_t addr, uint16_t port, uint32_t *slot)
{
	rdmat_cm_t c;
	int ret;

	iw_cm_init(&c, RDMAT_CM_LISTEN, 0);
	c.rcm_laddr = addr;
	c.rcm_lport = port;
	c.rcm_backlog = 1;
	c.rcm_npeers = 1;
	c.rcm_peers[0] = o_ip;
	c.rcm_flags = RDMAT_CM_REJECT;
	ret = pio(p, RDMAT_IOC_CM, &c);
	if (slot != NULL)
		*slot = c.rcm_qp;
	return (ret);
}

static void
t_rcports(peer_t *a, peer_t *b)
{
	const uint16_t p1 = iw_port(), p2 = iw_port();
	rdmat_cm_t c;
	uint32_t slot;
	int hfd = -1, r1, r2, r3, r4, r5;

	iw_no_pair = 1;
	r1 = fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ);
	iw_no_pair = 0;
	if (r1 != 0) {
		result(0, "rcports", "setup failed");
		return;
	}
	r1 = rc_host_bind(o_ip, p1, 1, &hfd);
	r2 = rc_listen(a, o_ip, p1, &slot);
	if (hfd >= 0)
		(void) close(hfd);
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(a, RDMAT_IOC_CM, &c);
	result(r1 == 0 && r2 == 0, "rcports", "host listener on %u, then a "
	    "RoCE listener: %s", ntohs(p1), strerror(r2));

	r3 = rc_listen(a, o_ip, p2, &slot);
	r4 = rc_host_bind(o_ip, p2, 0, NULL);
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(a, RDMAT_IOC_CM, &c);
	r5 = rc_listen(a, o_ip, p2, &slot);
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(a, RDMAT_IOC_CM, &c);
	result(r3 == 0 && r4 == 0 && r5 == 0, "rcports", "RoCE listener on "
	    "%u: %s; host bind %s; listening again after it went: %s",
	    ntohs(p2), strerror(r3), strerror(r4), strerror(r5));
}

static void
t_rccycle(peer_t *a, peer_t *b, uint32_t n)
{
	const uint16_t port = iw_port();
	rdmat_cm_t c;
	peer_t a2;
	uint32_t baddr, slot;
	int ret, i;

	if (fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ) != 0 ||
	    iw_listen(b, port, o_ip, RDMAT_CM_AUTO, &baddr, &slot) != 0) {
		result(0, "rccycle", "setup failed");
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
		result(0, "rccycle", "second session: %s", strerror(ret));
		(void) close(a2.p_fd);
		return;
	}
	iw_cm_init(&c, RDMAT_CM_CYCLE, 0);
	c.rcm_laddr = o_ip;
	c.rcm_raddr = baddr;
	c.rcm_rport = port;
	c.rcm_count = n;
	c.rcm_flags = RDMAT_CM_FAST;
	ret = pio(&a2, RDMAT_IOC_CM, &c);
	(void) close(a2.p_fd);
	result(ret == 0 && c.rcm_done == n, "rccycle", "%u of %u cycles in "
	    "%.2f s (%.0f/s): connect min %.0f us avg %.0f us max %.0f us, "
	    "disconnect avg %.0f us; last event %u status %d", c.rcm_done, n,
	    c.rcm_ns / 1e9, c.rcm_ns != 0 ? c.rcm_done * 1e9 / c.rcm_ns : 0,
	    c.rcm_conn_min_ns / 1e3, c.rcm_done != 0 ?
	    c.rcm_conn_ns / 1e3 / c.rcm_done : 0, c.rcm_conn_max_ns / 1e3,
	    c.rcm_done != 0 ? c.rcm_disc_ns / 1e3 / c.rcm_done : 0,
	    c.rcm_event, c.rcm_status);

	/* B frees each automatic QP when its timewait ends. */
	for (i = 0; i < 40; i++) {
		iw_cm_init(&c, RDMAT_CM_STATUS, slot);
		if (pio(b, RDMAT_IOC_CM, &c) != 0 || c.rcm_live == 0)
			break;
		(void) usleep(100000);
	}
	result(c.rcm_live == 0 && c.rcm_accepts == n, "rccycle", "B: %u "
	    "requests, %u accepted, %u rejected, %u still live", c.rcm_reqs,
	    c.rcm_accepts, c.rcm_rejects, c.rcm_live);
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(b, RDMAT_IOC_CM, &c);
}

static uint64_t
rc_kstat(kstat_ctl_t *kc, kstat_t *ks, const char *name)
{
	kstat_named_t *kn;

	if (kstat_read(kc, ks, NULL) < 0 ||
	    (kn = kstat_data_lookup(ks, (char *)name)) == NULL)
		return (0);
	return (kn->value.ui64);
}

/* The local GSI agents dropped nothing well formed, and sent nothing bad. */
static void
t_rcgsi(peer_t *a, peer_t *b)
{
	static const char *const bad[] = { "rx_rate_drop", "rx_queue_drop",
	    "rx_nomem", "tx_error", "tx_nobufs", NULL };
	kstat_ctl_t *kc;
	kstat_t *ks;
	char name[KSTAT_STRLEN];
	uint64_t v;
	int i, ok = 1, found = 0;

	(void) a;
	(void) b;
	if ((kc = kstat_open()) == NULL) {
		result(0, "rcgsi", "kstat_open: %s", strerror(errno));
		return;
	}
	(void) snprintf(name, sizeof (name), "%s_gsi1", o_dev);
	for (ks = kc->kc_chain; ks != NULL; ks = ks->ks_next) {
		if (strcmp(ks->ks_module, "rdmak") != 0 ||
		    strcmp(ks->ks_name, name) != 0)
			continue;
		found = 1;
		for (i = 0; bad[i] != NULL; i++) {
			if ((v = rc_kstat(kc, ks, bad[i])) != 0) {
				ok = 0;
				(void) printf("  %s %s %llu\n", name, bad[i],
				    (unsigned long long)v);
			}
		}
		result(ok, "rcgsi", "%s: rx %llu (bad %llu) tx %llu, AH hits "
		    "%llu creates %llu evictions %llu", name,
		    (unsigned long long)rc_kstat(kc, ks, "rx"),
		    (unsigned long long)rc_kstat(kc, ks, "rx_bad"),
		    (unsigned long long)rc_kstat(kc, ks, "tx"),
		    (unsigned long long)rc_kstat(kc, ks, "ah_hit"),
		    (unsigned long long)rc_kstat(kc, ks, "ah_create"),
		    (unsigned long long)rc_kstat(kc, ks, "ah_evict"));
	}
	if (!found)
		result(0, "rcgsi", "no kstat rdmak:0:%s", name);
	(void) kstat_close(kc);
}

static void
t_rcearly(peer_t *a, peer_t *b)
{
	const uint16_t port = iw_port();
	rdmat_cm_t c;
	uint32_t baddr, slot;
	uint64_t t0, ms;
	int ret;

	iw_no_pair = 1;
	ret = fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ);
	iw_no_pair = 0;
	if (ret != 0 || iw_listen(b, port, o_ip, RDMAT_CM_AUTO, &baddr,
	    &slot) != 0) {
		result(0, "rcearly", "setup failed");
		return;
	}
	t0 = now_ns();
	ret = rc_connect(a, baddr, port, 5000, &c);
	ms = (now_ns() - t0) / 1000000;
	result(ret == ECONNREFUSED && c.rcm_reason == RC_REJ_CONSUMER &&
	    ms < 1000, "rcearly", "resolution refused: %s, reason %u after "
	    "%llu ms", strerror(ret), c.rcm_reason, (unsigned long long)ms);
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(b, RDMAT_IOC_CM, &c);
}

static void
t_rcrejto(peer_t *a, peer_t *b)
{
	const uint16_t port = iw_port();
	rdmat_cm_t c;
	uint32_t baddr, slot;
	int ret, i;

	iw_no_pair = 1;
	ret = fresh(a, b, RDMAT_QPT_RC, RDMAT_POLL_TASKQ);
	iw_no_pair = 0;
	if (ret != 0 || iw_listen(b, port, o_ip, RDMAT_CM_AUTO |
	    RDMAT_CM_SLOW, &baddr, &slot) != 0) {
		result(0, "rcrejto", "setup failed");
		return;
	}
	ret = rc_connect(a, baddr, port, 100, &c);
	/* B decides 500 ms after the request; give it a second more. */
	for (i = 0; i < 15; i++) {
		(void) usleep(100000);
		iw_cm_init(&c, RDMAT_CM_STATUS, slot);
		if (pio(b, RDMAT_IOC_CM, &c) != 0 || c.rcm_rejects != 0)
			break;
	}
	result(ret != 0 && c.rcm_reqs == 1 && c.rcm_accepts == 0 &&
	    c.rcm_live == 0, "rcrejto", "A gave up (%s); B: %u requests, "
	    "%u accepted, %u refused, %u still live", strerror(ret),
	    c.rcm_reqs, c.rcm_accepts, c.rcm_rejects, c.rcm_live);
	iw_cm_init(&c, RDMAT_CM_UNLISTEN, slot);
	(void) pio(b, RDMAT_IOC_CM, &c);
}

/* Returns nonzero for a name that is not a RoCE CM test. */
int
rc_test(peer_t *a, peer_t *b, const char *t)
{
	if (strcmp(t, "rcerrors") == 0)
		t_rcerrors(a, b);
	else if (strcmp(t, "rcports") == 0)
		t_rcports(a, b);
	else if (strncmp(t, "rccycle", 7) == 0)
		t_rccycle(a, b, t[7] == '=' ? (uint32_t)atoi(t + 8) : 1000);
	else if (strcmp(t, "rcgsi") == 0)
		t_rcgsi(a, b);
	else if (strcmp(t, "rcearly") == 0)
		t_rcearly(a, b);
	else if (strcmp(t, "rcrejto") == 0)
		t_rcrejto(a, b);
	else
		return (-1);
	return (0);
}

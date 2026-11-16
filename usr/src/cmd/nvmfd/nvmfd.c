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
 * Provenance: ported to illumos from FreeBSD
 * tools/tools/nvmf/nvmfd/nvmfd.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * The NVMe over Fabrics target daemon.  It listens on the discovery and I/O
 * ports, runs each connection's Connect exchange in a thread of its own, and
 * hands I/O port queue pairs to the nvmft port provider.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <err.h>
#include <errno.h>
#include <libnvmf.h>
#include <netdb.h>
#include <poll.h>
#include <priv.h>
#include <priv_utils.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

/* Connections that have not finished Connect, in all and from one address. */
#define	NVMFD_MAX_HANDSHAKES		256
#define	NVMFD_MAX_HANDSHAKES_PER_HOST	32

/* Discovery sessions, in all and from one address. */
#define	NVMFD_MAX_SESSIONS		64
#define	NVMFD_MAX_SESSIONS_PER_HOST	4

/* Messages about peers logged in any one window; the rest are counted. */
#define	NVMFD_PEER_LOG_BURST		20
#define	NVMFD_PEER_LOG_WINDOW		10	/* seconds */

/* The daemon user and group. */
#define	NVMFD_UID			1
#define	NVMFD_GID			12

bool data_digests = false;
bool header_digests = false;
bool flow_control_disable = false;
uint32_t maxh2cdata = 256 * 1024;

static bool debug;
static volatile sig_atomic_t quit;

struct listen_socket {
	int		ls_fd;
	enum listen_kind ls_kind;
};

static struct listen_socket *listen_sockets;
static nfds_t num_listen_sockets;

static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;
static nvmfd_conn_t *conns;
static uint_t num_handshakes;
static uint_t num_sessions;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static hrtime_t log_window;
static uint_t log_count;
static uint_t log_dropped;

static void
usage(void)
{
	(void) fprintf(stderr,
	    "usage: nvmfd [-dFfGg] [-a address]... [-H maxh2cdata] "
	    "[-P discovery_port]\n"
	    "           [-p io_port] [-t tcp]\n"
	    "\n"
	    "nvmfd does not authenticate hosts and does not encrypt.  Any "
	    "host that can\n"
	    "reach its ports can connect; restrict them with -a and with "
	    "STMF host groups.\n");
	exit(2);
}

void
nvmfd_log(int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (debug) {
		(void) vfprintf(stderr, fmt, ap);
		(void) fputc('\n', stderr);
	} else {
		vsyslog(pri, fmt, ap);
	}
	va_end(ap);
}

/*
 * Log something a peer caused.  A peer can cause as many of these as it
 * likes, so only the first few in each window are written.
 */
void
nvmfd_peer_log(const nvmfd_conn_t *conn, const char *fmt, ...)
{
	char msg[512];
	hrtime_t now = gethrtime();
	uint_t dropped = 0;
	va_list ap;
	bool emit;

	(void) pthread_mutex_lock(&log_lock);
	if (now - log_window >= (hrtime_t)NVMFD_PEER_LOG_WINDOW * NANOSEC) {
		dropped = log_dropped;
		log_dropped = 0;
		log_count = 0;
		log_window = now;
	}
	emit = log_count < NVMFD_PEER_LOG_BURST;
	if (emit)
		log_count++;
	else
		log_dropped++;
	(void) pthread_mutex_unlock(&log_lock);

	if (dropped != 0) {
		nvmfd_log(LOG_NOTICE, "%u messages about peers were not logged",
		    dropped);
	}
	if (!emit)
		return;

	va_start(ap, fmt);
	(void) vsnprintf(msg, sizeof (msg), fmt, ap);
	va_end(ap);
	nvmfd_log(LOG_NOTICE, "%s: %s", conn->nc_name, msg);
}

static bool
same_host(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
	if (a->ss_family != b->ss_family)
		return (false);

	switch (a->ss_family) {
	case AF_INET:
		return (((const struct sockaddr_in *)a)->sin_addr.s_addr ==
		    ((const struct sockaddr_in *)b)->sin_addr.s_addr);
	case AF_INET6:
		return (IN6_ARE_ADDR_EQUAL(
		    &((const struct sockaddr_in6 *)a)->sin6_addr,
		    &((const struct sockaddr_in6 *)b)->sin6_addr));
	default:
		return (false);
	}
}

/* Count the connections from conn's address.  Call with conn_lock held. */
static uint_t
count_from_host(const nvmfd_conn_t *conn, bool sessions)
{
	const nvmfd_conn_t *c;
	uint_t n = 0;

	for (c = conns; c != NULL; c = c->nc_next) {
		if (c->nc_session == sessions &&
		    same_host(&c->nc_peer, &conn->nc_peer))
			n++;
	}
	return (n);
}

/* Admit a new connection, or return NULL with the socket still open. */
static nvmfd_conn_t *
conn_begin(int s, enum listen_kind kind)
{
	nvmfd_conn_t *conn;
	socklen_t len;
	bool ok;

	conn = calloc(1, sizeof (*conn));
	if (conn == NULL)
		return (NULL);
	conn->nc_fd = s;
	conn->nc_kind = kind;

	len = sizeof (conn->nc_peer);
	if (getpeername(s, (struct sockaddr *)&conn->nc_peer, &len) != 0) {
		free(conn);
		return (NULL);
	}
	if (getnameinfo((struct sockaddr *)&conn->nc_peer, len, conn->nc_name,
	    sizeof (conn->nc_name), NULL, 0, NI_NUMERICHOST) != 0)
		(void) strlcpy(conn->nc_name, "?", sizeof (conn->nc_name));

	(void) pthread_mutex_lock(&conn_lock);
	ok = num_handshakes < NVMFD_MAX_HANDSHAKES &&
	    count_from_host(conn, false) < NVMFD_MAX_HANDSHAKES_PER_HOST;
	if (ok) {
		num_handshakes++;
		conn->nc_next = conns;
		conns = conn;
	}
	(void) pthread_mutex_unlock(&conn_lock);

	if (!ok) {
		nvmfd_peer_log(conn, "Too many connections; dropping one");
		free(conn);
		return (NULL);
	}
	return (conn);
}

/* Turn a discovery connection that has sent Connect into a session. */
bool
nvmfd_conn_session(nvmfd_conn_t *conn)
{
	bool ok;

	(void) pthread_mutex_lock(&conn_lock);
	ok = num_sessions < NVMFD_MAX_SESSIONS &&
	    count_from_host(conn, true) < NVMFD_MAX_SESSIONS_PER_HOST;
	if (ok) {
		conn->nc_session = true;
		num_handshakes--;
		num_sessions++;
	}
	(void) pthread_mutex_unlock(&conn_lock);
	return (ok);
}

static void
conn_end(nvmfd_conn_t *conn)
{
	nvmfd_conn_t **cp;

	(void) pthread_mutex_lock(&conn_lock);
	for (cp = &conns; *cp != conn; cp = &(*cp)->nc_next)
		;
	*cp = conn->nc_next;
	if (conn->nc_session)
		num_sessions--;
	else
		num_handshakes--;
	(void) pthread_mutex_unlock(&conn_lock);

	(void) close(conn->nc_fd);
	free(conn);
}

static void *
conn_thread(void *arg)
{
	nvmfd_conn_t *conn = arg;

	if (conn->nc_kind == LISTEN_DISCOVERY)
		discovery_connection(conn);
	else
		io_connection(conn);
	conn_end(conn);
	return (NULL);
}

static void
accept_connection(const struct listen_socket *ls, const pthread_attr_t *attr)
{
	struct timespec delay = { 0, 100 * MICROSEC };
	nvmfd_conn_t *conn;
	pthread_t thr;
	int error, on = 1, s;

	s = accept(ls->ls_fd, NULL, NULL);
	if (s == -1) {
		if (errno == EINTR || errno == ECONNABORTED)
			return;

		/* Out of descriptors or memory: wait rather than spin. */
		nvmfd_log(LOG_WARNING, "accept: %s", strerror(errno));
		(void) nanosleep(&delay, NULL);
		return;
	}

	/* The kernel transport inherits the option with the socket. */
	(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof (on));

	conn = conn_begin(s, ls->ls_kind);
	if (conn == NULL) {
		(void) close(s);
		return;
	}

	error = pthread_create(&thr, attr, conn_thread, conn);
	if (error != 0) {
		nvmfd_log(LOG_WARNING, "Failed to create a thread: %s",
		    strerror(error));
		conn_end(conn);
	}
}

/* ARGSUSED */
static void *
accept_thread(void *arg)
{
	pthread_attr_t attr;
	struct pollfd *fds;
	nfds_t i;

	(void) pthread_attr_init(&attr);
	(void) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	fds = calloc(num_listen_sockets, sizeof (*fds));
	if (fds == NULL) {
		nvmfd_log(LOG_ERR, "Out of memory");
		exit(1);
	}
	for (i = 0; i < num_listen_sockets; i++) {
		fds[i].fd = listen_sockets[i].ls_fd;
		fds[i].events = POLLIN;
	}

	for (;;) {
		if (poll(fds, num_listen_sockets, -1) == -1) {
			if (errno == EINTR)
				continue;
			nvmfd_log(LOG_ERR, "poll: %s", strerror(errno));
			exit(1);
		}

		for (i = 0; i < num_listen_sockets; i++) {
			if ((fds[i].revents & POLLIN) != 0)
				accept_connection(&listen_sockets[i], &attr);
		}
	}
}

static void
create_passive_sockets(const char *address, const char *port,
    enum listen_kind kind)
{
	struct addrinfo hints, *ai, *list;
	struct listen_socket *ls;
	bool created = false;
	int error, on = 1, s;

	(void) memset(&hints, 0, sizeof (hints));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	error = getaddrinfo(address, port, &hints, &list);
	if (error != 0) {
		errx(1, "%s port %s: %s", address != NULL ? address : "*",
		    port, gai_strerror(error));
	}

	for (ai = list; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == -1)
			continue;

		(void) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on,
		    sizeof (on));

		/* The IPv4 wildcard gets a socket of its own. */
		if (ai->ai_family == AF_INET6) {
			(void) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on,
			    sizeof (on));
		}

		if (bind(s, ai->ai_addr, ai->ai_addrlen) != 0) {
			warn("bind %s port %s", address != NULL ? address :
			    "*", port);
			(void) close(s);
			continue;
		}
		if (listen(s, SOMAXCONN) != 0)
			err(1, "listen");

		listen_sockets = reallocf(listen_sockets,
		    (num_listen_sockets + 1) * sizeof (*listen_sockets));
		if (listen_sockets == NULL)
			err(1, "reallocf");
		ls = &listen_sockets[num_listen_sockets++];
		ls->ls_fd = s;
		ls->ls_kind = kind;
		if (kind == LISTEN_IO)
			discovery_add_io_listener(s);
		created = true;
	}

	freeaddrinfo(list);
	if (!created) {
		errx(1, "Failed to listen on %s port %s",
		    address != NULL ? address : "*", port);
	}
}

/* ARGSUSED */
static void
handle_sig(int sig)
{
	quit = 1;
}

/*
 * The termination signals are blocked in every thread and are taken only in
 * main(), inside sigsuspend().
 */
static void
setup_signals(sigset_t *origp)
{
	static const int sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction sa;
	sigset_t set;
	uint_t i;

	(void) memset(&sa, 0, sizeof (sa));
	(void) sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_IGN;
	(void) sigaction(SIGPIPE, &sa, NULL);

	(void) sigemptyset(&set);
	sa.sa_handler = handle_sig;
	for (i = 0; i < sizeof (sigs) / sizeof (sigs[0]); i++) {
		(void) sigaction(sigs[i], &sa, NULL);
		(void) sigaddset(&set, sigs[i]);
	}
	(void) pthread_sigmask(SIG_BLOCK, &set, origp);
}

int
main(int ac, char **av)
{
	const char **addresses = NULL;
	const char *dport, *ioport, *transport;
	char *end;
	unsigned long long value;
	sigset_t orig;
	pthread_t thr;
	uint_t i, naddresses = 0;
	int ch, error;
	bool foreground = false;

	/* 7.4.9.3 Default port for discovery. */
	dport = "8009";
	ioport = "4420";
	transport = "tcp";
	while ((ch = getopt(ac, av, "a:dFfGgH:P:p:t:")) != -1) {
		switch (ch) {
		case 'a':
			addresses = reallocf(addresses,
			    (naddresses + 1) * sizeof (*addresses));
			if (addresses == NULL)
				err(1, "reallocf");
			addresses[naddresses++] = optarg;
			break;
		case 'd':
			debug = true;
			foreground = true;
			break;
		case 'F':
			flow_control_disable = true;
			break;
		case 'f':
			foreground = true;
			break;
		case 'G':
			data_digests = true;
			break;
		case 'g':
			header_digests = true;
			break;
		case 'H':
			errno = 0;
			value = strtoull(optarg, &end, 0);
			if (errno != 0 || end == optarg || *end != '\0' ||
			    value < 4096 || value > UINT32_MAX ||
			    value % 4 != 0)
				errx(1, "Invalid MAXH2CDATA value %s", optarg);
			maxh2cdata = (uint32_t)value;
			break;
		case 'P':
			dport = optarg;
			break;
		case 'p':
			ioport = optarg;
			break;
		case 't':
			transport = optarg;
			break;
		default:
			usage();
		}
	}

	if (ac != optind)
		usage();

	if (strcasecmp(transport, "tcp") != 0)
		errx(1, "Invalid transport %s", transport);

	openlog("nvmfd", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	init_nvmft();

	if (naddresses == 0) {
		create_passive_sockets(NULL, dport, LISTEN_DISCOVERY);
		create_passive_sockets(NULL, ioport, LISTEN_IO);
	}
	for (i = 0; i < naddresses; i++) {
		create_passive_sockets(addresses[i], dport, LISTEN_DISCOVERY);
		create_passive_sockets(addresses[i], ioport, LISTEN_IO);
	}
	free(addresses);

	/*
	 * The control node is open and the ports are bound.  From here the
	 * daemon needs sys_devices for the node's ioctls and nothing else
	 * beyond what any process has.
	 */
	if (__init_daemon_priv(PU_RESETGROUPS | PU_LIMITPRIVS, NVMFD_UID,
	    NVMFD_GID, PRIV_SYS_DEVICES, (char *)NULL) != 0)
		err(1, "Failed to drop privileges");

	if (!foreground && daemon(0, 0) != 0)
		err(1, "Failed to fork into the background");

	if (priv_set(PRIV_OFF, PRIV_ALLSETS, PRIV_PROC_FORK, PRIV_PROC_EXEC,
	    PRIV_PROC_INFO, PRIV_PROC_SESSION, PRIV_FILE_LINK_ANY,
	    (char *)NULL) != 0) {
		nvmfd_log(LOG_ERR, "Failed to drop privileges: %s",
		    strerror(errno));
		return (1);
	}

	setup_signals(&orig);

	error = pthread_create(&thr, NULL, accept_thread, NULL);
	if (error != 0) {
		nvmfd_log(LOG_ERR, "Failed to create a thread: %s",
		    strerror(error));
		return (1);
	}

	while (quit == 0)
		(void) sigsuspend(&orig);
	return (0);
}

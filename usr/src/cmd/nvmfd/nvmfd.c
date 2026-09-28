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
 * Differences from the FreeBSD daemon:
 *   - The accept loop uses poll(2) instead of kqueue(2); there are only a
 *     handful of listening sockets so this is equivalent and portable.
 *   - The userland block-device backend and its "-K" (kernel I/O) toggle are
 *     dropped.  On illumos the namespace data always lives behind STMF, so
 *     every accepted I/O association is handed to the kernel nvmft port
 *     provider.  There are no positional device arguments.
 *   - kldload(nvmft) is replaced by relying on the nvmft driver being attached
 *     (it is a COMSTAR/STMF port provider configured out of band by stmfadm).
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <libnvmf.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

bool data_digests = false;
bool header_digests = false;
bool flow_control_disable = false;
uint32_t maxh2cdata = 256 * 1024;

static const char *subnqn;
static volatile sig_atomic_t quit = 0;

/*
 * Each accepted connection runs its Fabrics CONNECT handshake in a detached
 * worker thread that blocks on socket reads.  A stalled peer would otherwise
 * pin a thread indefinitely, so bound both how long a handshake read may block
 * (SO_RCVTIMEO, applied per accepted socket) and how many handshakes may run
 * concurrently.  The cap is deliberately generous: it only sheds load under a
 * connection flood, well above any legitimate fan-in.
 */
#define	NVMFD_HANDSHAKE_RCVTIMEO	30	/* seconds */
#define	NVMFD_MAX_HANDSHAKES		1024

static pthread_mutex_t handshake_lock = PTHREAD_MUTEX_INITIALIZER;
static u_int handshake_count;

/*
 * Reserve a handshake slot for a newly accepted connection.  Returns false (and
 * reserves nothing) when the cap is reached so the caller closes the socket.
 */
bool
nvmfd_handshake_begin(void)
{
	bool ok;

	(void) pthread_mutex_lock(&handshake_lock);
	ok = handshake_count < NVMFD_MAX_HANDSHAKES;
	if (ok)
		handshake_count++;
	(void) pthread_mutex_unlock(&handshake_lock);
	return (ok);
}

/* Release the slot reserved by nvmfd_handshake_begin() when a worker exits. */
void
nvmfd_handshake_end(void)
{
	(void) pthread_mutex_lock(&handshake_lock);
	assert(handshake_count > 0);
	handshake_count--;
	(void) pthread_mutex_unlock(&handshake_lock);
}

/*
 * Listening sockets are tracked in a small pollfd array.  Each entry carries
 * a kind tag so the accept loop knows which handler to dispatch the new
 * connection to.
 */
enum listen_kind {
	LISTEN_DISCOVERY = 1,
	LISTEN_IO = 2
};

struct listen_socket {
	int		ls_fd;
	enum listen_kind ls_kind;
};

static struct listen_socket *listen_sockets;
static nfds_t num_listen_sockets;

static void
usage(void)
{
	(void) fprintf(stderr,
	    "nvmfd [-dFGg] [-H MAXH2CDATA] [-P dport] [-p ioport] "
	    "[-t transport] [-n subnqn]\n");
	exit(1);
}

static void
handle_sig(int sig __unused)
{
	quit = 1;
}

static void
add_listen_socket(int s, enum listen_kind kind)
{
	struct listen_socket *ls;

	if (listen(s, -1) != 0)
		err(1, "listen");

	listen_sockets = reallocf(listen_sockets,
	    (num_listen_sockets + 1) * sizeof (*listen_sockets));
	if (listen_sockets == NULL)
		err(1, "reallocf");

	ls = &listen_sockets[num_listen_sockets];
	ls->ls_fd = s;
	ls->ls_kind = kind;
	num_listen_sockets++;
}

static void
create_passive_sockets(const char *port, bool discovery)
{
	struct addrinfo hints, *ai, *list;
	bool created;
	int error, s;

	(void) memset(&hints, 0, sizeof (hints));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_protocol = IPPROTO_TCP;
	error = getaddrinfo(NULL, port, &hints, &list);
	if (error != 0)
		errx(1, "%s", gai_strerror(error));
	created = false;

	for (ai = list; ai != NULL; ai = ai->ai_next) {
		int on = 1;

		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == -1)
			continue;

		/* Allow a daemon restart to rebind while old conns linger. */
		(void) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof (on));

		if (bind(s, ai->ai_addr, ai->ai_addrlen) != 0) {
			(void) close(s);
			continue;
		}

		if (discovery) {
			add_listen_socket(s, LISTEN_DISCOVERY);
		} else {
			add_listen_socket(s, LISTEN_IO);
			discovery_add_io_controller(s, subnqn);
		}
		created = true;
	}

	freeaddrinfo(list);
	if (!created)
		err(1, "Failed to create any listen sockets");
}

static void
handle_connections(void)
{
	struct pollfd *fds;
	struct timeval rcvtimeo;
	nfds_t i;
	int s;
	int nodelay;

	(void) signal(SIGHUP, handle_sig);
	(void) signal(SIGINT, handle_sig);
	(void) signal(SIGQUIT, handle_sig);
	(void) signal(SIGTERM, handle_sig);

	fds = calloc(num_listen_sockets, sizeof (*fds));
	if (fds == NULL)
		err(1, "calloc");
	for (i = 0; i < num_listen_sockets; i++) {
		fds[i].fd = listen_sockets[i].ls_fd;
		fds[i].events = POLLIN;
	}

	while (quit == 0) {
		if (poll(fds, num_listen_sockets, -1) == -1) {
			if (errno == EINTR)
				continue;
			err(1, "poll");
		}

		for (i = 0; i < num_listen_sockets; i++) {
			if ((fds[i].revents & POLLIN) == 0)
				continue;

			s = accept(fds[i].fd, NULL, NULL);
			if (s == -1) {
				warn("accept");
				continue;
			}

			/*
			 * Disable Nagle on the data connection before it is
			 * handed off to the kernel: NVMe/TCP is a request/
			 * response PDU protocol and Nagle vs the peer's delayed
			 * ACKs adds ~40ms stalls per round trip, most visible on
			 * C2H read-data PDUs.  The option persists into the
			 * kernel-owned socket after the handoff.
			 */
			nodelay = 1;
			(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
			    &nodelay, sizeof (nodelay));

			/*
			 * Bound how long the handshake worker can block on a
			 * stalled peer's reads.  The option persists into the
			 * kernel-owned socket but only governs the userland
			 * handshake; the kernel installs its own callbacks.
			 */
			rcvtimeo.tv_sec = NVMFD_HANDSHAKE_RCVTIMEO;
			rcvtimeo.tv_usec = 0;
			(void) setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
			    &rcvtimeo, sizeof (rcvtimeo));

			switch (listen_sockets[i].ls_kind) {
			case LISTEN_DISCOVERY:
				handle_discovery_socket(s);
				break;
			case LISTEN_IO:
				handle_io_socket(s);
				break;
			}
		}
	}

	free(fds);
}

int
main(int ac, char **av)
{
	const char *dport, *ioport, *transport;
	char *end;
	unsigned long long value;
	int ch, error;
	bool daemonize;
	static char nqn[NVMF_NQN_MAX_LEN];

	/* 7.4.9.3 Default port for discovery. */
	dport = "8009";

	daemonize = true;
	ioport = "0";
	subnqn = NULL;
	transport = "tcp";
	while ((ch = getopt(ac, av, "dFgGH:n:P:p:t:")) != -1) {
		switch (ch) {
		case 'd':
			daemonize = false;
			break;
		case 'F':
			flow_control_disable = true;
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
			if (errno != 0 || *end != '\0')
				errx(1, "Invalid MAXH2CDATA value %s", optarg);
			if (value < 4096 || value > UINT32_MAX ||
			    value % 4 != 0)
				errx(1, "Invalid MAXH2CDATA value %s", optarg);
			maxh2cdata = (uint32_t)value;
			break;
		case 'n':
			subnqn = optarg;
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

	av += optind;
	ac -= optind;

	/* The block backend is gone; positional arguments are not accepted. */
	if (ac != 0)
		usage();

	if (strcasecmp(transport, "tcp") != 0)
		errx(1, "Invalid transport %s", transport);

	if (subnqn == NULL) {
		error = nvmf_nqn_from_hostuuid(nqn);
		if (error != 0)
			errc(1, error, "Failed to generate NQN");
		subnqn = nqn;
	}

	init_discovery();
	init_io(subnqn);

	if (daemonize) {
		if (daemon(0, 0) != 0)
			err(1, "Failed to fork into the background");
	}

	create_passive_sockets(dport, true);
	create_passive_sockets(ioport, false);

	handle_connections();
	shutdown_io();
	return (0);
}

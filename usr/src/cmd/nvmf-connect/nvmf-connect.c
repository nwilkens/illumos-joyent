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
 * nvmf-connect: NVMe-over-Fabrics host (initiator) connect tool.
 *
 * Ported from FreeBSD sbin/nvmecontrol (connect.c / fabrics.c, Copyright (c)
 * 2023-2024 Chelsio Communications, Inc., John Baldwin, BSD-2-Clause).  This is
 * the host-side counterpart of the target daemon nvmfd: it runs the Fabrics
 * CONNECT handshake in userland with libnvmf, enables the controller, fetches
 * Identify, negotiates I/O queues, and hands the established socket-backed
 * qpairs to the kernel nvmf_host driver via nvmf_handoff_host().  The kernel
 * then owns the association and presents each namespace as a blkdev disk.
 *
 *	nvmf-connect connect  [opts] <address:port> <subnqn>
 *	nvmf-connect discover [opts] <address[:port]>
 *
 * A standalone tool keeps this off the existing nvmeadm; the verbs mirror
 * FreeBSD 'nvmecontrol connect/discover' (and Linux 'nvme connect/discover').
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libnvmf.h>

/*
 * Defaults that FreeBSD exposes via libnvmf constants the illumos headers do
 * not (yet) carry.  Values match FreeBSD nvmecontrol.
 */
#define	NVMF_DEFAULT_IO_ENTRIES		1024
#define	NVMF_DEFAULT_RECONNECT_DELAY	10	/* seconds */
#define	NVMF_DEFAULT_CONTROLLER_LOSS	600	/* seconds */

static const char *progname;

static void
usage(void)
{
	(void) fprintf(stderr,
	    "usage:\n"
	    "  %s connect  [-t tcp] [-c dynamic|static|<id>]\n"
	    "             [-i nr_io_queues] [-Q queue_size] [-k kato_sec]\n"
	    "             [-r reconnect_delay]\n"
	    "             [-l ctrl_loss_tmo] [-q hostnqn] [-F] [-g] [-G]\n"
	    "             <address:port> <subnqn>\n"
	    "  %s discover   [-t tcp] [-q hostnqn] <address[:port]>\n"
	    "  %s disconnect [<subnqn>]\n"
	    "  %s list\n",
	    progname, progname, progname, progname);
	exit(2);
}

/*
 * Split "host:port", "[v6]:port", "host", "[v6]", "v6" into address + port.
 * *tofree (if set) must be freed by the caller.  port is NULL if absent.
 * (FreeBSD nvmf_parse_address.)
 */
static void
parse_address(const char *in, const char **address, const char **port,
    char **tofree)
{
	char *cp;

	*tofree = NULL;
	if (in[0] == '[') {
		cp = strchr(in + 1, ']');
		if (cp == NULL || cp == in + 1)
			errx(2, "invalid address %s", in);
		*tofree = strndup(in + 1, cp - (in + 1));
		*address = *tofree;
		cp++;
		if (*cp == '\0') {
			*port = NULL;
		} else if (*cp == ':' && cp[1] != '\0') {
			*port = cp + 1;
		} else {
			errx(2, "invalid address %s", in);
		}
		return;
	}

	cp = strchr(in, ':');
	if (cp == NULL) {			/* no colon: bare address */
		*address = in;
		*port = NULL;
		return;
	}
	if (strchr(cp + 1, ':') != NULL) {	/* 2nd colon: bare IPv6 */
		*address = in;
		*port = NULL;
		return;
	}
	if (cp == in || cp[1] == '\0')
		errx(2, "invalid address %s", in);
	*tofree = strndup(in, cp - in);
	*address = *tofree;
	*port = cp + 1;
}

/*
 * Parse an unsigned numeric option, rejecting trailing garbage, overflow, and
 * values outside [0, max].  (Matches the validation nvmfd and nvmfadm use.)
 */
static ulong_t
parse_uint(const char *str, ulong_t max, const char *what)
{
	char *end;
	ulong_t value;

	errno = 0;
	value = strtoul(str, &end, 0);
	if (errno != 0 || end == str || *end != '\0' || value > max)
		errx(2, "invalid %s %s", what, str);
	return (value);
}

static uint16_t
parse_cntlid(const char *cntlid)
{
	if (strcasecmp(cntlid, "dynamic") == 0)
		return (NVMF_CNTLID_DYNAMIC);
	if (strcasecmp(cntlid, "static") == 0)
		return (NVMF_CNTLID_STATIC_ANY);
	return ((uint16_t)parse_uint(cntlid, NVMF_CNTLID_STATIC_MAX,
	    "controller ID"));
}

/*
 * NVMe/TCP is a request/response PDU protocol; Nagle's algorithm interacts with
 * the peer's delayed ACKs to add ~40ms stalls per round trip (most visible on
 * C2H read-data PDUs), so disable it on every fabric socket.  The option is a
 * property of the TCP endpoint, so it persists across the fd handoff into the
 * kernel-owned socket and applies to the kernel's PDU sends too.
 */
static void
tcp_set_nodelay(int s)
{
	int on = 1;

	(void) setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof (on));
}

/*
 * getaddrinfo + socket + connect.  Returns the connected fd, or -1.  When
 * ai_out/list_out are supplied the winning addrinfo and its list are returned
 * (not freed) so the same target can be reconnected for each I/O queue.
 */
static int
tcp_connect(int adrfam, const char *address, const char *port,
    struct addrinfo **ai_out, struct addrinfo **list_out)
{
	struct addrinfo hints, *ai, *list;
	int error, s;

	(void) memset(&hints, 0, sizeof (hints));
	hints.ai_family = adrfam;
	hints.ai_protocol = IPPROTO_TCP;
	error = getaddrinfo(address, port, &hints, &list);
	if (error != 0) {
		warnx("%s: %s", address, gai_strerror(error));
		return (-1);
	}

	for (ai = list; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == -1)
			continue;
		if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0) {
			(void) close(s);
			continue;
		}
		tcp_set_nodelay(s);
		if (ai_out != NULL)
			*ai_out = ai;
		if (list_out != NULL)
			*list_out = list;
		else
			freeaddrinfo(list);
		return (s);
	}

	freeaddrinfo(list);
	warn("failed to connect to %s:%s", address, port ? port : "(none)");
	return (-1);
}

/* Connect a fresh socket to a previously chosen addrinfo (for I/O queues). */
static int
tcp_connect_ai(struct addrinfo *ai)
{
	int s;

	s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (s == -1)
		return (-1);
	if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0) {
		(void) close(s);
		return (-1);
	}
	tcp_set_nodelay(s);
	return (s);
}

/*
 * Drive CC.EN=1 and wait for CSTS.RDY over the admin qpair using Fabrics
 * property get/set.  illumos lacks FreeBSD's NVME_CC and NVME_CAP field macros,
 * so the register fields are manipulated with explicit shifts (NVMe base spec):
 *   CAP: MQES[15:0], TO[31:24] (500ms units), CSS NVM=bit37, MPSMIN[51:48],
 *        MPSMAX[55:52].
 *   CC : EN[0], CSS[6:4], MPS[10:7], AMS[13:11], SHN[15:14], IOSQES[19:16],
 *        IOCQES[23:20].
 *   CSTS: RDY[0].
 * On success *mqesp holds CAP.MQES.  Returns 0 or non-zero on failure.
 */
static int
enable_controller(struct nvmf_qpair *qp, const char *subnqn, uint16_t *mqesp)
{
	uint64_t cap, cc, csts;
	uint_t mps, mpsmin, mpsmax;
	int error, timo;

	error = nvmf_read_property(qp, NVMF_PROP_CAP, 8, &cap);
	if (error != 0) {
		warnc(error, "failed to fetch CAP");
		return (error);
	}
	if (((cap >> 37) & 0x1) == 0) {
		warnx("controller %s does not support the NVM command set",
		    subnqn);
		return (EINVAL);
	}
	if (mqesp != NULL)
		*mqesp = (uint16_t)(cap & 0xffff);

	mpsmin = (uint_t)((cap >> 48) & 0xf);
	mpsmax = (uint_t)((cap >> 52) & 0xf);
	/* Prefer the host page size if it fits the controller's range. */
	mps = (uint_t)(ffs(getpagesize()) - 1);
	if (mps < mpsmin + 12)
		mps = mpsmin;
	else if (mps > mpsmax + 12)
		mps = mpsmax;
	else
		mps -= 12;

	error = nvmf_read_property(qp, NVMF_PROP_CC, 4, &cc);
	if (error != 0) {
		warnc(error, "failed to fetch CC");
		return (error);
	}
	cc &= ~(((uint64_t)0xf << 20) | ((uint64_t)0xf << 16) |
	    ((uint64_t)0x3 << 14) | ((uint64_t)0x7 << 11) |
	    ((uint64_t)0xf << 7) | ((uint64_t)0x7 << 4));
	cc |= ((uint64_t)4 << 20);	/* IOCQES: 16-byte CQE */
	cc |= ((uint64_t)6 << 16);	/* IOSQES: 64-byte SQE */
	cc |= ((uint64_t)mps << 7);	/* MPS */
	/* AMS=0 (round-robin), CSS=0 (NVM command set). */
	cc |= 0x1;			/* EN=1 */
	error = nvmf_write_property(qp, NVMF_PROP_CC, 4, cc);
	if (error != 0) {
		warnc(error, "failed to set CC");
		return (error);
	}

	timo = (int)((cap >> 24) & 0xff);	/* CAP.TO, 500ms units */
	for (;;) {
		error = nvmf_read_property(qp, NVMF_PROP_CSTS, 4, &csts);
		if (error != 0) {
			warnc(error, "failed to fetch CSTS");
			return (error);
		}
		if ((csts & 0x1) != 0)
			break;
		if (timo == 0) {
			warnx("controller %s failed to become ready", subnqn);
			return (EIO);
		}
		timo--;
		(void) usleep(500 * 1000);
	}
	return (0);
}

struct conn_opts {
	const char	*transport;
	uint16_t	cntlid;
	uint16_t	num_io_queues;
	uint16_t	queue_size;
	uint32_t	kato_sec;
	uint32_t	reconnect_delay;
	uint32_t	ctrl_loss_tmo;
	const char	*hostnqn;
	boolean_t	flow_control;
	boolean_t	header_digests;
	boolean_t	data_digests;
};

static void
opts_defaults(struct conn_opts *o)
{
	(void) memset(o, 0, sizeof (*o));
	o->transport = "tcp";
	o->cntlid = NVMF_CNTLID_DYNAMIC;
	o->num_io_queues = 1;
	o->queue_size = 0;
	o->kato_sec = NVMF_KATO_DEFAULT / 1000;
	o->reconnect_delay = NVMF_DEFAULT_RECONNECT_DELAY;
	o->ctrl_loss_tmo = NVMF_DEFAULT_CONTROLLER_LOSS;
}

static void
tcp_assoc_params(nvmf_association_params_t *ap, const struct conn_opts *o,
    boolean_t discovery)
{
	(void) memset(ap, 0, sizeof (*ap));
	ap->nap_sq_flow_control = discovery ? B_FALSE : o->flow_control;
	ap->nap_tcp.pda = 0;
	ap->nap_tcp.header_digests = discovery ? B_FALSE : o->header_digests;
	ap->nap_tcp.data_digests = discovery ? B_FALSE : o->data_digests;
	ap->nap_tcp.maxr2t = 1;
}

static int
cmd_connect(int argc, char *argv[])
{
	struct conn_opts o;
	nvmf_association_params_t aparams;
	nvmf_qpair_params_t qp;
	nvme_identify_ctrl_t cdata;
	nvmf_discovery_log_page_entry_t dle;
	struct nvmf_association *na = NULL;
	struct nvmf_qpair *admin = NULL, **io = NULL;
	struct addrinfo *ai = NULL, *list = NULL;
	const char *address, *port, *subnqn, *hostnqn;
	char *tofree = NULL, hostnqn_buf[NVMF_NQN_MAX_LEN];
	uint8_t hostid[16];
	uint16_t mqes = 0;
	uint_t qsize, queues, i;
	int c, error, rc = 1, admin_fd;

	opts_defaults(&o);
	optind = 1;
	while ((c = getopt(argc, argv, "t:c:i:Q:k:r:l:q:FgG")) != -1) {
		switch (c) {
		case 't':
			o.transport = optarg;
			break;
		case 'c':
			o.cntlid = parse_cntlid(optarg);
			break;
		case 'i':
			o.num_io_queues = (uint16_t)parse_uint(optarg,
			    UINT16_MAX, "number of I/O queues");
			break;
		case 'Q':
			o.queue_size = (uint16_t)parse_uint(optarg, UINT16_MAX,
			    "queue size");
			break;
		case 'k':
			o.kato_sec = (uint32_t)parse_uint(optarg, UINT32_MAX,
			    "keep-alive timeout");
			break;
		case 'r':
			o.reconnect_delay = (uint32_t)parse_uint(optarg,
			    UINT32_MAX, "reconnect delay");
			break;
		case 'l':
			o.ctrl_loss_tmo = (uint32_t)parse_uint(optarg,
			    UINT32_MAX, "controller loss timeout");
			break;
		case 'q':
			o.hostnqn = optarg;
			break;
		case 'F':
			o.flow_control = B_TRUE;
			break;
		case 'g':
			o.header_digests = B_TRUE;
			break;
		case 'G':
			o.data_digests = B_TRUE;
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 2)
		usage();
	if (strcasecmp(o.transport, "tcp") != 0)
		errx(2, "unsupported transport %s", o.transport);
	if (o.num_io_queues == 0)
		errx(2, "invalid number of I/O queues");

	parse_address(argv[optind], &address, &port, &tofree);
	if (port == NULL)
		errx(2, "explicit port required for connect");
	subnqn = argv[optind + 1];

	if (nvmf_hostid_from_hostuuid(hostid) != 0)
		errx(1, "failed to derive host id");
	if (o.hostnqn != NULL) {
		hostnqn = o.hostnqn;
	} else {
		if (nvmf_nqn_from_hostuuid(hostnqn_buf) != 0)
			errx(1, "failed to derive host NQN");
		hostnqn = hostnqn_buf;
	}

	tcp_assoc_params(&aparams, &o, B_FALSE);
	na = nvmf_allocate_association(NVMF_TRTYPE_TCP, B_FALSE, &aparams);
	if (na == NULL)
		errx(1, "failed to create association");

	/* Admin connection; remember the addrinfo to reuse for I/O queues. */
	admin_fd = tcp_connect(AF_UNSPEC, address, port, &ai, &list);
	if (admin_fd == -1)
		goto out;
	(void) memset(&qp, 0, sizeof (qp));
	qp.nqp_admin = B_TRUE;
	qp.nqp_tcp.fd = admin_fd;
	admin = nvmf_connect(na, &qp, 0, NVMF_MIN_ADMIN_MAX_SQ_SIZE, hostid,
	    o.cntlid, subnqn, hostnqn, o.kato_sec * 1000);
	if (admin == NULL) {
		warnx("failed to connect admin queue: %s",
		    nvmf_association_error(na));
		(void) close(admin_fd);
		goto out;
	}
	if (enable_controller(admin, subnqn, &mqes) != 0)
		goto out;
	error = nvmf_host_identify_controller(admin, &cdata);
	if (error != 0) {
		warnc(error, "failed to identify controller %s", subnqn);
		goto out;
	}
	nvmf_update_association(na, &cdata);

	/* I/O queue size: 0's-based MQES gives the max entries. */
	qsize = o.queue_size;
	if (qsize == 0)
		qsize = MIN(NVMF_DEFAULT_IO_ENTRIES, (uint_t)mqes + 1);
	else if (qsize > (uint_t)mqes + 1)
		errx(1, "I/O queue size exceeds controller maximum (%u)",
		    mqes + 1);

	error = nvmf_host_request_queues(admin, o.num_io_queues, &queues);
	if (error != 0) {
		warnc(error, "failed to request I/O queues");
		goto out;
	}
	if (queues < o.num_io_queues) {
		warnx("controller granted %u I/O queues, requested %u",
		    queues, o.num_io_queues);
		goto out;
	}

	io = calloc(o.num_io_queues, sizeof (*io));
	if (io == NULL)
		err(1, "calloc");
	for (i = 0; i < o.num_io_queues; i++) {
		int iofd = tcp_connect_ai(ai);

		if (iofd == -1) {
			warn("failed to open I/O queue %u", i + 1);
			goto out;
		}
		(void) memset(&qp, 0, sizeof (qp));
		qp.nqp_admin = B_FALSE;
		qp.nqp_tcp.fd = iofd;
		io[i] = nvmf_connect(na, &qp, (uint16_t)(i + 1), qsize, hostid,
		    nvmf_cntlid(admin), subnqn, hostnqn, 0);
		if (io[i] == NULL) {
			warnx("failed to connect I/O queue %u: %s", i + 1,
			    nvmf_association_error(na));
			(void) close(iofd);
			goto out;
		}
	}

	error = nvmf_init_dle_from_admin_qp(admin, &cdata, &dle);
	if (error != 0) {
		warnc(error, "failed to build handoff parameters");
		goto out;
	}

	/* nvmf_handoff_host() frees admin + all io qpairs, even on error. */
	error = nvmf_handoff_host(&dle, hostnqn, admin, o.num_io_queues, io,
	    &cdata, o.reconnect_delay, o.ctrl_loss_tmo);
	admin = NULL;
	if (io != NULL) {
		for (i = 0; i < o.num_io_queues; i++)
			io[i] = NULL;
	}
	if (error != 0) {
		warnc(error, "failed to hand off association to the kernel");
		goto out;
	}

	(void) printf("connected to %s at %s:%s\n", subnqn, address, port);
	rc = 0;

out:
	if (io != NULL) {
		for (i = 0; i < o.num_io_queues; i++) {
			if (io[i] != NULL)
				nvmf_free_qpair(io[i]);
		}
		free(io);
	}
	if (admin != NULL)
		nvmf_free_qpair(admin);
	if (na != NULL)
		nvmf_free_association(na);
	if (list != NULL)
		freeaddrinfo(list);
	free(tofree);
	return (rc);
}

static const char *
adrfam_str(uint8_t adrfam)
{
	switch (adrfam) {
	case NVMF_ADRFAM_IPV4: return ("ipv4");
	case NVMF_ADRFAM_IPV6: return ("ipv6");
	default: return ("?");
	}
}

static int
cmd_discover(int argc, char *argv[])
{
	struct conn_opts o;
	nvmf_association_params_t aparams;
	nvmf_qpair_params_t qp;
	nvmf_discovery_log_page_t *log = NULL;
	struct nvmf_association *na = NULL;
	struct nvmf_qpair *admin = NULL;
	struct addrinfo *ai = NULL, *list = NULL;
	const char *address, *port, *hostnqn;
	char *tofree = NULL, hostnqn_buf[NVMF_NQN_MAX_LEN];
	uint8_t hostid[16];
	uint16_t mqes;
	uint64_t i;
	int c, error, rc = 1, fd;

	opts_defaults(&o);
	optind = 1;
	while ((c = getopt(argc, argv, "t:q:")) != -1) {
		switch (c) {
		case 't': o.transport = optarg; break;
		case 'q': o.hostnqn = optarg; break;
		default: usage();
		}
	}
	if (argc - optind != 1)
		usage();
	if (strcasecmp(o.transport, "tcp") != 0)
		errx(2, "unsupported transport %s", o.transport);

	parse_address(argv[optind], &address, &port, &tofree);
	if (port == NULL)
		port = "8009";			/* 7.4.9.3 discovery default */

	if (nvmf_hostid_from_hostuuid(hostid) != 0)
		errx(1, "failed to derive host id");
	if (o.hostnqn != NULL) {
		hostnqn = o.hostnqn;
	} else {
		if (nvmf_nqn_from_hostuuid(hostnqn_buf) != 0)
			errx(1, "failed to derive host NQN");
		hostnqn = hostnqn_buf;
	}

	tcp_assoc_params(&aparams, &o, B_TRUE);
	na = nvmf_allocate_association(NVMF_TRTYPE_TCP, B_FALSE, &aparams);
	if (na == NULL)
		errx(1, "failed to create discovery association");

	fd = tcp_connect(AF_UNSPEC, address, port, &ai, &list);
	if (fd == -1)
		goto out;
	(void) memset(&qp, 0, sizeof (qp));
	qp.nqp_admin = B_TRUE;
	qp.nqp_tcp.fd = fd;
	admin = nvmf_connect(na, &qp, 0, NVMF_MIN_ADMIN_MAX_SQ_SIZE, hostid,
	    NVMF_CNTLID_DYNAMIC, NVMF_DISCOVERY_NQN, hostnqn, 0);
	if (admin == NULL) {
		warnx("failed to connect discovery controller: %s",
		    nvmf_association_error(na));
		(void) close(fd);
		goto out;
	}
	if (enable_controller(admin, NVMF_DISCOVERY_NQN, &mqes) != 0)
		goto out;

	error = nvmf_host_fetch_discovery_log_page(admin, &log);
	if (error != 0) {
		warnc(error, "failed to fetch discovery log page");
		goto out;
	}

	(void) printf("Discovery log for %s:%s (%llu records)\n", address, port,
	    (unsigned long long)log->ndlp_numrec);
	for (i = 0; i < log->ndlp_numrec; i++) {
		nvmf_discovery_log_page_entry_t *e = &log->ndlp_entries[i];

		(void) printf("  [%llu] trtype=%s adrfam=%s\n",
		    (unsigned long long)i,
		    e->ndle_trtype == NVMF_TRTYPE_TCP ? "tcp" : "?",
		    adrfam_str(e->ndle_adrfam));
		(void) printf("        subnqn:  %.*s\n",
		    (int)sizeof (e->ndle_subnqn), (char *)e->ndle_subnqn);
		(void) printf("        traddr:  %.*s\n",
		    (int)sizeof (e->ndle_traddr), (char *)e->ndle_traddr);
		(void) printf("        trsvcid: %.*s  cntlid: %u\n",
		    (int)sizeof (e->ndle_trsvcid), (char *)e->ndle_trsvcid,
		    e->ndle_cntlid);
	}
	rc = 0;

out:
	free(log);
	if (admin != NULL)
		nvmf_free_qpair(admin);
	if (na != NULL)
		nvmf_free_association(na);
	if (list != NULL)
		freeaddrinfo(list);
	free(tofree);
	return (rc);
}

static int
cmd_list(int argc, char *argv[])
{
	nvlist_t *nvl = NULL;
	nvlist_t **ns = NULL;
	char *s, *svc;
	boolean_t connected = B_FALSE;
	uint32_t u32;
	uint_t nscnt = 0, i;
	int error;

	(void) argc;
	(void) argv;

	error = nvmf_list_controller(&nvl);
	if (error != 0) {
		warnc(error, "failed to list host controller");
		return (1);
	}

	(void) nvlist_lookup_boolean_value(nvl, "connected", &connected);
	if (!connected && nvlist_lookup_string(nvl, "subnqn", &s) != 0) {
		(void) printf("no fabric controller connected\n");
		nvlist_free(nvl);
		return (0);
	}

	(void) printf("nvmf controller (%s)\n",
	    connected ? "connected" : "disconnected");
	if (nvlist_lookup_string(nvl, "subnqn", &s) == 0)
		(void) printf("    subnqn:    %s\n", s);
	if (nvlist_lookup_string(nvl, "traddr", &s) == 0) {
		svc = NULL;
		(void) nvlist_lookup_string(nvl, "trsvcid", &svc);
		(void) printf("    transport: tcp  address: %s:%s\n", s,
		    svc != NULL ? svc : "");
	}
	if (nvlist_lookup_string(nvl, "hostnqn", &s) == 0)
		(void) printf("    hostnqn:   %s\n", s);
	if (nvlist_lookup_uint32(nvl, "num_io_queues", &u32) == 0)
		(void) printf("    io queues: %u\n", u32);
	if (nvlist_lookup_string(nvl, "model", &s) == 0 && s[0] != '\0')
		(void) printf("    model:     %s\n", s);
	if (nvlist_lookup_string(nvl, "serial", &s) == 0 && s[0] != '\0')
		(void) printf("    serial:    %s\n", s);
	if (nvlist_lookup_string(nvl, "firmware", &s) == 0 && s[0] != '\0')
		(void) printf("    firmware:  %s\n", s);

	if (nvlist_lookup_nvlist_array(nvl, "namespaces", &ns, &nscnt) == 0) {
		for (i = 0; i < nscnt; i++) {
			uint32_t nsid = 0, blksize = 0;
			uint64_t size = 0;
			boolean_t nsconn = B_FALSE;

			(void) nvlist_lookup_uint32(ns[i], "nsid", &nsid);
			(void) nvlist_lookup_uint64(ns[i], "size", &size);
			(void) nvlist_lookup_uint32(ns[i], "blksize", &blksize);
			(void) nvlist_lookup_boolean_value(ns[i], "connected",
			    &nsconn);
			(void) printf("    namespace %u: %.2f GiB, %u-byte "
			    "blocks (%s)\n", nsid,
			    (double)size / (1024.0 * 1024.0 * 1024.0), blksize,
			    nsconn ? "online" : "offline");
		}
	}

	nvlist_free(nvl);
	return (0);
}

static int
cmd_disconnect(int argc, char *argv[])
{
	const char *subnqn = NULL;
	int error;

	/*
	 * The single-instance host disconnects its one association regardless of
	 * the NQN, so the subnqn argument is optional and advisory; it is
	 * accepted for symmetry with connect and forward compatibility.
	 */
	if (argc > 1)
		subnqn = argv[1];

	error = nvmf_disconnect_host(subnqn != NULL ? subnqn : "");
	if (error != 0) {
		warnc(error, "failed to disconnect");
		return (1);
	}

	(void) printf("disconnected\n");
	return (0);
}

int
main(int argc, char *argv[])
{
	progname = argv[0];

	if (argc < 2)
		usage();

	if (strcmp(argv[1], "connect") == 0)
		return (cmd_connect(argc - 1, argv + 1));
	if (strcmp(argv[1], "discover") == 0)
		return (cmd_discover(argc - 1, argv + 1));
	if (strcmp(argv[1], "disconnect") == 0)
		return (cmd_disconnect(argc - 1, argv + 1));
	if (strcmp(argv[1], "list") == 0)
		return (cmd_list(argc - 1, argv + 1));

	usage();
	return (2);
}

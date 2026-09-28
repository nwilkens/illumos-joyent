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
 * Start and stop nvmf_rdma listeners for testing, until nvmfd does.
 *
 *	nvmfrdmactl listen [-e entries] [-i icd] [-q max-qid] addr[:port]
 *	    peer ...
 *	nvmfrdmactl unlisten id
 *
 * Build: gcc -m64 -I <uts>/common -o nvmfrdmactl nvmfrdmactl.c -lnsl
 * -lsocket
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/nvme/nvmf_rdma.h>

static void
usage(void)
{
	(void) fprintf(stderr, "usage: nvmfrdmactl listen [-e entries] "
	    "[-i icd] [-q max-qid] addr[:port] peer ...\n"
	    "       nvmfrdmactl unlisten id\n");
	exit(2);
}

static uint32_t
num(const char *s)
{
	char *end;
	unsigned long v;

	v = strtoul(s, &end, 0);
	if (*s == '\0' || *end != '\0' || v > UINT32_MAX)
		errx(2, "bad number: %s", s);
	return ((uint32_t)v);
}

static uint32_t
addr(const char *s)
{
	struct in_addr a;

	if (inet_pton(AF_INET, s, &a) != 1)
		errx(2, "bad IPv4 address: %s", s);
	return (a.s_addr);
}

static int
listen_cmd(int fd, int argc, char **argv)
{
	nvmf_rdma_listen_t l;
	char *port;
	int c, i;

	bzero(&l, sizeof (l));
	l.nrl_icd = NVMF_RDMA_ICD_DEFAULT;
	while ((c = getopt(argc, argv, "e:i:q:")) != -1) {
		switch (c) {
		case 'e':
			l.nrl_io_entries = num(optarg);
			break;
		case 'i':
			l.nrl_icd = num(optarg);
			break;
		case 'q':
			l.nrl_max_qid = num(optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 2 || argc - 1 > NVMF_RDMA_MAX_PEERS)
		usage();
	if ((port = strchr(argv[0], ':')) != NULL) {
		*port++ = '\0';
		if (num(port) == 0 || num(port) > UINT16_MAX)
			errx(2, "bad port: %s", port);
		l.nrl_port = (uint16_t)num(port);
	}
	l.nrl_addr = addr(argv[0]);
	for (i = 1; i < argc; i++)
		l.nrl_peers[l.nrl_npeers++] = addr(argv[i]);
	if (ioctl(fd, NVMF_RDMA_IOC_LISTEN, &l) != 0)
		err(1, "listen");
	(void) printf("%u\n", l.nrl_id);
	return (0);
}

int
main(int argc, char **argv)
{
	nvmf_rdma_unlisten_t u;
	int fd;

	if (argc < 2)
		usage();
	if ((fd = open(NVMF_RDMA_DEV, O_RDWR)) < 0)
		err(1, "%s", NVMF_RDMA_DEV);
	if (strcmp(argv[1], "listen") == 0)
		return (listen_cmd(fd, argc - 1, argv + 1));
	if (strcmp(argv[1], "unlisten") == 0 && argc == 3) {
		u.nru_id = num(argv[2]);
		if (ioctl(fd, NVMF_RDMA_IOC_UNLISTEN, &u) != 0)
			err(1, "unlisten");
		return (0);
	}
	usage();
	return (2);
}

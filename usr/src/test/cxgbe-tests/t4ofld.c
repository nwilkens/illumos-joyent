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
 * Drive the t4nex offload core test client (T4_IOCTL_OFLD_TEST) on hardware.
 *
 *	t4ofld DEV open | close | status
 *	t4ofld DEV listen PORT LADDR LPORT
 *	t4ofld DEV unlisten
 *	t4ofld DEV connect PORT LADDR LPORT FADDR FPORT DMAC
 *	t4ofld DEV send TID LEN
 *	t4ofld DEV disconnect TID | abort TID
 *
 * DEV is the nexus minor node, for example /devices/pci@...:t4nex,0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/ethernet.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "t4nex.h"

static void
usage(void)
{
	(void) fprintf(stderr, "usage: t4ofld DEV open|close|status|unlisten\n"
	    "       t4ofld DEV listen PORT LADDR LPORT\n"
	    "       t4ofld DEV connect PORT LADDR LPORT FADDR FPORT DMAC\n"
	    "       t4ofld DEV send TID LEN\n"
	    "       t4ofld DEV disconnect|abort TID\n");
	exit(2);
}

static uint32_t
num(const char *s, uint32_t max)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno != 0 || *end != '\0' || v > max)
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

static void
mac(const char *s, uint8_t *m)
{
	uint_t b[ETHERADDRL];

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4],
	    &b[5]) != ETHERADDRL)
		errx(2, "bad MAC address: %s", s);
	for (uint_t i = 0; i < ETHERADDRL; i++) {
		if (b[i] > 0xff)
			errx(2, "bad MAC address: %s", s);
		m[i] = (uint8_t)b[i];
	}
}

int
main(int argc, char **argv)
{
	t4_ofld_test_t t;
	const char *cmd;
	int fd, rc;

	if (argc < 3)
		usage();
	cmd = argv[2];
	bzero(&t, sizeof (t));
	t.tot_timeout_ms = 5000;
	t.tot_vlan = 0xfff;

	if (strcmp(cmd, "open") == 0 && argc == 3) {
		t.tot_op = T4_OFLD_TEST_OPEN;
	} else if (strcmp(cmd, "close") == 0 && argc == 3) {
		t.tot_op = T4_OFLD_TEST_CLOSE;
	} else if (strcmp(cmd, "status") == 0 && argc == 3) {
		t.tot_op = T4_OFLD_TEST_STATUS;
	} else if (strcmp(cmd, "unlisten") == 0 && argc == 3) {
		t.tot_op = T4_OFLD_TEST_UNLISTEN;
	} else if (strcmp(cmd, "listen") == 0 && argc == 6) {
		t.tot_op = T4_OFLD_TEST_LISTEN;
		t.tot_port = num(argv[3], 3);
		t.tot_laddr = addr(argv[4]);
		t.tot_lport = htons((uint16_t)num(argv[5], UINT16_MAX));
	} else if (strcmp(cmd, "connect") == 0 && argc == 9) {
		t.tot_op = T4_OFLD_TEST_CONNECT;
		t.tot_port = num(argv[3], 3);
		t.tot_laddr = addr(argv[4]);
		t.tot_lport = htons((uint16_t)num(argv[5], UINT16_MAX));
		t.tot_faddr = addr(argv[6]);
		t.tot_fport = htons((uint16_t)num(argv[7], UINT16_MAX));
		mac(argv[8], t.tot_dmac);
	} else if (strcmp(cmd, "send") == 0 && argc == 5) {
		t.tot_op = T4_OFLD_TEST_SEND;
		t.tot_id = num(argv[3], UINT32_MAX);
		t.tot_len = num(argv[4], 4096);
	} else if ((strcmp(cmd, "disconnect") == 0 ||
	    strcmp(cmd, "abort") == 0) && argc == 4) {
		t.tot_op = strcmp(cmd, "abort") == 0 ? T4_OFLD_TEST_ABORT :
		    T4_OFLD_TEST_DISCONNECT;
		t.tot_id = num(argv[3], UINT32_MAX);
	} else {
		usage();
	}

	if ((fd = open(argv[1], O_RDWR)) < 0)
		err(1, "open %s", argv[1]);
	rc = ioctl(fd, T4_IOCTL_OFLD_TEST, &t);
	(void) close(fd);

	(void) printf("op=%s rc=%d%s%s status=%d id=%u snd_isn=%u "
	    "rcv_isn=%u\n", cmd, rc, rc != 0 ? " " : "",
	    rc != 0 ? strerror(errno) : "", t.tot_status, t.tot_id,
	    t.tot_snd_isn, t.tot_rcv_isn);
	if (t.tot_op == T4_OFLD_TEST_STATUS) {
		(void) printf("listen_stid=%u accepts=%u refused=%u events=%u "
		    "nconn=%u\n", t.tot_id, t.tot_accepts, t.tot_refused,
		    t.tot_events, t.tot_nconn);
		for (uint_t i = 0; i < t.tot_nconn && i < T4_OFLD_TEST_NCONN;
		    i++) {
			const t4_ofld_test_conn_t *c = &t.tot_conn[i];

			(void) printf("  tid=%u flags=0x%x snd_isn=%u "
			    "rcv_isn=%u rx=%llu tx=%llu\n", c->totc_tid,
			    c->totc_flags, c->totc_snd_isn, c->totc_rcv_isn,
			    (unsigned long long)c->totc_rx_bytes,
			    (unsigned long long)c->totc_tx_bytes);
		}
	}
	return (rc == 0 ? 0 : 1);
}

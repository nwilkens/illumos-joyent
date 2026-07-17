/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/netlb.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define	ICE_LB_INTERNAL_MAC	1

static void
usage(const char *prog)
{
	(void) fprintf(stderr, "usage: %s device list|get|normal|mac\n", prog);
	exit(EXIT_FAILURE);
}

static void
fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	lb_property_t *info;
	lb_info_sz_t infosz;
	uint32_t mode;
	size_t i;
	int fd;

	if (argc != 3)
		usage(argv[0]);

	if ((fd = open(argv[1], O_RDWR)) < 0)
		fail("open");

	if (strcmp(argv[2], "list") == 0) {
		if (ioctl(fd, LB_GET_INFO_SIZE, &infosz) != 0)
			fail("LB_GET_INFO_SIZE");
		if ((info = calloc(1, infosz)) == NULL)
			fail("calloc");
		if (ioctl(fd, LB_GET_INFO, info) != 0)
			fail("LB_GET_INFO");
		for (i = 0; i < infosz / sizeof (*info); i++)
			(void) printf("%s %u\n", info[i].key, info[i].value);
		free(info);
	} else if (strcmp(argv[2], "get") == 0) {
		if (ioctl(fd, LB_GET_MODE, &mode) != 0)
			fail("LB_GET_MODE");
		(void) printf("%u\n", mode);
	} else {
		if (strcmp(argv[2], "normal") == 0)
			mode = 0;
		else if (strcmp(argv[2], "mac") == 0)
			mode = ICE_LB_INTERNAL_MAC;
		else
			usage(argv[0]);
		if (ioctl(fd, LB_SET_MODE, &mode) != 0)
			fail("LB_SET_MODE");
	}

	(void) close(fd);
	return (EXIT_SUCCESS);
}

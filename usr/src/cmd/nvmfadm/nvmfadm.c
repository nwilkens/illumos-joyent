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
 * nvmfadm - minimal control utility for the NVMe-over-Fabrics COMSTAR target
 * port provider (nvmft).
 *
 * This creates and destroys subsystem target ports: one SubNQN maps to one STMF
 * stmf_local_port_t.  The namespace (LU) mapping itself is done with the
 * existing sbdadm/stmfadm tools (sbdadm create-lu, stmfadm add-view); see
 * CONFIG.md.  nvmfadm talks to /devices/pseudo/nvmft@0:admin via the
 * NVMFT_IOC_SUBSYS_* ioctls defined in <sys/nvme/nvmf_ioctl.h>, passing a
 * packed nvlist in a struct nvmf_ioc_nv carrier (the same convention the
 * Fabrics host control path uses).
 *
 * Usage:
 *   nvmfadm create-subsys [-s serial] [-p portid] <subnqn>
 *   nvmfadm delete-subsys <subnqn>
 *   nvmfadm list-subsys
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <libnvpair.h>

#include <sys/nvme/nvmf_ioctl.h>

#define	NVMFT_DEV	"/devices/pseudo/nvmft@0:admin"

static const char *progname;

static void
usage(void)
{
	(void) fprintf(stderr,
	    "usage:\n"
	    "  %s create-subsys [-s serial] [-p portid] <subnqn>\n"
	    "  %s delete-subsys <subnqn>\n"
	    "  %s list-subsys\n",
	    progname, progname, progname);
	exit(2);
}

static int
nvmft_open(void)
{
	int fd = open(NVMFT_DEV, O_RDONLY);

	if (fd < 0) {
		(void) fprintf(stderr, "%s: cannot open %s: %s\n",
		    progname, NVMFT_DEV, strerror(errno));
		(void) fprintf(stderr,
		    "%s: is the nvmft driver loaded? (modload nvmft)\n",
		    progname);
	}
	return (fd);
}

/*
 * Pack nvl and issue the ioctl.  No reply is expected (size == 0), so the call
 * passes the request buffer only.
 */
static int
nvmft_ioctl_send(int fd, int cmd, nvlist_t *nvl)
{
	struct nvmf_ioc_nv ionv;
	char *packed = NULL;
	size_t packed_len = 0;
	int ret = 1;

	if (nvlist_pack(nvl, &packed, &packed_len, NV_ENCODE_NATIVE, 0) != 0) {
		(void) fprintf(stderr, "%s: failed to pack request\n",
		    progname);
		return (1);
	}

	bzero(&ionv, sizeof (ionv));
	ionv.data = packed;
	ionv.len = packed_len;
	ionv.size = packed_len;

	if (ioctl(fd, cmd, &ionv) != 0) {
		(void) fprintf(stderr, "%s: ioctl failed: %s\n", progname,
		    strerror(errno));
	} else {
		ret = 0;
	}

	free(packed);
	return (ret);
}

/*
 * Issue a list ioctl: first call with size == 0 to learn the reply length, then
 * allocate and copy out.  Returns an unpacked nvlist in *replyp on success.
 */
static int
nvmft_ioctl_recv(int fd, int cmd, nvlist_t **replyp)
{
	struct nvmf_ioc_nv ionv;
	char *buf;
	size_t need;
	nvlist_t *reply = NULL;

	/* First call with size == 0 returns the required buffer length. */
	bzero(&ionv, sizeof (ionv));
	if (ioctl(fd, cmd, &ionv) != 0) {
		(void) fprintf(stderr, "%s: ioctl (size query) failed: %s\n",
		    progname, strerror(errno));
		return (1);
	}
	if (ionv.len == 0) {
		*replyp = NULL;
		return (0);
	}
	need = ionv.len;

	buf = malloc(need);
	if (buf == NULL) {
		(void) fprintf(stderr, "%s: out of memory\n", progname);
		return (1);
	}

	/* Second call: provide the buffer (size > 0) so the kernel fills it. */
	bzero(&ionv, sizeof (ionv));
	ionv.data = buf;
	ionv.size = need;
	if (ioctl(fd, cmd, &ionv) != 0) {
		(void) fprintf(stderr, "%s: ioctl failed: %s\n", progname,
		    strerror(errno));
		free(buf);
		return (1);
	}
	if (nvlist_unpack(buf, ionv.len, &reply, 0) != 0) {
		(void) fprintf(stderr, "%s: failed to unpack reply\n",
		    progname);
		free(buf);
		return (1);
	}

	free(buf);
	*replyp = reply;
	return (0);
}

static int
cmd_create_subsys(int argc, char **argv)
{
	nvlist_t *nvl;
	const char *subnqn, *serial = NULL;
	long portid = -1;
	int c, fd, ret;

	while ((c = getopt(argc, argv, "s:p:")) != -1) {
		switch (c) {
		case 's':
			serial = optarg;
			break;
		case 'p': {
			char *end;
			portid = strtol(optarg, &end, 0);
			if (*end != '\0' || portid < 0 || portid > UINT16_MAX) {
				(void) fprintf(stderr,
				    "%s: invalid portid '%s'\n", progname,
				    optarg);
				return (2);
			}
			break;
		}
		default:
			usage();
		}
	}

	if (optind != argc - 1)
		usage();
	subnqn = argv[optind];

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0) {
		(void) fprintf(stderr, "%s: out of memory\n", progname);
		return (1);
	}
	(void) nvlist_add_string(nvl, NVMFT_NV_SUBNQN, subnqn);
	if (serial != NULL)
		(void) nvlist_add_string(nvl, NVMFT_NV_SERIAL, serial);
	if (portid >= 0)
		(void) nvlist_add_uint64(nvl, NVMFT_NV_PORTID,
		    (uint64_t)portid);

	fd = nvmft_open();
	if (fd < 0) {
		nvlist_free(nvl);
		return (1);
	}

	ret = nvmft_ioctl_send(fd, NVMFT_IOC_SUBSYS_CREATE, nvl);
	(void) close(fd);
	nvlist_free(nvl);

	if (ret == 0) {
		(void) printf("created subsystem %s\n", subnqn);
		(void) printf("next: stmfadm online-target -t nvmft %s\n",
		    subnqn);
	}
	return (ret);
}

static int
cmd_delete_subsys(int argc, char **argv)
{
	nvlist_t *nvl;
	const char *subnqn;
	int fd, ret;

	if (argc != 2)
		usage();
	subnqn = argv[1];

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0) {
		(void) fprintf(stderr, "%s: out of memory\n", progname);
		return (1);
	}
	(void) nvlist_add_string(nvl, NVMFT_NV_SUBNQN, subnqn);

	fd = nvmft_open();
	if (fd < 0) {
		nvlist_free(nvl);
		return (1);
	}

	ret = nvmft_ioctl_send(fd, NVMFT_IOC_SUBSYS_DELETE, nvl);
	(void) close(fd);
	nvlist_free(nvl);

	if (ret == 0)
		(void) printf("deleted subsystem %s\n", subnqn);
	return (ret);
}

static int
cmd_list_subsys(void)
{
	nvlist_t *reply = NULL;
	char **nqns = NULL;
	uint_t n = 0, i;
	int fd, ret;

	fd = nvmft_open();
	if (fd < 0)
		return (1);

	ret = nvmft_ioctl_recv(fd, NVMFT_IOC_SUBSYS_LIST, &reply);
	(void) close(fd);
	if (ret != 0)
		return (ret);

	if (reply != NULL && nvlist_lookup_string_array(reply,
	    NVMFT_NV_SUBNQNS, &nqns, &n) == 0) {
		for (i = 0; i < n; i++)
			(void) printf("%s\n", nqns[i]);
	}

	nvlist_free(reply);
	return (0);
}

int
main(int argc, char **argv)
{
	progname = argv[0];

	if (argc < 2)
		usage();

	if (strcmp(argv[1], "create-subsys") == 0)
		return (cmd_create_subsys(argc - 1, argv + 1));
	if (strcmp(argv[1], "delete-subsys") == 0)
		return (cmd_delete_subsys(argc - 1, argv + 1));
	if (strcmp(argv[1], "list-subsys") == 0)
		return (cmd_list_subsys());

	usage();
	return (2);
}

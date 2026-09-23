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
 * Exercise the ice(4D) firmware diagnostic ioctls on a live device: the
 * privilege checks, the size check, the debug dump cluster limits, and a
 * firmware log configuration read.  The link comes from $ICE_TEST_LINK.
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <priv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ice_ioctl.h"

static const char *link_path;
static int failures;

static int
diag(int fd, int cmd, void *arg, size_t len)
{
	struct strioctl sio;

	(void) memset(&sio, 0, sizeof (sio));
	sio.ic_cmd = cmd;
	sio.ic_timout = 0;
	sio.ic_len = (int)len;
	sio.ic_dp = arg;
	return (ioctl(fd, I_STR, &sio) == -1 ? errno : 0);
}

static void
expect(const char *what, int got, int want, int alt)
{
	if (got == want || (alt != 0 && got == alt)) {
		(void) printf("PASS: %s\n", what);
		return;
	}
	(void) printf("FAIL: %s: got %s, want %s\n", what, strerror(got),
	    strerror(want));
	failures++;
}

/*
 * Drop one privilege in a child and check that the driver refuses the
 * command.  The child exits with the errno it saw.
 */
static void
without(const char *priv)
{
	ice_ioc_fwlog_cfg_t cfg;
	char what[80];
	pid_t pid;
	int fd, status;

	if ((pid = fork()) == -1)
		err(EXIT_FAILURE, "fork");
	if (pid == 0) {
		if ((fd = open(link_path, O_RDWR)) == -1)
			_exit(255);
		if (priv_set(PRIV_OFF, PRIV_EFFECTIVE, priv, NULL) != 0)
			_exit(254);
		(void) memset(&cfg, 0, sizeof (cfg));
		_exit(diag(fd, ICE_IOC_FWLOG_GET, &cfg, sizeof (cfg)));
	}
	if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status))
		errx(EXIT_FAILURE, "child for %s did not exit", priv);

	(void) snprintf(what, sizeof (what), "FWLOG_GET without %s", priv);
	expect(what, WEXITSTATUS(status), EPERM, 0);
}

int
main(void)
{
	const char *link = getenv("ICE_TEST_LINK");
	static ice_ioc_fwdump_t dump;
	ice_ioc_fwlog_cfg_t cfg;
	char path[128];
	int fd, ret;

	if (link == NULL) {
		(void) printf("SKIP: ICE_TEST_LINK is not set\n");
		return (4);
	}
	(void) snprintf(path, sizeof (path), "/dev/net/%s", link);
	link_path = path;
	if ((fd = open(path, O_RDWR)) == -1)
		err(EXIT_FAILURE, "open %s", path);

	/* Firmware without logging support answers ENOTSUP. */
	(void) memset(&cfg, 0, sizeof (cfg));
	ret = diag(fd, ICE_IOC_FWLOG_GET, &cfg, sizeof (cfg));
	expect("FWLOG_GET module 0", ret, 0, ENOTSUP);
	if (ret == 0 && cfg.ifc_level > ICE_FWLOG_LEVEL_MAX) {
		(void) printf("FAIL: level %u out of range\n", cfg.ifc_level);
		failures++;
	}

	cfg.ifc_module = ICE_FWLOG_NMODULES;
	expect("FWLOG_GET bad module",
	    diag(fd, ICE_IOC_FWLOG_GET, &cfg, sizeof (cfg)), EINVAL, 0);
	expect("FWLOG_GET short buffer",
	    diag(fd, ICE_IOC_FWLOG_GET, &cfg, sizeof (cfg) - 1), EINVAL, 0);

	/* EMP DRAM (4) is never dumped. */
	(void) memset(&dump, 0, sizeof (dump));
	dump.ifd_cluster = 4;
	expect("FWDUMP refused cluster",
	    diag(fd, ICE_IOC_FWDUMP, &dump, sizeof (dump)), EINVAL, 0);

	/* The switch cluster is allowed on E810; E830 numbers start at 100. */
	(void) memset(&dump, 0, sizeof (dump));
	ret = diag(fd, ICE_IOC_FWDUMP, &dump, sizeof (dump));
	if (ret == EINVAL) {
		dump.ifd_cluster = 100;
		ret = diag(fd, ICE_IOC_FWDUMP, &dump, sizeof (dump));
	}
	expect("FWDUMP switch cluster", ret, 0, EIO);
	if (ret == 0 && dump.ifd_len > ICE_IOC_BUFSZ) {
		(void) printf("FAIL: dump length %u\n", dump.ifd_len);
		failures++;
	}

	without(PRIV_SYS_CONFIG);
	without(PRIV_SYS_DEVICES);

	(void) close(fd);
	return (failures == 0 ? 0 : 1);
}

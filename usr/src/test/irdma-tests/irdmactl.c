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
 * irdmactl: drive the irdma test ioctls from the hardware acceptance
 * script.
 *
 *	irdmactl <device> status
 *	irdmactl <device> probe | hold | release | reset
 *	irdmactl <device> irm-remove <n> | irm-add <n>
 *	irdmactl <device> wait [seconds]	wait for a probe to finish
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "../../uts/common/io/irdma/irdma_ioctl.h"

static int
status(int fd, irdma_ioc_status_t *st)
{
	bzero(st, sizeof (*st));
	return (ioctl(fd, IRDMA_IOC_STATUS, st));
}

static void
print_status(const irdma_ioc_status_t *st)
{
	(void) printf("progress=0x%x flags=0x%x gen=%u vectors=%u "
	    "cqp_submitted=%llu cqp_completed=%llu cqp_timeouts=%llu "
	    "test_busy=%u test_error=%d test_runs=%u sds=%u qps=%u "
	    "pbles=%u\n", st->irs_progress, st->irs_flags,
	    st->irs_generation, st->irs_vectors,
	    (unsigned long long)st->irs_cqp_submitted,
	    (unsigned long long)st->irs_cqp_completed,
	    (unsigned long long)st->irs_cqp_timeouts, st->irs_test_busy,
	    st->irs_test_error, st->irs_test_runs, st->irs_hmc_sds,
	    st->irs_qp_cnt, st->irs_pble_cnt);
}

static int
test(int fd, uint32_t action, uint32_t arg)
{
	irdma_ioc_test_t t;

	t.irt_action = action;
	t.irt_arg = arg;
	return (ioctl(fd, IRDMA_IOC_TEST, &t));
}

int
main(int argc, char **argv)
{
	irdma_ioc_status_t st;
	uint32_t arg = 0;
	int fd, i, secs;

	if (argc < 3)
		errx(2, "usage: irdmactl <device> <command> [arg]");
	if (argc > 3)
		arg = (uint32_t)strtoul(argv[3], NULL, 0);
	if ((fd = open(argv[1], O_RDWR)) < 0)
		err(1, "open %s", argv[1]);

	if (strcmp(argv[2], "status") == 0) {
		if (status(fd, &st) != 0)
			err(1, "status");
		print_status(&st);
	} else if (strcmp(argv[2], "probe") == 0) {
		if (test(fd, IRDMA_TEST_CQP_NOP, 0) != 0)
			err(1, "probe");
	} else if (strcmp(argv[2], "hold") == 0) {
		if (test(fd, IRDMA_TEST_HOLD_CQES, 0) != 0)
			err(1, "hold");
	} else if (strcmp(argv[2], "release") == 0) {
		if (test(fd, IRDMA_TEST_RELEASE_CQES, 0) != 0)
			err(1, "release");
	} else if (strcmp(argv[2], "reset") == 0) {
		if (test(fd, IRDMA_TEST_RESET, 0) != 0)
			err(1, "reset");
	} else if (strcmp(argv[2], "irm-remove") == 0) {
		if (test(fd, IRDMA_TEST_IRM_REMOVE, arg) != 0)
			err(1, "irm-remove");
	} else if (strcmp(argv[2], "irm-add") == 0) {
		if (test(fd, IRDMA_TEST_IRM_ADD, arg) != 0)
			err(1, "irm-add");
	} else if (strcmp(argv[2], "wait") == 0) {
		secs = arg != 0 ? (int)arg : 30;
		for (i = 0; i < secs * 10; i++) {
			if (status(fd, &st) != 0)
				err(1, "status");
			if (st.irs_test_busy == 0)
				break;
			(void) usleep(100000);
		}
		print_status(&st);
		if (st.irs_test_busy != 0)
			errx(1, "probe still running");
		(void) close(fd);
		return (st.irs_test_error == 0 ? 0 : 3);
	} else {
		errx(2, "unknown command %s", argv[2]);
	}
	(void) close(fd);
	return (0);
}

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

#ifndef _IRDMA_IOCTL_H
#define	_IRDMA_IOCTL_H

/*
 * Private ioctls on /devices/.../irdma@0:irdma, for the acceptance tests.
 * Only the global zone with {PRIV_SYS_DEVICES} may use them.
 *
 * IRDMA_IOC_STATUS	Read the bring-up progress, flags and counters.
 * IRDMA_IOC_TEST	Start one test action (irt_action).  Actions that
 *			wait on the device run on a taskq and return at once;
 *			IRDMA_IOC_STATUS reports their result.  Only DEBUG
 *			builds accept this command; others fail with ENOTSUP.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	IRDMA_IOC		(('I' << 24) | ('R' << 16) | ('D' << 8))
#define	IRDMA_IOC_STATUS	(IRDMA_IOC | 0x01)
#define	IRDMA_IOC_TEST		(IRDMA_IOC | 0x02)

typedef enum irdma_test_action {
	IRDMA_TEST_CQP_NOP = 1,		/* one CQP command, on the taskq */
	IRDMA_TEST_HOLD_CQES,		/* stop consuming CCQ entries */
	IRDMA_TEST_RELEASE_CQES,
	IRDMA_TEST_RESET,		/* ask ice for a PF reset */
	IRDMA_TEST_IRM_REMOVE,		/* irt_arg LAN vectors */
	IRDMA_TEST_IRM_ADD
} irdma_test_action_t;

typedef struct irdma_ioc_test {
	uint32_t	irt_action;
	uint32_t	irt_arg;
} irdma_ioc_test_t;

typedef struct irdma_ioc_status {
	uint32_t	irs_progress;	/* bit per irdma_step_t */
	uint32_t	irs_flags;
	uint32_t	irs_generation;
	uint32_t	irs_vectors;
	uint64_t	irs_cqp_submitted;
	uint64_t	irs_cqp_completed;
	uint64_t	irs_cqp_timeouts;
	uint32_t	irs_test_busy;	/* a taskq action is running */
	int32_t		irs_test_error;	/* errno of the last action */
	uint32_t	irs_test_runs;
	uint32_t	irs_hmc_sds;
	uint32_t	irs_qp_cnt;
	uint32_t	irs_pble_cnt;
} irdma_ioc_status_t;

#ifdef __cplusplus
}
#endif

#endif /* _IRDMA_IOCTL_H */

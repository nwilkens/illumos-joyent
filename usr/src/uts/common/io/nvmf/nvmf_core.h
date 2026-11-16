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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/nvmf.h.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * The default settings of Fabrics hosts and controllers.  The ioctls and
 * their nvlists are in <sys/nvme/nvmf_ioctl.h>.
 */

#ifndef	_NVMF_CORE_H
#define	_NVMF_CORE_H

#include <sys/types.h>
#include <sys/nvme/nvmf_ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Default settings in Fabrics controllers.  These match values used by the
 * Linux target.
 */
#define	NVMF_MAX_IO_ENTRIES	(1024)
#define	NVMF_CC_EN_TIMEOUT	(15)	/* In 500ms units */

/* Allows for a 16k data buffer + SQE */
#define	NVMF_IOCCSZ		(sizeof (nvme_sqe_t) + 16 * 1024)
#define	NVMF_IORCSZ		(sizeof (nvme_cqe_t))

#define	NVMF_NN			(1024)

/*
 * Default settings for Fabrics hosts.  These match values used by
 * Linux.
 */
#define	NVMF_DEFAULT_RECONNECT_DELAY	10
#define	NVMF_DEFAULT_CONTROLLER_LOSS	600
#define	NVMF_DEFAULT_IO_ENTRIES		128

#ifdef __cplusplus
}
#endif

#endif /* _NVMF_CORE_H */

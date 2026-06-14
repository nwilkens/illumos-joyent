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
 * This header holds the shared Fabrics host/controller default settings and
 * the user/kernel handoff interface (ioctl command numbers and the packed
 * nvlist carrier struct).  In FreeBSD all of this lived in a single
 * sys/dev/nvmf/nvmf.h; in this port the on-wire spec structures were split out
 * into <sys/nvme/nvmf.h>, so what remains here is the driver-private control
 * interface.  The nvlist schema descriptions (which keys appear in each handoff
 * nvlist) are reproduced verbatim because they are the contract between the
 * user-space nvmf daemon and these kernel modules.
 */

#ifndef	_NVMF_CORE_H
#define	_NVMF_CORE_H

#include <sys/types.h>
#include <sys/nvme/nvmf_ioctl.h>	/* struct nvmf_ioc_nv, NVMF_IOC */

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

/*
 * struct nvmf_ioc_nv (the packed-nvlist carrier) is defined in the shared
 * <sys/nvme/nvmf_ioctl.h> included above so userland can use it too.
 */

/*
 * The fields in a qpair handoff nvlist are:
 *
 * Transport independent:
 *
 * bool		admin
 * bool		sq_flow_control
 * number	qsize
 * number	sqhd
 * number	sqtail			host only
 *
 * TCP transport:
 *
 * number	fd
 * number	rxpda
 * number	txpda
 * bool		header_digests
 * bool		data_digests
 * number	maxr2t
 * number	maxh2cdata
 * number	max_icd
 */

/*
 * The fields in the nvlist for NVMF_HANDOFF_HOST and
 * NVMF_RECONNECT_HOST are:
 *
 * number			trtype
 * number			kato	(optional)
 * number			reconnect_delay (optional)
 * number			controller_loss_timeout (optional)
 * qpair handoff nvlist		admin
 * qpair handoff nvlist array	io
 * binary			cdata	struct nvme_controller_data
 * NVMF_RECONNECT_PARAMS nvlist	rparams
 */

/*
 * The fields in the nvlist for NVMF_RECONNECT_PARAMS are:
 *
 * binary			dle	struct nvme_discovery_log_entry
 * string			hostnqn
 * number			num_io_queues
 * number			kato	(optional)
 * number			reconnect_delay (optional)
 * number			controller_loss_timeout (optional)
 * number			io_qsize
 * bool				sq_flow_control
 *
 * TCP transport:
 *
 * bool				header_digests
 * bool				data_digests
 */

/*
 * The fields in the nvlist for NVMF_CONNECTION_STATUS are:
 *
 * bool				connected
 * timespec nvlist		last_disconnect
 *  number			tv_sec
 *  number			tv_nsec
 */

/*
 * The fields in the nvlist for handing off a controller qpair are:
 *
 * number			trtype
 * qpair handoff nvlist		params
 * binary			cmd	struct nvmf_fabric_connect_cmd
 * binary			data	struct nvmf_fabric_connect_data
 */

/*
 * ioctl command numbers.
 *
 * FreeBSD encodes these with _IOW/_IOWR/_IO from <sys/ioccom.h>.  illumos uses
 * a flat command-number convention for driver ioctls; the magic and ordinals
 * are preserved from the FreeBSD definitions so the user/kernel ABI keeps the
 * same logical command set.  The argument for the handoff/reconnect commands is
 * always a struct nvmf_ioc_nv carrying a packed nvlist.  NVMF_IOC (the command
 * magic) is defined in the shared <sys/nvme/nvmf_ioctl.h>.
 */

/* Operations on /dev/nvmf */
#define	NVMF_HANDOFF_HOST	(NVMF_IOC | 200)
#define	NVMF_DISCONNECT_HOST	(NVMF_IOC | 201)
#define	NVMF_DISCONNECT_ALL	(NVMF_IOC | 202)

/* Operations on /dev/nvmeX */
#define	NVMF_RECONNECT_PARAMS	(NVMF_IOC | 203)
#define	NVMF_RECONNECT_HOST	(NVMF_IOC | 204)
#define	NVMF_CONNECTION_STATUS	(NVMF_IOC | 205)

#ifdef __cplusplus
}
#endif

#endif /* _NVMF_CORE_H */

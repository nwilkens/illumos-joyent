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
 * Provenance: ported to illumos from FreeBSD
 * tools/tools/nvmf/nvmfd/io.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * I/O subsystem listener.  This is the always-kernel-handoff variant of the
 * FreeBSD daemon: every accepted association runs the Fabrics CONNECT
 * handshake in userland (libnvmf nvmf_accept) and is then handed to the kernel
 * nvmft COMSTAR/STMF port provider.  The FreeBSD userland I/O controller (the
 * per-association admin/I/O queue threads backed by a block device) is dropped
 * because on illumos the namespace data always lives behind STMF.
 */

#include <err.h>
#include <errno.h>
#include <libnvmf.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

/*
 * Admin/I/O queue-size advertisements.  These are not carried in the public
 * headers; mirror the values libnvmf and the kernel nvmft port provider use
 * (FreeBSD: NVME_MAX_ADMIN_ENTRIES, NVMF_MAX_IO_ENTRIES).
 */
#ifndef	NVME_MAX_ADMIN_ENTRIES
#define	NVME_MAX_ADMIN_ENTRIES	4096
#endif
#ifndef	NVMF_MAX_IO_ENTRIES
#define	NVMF_MAX_IO_ENTRIES	1024
#endif

static struct nvmf_association *io_na;
static const char *nqn;

void
init_io(const char *subnqn)
{
	nvmf_association_params_t aparams;

	(void) memset(&aparams, 0, sizeof (aparams));
	aparams.nap_sq_flow_control = !flow_control_disable;
	aparams.nap_dynamic_controller_model = true;
	aparams.nap_max_admin_qsize = NVME_MAX_ADMIN_ENTRIES;
	aparams.nap_max_io_qsize = NVMF_MAX_IO_ENTRIES;
	aparams.nap_tcp.pda = 0;
	aparams.nap_tcp.header_digests = header_digests;
	aparams.nap_tcp.data_digests = data_digests;
	aparams.nap_tcp.maxh2cdata = maxh2cdata;
	io_na = nvmf_allocate_association(NVMF_TRTYPE_TCP, true, &aparams);
	if (io_na == NULL)
		err(1, "Failed to create I/O controller association");

	nqn = subnqn;

	init_nvmft(subnqn, &aparams);
}

void
shutdown_io(void)
{
	shutdown_nvmft(nqn);
}

static void *
io_socket_thread(void *arg)
{
	nvmf_fabric_connect_data_t data;
	struct nvmf_qpair_params qparams;
	struct nvmf_capsule *nc;
	struct nvmf_qpair *qp;
	int s;

	(void) pthread_detach(pthread_self());

	s = (int)(intptr_t)arg;
	(void) memset(&qparams, 0, sizeof (qparams));
	qparams.nqp_tcp.fd = s;

	nc = NULL;
	qp = nvmf_accept(io_na, &qparams, &nc, &data);
	if (qp == NULL) {
		warnx("Failed to create I/O qpair: %s",
		    nvmf_association_error(io_na));
		goto out;
	}

	/*
	 * Hand the established association to the kernel.  The kernel looks up
	 * the target port by the SubNQN carried in the CONNECT data, so the
	 * daemon does not validate the SubNQN here (an unknown SubNQN is
	 * rejected by the handoff ioctl).  The kernel adopts the socket during
	 * the ioctl (getf/ksocket_hold/releasef), taking its own reference, so
	 * this thread closes its userland fd afterwards regardless of outcome.
	 */
	nvmft_handoff_qpair(qp, nvmf_capsule_sqe(nc), &data);

out:
	if (nc != NULL)
		nvmf_free_capsule(nc);
	if (qp != NULL)
		nvmf_free_qpair(qp);
	(void) close(s);
	return (NULL);
}

void
handle_io_socket(int s)
{
	pthread_t thr;
	int error;

	error = pthread_create(&thr, NULL, io_socket_thread,
	    (void *)(intptr_t)s);
	if (error != 0) {
		warnc(error, "Failed to create I/O qpair thread");
		(void) close(s);
	}
}

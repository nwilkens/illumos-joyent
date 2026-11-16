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
 * Connections to the I/O port: the Connect exchange runs here and the queue
 * pair is then handed to the nvmft port provider.
 */

#include <errno.h>
#include <libnvmf.h>
#include <stddef.h>
#include <string.h>
#include <syslog.h>

#include "internal.h"

#define	NVMFD_MAX_ADMIN_ENTRIES	4096
#define	NVMFD_MAX_IO_ENTRIES	1024

void
io_connection(nvmfd_conn_t *conn)
{
	nvmf_association_params_t aparams;
	nvmf_fabric_connect_data_t data;
	nvmf_qpair_params_t qparams;
	struct nvmf_association *na;
	struct nvmf_capsule *nc = NULL;
	struct nvmf_qpair *qp = NULL;
	char nqn[NVMF_NQN_FIELD_SIZE];
	bool refused = false;
	int error;

	(void) memset(&aparams, 0, sizeof (aparams));
	aparams.nap_sq_flow_control = !flow_control_disable;
	aparams.nap_dynamic_controller_model = true;
	aparams.nap_max_admin_qsize = NVMFD_MAX_ADMIN_ENTRIES;
	aparams.nap_max_io_qsize = NVMFD_MAX_IO_ENTRIES;
	aparams.nap_tcp.pda = 0;
	aparams.nap_tcp.header_digests = header_digests;
	aparams.nap_tcp.data_digests = data_digests;
	aparams.nap_tcp.maxh2cdata = maxh2cdata;
	na = nvmf_allocate_association(NVMF_TRTYPE_TCP, true, &aparams);
	if (na == NULL) {
		nvmfd_log(LOG_ERR, "Failed to create I/O association");
		return;
	}

	(void) memset(&qparams, 0, sizeof (qparams));
	qparams.nqp_tcp.fd = conn->nc_fd;
	qparams.nqp_timeout_ms = NVMFD_IO_TIMEOUT;
	qp = nvmf_accept(na, &qparams, &nc, &data);
	if (qp == NULL) {
		nvmfd_peer_log(conn, "I/O port handshake failed: %s",
		    nvmf_association_error(na) != NULL ?
		    nvmf_association_error(na) : "unknown error");
		goto out;
	}

	/*
	 * Once the kernel has the socket it sends the Connect response.  If
	 * the handoff failed it has not touched the socket, and the refusal
	 * is this daemon's to send.  ENOENT is for the subsystem.
	 */
	error = nvmft_handoff_qpair(qp, nvmf_capsule_sqe(nc), &data, &refused);
	if (error == ENOENT) {
		nvmf_printable(nqn, sizeof (nqn), data.nfcd_subnqn,
		    sizeof (data.nfcd_subnqn));
		nvmfd_peer_log(conn, "Connect to unknown subsystem %s", nqn);
		nvmf_connect_invalid_parameters(nc, true,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_subnqn));
	} else if (error != 0) {
		nvmfd_peer_log(conn, "Handoff to the kernel failed: %s",
		    strerror(error));
		(void) nvmf_send_generic_error(nc,
		    NVME_CQE_SC_GEN_INTERNAL_ERR);
	} else if (refused) {
		nvmfd_peer_log(conn, "The kernel refused the Connect");
	}

out:
	if (nc != NULL)
		nvmf_free_capsule(nc);
	if (qp != NULL)
		nvmf_free_qpair(qp);
	nvmf_free_association(na);
}

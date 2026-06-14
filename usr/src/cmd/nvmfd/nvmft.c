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
 * Provenance: replaces FreeBSD tools/tools/nvmf/nvmfd/ctl.c.
 *
 * FreeBSD handed an established NVMe-oF association to the in-kernel CTL
 * frontend via /dev/cam/ctl (CTL_PORT_REQ to create the port, then CTL_NVMF /
 * CTL_NVMF_HANDOFF per qpair).  On illumos the target is the nvmft COMSTAR/STMF
 * port provider, so:
 *
 *   - The subsystem port (one stmf_local_port_t per SubNQN) and its namespace
 *     (LU) view entries are created out of band with sbdadm(8)/stmfadm(8);
 *     this daemon does NOT create or remove ports.  init_nvmft() therefore only
 *     opens the control device and shutdown_nvmft() only closes it.
 *
 *   - Each accepted, CONNECT-negotiated qpair is handed off with
 *     NVMFT_IOC_HANDOFF on the nvmft control device (NVMFT_DEV).  The handoff
 *     payload is the same packed nvlist libnvmf builds for the FreeBSD path
 *     (nvmf_handoff_controller_qpair): { trtype, params, cmd, data }.  The
 *     kernel reads the SubNQN from the CONNECT "data" to find the target port
 *     and dispatches admin vs I/O on the CONNECT command's queue id.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libnvmf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/nvme/nvmf_ioctl.h>

#include "internal.h"

static int nvmft_fd = -1;

static void
open_nvmft(void)
{
	if (nvmft_fd >= 0)
		return;

	nvmft_fd = open(NVMFT_DEV, O_RDWR);
	if (nvmft_fd == -1)
		err(1, "Failed to open %s (is the nvmft driver loaded?)",
		    NVMFT_DEV);
}

/*
 * On illumos the subsystem port is owned by STMF and created with stmfadm, so
 * there is nothing to create here.  Open the control device up front so a
 * misconfiguration (nvmft not attached) is reported at start-up rather than on
 * the first connection.
 */
void
init_nvmft(const char *subnqn __unused,
    const nvmf_association_params_t *params __unused)
{
	open_nvmft();
}

void
shutdown_nvmft(const char *subnqn __unused)
{
	if (nvmft_fd >= 0) {
		(void) close(nvmft_fd);
		nvmft_fd = -1;
	}
}

void
nvmft_handoff_qpair(struct nvmf_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data)
{
	struct nvmf_ioc_nv nv;
	int error;

	open_nvmft();

	(void) memset(&nv, 0, sizeof (nv));

	/*
	 * libnvmf packs the qpair (socket fd + negotiated transport/CONNECT
	 * parameters) plus the CONNECT cmd/data into a packed nvlist; nv.data
	 * points at the malloc'd buffer and nv.size holds its length.  This is
	 * identical to the FreeBSD CTL_NVMF_HANDOFF payload.
	 */
	error = nvmf_handoff_controller_qpair(qp, cmd, data, &nv);
	if (error != 0) {
		warnc(error, "Failed to prepare qpair for handoff");
		return;
	}

	/*
	 * libnvmf reports the packed length in nv.size (from nvlist_pack); the
	 * kernel copies in nv.size bytes.  Mirror it into nv.len so both fields
	 * carry the packed length, matching the carrier convention used by the
	 * other nvmft control ioctls.
	 */
	if (nv.size == 0)
		nv.size = nv.len;
	if (nv.len == 0)
		nv.len = nv.size;

	if (ioctl(nvmft_fd, NVMFT_IOC_HANDOFF, &nv) != 0)
		warn("ioctl(NVMFT_IOC_HANDOFF)");

	free(nv.data);
}

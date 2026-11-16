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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/nvmf_host.c.
 *
 * Original: Copyright (c) 2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Hand a connected host association to the nvmf_host driver, and the
 * disconnect, reconnect, status and list requests on its control node.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libnvmf.h"
#include "internal.h"

/*
 * Control device for the Fabrics host (initiator) driver.  The kernel
 * nvmf_host driver creates the minor "nvmf" on its single pseudo instance
 * (nvmf_host@0); there is no /dev devlink, so the handoff/disconnect/reconnect
 * wrappers open the /devices node directly, the same way nvmfadm reaches the
 * target via /devices/pseudo/nvmft@0:admin.
 */
#define	NVMF_HOST_DEV	"/devices/pseudo/nvmf_host@0:nvmf"

static bool
is_queue_pair_idle(struct nvmf_qpair *qp)
{
	if (qp->nq_sqhd != qp->nq_sqtail)
		return (false);
	if (!TAILQ_EMPTY(&qp->nq_rx_capsules))
		return (false);
	return (true);
}

static int
prepare_queues_for_handoff(struct nvmf_ioc_nv *nv,
    const nvmf_discovery_log_page_entry_t *dle, const char *hostnqn,
    struct nvmf_qpair *admin_qp, u_int num_queues,
    struct nvmf_qpair **io_queues, const nvme_identify_ctrl_t *cdata,
    uint32_t reconnect_delay, uint32_t controller_loss_timeout)
{
	const struct nvmf_association *na = admin_qp->nq_association;
	nvlist_t *nvl, *nvl_qp, *nvl_rparams, **io_nvls;
	u_int i;
	int error;

	if (num_queues == 0)
		return (EINVAL);

	/* Ensure trtype matches. */
	if (dle->ndle_trtype != na->na_trtype)
		return (EINVAL);

	/* All queue pairs must be idle. */
	if (!is_queue_pair_idle(admin_qp))
		return (EBUSY);
	for (i = 0; i < num_queues; i++) {
		if (!is_queue_pair_idle(io_queues[i]))
			return (EBUSY);
	}

	/* Fill out reconnect parameters. */
	if ((error = nvlist_alloc(&nvl_rparams, NV_UNIQUE_NAME, 0)) != 0)
		return (error);
	(void) nvlist_add_byte_array(nvl_rparams, "dle",
	    (uchar_t *)(uintptr_t)dle, sizeof (*dle));
	(void) nvlist_add_string(nvl_rparams, "hostnqn", hostnqn);
	(void) nvlist_add_uint64(nvl_rparams, "num_io_queues", num_queues);
	(void) nvlist_add_uint64(nvl_rparams, "kato", admin_qp->nq_kato);
	(void) nvlist_add_uint64(nvl_rparams, "reconnect_delay",
	    reconnect_delay);
	(void) nvlist_add_uint64(nvl_rparams, "controller_loss_timeout",
	    controller_loss_timeout);
	(void) nvlist_add_uint64(nvl_rparams, "io_qsize",
	    io_queues[0]->nq_qsize);
	(void) nvlist_add_boolean_value(nvl_rparams, "sq_flow_control",
	    na->na_params.nap_sq_flow_control);
	switch (na->na_trtype) {
	case NVMF_TRTYPE_TCP:
		(void) nvlist_add_boolean_value(nvl_rparams, "header_digests",
		    na->na_params.nap_tcp.header_digests);
		(void) nvlist_add_boolean_value(nvl_rparams, "data_digests",
		    na->na_params.nap_tcp.data_digests);
		break;
	default:
		nvlist_free(nvl_rparams);
		return (EINVAL);
	}

	if ((error = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0) {
		nvlist_free(nvl_rparams);
		return (error);
	}
	(void) nvlist_add_uint64(nvl, "trtype", na->na_trtype);
	(void) nvlist_add_uint64(nvl, "kato", admin_qp->nq_kato);
	(void) nvlist_add_uint64(nvl, "reconnect_delay", reconnect_delay);
	(void) nvlist_add_uint64(nvl, "controller_loss_timeout",
	    controller_loss_timeout);
	(void) nvlist_add_nvlist(nvl, "rparams", nvl_rparams);
	nvlist_free(nvl_rparams);

	/* First, the admin queue. */
	error = nvmf_kernel_handoff_params(admin_qp, &nvl_qp);
	if (error) {
		nvlist_free(nvl);
		return (error);
	}
	(void) nvlist_add_nvlist(nvl, "admin", nvl_qp);
	nvlist_free(nvl_qp);

	/*
	 * Next, the I/O queues.  illumos nvlists have no append-array
	 * primitive (FreeBSD uses nvlist_append_nvlist_array), and the kernel
	 * host reads "io" with a single nvlist_lookup_nvlist_array(), so build
	 * the whole array of qpair nvlists first and add it in one call.
	 */
	io_nvls = calloc(num_queues, sizeof (*io_nvls));
	if (io_nvls == NULL) {
		nvlist_free(nvl);
		return (ENOMEM);
	}
	for (i = 0; i < num_queues; i++) {
		error = nvmf_kernel_handoff_params(io_queues[i], &io_nvls[i]);
		if (error) {
			while (i-- > 0)
				nvlist_free(io_nvls[i]);
			free(io_nvls);
			nvlist_free(nvl);
			return (error);
		}
	}
	(void) nvlist_add_nvlist_array(nvl, "io", io_nvls, num_queues);
	for (i = 0; i < num_queues; i++)
		nvlist_free(io_nvls[i]);
	free(io_nvls);

	(void) nvlist_add_byte_array(nvl, "cdata",
	    (uchar_t *)(uintptr_t)cdata, sizeof (*cdata));

	error = nvmf_pack_ioc_nvlist(nv, nvl);
	nvlist_free(nvl);
	return (error);
}

int
nvmf_handoff_host(const nvmf_discovery_log_page_entry_t *dle,
    const char *hostnqn, struct nvmf_qpair *admin_qp, u_int num_queues,
    struct nvmf_qpair **io_queues, const nvme_identify_ctrl_t *cdata,
    uint32_t reconnect_delay, uint32_t controller_loss_timeout)
{
	struct nvmf_ioc_nv nv;
	u_int i;
	int error, fd;

	fd = open(NVMF_HOST_DEV, O_RDWR);
	if (fd == -1) {
		error = errno;
		goto out;
	}

	error = prepare_queues_for_handoff(&nv, dle, hostnqn, admin_qp,
	    num_queues, io_queues, cdata, reconnect_delay,
	    controller_loss_timeout);
	if (error != 0)
		goto out;

	if (ioctl(fd, NVMF_HANDOFF_HOST, &nv) == -1)
		error = errno;
	free(nv.data);

out:
	if (fd >= 0)
		(void) close(fd);
	for (i = 0; i < num_queues; i++)
		nvmf_free_qpair(io_queues[i]);
	nvmf_free_qpair(admin_qp);
	return (error);
}

int
nvmf_disconnect_host(const char *host)
{
	int error, fd;

	error = 0;
	fd = open(NVMF_HOST_DEV, O_RDWR);
	if (fd == -1) {
		error = errno;
		goto out;
	}

	if (ioctl(fd, NVMF_DISCONNECT_HOST, &host) == -1)
		error = errno;

out:
	if (fd >= 0)
		(void) close(fd);
	return (error);
}

int
nvmf_disconnect_all(void)
{
	int error, fd;

	error = 0;
	fd = open(NVMF_HOST_DEV, O_RDWR);
	if (fd == -1) {
		error = errno;
		goto out;
	}

	if (ioctl(fd, NVMF_DISCONNECT_ALL) == -1)
		error = errno;

out:
	if (fd >= 0)
		(void) close(fd);
	return (error);
}

static int
nvmf_read_ioc_nv(int fd, int com, nvlist_t **nvlp)
{
	struct nvmf_ioc_nv nv;
	nvlist_t *nvl;
	int error;

	(void) memset(&nv, 0, sizeof (nv));
	if (ioctl(fd, com, &nv) == -1)
		return (errno);

	nv.data = malloc(nv.len);
	if (nv.data == NULL)
		return (ENOMEM);
	nv.size = nv.len;
	if (ioctl(fd, com, &nv) == -1) {
		error = errno;
		free(nv.data);
		return (error);
	}

	error = nvlist_unpack(nv.data, nv.len, &nvl, 0);
	free(nv.data);
	if (error != 0)
		return (error);

	*nvlp = nvl;
	return (0);
}

int
nvmf_reconnect_params(int fd, nvlist_t **nvlp)
{
	return (nvmf_read_ioc_nv(fd, NVMF_RECONNECT_PARAMS, nvlp));
}

int
nvmf_list_controller(nvlist_t **nvlp)
{
	int error, fd;

	fd = open(NVMF_HOST_DEV, O_RDWR);
	if (fd == -1)
		return (errno);

	error = nvmf_read_ioc_nv(fd, NVMF_LIST_CONTROLLER, nvlp);
	(void) close(fd);
	return (error);
}

int
nvmf_reconnect_host(int fd, const nvmf_discovery_log_page_entry_t *dle,
    const char *hostnqn, struct nvmf_qpair *admin_qp, u_int num_queues,
    struct nvmf_qpair **io_queues, const nvme_identify_ctrl_t *cdata,
    uint32_t reconnect_delay, uint32_t controller_loss_timeout)
{
	struct nvmf_ioc_nv nv;
	u_int i;
	int error;

	error = prepare_queues_for_handoff(&nv, dle, hostnqn, admin_qp,
	    num_queues, io_queues, cdata, reconnect_delay,
	    controller_loss_timeout);
	if (error != 0)
		goto out;

	if (ioctl(fd, NVMF_RECONNECT_HOST, &nv) == -1)
		error = errno;
	free(nv.data);

out:
	for (i = 0; i < num_queues; i++)
		nvmf_free_qpair(io_queues[i]);
	nvmf_free_qpair(admin_qp);
	return (error);
}

int
nvmf_connection_status(int fd, nvlist_t **nvlp)
{
	return (nvmf_read_ioc_nv(fd, NVMF_CONNECTION_STATUS, nvlp));
}

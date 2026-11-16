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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/host/nvmf_cmd.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Fabric and admin command builders for the host, and the helpers that wait
 * for a command.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

#include "nvmf_var.h"
#include "../nvme/nvme_reg.h"
#include "../nvmf/nvmf_transport_internal.h"

#ifndef	NVME_OPC_FABRICS_COMMANDS
#define	NVME_OPC_FABRICS_COMMANDS	0x7f
#endif

static kmutex_t nvmf_status_lock;
static kcondvar_t nvmf_status_cv;

void
nvmf_cmd_init(void)
{
	mutex_init(&nvmf_status_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&nvmf_status_cv, NULL, CV_DRIVER, NULL);
}

void
nvmf_cmd_fini(void)
{
	cv_destroy(&nvmf_status_cv);
	mutex_destroy(&nvmf_status_lock);
}

/*
 * Send one admin command.  data, when not NULL, receives len bytes of
 * controller data.
 */
static boolean_t
nvmf_cmd_admin(nvmf_softc_t *sc, void *sqe, void *data, size_t len,
    nvmf_request_complete_t *req_cb, void *req_cb_arg,
    nvmf_io_complete_t *io_cb, void *io_cb_arg, int how)
{
	struct nvmf_host_qpair *qp;
	nvmf_memdesc_t mem;
	nvmf_request_t *req;

	qp = nvmf_admin_hold(sc);
	if (qp == NULL)
		return (B_FALSE);

	req = nvmf_allocate_request(qp, sqe, req_cb, req_cb_arg, how);
	if (req == NULL) {
		nvmf_qp_rele(sc);
		return (B_FALSE);
	}
	if (data != NULL) {
		mem.nmd_type = NVMF_MEMDESC_VADDR;
		mem.nmd_len = len;
		mem.nmd_u.nmd_vaddr = data;
		VERIFY0(nvmf_capsule_append_data(req->nc, &mem, len, B_FALSE,
		    io_cb, io_cb_arg));
	}
	nvmf_submit_request(req);
	nvmf_qp_rele(sc);
	return (B_TRUE);
}

boolean_t
nvmf_cmd_get_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    nvmf_request_complete_t *cb, void *cb_arg, int how)
{
	nvmf_fabric_prop_get_cmd_t cmd;

	bzero(&cmd, sizeof (cmd));
	cmd.nfpg_opcode = NVME_OPC_FABRICS_COMMANDS;
	cmd.nfpg_fctype = NVMF_FCTYPE_PROPERTY_GET;
	switch (size) {
	case 4:
		cmd.nfpg_attrib.size = NVMF_PROP_SIZE_4;
		break;
	case 8:
		cmd.nfpg_attrib.size = NVMF_PROP_SIZE_8;
		break;
	default:
		panic("Invalid property size");
	}
	cmd.nfpg_ofst = offset;

	return (nvmf_cmd_admin(sc, &cmd, NULL, 0, cb, cb_arg, NULL, NULL,
	    how));
}

boolean_t
nvmf_cmd_set_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    uint64_t value, nvmf_request_complete_t *cb, void *cb_arg, int how)
{
	nvmf_fabric_prop_set_cmd_t cmd;

	bzero(&cmd, sizeof (cmd));
	cmd.nfps_opcode = NVME_OPC_FABRICS_COMMANDS;
	cmd.nfps_fctype = NVMF_FCTYPE_PROPERTY_SET;
	switch (size) {
	case 4:
		cmd.nfps_attrib.size = NVMF_PROP_SIZE_4;
		cmd.nfps_value.u32.low = (uint32_t)value;
		break;
	case 8:
		cmd.nfps_attrib.size = NVMF_PROP_SIZE_8;
		cmd.nfps_value.u64 = value;
		break;
	default:
		panic("Invalid property size");
	}
	cmd.nfps_ofst = offset;

	return (nvmf_cmd_admin(sc, &cmd, NULL, 0, cb, cb_arg, NULL, NULL,
	    how));
}

boolean_t
nvmf_cmd_keep_alive(nvmf_softc_t *sc, nvmf_request_complete_t *cb, void *cb_arg,
    int how)
{
	nvme_sqe_t cmd;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_KEEP_ALIVE;

	return (nvmf_cmd_admin(sc, &cmd, NULL, 0, cb, cb_arg, NULL, NULL,
	    how));
}

boolean_t
nvmf_cmd_identify_active_namespaces(nvmf_softc_t *sc, uint32_t id,
    nvme_identify_nsid_list_t *nslist, nvmf_request_complete_t *req_cb,
    void *req_cb_arg, nvmf_io_complete_t *io_cb, void *io_cb_arg, int how)
{
	nvme_sqe_t cmd;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_IDENTIFY;

	/* 5.15.1 Use CNS of 0x02 for the active namespace ID list. */
	cmd.sqe_cdw10 = 2;
	cmd.sqe_nsid = id;

	return (nvmf_cmd_admin(sc, &cmd, nslist, sizeof (*nslist), req_cb,
	    req_cb_arg, io_cb, io_cb_arg, how));
}

boolean_t
nvmf_cmd_identify_namespace(nvmf_softc_t *sc, uint32_t id,
    nvme_identify_nsid_t *nsdata, nvmf_request_complete_t *req_cb,
    void *req_cb_arg, nvmf_io_complete_t *io_cb, void *io_cb_arg, int how)
{
	nvme_sqe_t cmd;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_IDENTIFY;

	/* 5.15.1 Use CNS of 0x00 for namespace data. */
	cmd.sqe_cdw10 = 0;
	cmd.sqe_nsid = id;

	return (nvmf_cmd_admin(sc, &cmd, nsdata, sizeof (*nsdata), req_cb,
	    req_cb_arg, io_cb, io_cb_arg, how));
}

boolean_t
nvmf_cmd_get_log_page(nvmf_softc_t *sc, uint32_t nsid, uint8_t lid,
    uint64_t offset, void *page, size_t len, nvmf_request_complete_t *req_cb,
    void *req_cb_arg, nvmf_io_complete_t *io_cb, void *io_cb_arg, int how)
{
	nvme_sqe_t cmd;
	size_t numd;

	ASSERT(len != 0 && len % 4 == 0);
	ASSERT(offset % 4 == 0);

	numd = (len / 4) - 1;
	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_GET_LOG_PAGE;
	cmd.sqe_nsid = nsid;
	cmd.sqe_cdw10 = (uint32_t)(numd << 16 | lid);
	cmd.sqe_cdw11 = (uint32_t)(numd >> 16);
	cmd.sqe_cdw12 = (uint32_t)offset;
	cmd.sqe_cdw13 = (uint32_t)(offset >> 32);

	return (nvmf_cmd_admin(sc, &cmd, page, len, req_cb, req_cb_arg, io_cb,
	    io_cb_arg, how));
}

void
nvmf_complete(void *arg, const nvme_cqe_t *cqe)
{
	nvmf_completion_status_t *status = arg;

	status->cqe = *cqe;
	mutex_enter(&nvmf_status_lock);
	status->done = B_TRUE;
	cv_broadcast(&nvmf_status_cv);
	mutex_exit(&nvmf_status_lock);
}

void
nvmf_io_complete(void *arg, size_t xfered, int error)
{
	nvmf_completion_status_t *status = arg;

	status->io_error = error;
	status->io_xfered = xfered;
	mutex_enter(&nvmf_status_lock);
	status->io_done = B_TRUE;
	cv_broadcast(&nvmf_status_cv);
	mutex_exit(&nvmf_status_lock);
}

/*
 * Wait for a synchronous admin command.  The status lives on the caller's
 * stack, so a command that outlives secs seconds is aborted by shutting the
 * admin queue down and the wait continues until that abort arrives.
 */
void
nvmf_wait_for_reply(nvmf_softc_t *sc, nvmf_completion_status_t *status,
    uint_t secs)
{
	boolean_t expired = B_FALSE;
	clock_t deadline;

	deadline = ddi_get_lbolt() + drv_usectohz((clock_t)secs * MICROSEC);

	mutex_enter(&nvmf_status_lock);
	while (!status->done || !status->io_done) {
		if (expired) {
			cv_wait(&nvmf_status_cv, &nvmf_status_lock);
			continue;
		}
		if (cv_timedwait(&nvmf_status_cv, &nvmf_status_lock,
		    deadline) != -1)
			continue;

		expired = B_TRUE;
		mutex_exit(&nvmf_status_lock);
		dev_err(sc->dip, CE_WARN,
		    "!admin command timed out, disconnecting");
		nvmf_disconnect(sc);
		nvmf_shutdown_admin(sc);
		mutex_enter(&nvmf_status_lock);
	}
	mutex_exit(&nvmf_status_lock);
}

/* True if the command succeeded and moved exactly len bytes of data. */
boolean_t
nvmf_status_ok(const nvmf_completion_status_t *status, size_t len)
{
	return (!nvmf_cqe_failed(&status->cqe) && status->io_error == 0 &&
	    status->io_xfered == len);
}

int
nvmf_read_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    uint64_t *value, uint_t secs)
{
	const nvmf_fabric_prop_get_rsp_t *rsp;
	nvmf_completion_status_t status;

	nvmf_status_init(&status);
	if (!nvmf_cmd_get_property(sc, offset, size, nvmf_complete, &status,
	    KM_SLEEP))
		return (ECONNABORTED);
	nvmf_wait_for_reply(sc, &status, secs);

	if (nvmf_cqe_failed(&status.cqe)) {
		dev_err(sc->dip, CE_WARN, "!PROPERTY_GET failed");
		return (EIO);
	}

	rsp = (const nvmf_fabric_prop_get_rsp_t *)&status.cqe;
	if (size == 8)
		*value = rsp->nfpr_value.u64;
	else
		*value = rsp->nfpr_value.u32.low;
	return (0);
}

static int
nvmf_write_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    uint64_t value, uint_t secs)
{
	nvmf_completion_status_t status;

	nvmf_status_init(&status);
	if (!nvmf_cmd_set_property(sc, offset, size, value, nvmf_complete,
	    &status, KM_SLEEP))
		return (ECONNABORTED);
	nvmf_wait_for_reply(sc, &status, secs);

	if (nvmf_cqe_failed(&status.cqe)) {
		dev_err(sc->dip, CE_WARN, "!PROPERTY_SET failed");
		return (EIO);
	}
	return (0);
}

void
nvmf_shutdown_controller(nvmf_softc_t *sc)
{
	nvme_reg_cc_t reg;
	uint64_t cc;

	if (nvmf_read_property(sc, NVMF_PROP_CC, 4, &cc,
	    nvmf_shutdown_timeout) != 0)
		return;

	reg.r = (uint32_t)cc;
	reg.b.cc_shn = NVME_CC_SHN_NORMAL;

	(void) nvmf_write_property(sc, NVMF_PROP_CC, 4, reg.r,
	    nvmf_shutdown_timeout);
}

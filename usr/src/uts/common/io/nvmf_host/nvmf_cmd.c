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
 * Fabric and admin command builders for the host.  This is pure protocol
 * logic and ports almost verbatim; the only substitutions are the SQE type
 * (struct nvme_command -> nvme_sqe_t), the Fabrics command structures
 * (struct nvmf_fabric_prop_*_cmd -> nvmf_fabric_prop_*_cmd_t from
 * <sys/nvme/nvmf.h>), the data-descriptor (struct memdesc -> nvmf_memdesc_t),
 * and KM_SLEEP/KM_NOSLEEP for M_WAITOK/M_NOWAIT.  illumos is little-endian
 * only, so the FreeBSD htole*() calls become plain assignments.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

#include "nvmf_var.h"
#include "../nvmf/nvmf_transport_internal.h"

/* FreeBSD spells the Fabrics opcode NVME_OPC_FABRICS_COMMANDS == 0x7f. */
#ifndef	NVME_OPC_FABRICS_COMMANDS
#define	NVME_OPC_FABRICS_COMMANDS	0x7f
#endif

boolean_t
nvmf_cmd_get_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    nvmf_request_complete_t *cb, void *cb_arg, int how)
{
	nvmf_fabric_prop_get_cmd_t cmd;
	nvmf_request_t *req;

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

	req = nvmf_allocate_request(sc->admin, &cmd, cb, cb_arg, how);
	if (req != NULL)
		nvmf_submit_request(req);
	return (req != NULL);
}

boolean_t
nvmf_cmd_set_property(nvmf_softc_t *sc, uint32_t offset, uint8_t size,
    uint64_t value, nvmf_request_complete_t *cb, void *cb_arg, int how)
{
	nvmf_fabric_prop_set_cmd_t cmd;
	nvmf_request_t *req;

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

	req = nvmf_allocate_request(sc->admin, &cmd, cb, cb_arg, how);
	if (req != NULL)
		nvmf_submit_request(req);
	return (req != NULL);
}

boolean_t
nvmf_cmd_keep_alive(nvmf_softc_t *sc, nvmf_request_complete_t *cb, void *cb_arg,
    int how)
{
	nvme_sqe_t cmd;
	nvmf_request_t *req;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_KEEP_ALIVE;

	req = nvmf_allocate_request(sc->admin, &cmd, cb, cb_arg, how);
	if (req != NULL)
		nvmf_submit_request(req);
	return (req != NULL);
}

boolean_t
nvmf_cmd_identify_active_namespaces(nvmf_softc_t *sc, uint32_t id,
    nvme_identify_nsid_list_t *nslist, nvmf_request_complete_t *req_cb,
    void *req_cb_arg, nvmf_io_complete_t *io_cb, void *io_cb_arg, int how)
{
	nvme_sqe_t cmd;
	nvmf_memdesc_t mem;
	nvmf_request_t *req;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_IDENTIFY;

	/* 5.15.1 Use CNS of 0x02 for the active namespace ID list. */
	cmd.sqe_cdw10 = 2;
	cmd.sqe_nsid = id;

	req = nvmf_allocate_request(sc->admin, &cmd, req_cb, req_cb_arg, how);
	if (req == NULL)
		return (B_FALSE);
	mem.nmd_type = NVMF_MEMDESC_VADDR;
	mem.nmd_len = sizeof (*nslist);
	mem.nmd_u.nmd_vaddr = nslist;
	(void) nvmf_capsule_append_data(req->nc, &mem, sizeof (*nslist),
	    B_FALSE, io_cb, io_cb_arg);
	nvmf_submit_request(req);
	return (B_TRUE);
}

boolean_t
nvmf_cmd_identify_namespace(nvmf_softc_t *sc, uint32_t id,
    nvme_identify_nsid_t *nsdata, nvmf_request_complete_t *req_cb,
    void *req_cb_arg, nvmf_io_complete_t *io_cb, void *io_cb_arg, int how)
{
	nvme_sqe_t cmd;
	nvmf_memdesc_t mem;
	nvmf_request_t *req;

	bzero(&cmd, sizeof (cmd));
	cmd.sqe_opc = NVME_OPC_IDENTIFY;

	/* 5.15.1 Use CNS of 0x00 for namespace data. */
	cmd.sqe_cdw10 = 0;
	cmd.sqe_nsid = id;

	req = nvmf_allocate_request(sc->admin, &cmd, req_cb, req_cb_arg, how);
	if (req == NULL)
		return (B_FALSE);
	mem.nmd_type = NVMF_MEMDESC_VADDR;
	mem.nmd_len = sizeof (*nsdata);
	mem.nmd_u.nmd_vaddr = nsdata;
	(void) nvmf_capsule_append_data(req->nc, &mem, sizeof (*nsdata),
	    B_FALSE, io_cb, io_cb_arg);
	nvmf_submit_request(req);
	return (B_TRUE);
}

boolean_t
nvmf_cmd_get_log_page(nvmf_softc_t *sc, uint32_t nsid, uint8_t lid,
    uint64_t offset, void *buf, size_t len, nvmf_request_complete_t *req_cb,
    void *req_cb_arg, nvmf_io_complete_t *io_cb, void *io_cb_arg, int how)
{
	nvme_sqe_t cmd;
	nvmf_memdesc_t mem;
	nvmf_request_t *req;
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

	req = nvmf_allocate_request(sc->admin, &cmd, req_cb, req_cb_arg, how);
	if (req == NULL)
		return (B_FALSE);
	mem.nmd_type = NVMF_MEMDESC_VADDR;
	mem.nmd_len = len;
	mem.nmd_u.nmd_vaddr = buf;
	(void) nvmf_capsule_append_data(req->nc, &mem, len, B_FALSE, io_cb,
	    io_cb_arg);
	nvmf_submit_request(req);
	return (B_TRUE);
}

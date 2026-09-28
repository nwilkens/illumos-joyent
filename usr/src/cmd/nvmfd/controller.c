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
 * tools/tools/nvmf/nvmfd/controller.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Shared controller-property and admin-command dispatch shell.  On illumos
 * this is used only by the userland Discovery controller (discovery.c); the
 * I/O subsystem's admin queue is handled by the kernel after handoff.  The
 * logic is unchanged from FreeBSD: only the libnvmf API is consumed and that
 * API is preserved verbatim in the illumos port.
 */

#include <sys/byteorder.h>
#include <err.h>
#include <errno.h>
#include <libnvmf.h>
#include <stdlib.h>

#include "nvme_reg.h"

#include "internal.h"

/*
 * The Fabrics command opcode is not yet carried in the illumos register header
 * (NVMe base spec: 0x7f).  libnvmf defines it locally the same way.
 */
#ifndef	NVME_OPC_FABRICS_COMMANDS
#define	NVME_OPC_FABRICS_COMMANDS	0x7f
#endif

struct controller {
	struct nvmf_qpair *qp;

	uint64_t cap;
	uint32_t vs;
	uint32_t cc;
	uint32_t csts;

	bool shutdown;

	nvme_identify_ctrl_t cdata;
};

static bool
update_cc(struct controller *c, uint32_t new_cc)
{
	nvme_reg_cc_t old_reg, new_reg;
	nvme_reg_csts_t csts;

	if (c->shutdown)
		return (false);
	if (!nvmf_validate_cc(c->qp, c->cap, c->cc, new_cc))
		return (false);

	old_reg.r = c->cc;
	new_reg.r = new_cc;
	c->cc = new_cc;
	csts.r = c->csts;

	/* Handle shutdown requests. */
	if (new_reg.b.cc_shn != old_reg.b.cc_shn && new_reg.b.cc_shn != 0) {
		csts.b.csts_shst = NVME_CSTS_SHN_COMPLETE;
		c->csts = csts.r;
		c->shutdown = true;
	}

	if (new_reg.b.cc_en != old_reg.b.cc_en) {
		if (new_reg.b.cc_en == 0) {
			/* Controller reset. */
			c->csts = 0;
			c->shutdown = true;
		} else {
			csts.b.csts_rdy = 1;
			c->csts = csts.r;
		}
	}
	return (true);
}

static void
handle_property_get(const struct controller *c, const struct nvmf_capsule *nc,
    const nvmf_fabric_prop_get_cmd_t *pget)
{
	nvmf_fabric_prop_get_rsp_t rsp;

	nvmf_init_cqe(&rsp, nc, 0);

	switch (LE_32(pget->nfpg_ofst)) {
	case NVMF_PROP_CAP:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_8)
			goto error;
		rsp.nfpr_value.u64 = LE_64(c->cap);
		break;
	case NVMF_PROP_VS:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = LE_32(c->vs);
		break;
	case NVMF_PROP_CC:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = LE_32(c->cc);
		break;
	case NVMF_PROP_CSTS:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = LE_32(c->csts);
		break;
	default:
		goto error;
	}

	(void) nvmf_send_response(nc, &rsp);
	return;
error:
	(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
}

static void
handle_property_set(struct controller *c, const struct nvmf_capsule *nc,
    const nvmf_fabric_prop_set_cmd_t *pset)
{
	switch (LE_32(pset->nfps_ofst)) {
	case NVMF_PROP_CC:
		if (pset->nfps_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		if (!update_cc(c, LE_32(pset->nfps_value.u32.low)))
			goto error;
		break;
	default:
		goto error;
	}

	(void) nvmf_send_success(nc);
	return;
error:
	(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
}

static void
handle_fabrics_command(struct controller *c, const struct nvmf_capsule *nc,
    const nvmf_fabric_cmd_t *fc)
{
	switch (fc->nfc_fctype) {
	case NVMF_FCTYPE_PROPERTY_GET:
		handle_property_get(c, nc,
		    (const nvmf_fabric_prop_get_cmd_t *)fc);
		break;
	case NVMF_FCTYPE_PROPERTY_SET:
		handle_property_set(c, nc,
		    (const nvmf_fabric_prop_set_cmd_t *)fc);
		break;
	case NVMF_FCTYPE_CONNECT:
		warnx("CONNECT command on connected queue");
		(void) nvmf_send_generic_error(nc,
		    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
		break;
	case NVMF_FCTYPE_DISCONNECT:
		warnx("DISCONNECT command on admin queue");
		(void) nvmf_send_error(nc, NVME_CQE_SCT_SPECIFIC,
		    NVMF_FABRIC_SC_INVALID_QUEUE_TYPE);
		break;
	default:
		warnx("Unsupported fabrics command %#x", fc->nfc_fctype);
		(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_OPC);
		break;
	}
}

static void
handle_identify_command(const struct controller *c,
    const struct nvmf_capsule *nc, const nvme_sqe_t *cmd)
{
	uint8_t cns;

	cns = LE_32(cmd->sqe_cdw10) & 0xFF;
	switch (cns) {
	case 1:
		break;
	default:
		warnx("Unsupported CNS %#x for IDENTIFY", cns);
		goto error;
	}

	(void) nvmf_send_controller_data(nc, &c->cdata, sizeof (c->cdata));
	return;
error:
	(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
}

void
controller_handle_admin_commands(struct controller *c, handle_command *cb,
    void *cb_arg)
{
	struct nvmf_qpair *qp = c->qp;
	const nvme_sqe_t *cmd;
	struct nvmf_capsule *nc;
	nvme_reg_cc_t cc;
	int error;

	for (;;) {
		error = nvmf_controller_receive_capsule(qp, &nc);
		if (error != 0) {
			if (error != ECONNRESET)
				warnc(error, "Failed to read command capsule");
			break;
		}

		cmd = nvmf_capsule_sqe(nc);

		/*
		 * Only permit Fabrics commands while a controller is disabled.
		 */
		cc.r = c->cc;
		if (cc.b.cc_en == 0 &&
		    cmd->sqe_opc != NVME_OPC_FABRICS_COMMANDS) {
			warnx("Unsupported admin opcode %#x while disabled",
			    cmd->sqe_opc);
			(void) nvmf_send_generic_error(nc,
			    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
			nvmf_free_capsule(nc);
			continue;
		}

		if (cb(nc, cmd, cb_arg)) {
			nvmf_free_capsule(nc);
			continue;
		}

		switch (cmd->sqe_opc) {
		case NVME_OPC_FABRICS_COMMANDS:
			handle_fabrics_command(c, nc,
			    (const nvmf_fabric_cmd_t *)cmd);
			break;
		case NVME_OPC_IDENTIFY:
			handle_identify_command(c, nc, cmd);
			break;
		default:
			warnx("Unsupported admin opcode %#x", cmd->sqe_opc);
			(void) nvmf_send_generic_error(nc,
			    NVME_CQE_SC_GEN_INV_OPC);
			break;
		}
		nvmf_free_capsule(nc);
	}
}

struct controller *
init_controller(struct nvmf_qpair *qp,
    const nvme_identify_ctrl_t *cdata)
{
	struct controller *c;

	c = calloc(1, sizeof (*c));
	if (c == NULL)
		err(1, "calloc");
	c->qp = qp;
	c->cap = nvmf_controller_cap(c->qp);
	c->vs = cdata->id_ver;
	c->cdata = *cdata;

	return (c);
}

void
free_controller(struct controller *c)
{
	free(c);
}

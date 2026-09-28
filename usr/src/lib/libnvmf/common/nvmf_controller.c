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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/nvmf_controller.c.
 *
 * Original: Copyright (c) 2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Controller-side (target) Fabrics handshake: accept an association, run the
 * CONNECT command, build admin and I/O queue pairs, and the response/error
 * helpers nvmfd uses.  Ported field-for-field; OS-glue substitutions:
 *
 *   FreeBSD                            illumos
 *   -------                            -------
 *   struct nvmf_fabric_connect_cmd     nvmf_fabric_connect_cmd_t  (nfcc_*)
 *   struct nvmf_fabric_connect_rsp     nvmf_fabric_connect_rsp_t  (nfcr_*)
 *   struct nvme_completion             nvme_cqe_t (cqe_sf bit-field status)
 *   NVMEF(NVME_STATUS_SCT/SC, ...)     NVMF_SC_TYPE()/cqe_sf.sf_sct/sf_sc
 *   struct nvme_controller_data        nvme_identify_ctrl_t
 *   NVME_OPC_GET_LOG_PAGE cdwX         nvme_sqe_t sqe_cdwX
 */

#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/utsname.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "libnvmf.h"
#include "internal.h"

bool
nvmf_nqn_valid_strict(const char *nqn)
{
	size_t len;

	if (!nvmf_nqn_valid(nqn))
		return (false);

	/*
	 * Stricter checks from the spec.  Linux does not seem to
	 * require these.
	 */
	len = strlen(nqn);

	/*
	 * NVMF_NQN_MIN_LEN does not include '.' and require at least
	 * one character of a domain name.
	 */
	if (len < NVMF_NQN_MIN_LEN + 2)
		return (false);
	if (memcmp("nqn.", nqn, strlen("nqn.")) != 0)
		return (false);
	nqn += strlen("nqn.");

	/* Next 4 digits must be a year. */
	for (u_int i = 0; i < 4; i++) {
		if (!isdigit(nqn[i]))
			return (false);
	}
	nqn += 4;

	/* '-' between year and month. */
	if (nqn[0] != '-')
		return (false);
	nqn++;

	/* 2 digit month. */
	for (u_int i = 0; i < 2; i++) {
		if (!isdigit(nqn[i]))
			return (false);
	}
	nqn += 2;

	/* '.' between month and reverse domain name. */
	if (nqn[0] != '.')
		return (false);
	return (true);
}

void
nvmf_init_cqe(void *cqe, const struct nvmf_capsule *nc, uint16_t status)
{
	nvme_cqe_t *cpl = cqe;
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);

	(void) memset(cpl, 0, sizeof (*cpl));
	cpl->cqe_cid = cmd->sqe_cid;
	cpl->cqe_sf.sf_sc = NVMF_SC_GET_SC(status);
	cpl->cqe_sf.sf_sct = NVMF_SC_GET_SCT(status);
}

static struct nvmf_capsule *
nvmf_simple_response(const struct nvmf_capsule *nc, uint8_t sc_type,
    uint8_t sc_status)
{
	nvme_cqe_t cpl;
	uint16_t status;

	status = NVMF_SC_TYPE(sc_type, sc_status);
	nvmf_init_cqe(&cpl, nc, status);
	return (nvmf_allocate_response(nc->nc_qpair, &cpl));
}

int
nvmf_controller_receive_capsule(struct nvmf_qpair *qp,
    struct nvmf_capsule **ncp)
{
	struct nvmf_capsule *nc;
	int error;
	uint8_t sc_status;

	*ncp = NULL;
	error = nvmf_receive_capsule(qp, &nc);
	if (error != 0)
		return (error);

	sc_status = nvmf_validate_command_capsule(nc);
	if (sc_status != NVME_CQE_SC_GEN_SUCCESS) {
		(void) nvmf_send_generic_error(nc, sc_status);
		nvmf_free_capsule(nc);
		return (EPROTO);
	}

	*ncp = nc;
	return (0);
}

int
nvmf_controller_transmit_response(struct nvmf_capsule *nc)
{
	struct nvmf_qpair *qp = nc->nc_qpair;

	/* Set SQHD. */
	if (qp->nq_flow_control) {
		qp->nq_sqhd = (qp->nq_sqhd + 1) % qp->nq_qsize;
		nc->nc_cqe.cqe_sqhd = LE_16(qp->nq_sqhd);
	} else {
		nc->nc_cqe.cqe_sqhd = 0;
	}

	return (nvmf_transmit_capsule(nc));
}

int
nvmf_send_response(const struct nvmf_capsule *cc, const void *cqe)
{
	struct nvmf_capsule *rc;
	int error;

	rc = nvmf_allocate_response(cc->nc_qpair, cqe);
	if (rc == NULL)
		return (ENOMEM);
	error = nvmf_controller_transmit_response(rc);
	nvmf_free_capsule(rc);
	return (error);
}

int
nvmf_send_error(const struct nvmf_capsule *cc, uint8_t sc_type,
    uint8_t sc_status)
{
	struct nvmf_capsule *rc;
	int error;

	rc = nvmf_simple_response(cc, sc_type, sc_status);
	error = nvmf_controller_transmit_response(rc);
	nvmf_free_capsule(rc);
	return (error);
}

int
nvmf_send_generic_error(const struct nvmf_capsule *nc, uint8_t sc_status)
{
	return (nvmf_send_error(nc, NVME_CQE_SCT_GENERIC, sc_status));
}

int
nvmf_send_success(const struct nvmf_capsule *nc)
{
	return (nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_SUCCESS));
}

void
nvmf_connect_invalid_parameters(const struct nvmf_capsule *cc, bool data,
    uint16_t offset)
{
	nvmf_fabric_connect_rsp_t rsp;
	struct nvmf_capsule *rc;

	nvmf_init_cqe(&rsp, cc,
	    NVMF_SC_TYPE(NVME_CQE_SCT_SPECIFIC, NVMF_FABRIC_SC_INVALID_PARAM));
	rsp.nfcr_status_code_specific.invalid.ipo = LE_16(offset);
	rsp.nfcr_status_code_specific.invalid.iattr = data ? 1 : 0;
	rc = nvmf_allocate_response(cc->nc_qpair, &rsp);
	(void) nvmf_transmit_capsule(rc);
	nvmf_free_capsule(rc);
}

struct nvmf_qpair *
nvmf_accept(struct nvmf_association *na, const nvmf_qpair_params_t *params,
    struct nvmf_capsule **ccp, nvmf_fabric_connect_data_t *data)
{
	static const uint8_t hostid_zero[sizeof (data->nfcd_hostid)];
	const nvmf_fabric_connect_cmd_t *cmd;
	struct nvmf_qpair *qp;
	struct nvmf_capsule *cc, *rc;
	u_int qsize;
	int error;
	uint16_t cntlid;
	uint8_t sc_status;

	qp = NULL;
	cc = NULL;
	rc = NULL;
	*ccp = NULL;
	na_clear_error(na);
	if (!na->na_controller) {
		na_error(na, "Cannot accept on a host");
		goto error;
	}

	qp = nvmf_allocate_qpair(na, params);
	if (qp == NULL)
		goto error;

	/* Read the CONNECT capsule. */
	error = nvmf_receive_capsule(qp, &cc);
	if (error != 0) {
		na_error(na, "Failed to receive CONNECT: %s", strerror(error));
		goto error;
	}

	sc_status = nvmf_validate_command_capsule(cc);
	if (sc_status != 0) {
		na_error(na, "CONNECT command failed to validate: %u",
		    sc_status);
		rc = nvmf_simple_response(cc, NVME_CQE_SCT_GENERIC, sc_status);
		goto error;
	}

	cmd = nvmf_capsule_sqe(cc);
	if (cmd->nfcc_opcode != NVME_OPC_FABRICS_COMMANDS ||
	    cmd->nfcc_fctype != NVMF_FCTYPE_CONNECT) {
		na_error(na, "Invalid opcode in CONNECT (%u,%u)",
		    cmd->nfcc_opcode, cmd->nfcc_fctype);
		rc = nvmf_simple_response(cc, NVME_CQE_SCT_GENERIC,
		    NVME_CQE_SC_GEN_INV_OPC);
		goto error;
	}

	if (cmd->nfcc_recfmt != LE_16(0)) {
		na_error(na, "Unsupported CONNECT record format %u",
		    LE_16(cmd->nfcc_recfmt));
		rc = nvmf_simple_response(cc, NVME_CQE_SCT_SPECIFIC,
		    NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT);
		goto error;
	}

	qsize = LE_16(cmd->nfcc_sqsize) + 1;
	if (cmd->nfcc_qid == 0) {
		/* Admin queue limits. */
		if (qsize < NVME_MIN_ADMIN_ENTRIES ||
		    qsize > NVME_MAX_ADMIN_ENTRIES ||
		    qsize > na->na_params.nap_max_admin_qsize) {
			na_error(na, "Invalid queue size %u", qsize);
			nvmf_connect_invalid_parameters(cc, false,
			    offsetof(nvmf_fabric_connect_cmd_t, nfcc_sqsize));
			goto error;
		}
		qp->nq_admin = true;
	} else {
		/* I/O queues not allowed for discovery. */
		if (na->na_params.nap_max_io_qsize == 0) {
			na_error(na, "I/O queue on discovery controller");
			nvmf_connect_invalid_parameters(cc, false,
			    offsetof(nvmf_fabric_connect_cmd_t, nfcc_qid));
			goto error;
		}

		/* I/O queue limits. */
		if (qsize < NVME_MIN_IO_ENTRIES ||
		    qsize > NVME_MAX_IO_ENTRIES ||
		    qsize > na->na_params.nap_max_io_qsize) {
			na_error(na, "Invalid queue size %u", qsize);
			nvmf_connect_invalid_parameters(cc, false,
			    offsetof(nvmf_fabric_connect_cmd_t, nfcc_sqsize));
			goto error;
		}

		/* KATO is reserved for I/O queues. */
		if (cmd->nfcc_kato != 0) {
			na_error(na,
			    "KeepAlive timeout specified for I/O queue");
			nvmf_connect_invalid_parameters(cc, false,
			    offsetof(nvmf_fabric_connect_cmd_t, nfcc_kato));
			goto error;
		}
		qp->nq_admin = false;
	}
	qp->nq_qsize = qsize;

	/* Fetch CONNECT data. */
	if (nvmf_capsule_data_len(cc) != sizeof (*data)) {
		na_error(na, "Invalid data payload length for CONNECT: %zu",
		    nvmf_capsule_data_len(cc));
		nvmf_connect_invalid_parameters(cc, false,
		    offsetof(nvmf_fabric_connect_cmd_t, nfcc_sgl1));
		goto error;
	}

	error = nvmf_receive_controller_data(cc, 0, data, sizeof (*data));
	if (error != 0) {
		na_error(na, "Failed to read data for CONNECT: %s",
		    strerror(error));
		rc = nvmf_simple_response(cc, NVME_CQE_SCT_GENERIC,
		    NVME_CQE_SC_GEN_DATA_XFR_ERR);
		goto error;
	}

	/* The hostid must be non-zero. */
	if (memcmp(data->nfcd_hostid, hostid_zero, sizeof (hostid_zero)) == 0) {
		na_error(na, "HostID in CONNECT data is zero");
		nvmf_connect_invalid_parameters(cc, true,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_hostid));
		goto error;
	}

	cntlid = LE_16(data->nfcd_cntlid);
	if (cmd->nfcc_qid == 0) {
		if (na->na_params.nap_dynamic_controller_model) {
			if (cntlid != NVMF_CNTLID_DYNAMIC) {
				na_error(na, "Invalid controller ID %#x",
				    cntlid);
				nvmf_connect_invalid_parameters(cc, true,
				    offsetof(nvmf_fabric_connect_data_t,
				    nfcd_cntlid));
				goto error;
			}
		} else {
			if (cntlid > NVMF_CNTLID_STATIC_MAX &&
			    cntlid != NVMF_CNTLID_STATIC_ANY) {
				na_error(na, "Invalid controller ID %#x",
				    cntlid);
				nvmf_connect_invalid_parameters(cc, true,
				    offsetof(nvmf_fabric_connect_data_t,
				    nfcd_cntlid));
				goto error;
			}
		}
	} else {
		/* Wildcard Controller IDs are only valid on an Admin queue. */
		if (cntlid > NVMF_CNTLID_STATIC_MAX) {
			na_error(na, "Invalid controller ID %#x", cntlid);
			nvmf_connect_invalid_parameters(cc, true,
			    offsetof(nvmf_fabric_connect_data_t, nfcd_cntlid));
			goto error;
		}
	}

	/* Simple validation of each NQN. */
	if (!nvmf_nqn_valid((const char *)data->nfcd_subnqn)) {
		na_error(na, "Invalid SubNQN %.*s",
		    (int)sizeof (data->nfcd_subnqn), data->nfcd_subnqn);
		nvmf_connect_invalid_parameters(cc, true,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_subnqn));
		goto error;
	}
	if (!nvmf_nqn_valid((const char *)data->nfcd_hostnqn)) {
		na_error(na, "Invalid HostNQN %.*s",
		    (int)sizeof (data->nfcd_hostnqn), data->nfcd_hostnqn);
		nvmf_connect_invalid_parameters(cc, true,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_hostnqn));
		goto error;
	}

	if (na->na_params.nap_sq_flow_control ||
	    (cmd->nfcc_cattr & NVMF_CONNECT_ATTR_DISABLE_SQ_FC) == 0)
		qp->nq_flow_control = true;
	else
		qp->nq_flow_control = false;
	qp->nq_sqhd = 0;
	qp->nq_kato = LE_32(cmd->nfcc_kato);
	*ccp = cc;
	return (qp);
error:
	if (rc != NULL) {
		(void) nvmf_transmit_capsule(rc);
		nvmf_free_capsule(rc);
	}
	if (cc != NULL)
		nvmf_free_capsule(cc);
	if (qp != NULL)
		nvmf_free_qpair(qp);
	return (NULL);
}

int
nvmf_finish_accept(const struct nvmf_capsule *cc, uint16_t cntlid)
{
	nvmf_fabric_connect_rsp_t rsp;
	struct nvmf_qpair *qp = cc->nc_qpair;
	struct nvmf_capsule *rc;
	int error;

	nvmf_init_cqe(&rsp, cc, 0);
	if (qp->nq_flow_control)
		rsp.nfcr_sqhd = LE_16(qp->nq_sqhd);
	else
		rsp.nfcr_sqhd = LE_16(0xffff);
	rsp.nfcr_status_code_specific.success.cntlid = LE_16(cntlid);
	rc = nvmf_allocate_response(qp, &rsp);
	if (rc == NULL)
		return (ENOMEM);
	error = nvmf_transmit_capsule(rc);
	nvmf_free_capsule(rc);
	if (error == 0)
		qp->nq_cntlid = cntlid;
	return (error);
}

uint64_t
nvmf_controller_cap(struct nvmf_qpair *qp)
{
	const struct nvmf_association *na = qp->nq_association;

	return (_nvmf_controller_cap(na->na_params.nap_max_io_qsize,
	    NVMF_CC_EN_TIMEOUT));
}

bool
nvmf_validate_cc(struct nvmf_qpair *qp, uint64_t cap, uint32_t old_cc,
    uint32_t new_cc)
{
	const struct nvmf_association *na = qp->nq_association;

	return (_nvmf_validate_cc(na->na_params.nap_max_io_qsize, cap, old_cc,
	    new_cc));
}

void
nvmf_init_discovery_controller_data(struct nvmf_qpair *qp,
    nvme_identify_ctrl_t *cdata)
{
	const struct nvmf_association *na = qp->nq_association;
	struct utsname utsname;
	char *cp;

	(void) memset(cdata, 0, sizeof (*cdata));

	/*
	 * 5.2 Figure 37 states model name and serial are reserved,
	 * but Linux includes them.  Don't bother with serial, but
	 * do set model name.
	 */
	(void) uname(&utsname);
	nvmf_strpad(cdata->id_model, utsname.sysname, sizeof (cdata->id_model));
	nvmf_strpad(cdata->id_fwrev, utsname.release, sizeof (cdata->id_fwrev));
	cp = memchr(cdata->id_fwrev, '-', sizeof (cdata->id_fwrev));
	if (cp != NULL)
		(void) memset(cp, ' ',
		    sizeof (cdata->id_fwrev) - (cp - (char *)cdata->id_fwrev));

	cdata->id_cntlid = LE_16(qp->nq_cntlid);
	cdata->id_ver = LE_32(NVMF_NVME_REV_1_4);
	cdata->id_cntrltype = NVME_CNTRLTYPE_DISC;

	cdata->id_lpa.lp_extsup = 1;
	cdata->id_elpe = 0;

	cdata->id_maxcmd = LE_16(na->na_params.nap_max_admin_qsize);

	/* Transport-specific? */
	cdata->id_sgls.sgl_sup = NVME_SGL_SUP_UNALIGN;
	cdata->id_sgls.sgl_offset = 1;
	cdata->id_sgls.sgl_tport = 1;

	(void) strlcpy((char *)cdata->id_subnqn, NVMF_DISCOVERY_NQN,
	    sizeof (cdata->id_subnqn));
}

void
nvmf_init_io_controller_data(struct nvmf_qpair *qp, const char *serial,
    const char *subnqn, int nn, uint32_t ioccsz, nvme_identify_ctrl_t *cdata)
{
	const struct nvmf_association *na = qp->nq_association;
	struct utsname utsname;

	(void) uname(&utsname);

	(void) memset(cdata, 0, sizeof (*cdata));
	_nvmf_init_io_controller_data(qp->nq_cntlid,
	    na->na_params.nap_max_io_qsize, serial, utsname.sysname,
	    utsname.release, subnqn, nn, ioccsz, sizeof (nvme_cqe_t), cdata);
}

uint8_t
nvmf_get_log_page_id(const void *sqe)
{
	const nvme_sqe_t *cmd = sqe;

	assert(cmd->sqe_opc == NVME_OPC_GET_LOG_PAGE);
	return (LE_32(cmd->sqe_cdw10) & 0xff);
}

uint64_t
nvmf_get_log_page_length(const void *sqe)
{
	const nvme_sqe_t *cmd = sqe;
	uint32_t numd;

	assert(cmd->sqe_opc == NVME_OPC_GET_LOG_PAGE);
	numd = LE_32(cmd->sqe_cdw10) >> 16 |
	    (LE_32(cmd->sqe_cdw11) & 0xffff) << 16;
	return ((numd + 1) * 4);
}

uint64_t
nvmf_get_log_page_offset(const void *sqe)
{
	const nvme_sqe_t *cmd = sqe;

	assert(cmd->sqe_opc == NVME_OPC_GET_LOG_PAGE);
	return (LE_32(cmd->sqe_cdw12) |
	    (uint64_t)LE_32(cmd->sqe_cdw13) << 32);
}

int
nvmf_handoff_controller_qpair(struct nvmf_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data, struct nvmf_ioc_nv *nv)
{
	nvlist_t *nvl, *nvl_qp;
	int error;

	error = nvmf_kernel_handoff_params(qp, &nvl_qp);
	if (error)
		return (error);

	if ((error = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0) {
		nvlist_free(nvl_qp);
		return (error);
	}
	(void) nvlist_add_uint64(nvl, NVMFT_NV_TRTYPE,
	    qp->nq_association->na_trtype);
	(void) nvlist_add_nvlist(nvl, NVMFT_NV_PARAMS, nvl_qp);
	nvlist_free(nvl_qp);
	(void) nvlist_add_byte_array(nvl, NVMFT_NV_CMD,
	    (uchar_t *)(uintptr_t)cmd, sizeof (*cmd));
	(void) nvlist_add_byte_array(nvl, NVMFT_NV_DATA,
	    (uchar_t *)(uintptr_t)data, sizeof (*data));

	error = nvmf_pack_ioc_nvlist(nv, nvl);
	nvlist_free(nvl);
	return (error);
}

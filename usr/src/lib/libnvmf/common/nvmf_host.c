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
 * Host-side (initiator) Fabrics helpers: CONNECT, property get/set, IDENTIFY,
 * discovery-log fetch, queue-count negotiation, and the kernel handoff/
 * disconnect/reconnect ioctls.  Ported field-for-field; OS-glue substitutions:
 *
 *   FreeBSD                            illumos
 *   -------                            -------
 *   struct nvmf_fabric_*               nvmf_fabric_*_t (illumos field names)
 *   struct nvme_completion             nvme_cqe_t (cqe_sf bit-field status)
 *   struct nvme_controller_data        nvme_identify_ctrl_t
 *   struct nvme_namespace_data         nvme_identify_nsid_t
 *   struct nvme_discovery_log[_entry]  nvmf_discovery_log_page[_entry]_t
 *   nvme_discovery_log_swapbytes()     no-op (illumos is little-endian only)
 *   sysctlbyname("kern.hostuuid")      SMBIOS system UUID (libsmbios)
 *   uuid_from_string / uuid_enc_le      raw 16-byte SMBIOS UUID; uuid_unparse()
 *                                       (libuuid) for the string NQN form
 *
 * The Fabrics *host* kernel driver lives under io/nvmf_host and exposes the
 * NVMF_HANDOFF_HOST / RECONNECT / CONNECTION_STATUS ioctls on its control minor
 * (NVMF_HOST_DEV below); the handoff/reconnect/status wrappers drive them.
 * NVMF_DISCONNECT_HOST/ALL are issued here but not yet handled by the kernel
 * ioctl switch (they return ENOTTY until that case is wired).  The target
 * daemon (nvmfd) does not use these host entry points.
 */

#include <sys/types.h>
#include <sys/byteorder.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <smbios.h>

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

static void
nvmf_init_sqe(void *sqe, uint8_t opcode)
{
	nvme_sqe_t *cmd = sqe;

	(void) memset(cmd, 0, sizeof (*cmd));
	cmd->sqe_opc = opcode;
}

static void
nvmf_init_fabrics_sqe(void *sqe, uint8_t fctype)
{
	nvmf_capsule_cmd_t *cmd = sqe;

	nvmf_init_sqe(sqe, NVME_OPC_FABRICS_COMMANDS);
	cmd->nfc_fctype = fctype;
}

/* Status code (SC) helper for a received CQE. */
static uint16_t
nvmf_cqe_status(const nvme_cqe_t *cqe)
{
	return (NVMF_SC_TYPE(cqe->cqe_sf.sf_sct, cqe->cqe_sf.sf_sc));
}

struct nvmf_qpair *
nvmf_connect(struct nvmf_association *na, const nvmf_qpair_params_t *params,
    uint16_t qid, u_int queue_size, const uint8_t hostid[16], uint16_t cntlid,
    const char *subnqn, const char *hostnqn, uint32_t kato)
{
	nvmf_fabric_connect_cmd_t cmd;
	nvmf_fabric_connect_data_t data;
	const nvmf_fabric_connect_rsp_t *rsp;
	struct nvmf_qpair *qp;
	struct nvmf_capsule *cc, *rc;
	int error;
	uint16_t sqhd, status;

	qp = NULL;
	cc = NULL;
	rc = NULL;
	na_clear_error(na);
	if (na->na_controller) {
		na_error(na, "Cannot connect on a controller");
		goto error;
	}

	if (params->nqp_admin != (qid == 0)) {
		na_error(na, "Admin queue must use Queue ID 0");
		goto error;
	}

	if (qid == 0) {
		if (queue_size < NVME_MIN_ADMIN_ENTRIES ||
		    queue_size > NVME_MAX_ADMIN_ENTRIES) {
			na_error(na, "Invalid queue size %u", queue_size);
			goto error;
		}
	} else {
		if (queue_size < NVME_MIN_IO_ENTRIES ||
		    queue_size > NVME_MAX_IO_ENTRIES) {
			na_error(na, "Invalid queue size %u", queue_size);
			goto error;
		}

		/* KATO is only for Admin queues. */
		if (kato != 0) {
			na_error(na, "Cannot set KATO on I/O queues");
			goto error;
		}
	}

	qp = nvmf_allocate_qpair(na, params);
	if (qp == NULL)
		goto error;

	nvmf_init_fabrics_sqe(&cmd, NVMF_FCTYPE_CONNECT);
	cmd.nfcc_recfmt = 0;
	cmd.nfcc_qid = LE_16(qid);

	/* N.B. sqsize is 0's based. */
	cmd.nfcc_sqsize = LE_16(queue_size - 1);
	if (!na->na_params.nap_sq_flow_control)
		cmd.nfcc_cattr |= NVMF_CONNECT_ATTR_DISABLE_SQ_FC;
	cmd.nfcc_kato = LE_32(kato);

	cc = nvmf_allocate_command(qp, &cmd);
	if (cc == NULL) {
		na_error(na, "Failed to allocate command capsule: %s",
		    strerror(errno));
		goto error;
	}

	(void) memset(&data, 0, sizeof (data));
	(void) memcpy(data.nfcd_hostid, hostid, sizeof (data.nfcd_hostid));
	data.nfcd_cntlid = LE_16(cntlid);
	(void) strlcpy((char *)data.nfcd_subnqn, subnqn,
	    sizeof (data.nfcd_subnqn));
	(void) strlcpy((char *)data.nfcd_hostnqn, hostnqn,
	    sizeof (data.nfcd_hostnqn));

	error = nvmf_capsule_append_data(cc, &data, sizeof (data), true);
	if (error != 0) {
		na_error(na, "Failed to append data to CONNECT capsule: %s",
		    strerror(error));
		goto error;
	}

	error = nvmf_transmit_capsule(cc);
	if (error != 0) {
		na_error(na, "Failed to transmit CONNECT capsule: %s",
		    strerror(errno));
		goto error;
	}

	error = nvmf_receive_capsule(qp, &rc);
	if (error != 0) {
		na_error(na, "Failed to receive CONNECT response: %s",
		    strerror(error));
		goto error;
	}

	rsp = (const nvmf_fabric_connect_rsp_t *)&rc->nc_cqe;
	status = nvmf_cqe_status(&rc->nc_cqe);
	if (status != 0) {
		if (NVMF_SC_GET_SC(status) == NVMF_FABRIC_SC_INVALID_PARAM)
			na_error(na,
			    "CONNECT invalid parameter IATTR: %#x IPO: %#x",
			    rsp->nfcr_status_code_specific.invalid.iattr,
			    rsp->nfcr_status_code_specific.invalid.ipo);
		else
			na_error(na, "CONNECT failed, status %#x", status);
		goto error;
	}

	if (rc->nc_cqe.cqe_cid != cmd.nfcc_cid) {
		na_error(na, "Mismatched CID in CONNECT response");
		goto error;
	}

	if (!rc->nc_sqhd_valid) {
		na_error(na, "CONNECT response without valid SQHD");
		goto error;
	}

	sqhd = LE_16(rsp->nfcr_sqhd);
	if (sqhd == 0xffff) {
		if (na->na_params.nap_sq_flow_control) {
			na_error(na, "Controller disabled SQ flow control");
			goto error;
		}
		qp->nq_flow_control = false;
	} else {
		qp->nq_flow_control = true;
		qp->nq_sqhd = sqhd;
		qp->nq_sqtail = sqhd;
	}

	if (rsp->nfcr_status_code_specific.success.authreq) {
		na_error(na, "CONNECT response requests authentication\n");
		goto error;
	}

	qp->nq_qsize = queue_size;
	qp->nq_cntlid = LE_16(rsp->nfcr_status_code_specific.success.cntlid);
	qp->nq_kato = kato;
	/* XXX: Save qid in qp? */
	return (qp);

error:
	if (rc != NULL)
		nvmf_free_capsule(rc);
	if (cc != NULL)
		nvmf_free_capsule(cc);
	if (qp != NULL)
		nvmf_free_qpair(qp);
	return (NULL);
}

uint16_t
nvmf_cntlid(struct nvmf_qpair *qp)
{
	return (qp->nq_cntlid);
}

int
nvmf_host_transmit_command(struct nvmf_capsule *nc)
{
	struct nvmf_qpair *qp = nc->nc_qpair;
	uint16_t new_sqtail;
	int error;

	/* Fail if the queue is full. */
	new_sqtail = (qp->nq_sqtail + 1) % qp->nq_qsize;
	if (new_sqtail == qp->nq_sqhd)
		return (EBUSY);

	nc->nc_sqe.sqe_cid = LE_16(qp->nq_cid);

	/* 4.2 Skip CID of 0xFFFF. */
	qp->nq_cid++;
	if (qp->nq_cid == 0xFFFF)
		qp->nq_cid = 0;

	error = nvmf_transmit_capsule(nc);
	if (error != 0)
		return (error);

	qp->nq_sqtail = new_sqtail;
	return (0);
}

/* Receive a single capsule and update SQ FC accounting. */
static int
nvmf_host_receive_capsule(struct nvmf_qpair *qp, struct nvmf_capsule **ncp)
{
	struct nvmf_capsule *nc;
	int error;

	/* If the SQ is empty, there is no response to wait for. */
	if (qp->nq_sqhd == qp->nq_sqtail)
		return (EWOULDBLOCK);

	error = nvmf_receive_capsule(qp, &nc);
	if (error != 0)
		return (error);

	if (qp->nq_flow_control) {
		if (nc->nc_sqhd_valid)
			qp->nq_sqhd = LE_16(nc->nc_cqe.cqe_sqhd);
	} else {
		/*
		 * If SQ FC is disabled, just advance the head for each
		 * response capsule received so that we track the number of
		 * outstanding commands.
		 */
		qp->nq_sqhd = (qp->nq_sqhd + 1) % qp->nq_qsize;
	}
	*ncp = nc;
	return (0);
}

int
nvmf_host_receive_response(struct nvmf_qpair *qp, struct nvmf_capsule **ncp)
{
	struct nvmf_capsule *nc;

	/* Return the oldest previously received response. */
	if (!TAILQ_EMPTY(&qp->nq_rx_capsules)) {
		nc = TAILQ_FIRST(&qp->nq_rx_capsules);
		TAILQ_REMOVE(&qp->nq_rx_capsules, nc, nc_link);
		*ncp = nc;
		return (0);
	}

	return (nvmf_host_receive_capsule(qp, ncp));
}

int
nvmf_host_wait_for_response(struct nvmf_capsule *cc, struct nvmf_capsule **rcp)
{
	struct nvmf_qpair *qp = cc->nc_qpair;
	struct nvmf_capsule *rc;
	int error;

	/* Check if a response was already received. */
	TAILQ_FOREACH(rc, &qp->nq_rx_capsules, nc_link) {
		if (rc->nc_cqe.cqe_cid == cc->nc_sqe.sqe_cid) {
			TAILQ_REMOVE(&qp->nq_rx_capsules, rc, nc_link);
			*rcp = rc;
			return (0);
		}
	}

	/* Wait for a response. */
	for (;;) {
		error = nvmf_host_receive_capsule(qp, &rc);
		if (error != 0)
			return (error);

		if (rc->nc_cqe.cqe_cid != cc->nc_sqe.sqe_cid) {
			TAILQ_INSERT_TAIL(&qp->nq_rx_capsules, rc, nc_link);
			continue;
		}

		*rcp = rc;
		return (0);
	}
}

struct nvmf_capsule *
nvmf_keepalive(struct nvmf_qpair *qp)
{
	nvme_sqe_t cmd;

	if (!qp->nq_admin) {
		errno = EINVAL;
		return (NULL);
	}

	nvmf_init_sqe(&cmd, NVME_OPC_KEEP_ALIVE);

	return (nvmf_allocate_command(qp, &cmd));
}

static struct nvmf_capsule *
nvmf_get_property(struct nvmf_qpair *qp, uint32_t offset, uint8_t size)
{
	nvmf_fabric_prop_get_cmd_t cmd;

	nvmf_init_fabrics_sqe(&cmd, NVMF_FCTYPE_PROPERTY_GET);
	switch (size) {
	case 4:
		cmd.nfpg_attrib.size = NVMF_PROP_SIZE_4;
		break;
	case 8:
		cmd.nfpg_attrib.size = NVMF_PROP_SIZE_8;
		break;
	default:
		errno = EINVAL;
		return (NULL);
	}
	cmd.nfpg_ofst = LE_32(offset);

	return (nvmf_allocate_command(qp, &cmd));
}

int
nvmf_read_property(struct nvmf_qpair *qp, uint32_t offset, uint8_t size,
    uint64_t *value)
{
	struct nvmf_capsule *cc, *rc;
	const nvmf_fabric_prop_get_rsp_t *rsp;
	uint16_t status;
	int error;

	if (!qp->nq_admin)
		return (EINVAL);

	cc = nvmf_get_property(qp, offset, size);
	if (cc == NULL)
		return (errno);

	error = nvmf_host_transmit_command(cc);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_wait_for_response(cc, &rc);
	nvmf_free_capsule(cc);
	if (error != 0)
		return (error);

	rsp = (const nvmf_fabric_prop_get_rsp_t *)&rc->nc_cqe;
	status = nvmf_cqe_status(&rc->nc_cqe);
	if (status != 0) {
		(void) printf("NVMF: PROPERTY_GET failed, status %#x\n",
		    status);
		nvmf_free_capsule(rc);
		return (EIO);
	}

	if (size == 8)
		*value = LE_64(rsp->nfpr_value.u64);
	else
		*value = LE_32(rsp->nfpr_value.u32.low);
	nvmf_free_capsule(rc);
	return (0);
}

static struct nvmf_capsule *
nvmf_set_property(struct nvmf_qpair *qp, uint32_t offset, uint8_t size,
    uint64_t value)
{
	nvmf_fabric_prop_set_cmd_t cmd;

	nvmf_init_fabrics_sqe(&cmd, NVMF_FCTYPE_PROPERTY_SET);
	switch (size) {
	case 4:
		cmd.nfps_attrib.size = NVMF_PROP_SIZE_4;
		cmd.nfps_value.u32.low = LE_32(value);
		break;
	case 8:
		cmd.nfps_attrib.size = NVMF_PROP_SIZE_8;
		cmd.nfps_value.u64 = LE_64(value);
		break;
	default:
		errno = EINVAL;
		return (NULL);
	}
	cmd.nfps_ofst = LE_32(offset);

	return (nvmf_allocate_command(qp, &cmd));
}

int
nvmf_write_property(struct nvmf_qpair *qp, uint32_t offset, uint8_t size,
    uint64_t value)
{
	struct nvmf_capsule *cc, *rc;
	uint16_t status;
	int error;

	if (!qp->nq_admin)
		return (EINVAL);

	cc = nvmf_set_property(qp, offset, size, value);
	if (cc == NULL)
		return (errno);

	error = nvmf_host_transmit_command(cc);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_wait_for_response(cc, &rc);
	nvmf_free_capsule(cc);
	if (error != 0)
		return (error);

	status = nvmf_cqe_status(&rc->nc_cqe);
	if (status != 0) {
		(void) printf("NVMF: PROPERTY_SET failed, status %#x\n",
		    status);
		nvmf_free_capsule(rc);
		return (EIO);
	}

	nvmf_free_capsule(rc);
	return (0);
}

/*
 * Read the 16-byte SMBIOS system UUID.  This is the illumos analog of FreeBSD's
 * kern.hostuuid sysctl (see the PORT-TODO in the file header).  Returns 0 on
 * success with the raw UUID bytes in uuid[16], or an errno on failure.
 */
static int
nvmf_system_uuid(uint8_t uuid[16])
{
	smbios_hdl_t *shp;
	smbios_system_t sys;
	int err;

	shp = smbios_open(NULL, SMB_VERSION, 0, &err);
	if (shp == NULL)
		return (ENOENT);

	if (smbios_info_system(shp, &sys) == SMB_ERR ||
	    sys.smbs_uuid == NULL || sys.smbs_uuidlen != 16) {
		smbios_close(shp);
		return (ENOENT);
	}

	(void) memcpy(uuid, sys.smbs_uuid, 16);
	smbios_close(shp);
	return (0);
}

int
nvmf_hostid_from_hostuuid(uint8_t hostid[16])
{
	return (nvmf_system_uuid(hostid));
}

int
nvmf_nqn_from_hostuuid(char nqn[NVMF_NQN_MAX_LEN])
{
	uint8_t uuid[16];
	char uuid_str[NVMF_UUID_STRING_LEN + 1];
	int error;

	error = nvmf_system_uuid(uuid);
	if (error != 0)
		return (error);

	uuid_unparse(uuid, uuid_str);

	(void) strlcpy(nqn, NVMF_NQN_UUID_PRE, NVMF_NQN_MAX_LEN);
	(void) strlcat(nqn, uuid_str, NVMF_NQN_MAX_LEN);
	return (0);
}

int
nvmf_host_identify_controller(struct nvmf_qpair *qp,
    nvme_identify_ctrl_t *cdata)
{
	nvme_sqe_t cmd;
	struct nvmf_capsule *cc, *rc;
	int error;
	uint16_t status;

	if (!qp->nq_admin)
		return (EINVAL);

	nvmf_init_sqe(&cmd, NVME_OPC_IDENTIFY);

	/* 5.15.1 Use CNS of 0x01 for controller data. */
	cmd.sqe_cdw10 = LE_32(1);

	cc = nvmf_allocate_command(qp, &cmd);
	if (cc == NULL)
		return (errno);

	error = nvmf_capsule_append_data(cc, cdata, sizeof (*cdata), false);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_transmit_command(cc);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_wait_for_response(cc, &rc);
	nvmf_free_capsule(cc);
	if (error != 0)
		return (error);

	status = nvmf_cqe_status(&rc->nc_cqe);
	if (status != 0) {
		(void) printf("NVMF: IDENTIFY failed, status %#x\n", status);
		nvmf_free_capsule(rc);
		return (EIO);
	}

	nvmf_free_capsule(rc);
	return (0);
}

int
nvmf_host_identify_namespace(struct nvmf_qpair *qp, uint32_t nsid,
    nvme_identify_nsid_t *nsdata)
{
	nvme_sqe_t cmd;
	struct nvmf_capsule *cc, *rc;
	int error;
	uint16_t status;

	if (!qp->nq_admin)
		return (EINVAL);

	nvmf_init_sqe(&cmd, NVME_OPC_IDENTIFY);

	/* 5.15.1 Use CNS of 0x00 for namespace data. */
	cmd.sqe_cdw10 = LE_32(0);
	cmd.sqe_nsid = LE_32(nsid);

	cc = nvmf_allocate_command(qp, &cmd);
	if (cc == NULL)
		return (errno);

	error = nvmf_capsule_append_data(cc, nsdata, sizeof (*nsdata), false);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_transmit_command(cc);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_wait_for_response(cc, &rc);
	nvmf_free_capsule(cc);
	if (error != 0)
		return (error);

	status = nvmf_cqe_status(&rc->nc_cqe);
	if (status != 0) {
		(void) printf("NVMF: IDENTIFY failed, status %#x\n", status);
		nvmf_free_capsule(rc);
		return (EIO);
	}

	nvmf_free_capsule(rc);
	return (0);
}

static int
nvmf_get_discovery_log_page(struct nvmf_qpair *qp, uint64_t offset, void *buf,
    size_t len)
{
	nvme_sqe_t cmd;
	struct nvmf_capsule *cc, *rc;
	size_t numd;
	int error;
	uint16_t status;

	if (len % 4 != 0 || len == 0 || offset % 4 != 0)
		return (EINVAL);

	numd = (len / 4) - 1;
	nvmf_init_sqe(&cmd, NVME_OPC_GET_LOG_PAGE);
	cmd.sqe_cdw10 = LE_32(numd << 16 | NVMF_LOG_DISCOVERY);
	cmd.sqe_cdw11 = LE_32(numd >> 16);
	cmd.sqe_cdw12 = LE_32(offset);
	cmd.sqe_cdw13 = LE_32(offset >> 32);

	cc = nvmf_allocate_command(qp, &cmd);
	if (cc == NULL)
		return (errno);

	error = nvmf_capsule_append_data(cc, buf, len, false);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_transmit_command(cc);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_wait_for_response(cc, &rc);
	nvmf_free_capsule(cc);
	if (error != 0)
		return (error);

	status = nvmf_cqe_status(&rc->nc_cqe);
	if (NVMF_SC_GET_SC(status) == NVMF_FABRIC_SC_LOG_RESTART_DISCOVERY) {
		nvmf_free_capsule(rc);
		return (EAGAIN);
	}
	if (status != 0) {
		(void) printf("NVMF: GET_LOG_PAGE failed, status %#x\n",
		    status);
		nvmf_free_capsule(rc);
		return (EIO);
	}

	nvmf_free_capsule(rc);
	return (0);
}

int
nvmf_host_fetch_discovery_log_page(struct nvmf_qpair *qp,
    nvmf_discovery_log_page_t **logp)
{
	nvmf_discovery_log_page_t hdr, *log;
	size_t payload_len;
	int error;

	if (!qp->nq_admin)
		return (EINVAL);

	log = NULL;
	for (;;) {
		error = nvmf_get_discovery_log_page(qp, 0, &hdr, sizeof (hdr));
		if (error != 0) {
			free(log);
			return (error);
		}

		/* illumos is little-endian; no byte-swap of the header. */
		if (hdr.ndlp_recfmt != 0) {
			(void) printf(
			    "NVMF: Unsupported discovery log format: %d\n",
			    hdr.ndlp_recfmt);
			free(log);
			return (EINVAL);
		}

		if (hdr.ndlp_numrec > 1024) {
			(void) printf(
			    "NVMF: Too many discovery log entries: %ju\n",
			    (uintmax_t)hdr.ndlp_numrec);
			free(log);
			return (EFBIG);
		}

		payload_len = sizeof (log->ndlp_entries[0]) * hdr.ndlp_numrec;
		log = reallocf(log, sizeof (*log) + payload_len);
		if (log == NULL)
			return (ENOMEM);
		*log = hdr;
		if (hdr.ndlp_numrec == 0)
			break;

		error = nvmf_get_discovery_log_page(qp, sizeof (hdr),
		    log->ndlp_entries, payload_len);
		if (error == EAGAIN)
			continue;
		if (error != 0) {
			free(log);
			return (error);
		}

		/* Re-read the header and check the generation count. */
		error = nvmf_get_discovery_log_page(qp, 0, &hdr, sizeof (hdr));
		if (error != 0) {
			free(log);
			return (error);
		}

		if (log->ndlp_genctr != hdr.ndlp_genctr)
			continue;

		break;
	}
	*logp = log;
	return (0);
}

int
nvmf_init_dle_from_admin_qp(struct nvmf_qpair *qp,
    const nvme_identify_ctrl_t *cdata, nvmf_discovery_log_page_entry_t *dle)
{
	int error;
	uint16_t cntlid;
	uint8_t fcatt;

	(void) memset(dle, 0, sizeof (*dle));
	error = nvmf_populate_dle(qp, dle);
	if (error != 0)
		return (error);

	/*
	 * FCATT bit 0 selects the static controller model.  It lives in the
	 * NVMe-oF transport-specific region of Identify Controller (id_nvmof
	 * byte +10); see _nvmf_init_io_controller_data().
	 */
	fcatt = cdata->id_nvmof[10];
	if ((fcatt & 1) == 0)
		cntlid = NVMF_CNTLID_DYNAMIC;
	else
		cntlid = cdata->id_cntlid;
	dle->ndle_cntlid = LE_16(cntlid);
	(void) memcpy(dle->ndle_subnqn, cdata->id_subnqn,
	    sizeof (dle->ndle_subnqn));
	return (0);
}

int
nvmf_host_request_queues(struct nvmf_qpair *qp, u_int requested, u_int *actual)
{
	nvme_sqe_t cmd;
	struct nvmf_capsule *cc, *rc;
	int error;
	uint16_t status;

	if (!qp->nq_admin || requested < 1 || requested > 65535)
		return (EINVAL);

	/* The number of queues is 0's based. */
	requested--;

	nvmf_init_sqe(&cmd, NVME_OPC_SET_FEATURES);
	cmd.sqe_cdw10 = LE_32(NVME_FEAT_NUMBER_OF_QUEUES);

	/* Same number of completion and submission queues. */
	cmd.sqe_cdw11 = LE_32((requested << 16) | requested);

	cc = nvmf_allocate_command(qp, &cmd);
	if (cc == NULL)
		return (errno);

	error = nvmf_host_transmit_command(cc);
	if (error != 0) {
		nvmf_free_capsule(cc);
		return (error);
	}

	error = nvmf_host_wait_for_response(cc, &rc);
	nvmf_free_capsule(cc);
	if (error != 0)
		return (error);

	status = nvmf_cqe_status(&rc->nc_cqe);
	if (status != 0) {
		(void) printf("NVMF: SET_FEATURES failed, status %#x\n",
		    status);
		nvmf_free_capsule(rc);
		return (EIO);
	}

	*actual = (LE_32(rc->nc_cqe.cqe_dw0) & 0xffff) + 1;
	nvmf_free_capsule(rc);
	return (0);
}

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

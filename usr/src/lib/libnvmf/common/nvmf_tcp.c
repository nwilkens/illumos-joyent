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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/nvmf_tcp.c.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Userland NVMe/TCP transport: ICReq/ICResp connection setup, PDU framing,
 * CRC32C header/data digests, and command-buffer (R2T / H2C / C2H) data
 * movement over a BSD socket fd.  Ported field-for-field; OS-glue
 * substitutions:
 *
 *   FreeBSD                                 illumos
 *   -------                                 -------
 *   le16toh/le32toh, htole16/32              LE_16/LE_32 (<sys/byteorder.h>)
 *   le32enc/le32dec (fei array)              explicit byte stores/loads
 *   calculate_crc32c()                       nvmf_crc32c() (nvmf_crc32c.c)
 *   struct nvme_tcp_* (nvmf_proto.h)         nvmf_tcp_*_t (<sys/nvme/nvmf.h>),
 *                                            field names ntcph_*, nfic_*, etc.
 *   NVME_TCP_PDU_TYPE_*                      NVMF_TCP_PDU_TYPE_*
 *   struct nvme_command/completion           nvme_sqe_t / nvme_cqe_t
 *   __DECONST / nitems                       explicit casts / ARRAY_SIZE-style
 *
 * The on-wire PDU validation (nvmf_tcp_validate_pdu_header) is shared with the
 * kernel transport via <sys/nvme/nvmf_tcp.h>.
 */

#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/socket.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

#include "nvmf_tcp_impl.h"

static int
nvmf_tcp_handle_term_req(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_term_req_hdr_t *hdr;

	hdr = (void *)pdu->hdr;

	na_error(qp->qp.nq_association,
	    "Received termination request: fes %#x fei %#x",
	    LE_16(hdr->nttr_fes), tcp_fei_dec(hdr->nttr_fei));
	nvmf_tcp_free_pdu(pdu);
	return (ECONNRESET);
}

static int
nvmf_tcp_save_command_capsule(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_cmd_t *cmd;
	struct nvmf_capsule *nc;
	nvmf_tcp_capsule_t *tc;

	cmd = (void *)pdu->hdr;

	nc = nvmf_allocate_command(&qp->qp, cmd->ntc_ccsqe);
	if (nc == NULL) {
		nvmf_tcp_free_pdu(pdu);
		return (ENOMEM);
	}

	/* Keep the PSDT the host sent so that it can be validated. */
	(void) memcpy(&nc->nc_sqe, cmd->ntc_ccsqe, sizeof (nc->nc_sqe));

	tc = TCAP(nc);
	tc->rx_pdu = *pdu;

	TAILQ_INSERT_TAIL(&qp->rx_capsules, tc, link);
	return (0);
}

static int
nvmf_tcp_save_response_capsule(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu)
{
	nvmf_tcp_rsp_t *rsp;
	struct nvmf_capsule *nc;
	nvmf_tcp_capsule_t *tc;
	nvme_cqe_t *cqe;

	rsp = (void *)pdu->hdr;
	cqe = (nvme_cqe_t *)rsp->ntr_rccqe;

	nc = nvmf_allocate_response(&qp->qp, cqe);
	if (nc == NULL) {
		nvmf_tcp_free_pdu(pdu);
		return (ENOMEM);
	}

	nc->nc_sqhd_valid = true;
	tc = TCAP(nc);
	tc->rx_pdu = *pdu;

	TAILQ_INSERT_TAIL(&qp->rx_capsules, tc, link);

	/*
	 * Once the CQE has been received, no further transfers to the
	 * command buffer for the associated CID can occur.
	 */
	tcp_purge_command_buffer(qp, cqe->cqe_cid, 0, true);
	tcp_purge_command_buffer(qp, cqe->cqe_cid, 0, false);

	return (0);
}

int
nvmf_tcp_receive_pdu(nvmf_tcp_qpair_t *qp)
{
	nvmf_tcp_rxpdu_t pdu;
	int error;

	error = nvmf_tcp_read_pdu(qp, &pdu);
	if (error != 0)
		return (error);

	switch (pdu.hdr->ntcph_pdu_type) {
	default:
		nvmf_tcp_free_pdu(&pdu);
		return (ECONNRESET);
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
		return (nvmf_tcp_handle_term_req(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
		return (nvmf_tcp_save_command_capsule(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
		return (nvmf_tcp_save_response_capsule(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
		return (nvmf_tcp_handle_h2c_data(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		return (nvmf_tcp_handle_c2h_data(qp, &pdu));
	case NVMF_TCP_PDU_TYPE_R2T:
		return (nvmf_tcp_handle_r2t(qp, &pdu));
	}
}

static bool
nvmf_tcp_validate_ic_pdu(struct nvmf_association *na, nvmf_tcp_qpair_t *qp,
    const nvmf_tcp_common_pdu_hdr_t *ch, size_t pdu_len)
{
	const nvmf_tcp_ic_req_t *pdu;
	uint32_t plen;
	u_int hlen;

	/* Determine how large of a PDU header to return for errors. */
	hlen = ch->ntcph_hlen;
	plen = LE_32(ch->ntcph_plen);
	if (hlen < sizeof (*ch) || hlen > plen)
		hlen = sizeof (*ch);

	/*
	 * Errors must be reported for the lowest incorrect field
	 * first, so validate fields in order.
	 */

	/* Validate pdu_type. */

	/* Controllers only receive PDUs with a PDU direction of 0. */
	if (na->na_controller != ((ch->ntcph_pdu_type & 0x01) == 0)) {
		na_error(na, "NVMe/TCP: Invalid PDU type %u",
		    ch->ntcph_pdu_type);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 0, ch, pdu_len,
		    hlen);
		return (false);
	}

	switch (ch->ntcph_pdu_type) {
	case NVMF_TCP_PDU_TYPE_IC_REQ:
	case NVMF_TCP_PDU_TYPE_IC_RESP:
		break;
	default:
		na_error(na, "NVMe/TCP: Invalid PDU type %u",
		    ch->ntcph_pdu_type);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 0, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate flags. */
	if (ch->ntcph_flags != 0) {
		na_error(na, "NVMe/TCP: Invalid PDU header flags %#x",
		    ch->ntcph_flags);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 1, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate hlen. */
	if (ch->ntcph_hlen != 128) {
		na_error(na, "NVMe/TCP: Invalid PDU header length %u",
		    ch->ntcph_hlen);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 2, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate pdo. */
	if (ch->ntcph_pdo != 0) {
		na_error(na, "NVMe/TCP: Invalid PDU data offset %u",
		    ch->ntcph_pdo);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 3, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate plen. */
	if (plen != 128) {
		na_error(na, "NVMe/TCP: Invalid PDU length %u", plen);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 4, ch, pdu_len,
		    hlen);
		return (false);
	}

	/* Validate fields common to both ICReq and ICResp. */
	pdu = (const nvmf_tcp_ic_req_t *)ch;
	if (LE_16(pdu->nfic_pfv) != 0) {
		na_error(na, "NVMe/TCP: Unsupported PDU version %u",
		    LE_16(pdu->nfic_pfv));
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER,
		    8, ch, pdu_len, hlen);
		return (false);
	}

	if (pdu->nfic_hpda > NVMF_TCP_HPDA_MAX) {
		na_error(na, "NVMe/TCP: Unsupported PDA %u", pdu->nfic_hpda);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 10, ch, pdu_len,
		    hlen);
		return (false);
	}

	if (pdu->nfic_dgst.bits.reserved != 0) {
		na_error(na, "NVMe/TCP: Invalid digest settings");
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 11, ch, pdu_len,
		    hlen);
		return (false);
	}

	return (true);
}

static bool
nvmf_tcp_read_ic_req(struct nvmf_association *na, nvmf_tcp_qpair_t *qp,
    nvmf_tcp_ic_req_t *pdu)
{
	int error;

	error = nvmf_tcp_read_buffer(qp, pdu, sizeof (*pdu));
	if (error != 0) {
		na_error(na, "NVMe/TCP: Failed to read IC request: %s",
		    strerror(error));
		return (false);
	}

	return (nvmf_tcp_validate_ic_pdu(na, qp, &pdu->nfic_common,
	    sizeof (*pdu)));
}

static bool
nvmf_tcp_read_ic_resp(struct nvmf_association *na, nvmf_tcp_qpair_t *qp,
    nvmf_tcp_ic_resp_t *pdu)
{
	int error;

	error = nvmf_tcp_read_buffer(qp, pdu, sizeof (*pdu));
	if (error != 0) {
		na_error(na, "NVMe/TCP: Failed to read IC response: %s",
		    strerror(error));
		return (false);
	}

	return (nvmf_tcp_validate_ic_pdu(na, qp, &pdu->nfir_common,
	    sizeof (*pdu)));
}

static struct nvmf_association *
tcp_allocate_association(bool controller,
    const nvmf_association_params_t *params)
{
	nvmf_tcp_association_t *ta;

	if (controller) {
		/* 7.4.10.3 */
		if (params->nap_tcp.maxh2cdata < 4096 ||
		    params->nap_tcp.maxh2cdata % 4 != 0)
			return (NULL);
	}

	ta = calloc(1, sizeof (*ta));
	if (ta == NULL)
		return (NULL);

	return (&ta->na);
}

static void
tcp_update_association(struct nvmf_association *na,
    const nvme_identify_ctrl_t *cdata)
{
	nvmf_tcp_association_t *ta = TASSOC(na);

	/* IOCCSZ lives in the NVMe-oF region of Identify Controller (+0). */
	(void) memcpy(&ta->ioccsz, &cdata->id_nvmof[0], sizeof (ta->ioccsz));
	ta->ioccsz = LE_32(ta->ioccsz);
}

static void
tcp_free_association(struct nvmf_association *na)
{
	free(na);
}

static bool
tcp_connect(nvmf_tcp_qpair_t *qp, struct nvmf_association *na, bool admin)
{
	const nvmf_association_params_t *params = &na->na_params;
	nvmf_tcp_association_t *ta = TASSOC(na);
	nvmf_tcp_ic_req_t ic_req;
	nvmf_tcp_ic_resp_t ic_resp;
	uint32_t maxh2cdata;
	int error;

	if (!admin) {
		if (ta->ioccsz == 0) {
			na_error(na, "TCP I/O queues require cdata");
			return (false);
		}
		if (ta->ioccsz < 4) {
			na_error(na, "Invalid IOCCSZ %u", ta->ioccsz);
			return (false);
		}
	}

	(void) memset(&ic_req, 0, sizeof (ic_req));
	ic_req.nfic_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_IC_REQ;
	ic_req.nfic_common.ntcph_hlen = sizeof (ic_req);
	ic_req.nfic_common.ntcph_plen = LE_32(sizeof (ic_req));
	ic_req.nfic_pfv = LE_16(0);
	ic_req.nfic_hpda = params->nap_tcp.pda;
	if (params->nap_tcp.header_digests)
		ic_req.nfic_dgst.bits.hdgst_enable = 1;
	if (params->nap_tcp.data_digests)
		ic_req.nfic_dgst.bits.ddgst_enable = 1;
	ic_req.nfic_maxr2t = LE_32(params->nap_tcp.maxr2t);

	error = nvmf_tcp_write_pdu(qp, &ic_req, sizeof (ic_req));
	if (error != 0) {
		na_error(na, "Failed to write IC request: %s",
		    strerror(error));
		return (false);
	}

	if (!nvmf_tcp_read_ic_resp(na, qp, &ic_resp))
		return (false);

	/* Ensure the controller didn't enable digests we didn't request. */
	if ((!params->nap_tcp.header_digests &&
	    ic_resp.nfir_dgst.bits.hdgst_enable != 0) ||
	    (!params->nap_tcp.data_digests &&
	    ic_resp.nfir_dgst.bits.ddgst_enable != 0)) {
		na_error(na, "Controller enabled unrequested digests");
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER,
		    11, &ic_resp, sizeof (ic_resp), sizeof (ic_resp));
		return (false);
	}

	/*
	 * XXX: Is there an upper-bound to enforce here?  Perhaps pick
	 * some large value and report larger values as an unsupported
	 * parameter?
	 */
	maxh2cdata = LE_32(ic_resp.nfir_maxh2cdata);
	if (maxh2cdata < 4096 || maxh2cdata % 4 != 0) {
		na_error(na, "Invalid MAXH2CDATA %u", maxh2cdata);
		nvmf_tcp_report_error(na, qp,
		    NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD, 12, &ic_resp,
		    sizeof (ic_resp), sizeof (ic_resp));
		return (false);
	}

	qp->rxpda = (params->nap_tcp.pda + 1) * 4;
	qp->txpda = (ic_resp.nfir_cpda + 1) * 4;
	qp->header_digests = ic_resp.nfir_dgst.bits.hdgst_enable != 0;
	qp->data_digests = ic_resp.nfir_dgst.bits.ddgst_enable != 0;
	qp->maxr2t = params->nap_tcp.maxr2t;
	qp->maxh2cdata = maxh2cdata;
	if (admin)
		/* 7.4.3 */
		qp->max_icd = 8192;
	else
		qp->max_icd = (ta->ioccsz - 4) * 16;

	return (true);
}

static bool
tcp_accept(nvmf_tcp_qpair_t *qp, struct nvmf_association *na)
{
	const nvmf_association_params_t *params = &na->na_params;
	nvmf_tcp_ic_req_t ic_req;
	nvmf_tcp_ic_resp_t ic_resp;
	int error;

	if (!nvmf_tcp_read_ic_req(na, qp, &ic_req))
		return (false);

	(void) memset(&ic_resp, 0, sizeof (ic_resp));
	ic_resp.nfir_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_IC_RESP;
	ic_resp.nfir_common.ntcph_hlen = sizeof (ic_req);
	ic_resp.nfir_common.ntcph_plen = LE_32(sizeof (ic_req));
	ic_resp.nfir_pfv = LE_16(0);
	ic_resp.nfir_cpda = params->nap_tcp.pda;
	if (params->nap_tcp.header_digests &&
	    ic_req.nfic_dgst.bits.hdgst_enable != 0)
		ic_resp.nfir_dgst.bits.hdgst_enable = 1;
	if (params->nap_tcp.data_digests &&
	    ic_req.nfic_dgst.bits.ddgst_enable != 0)
		ic_resp.nfir_dgst.bits.ddgst_enable = 1;
	ic_resp.nfir_maxh2cdata = LE_32(params->nap_tcp.maxh2cdata);

	error = nvmf_tcp_write_pdu(qp, &ic_resp, sizeof (ic_resp));
	if (error != 0) {
		na_error(na, "Failed to write IC response: %s",
		    strerror(error));
		return (false);
	}

	qp->rxpda = (params->nap_tcp.pda + 1) * 4;
	qp->txpda = (ic_req.nfic_hpda + 1) * 4;
	qp->header_digests = ic_resp.nfir_dgst.bits.hdgst_enable != 0;
	qp->data_digests = ic_resp.nfir_dgst.bits.ddgst_enable != 0;
	qp->maxr2t = LE_32(ic_req.nfic_maxr2t);
	qp->maxh2cdata = params->nap_tcp.maxh2cdata;
	return (true);
}

static struct nvmf_qpair *
tcp_allocate_qpair(struct nvmf_association *na,
    const nvmf_qpair_params_t *qparams)
{
	const nvmf_association_params_t *aparams = &na->na_params;
	nvmf_tcp_qpair_t *qp;
	bool ok;

	if (aparams->nap_tcp.pda > NVMF_TCP_CPDA_MAX) {
		na_error(na, "Invalid PDA");
		return (NULL);
	}

	qp = calloc(1, sizeof (*qp));
	if (qp == NULL) {
		na_error(na, "Out of memory");
		return (NULL);
	}
	qp->s = qparams->nqp_tcp.fd;
	qp->qp.nq_deadline = nvmf_deadline(qparams->nqp_timeout_ms);
	LIST_INIT(&qp->rx_buffers);
	LIST_INIT(&qp->tx_buffers);
	TAILQ_INIT(&qp->rx_capsules);
	if (na->na_controller)
		ok = tcp_accept(qp, na);
	else
		ok = tcp_connect(qp, na, qparams->nqp_admin);
	if (!ok) {
		free(qp);
		return (NULL);
	}

	return (&qp->qp);
}

static void
tcp_free_qpair(struct nvmf_qpair *nq)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	nvmf_tcp_capsule_t *ntc, *tc;
	nvmf_tcp_command_buffer_t *ncb, *cb;

	TAILQ_FOREACH_SAFE(tc, &qp->rx_capsules, link, ntc) {
		TAILQ_REMOVE(&qp->rx_capsules, tc, link);
		nvmf_free_capsule(&tc->nc);
	}
	LIST_FOREACH_SAFE(cb, &qp->rx_buffers, link, ncb) {
		tcp_free_command_buffer(cb);
	}
	LIST_FOREACH_SAFE(cb, &qp->tx_buffers, link, ncb) {
		tcp_free_command_buffer(cb);
	}
	free(qp);
}

static void
tcp_kernel_handoff_params(struct nvmf_qpair *nq, nvlist_t *nvl)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);

	(void) nvlist_add_uint64(nvl, "fd", qp->s);
	(void) nvlist_add_uint64(nvl, "rxpda", qp->rxpda);
	(void) nvlist_add_uint64(nvl, "txpda", qp->txpda);
	(void) nvlist_add_boolean_value(nvl, "header_digests",
	    qp->header_digests);
	(void) nvlist_add_boolean_value(nvl, "data_digests", qp->data_digests);
	(void) nvlist_add_uint64(nvl, "maxr2t", qp->maxr2t);
	(void) nvlist_add_uint64(nvl, "maxh2cdata", qp->maxh2cdata);
	if (!nq->nq_association->na_controller)
		(void) nvlist_add_uint64(nvl, "max_icd", qp->max_icd);
}

static int
tcp_populate_dle(struct nvmf_qpair *nq, nvmf_discovery_log_page_entry_t *dle)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	struct sockaddr_storage ss;
	socklen_t ss_len;

	ss_len = sizeof (ss);
	if (getpeername(qp->s, (struct sockaddr *)&ss, &ss_len) == -1)
		return (errno);

	if (getnameinfo((struct sockaddr *)&ss, ss_len,
	    (char *)dle->ndle_traddr, sizeof (dle->ndle_traddr),
	    (char *)dle->ndle_trsvcid, sizeof (dle->ndle_trsvcid),
	    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
		return (EINVAL);

	return (0);
}

static struct nvmf_capsule *
tcp_allocate_capsule(struct nvmf_qpair *qp __unused)
{
	nvmf_tcp_capsule_t *nc;

	nc = calloc(1, sizeof (*nc));
	if (nc == NULL)
		return (NULL);
	return (&nc->nc);
}

static void
tcp_free_capsule(struct nvmf_capsule *nc)
{
	nvmf_tcp_capsule_t *tc = TCAP(nc);

	nvmf_tcp_free_pdu(&tc->rx_pdu);
	if (tc->cb != NULL)
		tcp_free_command_buffer(tc->cb);
	free(tc);
}

static int
tcp_transmit_command(struct nvmf_capsule *nc)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_capsule_t *tc = TCAP(nc);
	nvmf_tcp_cmd_t cmd;
	nvme_sqe_t *sqe;
	nvme_sgl_t *sgl;
	uint8_t sgl_type;
	int error;
	bool use_icd;

	use_icd = false;
	if (nc->nc_data_len != 0 && nc->nc_send_data &&
	    nc->nc_data_len <= qp->max_icd)
		use_icd = true;

	(void) memset(&cmd, 0, sizeof (cmd));
	cmd.ntc_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_CMD;
	(void) memcpy(cmd.ntc_ccsqe, &nc->nc_sqe, sizeof (nc->nc_sqe));

	/* Populate SGL in SQE. */
	sqe = (nvme_sqe_t *)cmd.ntc_ccsqe;
	sgl = &sqe->sqe_dptr.d_sgl;
	(void) memset(sgl, 0, sizeof (*sgl));
	sgl->sgl_addr = 0;
	sgl->sgl_len = LE_32(nc->nc_data_len);
	if (use_icd)
		sgl_type = NVME_SGL_TYPE_ICD;		/* in-capsule data */
	else
		sgl_type = NVME_SGL_TYPE_COMMAND_BUFFER; /* command buffer */
	sgl->sgl_type = (uint8_t)(sgl_type >> 4);
	sgl->sgl_zero = (uint8_t)(sgl_type & 0xf);

	/* Send command capsule. */
	error = nvmf_tcp_construct_pdu(qp, &cmd, sizeof (cmd), use_icd ?
	    nc->nc_data : NULL, use_icd ? nc->nc_data_len : 0);
	if (error != 0)
		return (error);

	/*
	 * If data will be transferred using a command buffer, allocate a
	 * buffer structure and queue it.
	 */
	if (nc->nc_data_len != 0 && !use_icd) {
		tc->cb = tcp_alloc_command_buffer(qp, nc->nc_data, 0,
		    nc->nc_data_len, sqe->sqe_cid, 0, !nc->nc_send_data);
		if (tc->cb == NULL)
			return (ENOMEM);
	}

	return (0);
}

static int
tcp_transmit_response(struct nvmf_capsule *nc)
{
	nvmf_tcp_qpair_t *qp = TQP(nc->nc_qpair);
	nvmf_tcp_rsp_t rsp;

	(void) memset(&rsp, 0, sizeof (rsp));
	rsp.ntr_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_RESP;
	(void) memcpy(rsp.ntr_rccqe, &nc->nc_cqe, sizeof (nc->nc_cqe));

	return (nvmf_tcp_construct_pdu(qp, &rsp, sizeof (rsp), NULL, 0));
}

static int
tcp_transmit_capsule(struct nvmf_capsule *nc)
{
	if (nc->nc_qe_len == sizeof (nvme_sqe_t))
		return (tcp_transmit_command(nc));
	else
		return (tcp_transmit_response(nc));
}

static int
tcp_receive_capsule(struct nvmf_qpair *nq, struct nvmf_capsule **ncp)
{
	nvmf_tcp_qpair_t *qp = TQP(nq);
	nvmf_tcp_capsule_t *tc;
	int error;

	while (TAILQ_EMPTY(&qp->rx_capsules)) {
		error = nvmf_tcp_receive_pdu(qp);
		if (error != 0)
			return (error);
	}
	tc = TAILQ_FIRST(&qp->rx_capsules);
	TAILQ_REMOVE(&qp->rx_capsules, tc, link);
	*ncp = &tc->nc;
	return (0);
}

static uint8_t
tcp_validate_command_capsule(const struct nvmf_capsule *nc)
{
	const nvmf_tcp_capsule_t *tc = CTCAP(nc);
	const nvme_sgl_t *sgl;
	uint8_t sgl_type;

	assert(tc->rx_pdu.hdr != NULL);

	sgl = &nc->nc_sqe.sqe_dptr.d_sgl;
	sgl_type = (uint8_t)((sgl->sgl_type << 4) | sgl->sgl_zero);
	switch (sgl_type) {
	case NVME_SGL_TYPE_ICD:
		if (tc->rx_pdu.data_len != LE_32(sgl->sgl_len))
			return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
		break;
	case NVME_SGL_TYPE_COMMAND_BUFFER:
		if (tc->rx_pdu.data_len != 0)
			return (NVME_CQE_SC_GEN_INV_FLD);
		break;
	default:
		return (NVME_CQE_SC_GEN_INV_SGL_DESC);
	}

	if (sgl->sgl_addr != 0)
		return (NVME_CQE_SC_GEN_INV_SGL_OFF);

	return (NVME_CQE_SC_GEN_SUCCESS);
}

static size_t
tcp_capsule_data_len(const struct nvmf_capsule *nc)
{
	assert(nc->nc_qe_len == sizeof (nvme_sqe_t));
	return (LE_32(nc->nc_sqe.sqe_dptr.d_sgl.sgl_len));
}

struct nvmf_transport_ops tcp_ops = {
	.allocate_association = tcp_allocate_association,
	.update_association = tcp_update_association,
	.free_association = tcp_free_association,
	.allocate_qpair = tcp_allocate_qpair,
	.free_qpair = tcp_free_qpair,
	.kernel_handoff_params = tcp_kernel_handoff_params,
	.populate_dle = tcp_populate_dle,
	.allocate_capsule = tcp_allocate_capsule,
	.free_capsule = tcp_free_capsule,
	.transmit_capsule = tcp_transmit_capsule,
	.receive_capsule = tcp_receive_capsule,
	.validate_command_capsule = tcp_validate_command_capsule,
	.capsule_data_len = tcp_capsule_data_len,
	.receive_controller_data = tcp_receive_controller_data,
	.send_controller_data = tcp_send_controller_data,
};

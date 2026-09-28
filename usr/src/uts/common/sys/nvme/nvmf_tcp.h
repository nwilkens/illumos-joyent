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
 * Provenance: ported to illumos from FreeBSD sys/dev/nvmf/nvmf_tcp.h.
 *
 * Original: Copyright (c) 2022-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * NVMe/TCP common PDU-header validation shared between the host (initiator) and
 * controller (target) sides of the TCP transport.  The validation logic in
 * nvmf_tcp_validate_pdu_header() is preserved field-for-field from the FreeBSD
 * source; only the OS-glue primitives differ:
 *
 *   FreeBSD                 illumos
 *   -------                 -------
 *   le32toh()/le16toh()     LE_32()/LE_16() (sys/byteorder.h)
 *   printf()                cmn_err(CE_WARN, ...)
 *   bool                    boolean_t
 *   MPASS()                 VERIFY3U()
 *   __assert_unreachable()  panic via VERIFY(0)
 *
 * The on-wire PDU structures (nvmf_tcp_common_pdu_hdr_t et al.) live in
 * <sys/nvme/nvmf.h> in the illumos tree (split out of FreeBSD's nvmf_proto.h);
 * this header consumes them and contributes only the validation helper, the
 * SGL type encodings, and the FreeBSD->illumos field-name aliases that the
 * straight port of nvmf_tcp.c relies upon.
 *
 * Section number references refer to NVM Express over Fabrics Revision 1.1
 * dated October 22, 2019.
 */

#ifndef	_SYS_NVME_NVMF_TCP_H
#define	_SYS_NVME_NVMF_TCP_H

#include <sys/types.h>
#include <sys/byteorder.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/nvme/nvmf.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SGL descriptor type/subtype encodings used by the TCP transport.
 *
 * FreeBSD defines these in nvmf_tcp.h via the NVME_SGL_TYPE() macro over the
 * base nvme.h constants.  The illumos on-wire SGL descriptor (nvme_sgl_t in
 * io/nvme/nvme_reg.h) splits the descriptor type byte into a 4-bit type
 * (sgl_type) and a 4-bit subtype (sgl_zero); here we encode the combined
 * (type << 4 | subtype) byte directly to match that layout.
 *
 * NVME_SGL_TYPE_ICD: in-capsule data, "Data Block" type with the "offset"
 * subtype, as required by NVMe/TCP for in-capsule data.
 *
 * NVME_SGL_TYPE_COMMAND_BUFFER: "Transport SGL Data Block" type with the
 * "transport-specific" subtype, used when data is moved out-of-line via R2T /
 * H2C / C2H PDUs.
 */
#define	NVMF_TCP_SGL_DESC_TYPE_DATA_BLOCK		0x0
#define	NVMF_TCP_SGL_DESC_TYPE_TRANSPORT_DATA_BLOCK	0x5
#define	NVMF_TCP_SGL_DESC_SUBTYPE_OFFSET		0x1
#define	NVMF_TCP_SGL_DESC_SUBTYPE_TRANSPORT		0xa

#define	NVMF_TCP_SGL_TYPE(type, subtype)	(((type) << 4) | (subtype))

#define	NVME_SGL_TYPE_ICD						\
	NVMF_TCP_SGL_TYPE(NVMF_TCP_SGL_DESC_TYPE_DATA_BLOCK,		\
	    NVMF_TCP_SGL_DESC_SUBTYPE_OFFSET)

#define	NVME_SGL_TYPE_COMMAND_BUFFER					\
	NVMF_TCP_SGL_TYPE(NVMF_TCP_SGL_DESC_TYPE_TRANSPORT_DATA_BLOCK,	\
	    NVMF_TCP_SGL_DESC_SUBTYPE_TRANSPORT)

/*
 * Validate common fields in a received PDU header.  If an error is
 * detected that requires an immediate disconnect, ECONNRESET is
 * returned.  If an error is detected that should be reported, EBADMSG
 * is returned and *fes and *fei are set to the values to be used in a
 * termination request PDU.  If no error is detected, 0 is returned
 * and *data_lenp is set to the length of any included data.
 */
static inline int
nvmf_tcp_validate_pdu_header(const nvmf_tcp_common_pdu_hdr_t *ch,
    boolean_t controller, boolean_t header_digests, boolean_t data_digests,
    uint8_t rxpda, uint32_t *data_lenp, uint16_t *fes, uint32_t *fei)
{
	uint32_t data_len, plen;
	uint_t expected_hlen, full_hlen;
	uint8_t digest_flags, valid_flags = 0;

	plen = LE_32(ch->ntcph_plen);
	full_hlen = ch->ntcph_hlen;
	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_HDGSTF) != 0)
		full_hlen += sizeof (uint32_t);
	if (plen == full_hlen)
		data_len = 0;
	else
		data_len = plen - ch->ntcph_pdo;

	/*
	 * Errors must be reported for the lowest incorrect field
	 * first, so validate fields in order.
	 */

	/* Validate pdu_type. */

	/* Controllers only receive PDUs with a PDU direction of 0. */
	if (controller != ((ch->ntcph_pdu_type & 0x01) == 0)) {
		cmn_err(CE_WARN, "NVMe/TCP: Invalid PDU type %u",
		    ch->ntcph_pdu_type);
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_pdu_type);
		return (EBADMSG);
	}

	switch (ch->ntcph_pdu_type) {
	case NVMF_TCP_PDU_TYPE_IC_REQ:
	case NVMF_TCP_PDU_TYPE_IC_RESP:
		/* Shouldn't get these for an established connection. */
		cmn_err(CE_WARN,
		    "NVMe/TCP: Received Initialize Connection PDU");
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_pdu_type);
		return (EBADMSG);
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
		/*
		 * 7.4.7 Termination requests with invalid PDU lengths
		 * result in an immediate connection termination
		 * without reporting an error.
		 */
		if (plen < sizeof (nvmf_tcp_term_req_hdr_t) ||
		    plen > NVMF_TCP_TERM_REQ_PDU_MAX_SIZE) {
			cmn_err(CE_WARN,
			    "NVMe/TCP: Received invalid termination request");
			return (ECONNRESET);
		}
		break;
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
	case NVMF_TCP_PDU_TYPE_R2T:
		break;
	default:
		cmn_err(CE_WARN, "NVMe/TCP: Invalid PDU type %u",
		    ch->ntcph_pdu_type);
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_pdu_type);
		return (EBADMSG);
	}

	/* Validate flags. */
	switch (ch->ntcph_pdu_type) {
	default:
		VERIFY(0);
		break;
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
		valid_flags = 0;
		break;
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
		valid_flags = NVMF_TCP_CH_FLAGS_HDGSTF |
		    NVMF_TCP_CH_FLAGS_DDGSTF;
		break;
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
	case NVMF_TCP_PDU_TYPE_R2T:
		valid_flags = NVMF_TCP_CH_FLAGS_HDGSTF;
		break;
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
		valid_flags = NVMF_TCP_CH_FLAGS_HDGSTF |
		    NVMF_TCP_CH_FLAGS_DDGSTF | NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU;
		break;
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		valid_flags = NVMF_TCP_CH_FLAGS_HDGSTF |
		    NVMF_TCP_CH_FLAGS_DDGSTF | NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU |
		    NVMF_TCP_C2H_DATA_FLAGS_SUCCESS;
		break;
	}
	if ((ch->ntcph_flags & ~valid_flags) != 0) {
		cmn_err(CE_WARN, "NVMe/TCP: Invalid PDU header flags 0x%x",
		    ch->ntcph_flags);
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_flags);
		return (EBADMSG);
	}

	/*
	 * Verify that digests are present iff enabled.  Note that the
	 * data digest will not be present if there is no data
	 * payload.
	 */
	digest_flags = 0;
	if (header_digests)
		digest_flags |= NVMF_TCP_CH_FLAGS_HDGSTF;
	if (data_digests && data_len != 0)
		digest_flags |= NVMF_TCP_CH_FLAGS_DDGSTF;
	if ((digest_flags & valid_flags) !=
	    (ch->ntcph_flags & (NVMF_TCP_CH_FLAGS_HDGSTF |
	    NVMF_TCP_CH_FLAGS_DDGSTF))) {
		cmn_err(CE_WARN, "NVMe/TCP: Invalid PDU header flags 0x%x",
		    ch->ntcph_flags);
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_flags);
		return (EBADMSG);
	}

	/* 7.4.5.2: SUCCESS in C2H requires LAST_PDU */
	if (ch->ntcph_pdu_type == NVMF_TCP_PDU_TYPE_C2H_DATA &&
	    (ch->ntcph_flags & (NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU |
	    NVMF_TCP_C2H_DATA_FLAGS_SUCCESS)) ==
	    NVMF_TCP_C2H_DATA_FLAGS_SUCCESS) {
		cmn_err(CE_WARN, "NVMe/TCP: Invalid PDU header flags 0x%x",
		    ch->ntcph_flags);
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_flags);
		return (EBADMSG);
	}

	/* Validate hlen. */
	switch (ch->ntcph_pdu_type) {
	default:
		VERIFY(0);
		break;
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
		expected_hlen = sizeof (nvmf_tcp_term_req_hdr_t);
		break;
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
		expected_hlen = sizeof (nvmf_tcp_cmd_t);
		break;
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
		expected_hlen = sizeof (nvmf_tcp_rsp_t);
		break;
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
		expected_hlen = sizeof (nvmf_tcp_h2c_data_hdr_t);
		break;
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		expected_hlen = sizeof (nvmf_tcp_c2h_data_hdr_t);
		break;
	case NVMF_TCP_PDU_TYPE_R2T:
		expected_hlen = sizeof (nvmf_tcp_r2t_hdr_t);
		break;
	}
	if (ch->ntcph_hlen != expected_hlen) {
		cmn_err(CE_WARN, "NVMe/TCP: Invalid PDU header length %u",
		    ch->ntcph_hlen);
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_hlen);
		return (EBADMSG);
	}

	/* Validate pdo. */
	switch (ch->ntcph_pdu_type) {
	default:
		VERIFY(0);
		break;
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
	case NVMF_TCP_PDU_TYPE_R2T:
		if (ch->ntcph_pdo != 0) {
			cmn_err(CE_WARN,
			    "NVMe/TCP: Invalid PDU data offset %u",
			    ch->ntcph_pdo);
			*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_pdo);
			return (EBADMSG);
		}
		break;
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		/* Permit PDO of 0 if there is no data. */
		if (data_len == 0 && ch->ntcph_pdo == 0)
			break;

		if (ch->ntcph_pdo < full_hlen || ch->ntcph_pdo > plen ||
		    ch->ntcph_pdo % rxpda != 0) {
			cmn_err(CE_WARN,
			    "NVMe/TCP: Invalid PDU data offset %u",
			    ch->ntcph_pdo);
			*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_pdo);
			return (EBADMSG);
		}
		break;
	}

	/* Validate plen. */
	if (plen < ch->ntcph_hlen) {
		cmn_err(CE_WARN, "NVMe/TCP: Invalid PDU length %u", plen);
		*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_plen);
		return (EBADMSG);
	}

	switch (ch->ntcph_pdu_type) {
	default:
		VERIFY(0);
		break;
	case NVMF_TCP_PDU_TYPE_H2C_TERM_REQ:
	case NVMF_TCP_PDU_TYPE_C2H_TERM_REQ:
		/* Checked above. */
		VERIFY3U(plen, <=, NVMF_TCP_TERM_REQ_PDU_MAX_SIZE);
		break;
	case NVMF_TCP_PDU_TYPE_CAPSULE_CMD:
	case NVMF_TCP_PDU_TYPE_H2C_DATA:
	case NVMF_TCP_PDU_TYPE_C2H_DATA:
		if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_DDGSTF) != 0 &&
		    data_len <= sizeof (uint32_t)) {
			cmn_err(CE_WARN,
			    "NVMe/TCP: PDU %u too short for digest",
			    ch->ntcph_pdu_type);
			*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_plen);
			return (EBADMSG);
		}
		break;
	case NVMF_TCP_PDU_TYPE_R2T:
	case NVMF_TCP_PDU_TYPE_CAPSULE_RESP:
		if (data_len != 0) {
			cmn_err(CE_WARN,
			    "NVMe/TCP: PDU %u with data length %u",
			    ch->ntcph_pdu_type, data_len);
			*fes = NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			*fei = offsetof(nvmf_tcp_common_pdu_hdr_t, ntcph_plen);
			return (EBADMSG);
		}
		break;
	}

	if ((ch->ntcph_flags & NVMF_TCP_CH_FLAGS_DDGSTF) != 0)
		data_len -= sizeof (uint32_t);

	*data_lenp = data_len;
	return (0);
}

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NVME_NVMF_TCP_H */

/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/* Derived from include/spdk/nvmf_spec.h from Intel's SPDK. */

/*
 * Provenance: ported to illumos from FreeBSD
 * sys/dev/nvmf/nvmf_proto.h (which is itself derived from Intel's SPDK
 * include/spdk/nvmf_spec.h). Only the NVMe over Fabrics wire/specification
 * definitions that are not already present in <sys/nvme.h> are reproduced
 * here. Base NVMe types are reused from <sys/nvme.h> rather than duplicated.
 * Field names, sizes, and semantics are preserved exactly from the source;
 * symbols have been renamed to illumos conventions and recorded in the port.
 */

#ifndef _SYS_NVME_NVMF_H
#define	_SYS_NVME_NVMF_H

#include <sys/nvme.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NVMe over Fabrics specification definitions.
 *
 * All structures below describe data exactly as it appears on the wire and are
 * laid out with pack(1) to match the specification, following the idiom used by
 * <sys/nvme.h>. Bit-fields are ordered least-significant-field-first; illumos is
 * built little-endian only, so no byte-order shuffling is required.
 */

/*
 * Size of the NVMe Qualified Name (NQN) fields used in Fabrics structures.
 * (FreeBSD: NVME_NQN_FIELD_SIZE)
 */
#define	NVMF_NQN_FIELD_SIZE		256

#pragma pack(1)

/*
 * Scatter/Gather List descriptor as it appears within a Fabrics capsule. The
 * public <sys/nvme.h> does not expose a generic on-wire SGL descriptor (the
 * nvme(4D) driver keeps its own bit-field form in the driver-private
 * io/nvme/nvme_reg.h), so the spec layout used by Fabrics commands is defined
 * here. (FreeBSD: struct nvme_sgl_descriptor)
 */
typedef struct {
	uint64_t	sgl_address;
	uint32_t	sgl_length;
	uint8_t		sgl_reserved[3];
	uint8_t		sgl_type;		/* type (hi nibble) / subtype (lo) */
} nvmf_sgl_descriptor_t;

/* SGL type / subtype packing within sgl_type. */
#define	NVMF_SGL_SUBTYPE_SHIFT		0
#define	NVMF_SGL_SUBTYPE_MASK		0xf
#define	NVMF_SGL_TYPE_SHIFT		4
#define	NVMF_SGL_TYPE_MASK		0xf
#define	NVMF_SGL_TYPE(type, subtype)	\
	((subtype) << NVMF_SGL_SUBTYPE_SHIFT | (type) << NVMF_SGL_TYPE_SHIFT)

/*
 * Fabrics-specific SGL descriptor subtype: invalidate the supplied key as part
 * of the transfer. (FreeBSD: NVME_SGL_SUBTYPE_INVALIDATE_KEY)
 */
#define	NVMF_SGL_SUBTYPE_INVALIDATE_KEY	0xf

/*
 * Generic 64-byte Fabrics command capsule. The opcode for all Fabrics commands
 * is the Fabrics command opcode and fctype selects the specific command.
 * (FreeBSD: struct nvmf_capsule_cmd)
 */
typedef struct {
	uint8_t		nfc_opcode;
	uint8_t		nfc_reserved1;
	uint16_t	nfc_cid;
	uint8_t		nfc_fctype;
	uint8_t		nfc_reserved2[35];
	uint8_t		nfc_fabric_specific[24];
} nvmf_capsule_cmd_t;

/*
 * Fabrics Command Set (Fabrics Command Type, fctype).
 * (FreeBSD: enum nvmf_fabric_cmd_types)
 */
typedef enum {
	NVMF_FCTYPE_PROPERTY_SET	= 0x00,
	NVMF_FCTYPE_CONNECT		= 0x01,
	NVMF_FCTYPE_PROPERTY_GET	= 0x04,
	NVMF_FCTYPE_AUTH_SEND		= 0x05,
	NVMF_FCTYPE_AUTH_RECV		= 0x06,
	NVMF_FCTYPE_DISCONNECT		= 0x08,
	NVMF_FCTYPE_VENDOR_START	= 0xc0
} nvmf_fabric_cmd_type_t;

/*
 * Fabrics command status codes (Command Specific status, status code type 0x1).
 * (FreeBSD: enum nvmf_fabric_cmd_status_code)
 */
typedef enum {
	NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT	= 0x80,
	NVMF_FABRIC_SC_CONTROLLER_BUSY		= 0x81,
	NVMF_FABRIC_SC_INVALID_PARAM		= 0x82,
	NVMF_FABRIC_SC_RESTART_DISCOVERY	= 0x83,
	NVMF_FABRIC_SC_INVALID_HOST		= 0x84,
	NVMF_FABRIC_SC_INVALID_QUEUE_TYPE	= 0x85,
	NVMF_FABRIC_SC_LOG_RESTART_DISCOVERY	= 0x90,
	NVMF_FABRIC_SC_AUTH_REQUIRED		= 0x91
} nvmf_fabric_cmd_status_code_t;

/*
 * RDMA QP service types. (FreeBSD: enum nvmf_rdma_qptype)
 */
typedef enum {
	NVMF_RDMA_QPTYPE_RELIABLE_CONNECTED	= 0x1,
	NVMF_RDMA_QPTYPE_RELIABLE_DATAGRAM	= 0x2
} nvmf_rdma_qptype_t;

/*
 * RDMA provider types. (FreeBSD: enum nvmf_rdma_prtype)
 */
typedef enum {
	NVMF_RDMA_PRTYPE_NONE	= 0x1,
	NVMF_RDMA_PRTYPE_IB	= 0x2,	/* InfiniBand */
	NVMF_RDMA_PRTYPE_ROCE	= 0x3,	/* RoCE v1 */
	NVMF_RDMA_PRTYPE_ROCE2	= 0x4,	/* RoCE v2 */
	NVMF_RDMA_PRTYPE_IWARP	= 0x5
} nvmf_rdma_prtype_t;

/*
 * RDMA connection management service types. (FreeBSD: enum nvmf_rdma_cms)
 */
typedef enum {
	NVMF_RDMA_CMS_RDMA_CM	= 0x1	/* Sockets based endpoint addressing */
} nvmf_rdma_cms_t;

/*
 * NVMe over Fabrics transport types. (FreeBSD: enum nvmf_trtype)
 */
typedef enum {
	NVMF_TRTYPE_RDMA	= 0x1,
	NVMF_TRTYPE_FC		= 0x2,
	NVMF_TRTYPE_TCP		= 0x3,
	NVMF_TRTYPE_INTRA_HOST	= 0xfe	/* loopback */
} nvmf_trtype_t;

/*
 * Address family types. (FreeBSD: enum nvmf_adrfam)
 */
typedef enum {
	NVMF_ADRFAM_IPV4	= 0x1,	/* AF_INET */
	NVMF_ADRFAM_IPV6	= 0x2,	/* AF_INET6 */
	NVMF_ADRFAM_IB		= 0x3,	/* AF_IB */
	NVMF_ADRFAM_FC		= 0x4,	/* Fibre Channel */
	NVMF_ADRFAM_INTRA_HOST	= 0xfe	/* loopback */
} nvmf_adrfam_t;

/*
 * NVM subsystem types. (FreeBSD: enum nvmf_subtype)
 */
typedef enum {
	NVMF_SUBTYPE_DISCOVERY		= 0x1,	/* referral to discovery svc */
	NVMF_SUBTYPE_NVME		= 0x2,	/* NVM subsystem */
	NVMF_SUBTYPE_DISCOVERY_CURRENT	= 0x3	/* current discovery subsys */
} nvmf_subtype_t;

/* Discovery Log Entry Flags. (FreeBSD: NVMF_DISCOVERY_LOG_EFLAGS_*) */
#define	NVMF_DISCOVERY_LOG_EFLAGS_DUPRETINFO	(1u << 0u)
#define	NVMF_DISCOVERY_LOG_EFLAGS_EPCSD		(1u << 1u)

/*
 * Fabric secure channel requirement. (FreeBSD: enum nvmf_treq_secure_channel)
 */
typedef enum {
	NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED	= 0x0,
	NVMF_TREQ_SECURE_CHANNEL_REQUIRED	= 0x1,
	NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED	= 0x2
} nvmf_treq_secure_channel_t;

/*
 * Common 64-byte Fabrics command. (FreeBSD: struct nvmf_fabric_cmd)
 */
typedef struct {
	uint8_t		nfc_opcode;
	uint8_t		nfc_reserved1;
	uint16_t	nfc_cid;
	uint8_t		nfc_fctype;
	uint8_t		nfc_reserved2[59];
} nvmf_fabric_cmd_t;

/*
 * Authentication Receive command (fctype NVMF_FCTYPE_AUTH_RECV).
 * (FreeBSD: struct nvmf_fabric_auth_recv_cmd)
 */
typedef struct {
	uint8_t			nfar_opcode;
	uint8_t			nfar_reserved1;
	uint16_t		nfar_cid;
	uint8_t			nfar_fctype;
	uint8_t			nfar_reserved2[19];
	nvmf_sgl_descriptor_t	nfar_sgl1;
	uint8_t			nfar_reserved3;
	uint8_t			nfar_spsp0;
	uint8_t			nfar_spsp1;
	uint8_t			nfar_secp;
	uint32_t		nfar_al;
	uint8_t			nfar_reserved4[16];
} nvmf_fabric_auth_recv_cmd_t;

/*
 * Authentication Send command (fctype NVMF_FCTYPE_AUTH_SEND).
 * (FreeBSD: struct nvmf_fabric_auth_send_cmd)
 */
typedef struct {
	uint8_t			nfas_opcode;
	uint8_t			nfas_reserved1;
	uint16_t		nfas_cid;
	uint8_t			nfas_fctype;
	uint8_t			nfas_reserved2[19];
	nvmf_sgl_descriptor_t	nfas_sgl1;
	uint8_t			nfas_reserved3;
	uint8_t			nfas_spsp0;
	uint8_t			nfas_spsp1;
	uint8_t			nfas_secp;
	uint32_t		nfas_tl;
	uint8_t			nfas_reserved4[16];
} nvmf_fabric_auth_send_cmd_t;

/*
 * Connect command data (in-capsule data, 1024 bytes).
 * (FreeBSD: struct nvmf_fabric_connect_data)
 */
typedef struct {
	uint8_t		nfcd_hostid[16];
	uint16_t	nfcd_cntlid;
	uint8_t		nfcd_reserved5[238];
	uint8_t		nfcd_subnqn[NVMF_NQN_FIELD_SIZE];
	uint8_t		nfcd_hostnqn[NVMF_NQN_FIELD_SIZE];
	uint8_t		nfcd_reserved6[256];
} nvmf_fabric_connect_data_t;

/*
 * Connect command (fctype NVMF_FCTYPE_CONNECT).
 * (FreeBSD: struct nvmf_fabric_connect_cmd)
 */
typedef struct {
	uint8_t			nfcc_opcode;
	uint8_t			nfcc_reserved1;
	uint16_t		nfcc_cid;
	uint8_t			nfcc_fctype;
	uint8_t			nfcc_reserved2[19];
	nvmf_sgl_descriptor_t	nfcc_sgl1;
	uint16_t		nfcc_recfmt;	/* Connect Record Format */
	uint16_t		nfcc_qid;	/* Queue Identifier */
	uint16_t		nfcc_sqsize;	/* Submission Queue Size */
	uint8_t			nfcc_cattr;	/* queue attributes */
	uint8_t			nfcc_reserved3;
	uint32_t		nfcc_kato;	/* keep alive timeout */
	uint8_t			nfcc_reserved4[12];
} nvmf_fabric_connect_cmd_t;

/* Controller ID values used by Connect. (FreeBSD: NVMF_CNTLID_*) */
#define	NVMF_CNTLID_DYNAMIC	0xffff
#define	NVMF_CNTLID_STATIC_ANY	0xfffe
/* Upper bound per NVMe-oF 1.1 section 5.3 Discovery Log Entry. */
#define	NVMF_CNTLID_STATIC_MAX	0xffef

/* Default Keep Alive Timeout in ms, NVMe 1.4b section 5.21.1.15. */
#define	NVMF_KATO_DEFAULT	120000

/* Connect command attributes (nfcc_cattr). (FreeBSD: NVMF_CONNECT_ATTR_*) */
#define	NVMF_CONNECT_ATTR_PRIORITY_CLASS	0x3
#define	NVMF_CONNECT_ATTR_DISABLE_SQ_FC		(1u << 2)
#define	NVMF_CONNECT_ATTR_IO_QUEUE_DELETION	(1u << 3)

/*
 * Connect response (16-byte completion). (FreeBSD: struct nvmf_fabric_connect_rsp)
 */
typedef struct {
	union {
		struct {
			uint16_t	cntlid;
			uint16_t	authreq;
		} success;
		struct {
			uint16_t	ipo;
			uint8_t		iattr;
			uint8_t		reserved;
		} invalid;
		uint32_t		raw;
	} nfcr_status_code_specific;
	uint32_t	nfcr_reserved0;
	uint16_t	nfcr_sqhd;
	uint16_t	nfcr_reserved1;
	uint16_t	nfcr_cid;
	uint16_t	nfcr_status;
} nvmf_fabric_connect_rsp_t;

/*
 * Disconnect command (fctype NVMF_FCTYPE_DISCONNECT).
 * (FreeBSD: struct nvmf_fabric_disconnect_cmd)
 */
typedef struct {
	uint8_t			nfdc_opcode;
	uint8_t			nfdc_reserved1;
	uint16_t		nfdc_cid;
	uint8_t			nfdc_fctype;
	uint8_t			nfdc_reserved2[19];
	nvmf_sgl_descriptor_t	nfdc_sgl1;
	uint16_t		nfdc_recfmt;	/* Disconnect Record Format */
	uint8_t			nfdc_reserved3[22];
} nvmf_fabric_disconnect_cmd_t;

/* Property size attribute values. (FreeBSD: NVMF_PROP_SIZE_*) */
#define	NVMF_PROP_SIZE_4	0
#define	NVMF_PROP_SIZE_8	1

/* Controller property offsets used by Property Get/Set. (FreeBSD: NVMF_PROP_*) */
#define	NVMF_PROP_CAP	0x00	/* Controller Capabilities */
#define	NVMF_PROP_VS	0x08	/* Version */
#define	NVMF_PROP_CC	0x14	/* Controller Configuration */
#define	NVMF_PROP_CSTS	0x1c	/* Controller Status */
#define	NVMF_PROP_NSSR	0x20	/* NVM Subsystem Reset */

/*
 * Property Get command (fctype NVMF_FCTYPE_PROPERTY_GET).
 * (FreeBSD: struct nvmf_fabric_prop_get_cmd)
 */
typedef struct {
	uint8_t		nfpg_opcode;
	uint8_t		nfpg_reserved1;
	uint16_t	nfpg_cid;
	uint8_t		nfpg_fctype;
	uint8_t		nfpg_reserved2[35];
	struct {
		uint8_t	size		: 3;
		uint8_t	reserved	: 5;
	} nfpg_attrib;
	uint8_t		nfpg_reserved3[3];
	uint32_t	nfpg_ofst;
	uint8_t		nfpg_reserved4[16];
} nvmf_fabric_prop_get_cmd_t;

/*
 * Property Get response (16-byte completion).
 * (FreeBSD: struct nvmf_fabric_prop_get_rsp)
 */
typedef struct {
	union {
		uint64_t	u64;
		struct {
			uint32_t	low;
			uint32_t	high;
		} u32;
	} nfpr_value;
	uint16_t	nfpr_sqhd;
	uint16_t	nfpr_reserved0;
	uint16_t	nfpr_cid;
	uint16_t	nfpr_status;
} nvmf_fabric_prop_get_rsp_t;

/*
 * Property Set command (fctype NVMF_FCTYPE_PROPERTY_SET).
 * (FreeBSD: struct nvmf_fabric_prop_set_cmd)
 */
typedef struct {
	uint8_t		nfps_opcode;
	uint8_t		nfps_reserved0;
	uint16_t	nfps_cid;
	uint8_t		nfps_fctype;
	uint8_t		nfps_reserved1[35];
	struct {
		uint8_t	size		: 3;
		uint8_t	reserved	: 5;
	} nfps_attrib;
	uint8_t		nfps_reserved2[3];
	uint32_t	nfps_ofst;
	union {
		uint64_t	u64;
		struct {
			uint32_t	low;
			uint32_t	high;
		} u32;
	} nfps_value;
	uint8_t		nfps_reserved4[8];
} nvmf_fabric_prop_set_cmd_t;

/* NQN string constants and lengths. (FreeBSD: NVMF_NQN_*, NVMF_*_NQN) */
#define	NVMF_NQN_MIN_LEN	11	/* spec prefix is 11 characters */
#define	NVMF_NQN_MAX_LEN	223
#define	NVMF_NQN_UUID_PRE_LEN	32
#define	NVMF_UUID_STRING_LEN	36
#define	NVMF_NQN_UUID_PRE	"nqn.2014-08.org.nvmexpress:uuid:"
#define	NVMF_DISCOVERY_NQN	"nqn.2014-08.org.nvmexpress.discovery"

/* Transport address string field maxima. (FreeBSD: NVMF_TR*_MAX_LEN) */
#define	NVMF_TRSTRING_MAX_LEN	32
#define	NVMF_TRADDR_MAX_LEN	256
#define	NVMF_TRSVCID_MAX_LEN	32

/*
 * RDMA transport-specific address subtype (TSAS).
 * (FreeBSD: struct nvmf_rdma_transport_specific_address_subtype)
 */
typedef struct {
	uint8_t		nrtsas_rdma_qptype;	/* nvmf_rdma_qptype_t */
	uint8_t		nrtsas_rdma_prtype;	/* nvmf_rdma_prtype_t */
	uint8_t		nrtsas_rdma_cms;	/* nvmf_rdma_cms_t */
	uint8_t		nrtsas_reserved0[5];
	uint16_t	nrtsas_rdma_pkey;	/* partition key for AF_IB */
	uint8_t		nrtsas_reserved2[246];
} nvmf_rdma_tsas_t;

/* TCP Secure Socket Type. (FreeBSD: enum nvme_tcp_secure_socket_type) */
typedef enum {
	NVMF_TCP_SECURITY_NONE		= 0,
	NVMF_TCP_SECURITY_TLS_1_2	= 1,
	NVMF_TCP_SECURITY_TLS_1_3	= 2
} nvmf_tcp_secure_socket_type_t;

/*
 * TCP transport-specific address subtype (TSAS).
 * (FreeBSD: struct nvme_tcp_transport_specific_address_subtype)
 */
typedef struct {
	uint8_t		nttsas_sectype;		/* nvmf_tcp_secure_socket_type_t */
	uint8_t		nttsas_reserved0[255];
} nvmf_tcp_tsas_t;

/*
 * Transport-specific address subtype union.
 * (FreeBSD: union nvmf_transport_specific_address_subtype)
 */
typedef union {
	uint8_t			raw[256];
	nvmf_rdma_tsas_t	rdma;
	nvmf_tcp_tsas_t		tcp;
} nvmf_tsas_t;

/* Minimum admin queue Max SQ size. (FreeBSD: NVMF_MIN_ADMIN_MAX_SQ_SIZE) */
#define	NVMF_MIN_ADMIN_MAX_SQ_SIZE	32

/*
 * Discovery Log Page entry. (FreeBSD: struct nvmf_discovery_log_page_entry)
 */
typedef struct {
	uint8_t		ndle_trtype;	/* nvmf_trtype_t */
	uint8_t		ndle_adrfam;	/* nvmf_adrfam_t */
	uint8_t		ndle_subtype;	/* nvmf_subtype_t */
	struct {
		uint8_t	secure_channel	: 2;	/* nvmf_treq_secure_channel_t */
		uint8_t	reserved	: 6;
	} ndle_treq;
	uint16_t	ndle_portid;	/* NVM subsystem port ID */
	uint16_t	ndle_cntlid;	/* Controller ID */
	uint16_t	ndle_asqsz;	/* Admin max SQ size */
	uint16_t	ndle_eflags;	/* Entry Flags */
	uint8_t		ndle_reserved0[20];
	uint8_t		ndle_trsvcid[NVMF_TRSVCID_MAX_LEN];
	uint8_t		ndle_reserved1[192];
	uint8_t		ndle_subnqn[256];
	uint8_t		ndle_traddr[NVMF_TRADDR_MAX_LEN];
	nvmf_tsas_t	ndle_tsas;
} nvmf_discovery_log_page_entry_t;

/*
 * Discovery Log Page header. (FreeBSD: struct nvmf_discovery_log_page)
 */
typedef struct {
	uint64_t	ndlp_genctr;
	uint64_t	ndlp_numrec;
	uint16_t	ndlp_recfmt;
	uint8_t		ndlp_reserved0[1006];
	nvmf_discovery_log_page_entry_t	ndlp_entries[];
} nvmf_discovery_log_page_t;

/*
 * RDMA fabric-specific connection management private data.
 */

/* (FreeBSD: struct nvmf_rdma_request_private_data) */
typedef struct {
	uint16_t	nrrpd_recfmt;	/* record format */
	uint16_t	nrrpd_qid;	/* queue id */
	uint16_t	nrrpd_hrqsize;	/* host receive queue size */
	uint16_t	nrrpd_hsqsize;	/* host send queue size */
	uint16_t	nrrpd_cntlid;	/* controller id */
	uint8_t		nrrpd_reserved[22];
} nvmf_rdma_request_private_data_t;

/* (FreeBSD: struct nvmf_rdma_accept_private_data) */
typedef struct {
	uint16_t	nrapd_recfmt;	/* record format */
	uint16_t	nrapd_crqsize;	/* controller receive queue size */
	uint8_t		nrapd_reserved[28];
} nvmf_rdma_accept_private_data_t;

/* (FreeBSD: struct nvmf_rdma_reject_private_data) */
typedef struct {
	uint16_t	nrjpd_recfmt;	/* record format */
	uint16_t	nrjpd_sts;	/* status */
} nvmf_rdma_reject_private_data_t;

/* (FreeBSD: union nvmf_rdma_private_data) */
typedef union {
	nvmf_rdma_request_private_data_t	pd_request;
	nvmf_rdma_accept_private_data_t		pd_accept;
	nvmf_rdma_reject_private_data_t		pd_reject;
} nvmf_rdma_private_data_t;

/* RDMA transport errors. (FreeBSD: enum nvmf_rdma_transport_error) */
typedef enum {
	NVMF_RDMA_ERROR_INVALID_PRIVATE_DATA_LENGTH	= 0x1,
	NVMF_RDMA_ERROR_INVALID_RECFMT			= 0x2,
	NVMF_RDMA_ERROR_INVALID_QID			= 0x3,
	NVMF_RDMA_ERROR_INVALID_HSQSIZE			= 0x4,
	NVMF_RDMA_ERROR_INVALID_HRQSIZE			= 0x5,
	NVMF_RDMA_ERROR_NO_RESOURCES			= 0x6,
	NVMF_RDMA_ERROR_INVALID_IRD			= 0x7,
	NVMF_RDMA_ERROR_INVALID_ORD			= 0x8
} nvmf_rdma_transport_error_t;

/*
 * TCP transport-specific definitions.
 */

/* NVMe/TCP PDU type. (FreeBSD: enum nvme_tcp_pdu_type) */
typedef enum {
	NVMF_TCP_PDU_TYPE_IC_REQ	= 0x00,	/* ICReq */
	NVMF_TCP_PDU_TYPE_IC_RESP	= 0x01,	/* ICResp */
	NVMF_TCP_PDU_TYPE_H2C_TERM_REQ	= 0x02,	/* TermReq */
	NVMF_TCP_PDU_TYPE_C2H_TERM_REQ	= 0x03,	/* TermResp */
	NVMF_TCP_PDU_TYPE_CAPSULE_CMD	= 0x04,	/* CapsuleCmd */
	NVMF_TCP_PDU_TYPE_CAPSULE_RESP	= 0x05,	/* CapsuleResp */
	NVMF_TCP_PDU_TYPE_H2C_DATA	= 0x06,	/* H2CData */
	NVMF_TCP_PDU_TYPE_C2H_DATA	= 0x07,	/* C2HData */
	NVMF_TCP_PDU_TYPE_R2T		= 0x09	/* R2T */
} nvmf_tcp_pdu_type_t;

/*
 * Common NVMe/TCP PDU header. (FreeBSD: struct nvme_tcp_common_pdu_hdr)
 */
typedef struct {
	uint8_t		ntcph_pdu_type;	/* nvmf_tcp_pdu_type_t */
	uint8_t		ntcph_flags;	/* pdu_type-specific flags */
	uint8_t		ntcph_hlen;	/* PDU header length (no HDGST) */
	uint8_t		ntcph_pdo;	/* PDU Data Offset from start of PDU */
	uint32_t	ntcph_plen;	/* total PDU bytes, including header */
} nvmf_tcp_common_pdu_hdr_t;

/* Common PDU header flags. (FreeBSD: NVME_TCP_CH_FLAGS_*) */
#define	NVMF_TCP_CH_FLAGS_HDGSTF	(1u << 0)
#define	NVMF_TCP_CH_FLAGS_DDGSTF	(1u << 1)

/*
 * ICReq PDU (nfic_common.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_IC_REQ).
 * (FreeBSD: struct nvme_tcp_ic_req)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	nfic_common;
	uint16_t			nfic_pfv;
	uint8_t				nfic_hpda;	/* host data alignment */
	union {
		uint8_t			raw;
		struct {
			uint8_t		hdgst_enable : 1;
			uint8_t		ddgst_enable : 1;
			uint8_t		reserved : 6;
		} bits;
	} nfic_dgst;
	uint32_t			nfic_maxr2t;
	uint8_t				nfic_reserved16[112];
} nvmf_tcp_ic_req_t;

/* Data alignment maxima. (FreeBSD: NVME_TCP_HPDA_MAX/CPDA_MAX/PDU_PDO_MAX_OFFSET) */
#define	NVMF_TCP_HPDA_MAX		31
#define	NVMF_TCP_CPDA_MAX		31
#define	NVMF_TCP_PDU_PDO_MAX_OFFSET	((NVMF_TCP_CPDA_MAX + 1) << 2)

/*
 * ICResp PDU (nfir_common.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_IC_RESP).
 * (FreeBSD: struct nvme_tcp_ic_resp)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	nfir_common;
	uint16_t			nfir_pfv;
	uint8_t				nfir_cpda;	/* ctrl data alignment */
	union {
		uint8_t			raw;
		struct {
			uint8_t		hdgst_enable : 1;
			uint8_t		ddgst_enable : 1;
			uint8_t		reserved : 6;
		} bits;
	} nfir_dgst;
	uint32_t			nfir_maxh2cdata;
	uint8_t				nfir_reserved16[112];
} nvmf_tcp_ic_resp_t;

/*
 * TermReq PDU. (FreeBSD: struct nvme_tcp_term_req_hdr)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	nttr_common;
	uint16_t			nttr_fes;
	uint8_t				nttr_fei[4];
	uint8_t				nttr_reserved14[10];
} nvmf_tcp_term_req_hdr_t;

/* Fatal Error Status values. (FreeBSD: enum nvme_tcp_term_req_fes) */
typedef enum {
	NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD		= 0x01,
	NVMF_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR		= 0x02,
	NVMF_TCP_TERM_REQ_FES_HDGST_ERROR			= 0x03,
	NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE	= 0x04,
	NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED	= 0x05,
	NVMF_TCP_TERM_REQ_FES_R2T_LIMIT_EXCEEDED		= 0x05,
	NVMF_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER = 0x06
} nvmf_tcp_term_req_fes_t;

/* TermReq PDU size limits. (FreeBSD: NVME_TCP_TERM_REQ_*) */
#define	NVMF_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE	128
#define	NVMF_TCP_TERM_REQ_PDU_MAX_SIZE	\
	(NVMF_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE + \
	sizeof (nvmf_tcp_term_req_hdr_t))

/*
 * CapsuleCmd PDU (ntc_common.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_CAPSULE_CMD).
 * The embedded ntc_ccsqe is a 64-byte NVMe submission queue entry; for Fabrics
 * commands it is a Fabrics command capsule (nvmf_capsule_cmd_t and friends).
 * (FreeBSD: struct nvme_tcp_cmd)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	ntc_common;
	uint8_t				ntc_ccsqe[64];
	/* icdoff hdgst padding + in-capsule data + ddgst (if enabled) */
} nvmf_tcp_cmd_t;

/*
 * CapsuleResp PDU (ntr_common.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_CAPSULE_RESP).
 * The embedded ntr_rccqe is a 16-byte NVMe completion queue entry.
 * (FreeBSD: struct nvme_tcp_rsp)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	ntr_common;
	uint8_t				ntr_rccqe[16];
} nvmf_tcp_rsp_t;

/*
 * H2CData PDU (nth2c_common.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_H2C_DATA).
 * (FreeBSD: struct nvme_tcp_h2c_data_hdr)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	nth2c_common;
	uint16_t			nth2c_cccid;
	uint16_t			nth2c_ttag;
	uint32_t			nth2c_datao;
	uint32_t			nth2c_datal;
	uint8_t				nth2c_reserved20[4];
} nvmf_tcp_h2c_data_hdr_t;

/* H2CData flags / PDO multiple. (FreeBSD: NVME_TCP_H2C_DATA_*) */
#define	NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU	(1u << 2)
#define	NVMF_TCP_H2C_DATA_FLAGS_SUCCESS		(1u << 3)
#define	NVMF_TCP_H2C_DATA_PDO_MULT		8u

/*
 * C2HData PDU (ntc2c_common.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_C2H_DATA).
 * (FreeBSD: struct nvme_tcp_c2h_data_hdr)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	ntc2c_common;
	uint16_t			ntc2c_cccid;
	uint8_t				ntc2c_reserved10[2];
	uint32_t			ntc2c_datao;
	uint32_t			ntc2c_datal;
	uint8_t				ntc2c_reserved20[4];
} nvmf_tcp_c2h_data_hdr_t;

/* C2HData flags / PDO multiple. (FreeBSD: NVME_TCP_C2H_DATA_*) */
#define	NVMF_TCP_C2H_DATA_FLAGS_SUCCESS		(1u << 3)
#define	NVMF_TCP_C2H_DATA_FLAGS_LAST_PDU	(1u << 2)
#define	NVMF_TCP_C2H_DATA_PDO_MULT		8u

/*
 * R2T PDU (ntr2t_common.ntcph_pdu_type == NVMF_TCP_PDU_TYPE_R2T).
 * (FreeBSD: struct nvme_tcp_r2t_hdr)
 */
typedef struct {
	nvmf_tcp_common_pdu_hdr_t	ntr2t_common;
	uint16_t			ntr2t_cccid;
	uint16_t			ntr2t_ttag;
	uint32_t			ntr2t_r2to;
	uint32_t			ntr2t_r2tl;
	uint8_t				ntr2t_reserved20[4];
} nvmf_tcp_r2t_hdr_t;

#pragma pack()	/* pack(1) */

/*
 * Compile-time validation of on-wire sizes, mirroring the _Static_assert checks
 * in the FreeBSD source so any layout drift is caught at build time.
 */
CTASSERT(sizeof (nvmf_sgl_descriptor_t) == 16);
CTASSERT(sizeof (nvmf_capsule_cmd_t) == 64);
CTASSERT(sizeof (nvmf_fabric_auth_recv_cmd_t) == 64);
CTASSERT(sizeof (nvmf_fabric_auth_send_cmd_t) == 64);
CTASSERT(sizeof (nvmf_fabric_connect_data_t) == 1024);
CTASSERT(sizeof (nvmf_fabric_connect_cmd_t) == 64);
CTASSERT(sizeof (nvmf_fabric_connect_rsp_t) == 16);
CTASSERT(sizeof (nvmf_fabric_disconnect_cmd_t) == 64);
CTASSERT(sizeof (nvmf_fabric_prop_get_cmd_t) == 64);
CTASSERT(sizeof (nvmf_fabric_prop_get_rsp_t) == 16);
CTASSERT(sizeof (nvmf_fabric_prop_set_cmd_t) == 64);
CTASSERT(sizeof (nvmf_rdma_tsas_t) == 256);
CTASSERT(sizeof (nvmf_tcp_tsas_t) == 256);
CTASSERT(sizeof (nvmf_tsas_t) == 256);
CTASSERT(sizeof (nvmf_discovery_log_page_entry_t) == 1024);
CTASSERT(sizeof (nvmf_rdma_request_private_data_t) == 32);
CTASSERT(sizeof (nvmf_rdma_accept_private_data_t) == 32);
CTASSERT(sizeof (nvmf_rdma_reject_private_data_t) == 4);
CTASSERT(sizeof (nvmf_rdma_private_data_t) == 32);
CTASSERT(sizeof (nvmf_tcp_common_pdu_hdr_t) == 8);
CTASSERT(sizeof (nvmf_tcp_ic_req_t) == 128);
CTASSERT(sizeof (nvmf_tcp_ic_resp_t) == 128);
CTASSERT(sizeof (nvmf_tcp_term_req_hdr_t) == 24);
CTASSERT(sizeof (nvmf_tcp_cmd_t) == 72);
CTASSERT(sizeof (nvmf_tcp_rsp_t) == 24);
CTASSERT(sizeof (nvmf_tcp_h2c_data_hdr_t) == 24);
CTASSERT(sizeof (nvmf_tcp_c2h_data_hdr_t) == 24);
CTASSERT(sizeof (nvmf_tcp_r2t_hdr_t) == 24);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NVME_NVMF_H */

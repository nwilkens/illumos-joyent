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

#ifndef	_LIBNVMF_NVMF_TCP_IMPL_H
#define	_LIBNVMF_NVMF_TCP_IMPL_H

/*
 * Definitions shared by the files of the userland NVMe/TCP transport.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/uio.h>

#include "libnvmf.h"
#include "internal.h"

/*
 * nvmf_tcp_validate_pdu_header() reports through cmn_err().  The library
 * writes nothing on its own, so the messages are dropped here.
 */
#include <sys/cmn_err.h>
#include <sys/debug.h>

#ifndef	_KERNEL
#define	cmn_err(level, ...)	((void)0)
#endif

#include <sys/nvme/nvmf_tcp.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvmf_tcp_qpair;

typedef struct nvmf_tcp_command_buffer {
	struct nvmf_tcp_qpair *qp;

	void	*data;
	size_t	data_len;
	size_t	data_xfered;
	uint32_t data_offset;

	uint16_t cid;
	uint16_t ttag;
	bool	linked;

	LIST_ENTRY(nvmf_tcp_command_buffer) link;
} nvmf_tcp_command_buffer_t;

LIST_HEAD(nvmf_tcp_command_buffer_list, nvmf_tcp_command_buffer);

typedef struct nvmf_tcp_association {
	struct nvmf_association na;

	uint32_t ioccsz;
} nvmf_tcp_association_t;

typedef struct nvmf_tcp_rxpdu {
	nvmf_tcp_common_pdu_hdr_t *hdr;
	uint32_t data_len;
} nvmf_tcp_rxpdu_t;

typedef struct nvmf_tcp_capsule {
	struct nvmf_capsule nc;

	nvmf_tcp_rxpdu_t rx_pdu;
	nvmf_tcp_command_buffer_t *cb;

	TAILQ_ENTRY(nvmf_tcp_capsule) link;
} nvmf_tcp_capsule_t;

typedef struct nvmf_tcp_qpair {
	struct nvmf_qpair qp;
	int s;

	uint8_t	txpda;
	uint8_t rxpda;
	bool header_digests;
	bool data_digests;
	uint32_t maxr2t;
	uint32_t maxh2cdata;
	uint32_t max_icd;	/* Host only */
	uint16_t next_ttag;	/* Controller only */
	bool terminated;	/* A termination request has been sent. */

	struct nvmf_tcp_command_buffer_list tx_buffers;
	struct nvmf_tcp_command_buffer_list rx_buffers;
	TAILQ_HEAD(, nvmf_tcp_capsule) rx_capsules;
} nvmf_tcp_qpair_t;

#define	TASSOC(na)	((nvmf_tcp_association_t *)(na))
#define	TCAP(nc)	((nvmf_tcp_capsule_t *)(nc))
#define	CTCAP(nc)	((const nvmf_tcp_capsule_t *)(nc))
#define	TQP(qp)		((nvmf_tcp_qpair_t *)(qp))

/* In-capsule data accepted by a controller: 8192 bytes, as for Fabrics. */
#define	NVMF_TCP_MAX_RX_ICD	8192

/* nvmf_tcp_pdu.c */
extern int nvmf_tcp_write_pdu(nvmf_tcp_qpair_t *qp, const void *pdu,
    size_t len);
extern uint32_t tcp_fei_dec(const uint8_t fei[4]);
extern void nvmf_tcp_report_error(struct nvmf_association *na,
    nvmf_tcp_qpair_t *qp, uint16_t fes, uint32_t fei, const void *rx_pdu,
    size_t pdu_len, u_int hlen);
extern int nvmf_tcp_read_buffer(nvmf_tcp_qpair_t *qp, void *buf, size_t len);
extern int nvmf_tcp_read_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu);
extern void nvmf_tcp_free_pdu(nvmf_tcp_rxpdu_t *pdu);
extern int nvmf_tcp_construct_pdu(nvmf_tcp_qpair_t *qp, void *hdr, size_t hlen,
    void *data, uint32_t data_len);

/* nvmf_tcp_data.c */
extern nvmf_tcp_command_buffer_t *tcp_alloc_command_buffer(
    nvmf_tcp_qpair_t *qp, void *data, uint32_t data_offset, size_t data_len,
    uint16_t cid, uint16_t ttag, bool receive);
extern void tcp_purge_command_buffer(nvmf_tcp_qpair_t *qp, uint16_t cid,
    uint16_t ttag, bool receive);
extern void tcp_free_command_buffer(nvmf_tcp_command_buffer_t *cb);
extern int nvmf_tcp_handle_h2c_data(nvmf_tcp_qpair_t *qp,
    nvmf_tcp_rxpdu_t *pdu);
extern int nvmf_tcp_handle_c2h_data(nvmf_tcp_qpair_t *qp,
    nvmf_tcp_rxpdu_t *pdu);
extern int nvmf_tcp_handle_r2t(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu);
extern int tcp_receive_controller_data(const struct nvmf_capsule *nc,
    uint32_t data_offset, void *buf, size_t len);
extern int tcp_send_controller_data(const struct nvmf_capsule *nc,
    const void *buf, size_t len);

/* nvmf_tcp.c */
extern int nvmf_tcp_receive_pdu(nvmf_tcp_qpair_t *qp);

#ifdef __cplusplus
}
#endif

#endif /* _LIBNVMF_NVMF_TCP_IMPL_H */

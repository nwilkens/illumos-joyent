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

#ifndef	_NVMF_TCP_IMPL_H
#define	_NVMF_TCP_IMPL_H

/*
 * Definitions shared by the files of the NVMe/TCP transport.
 */

#include <sys/types.h>
#include <sys/list.h>
#include <sys/atomic.h>
#include <sys/condvar.h>
#include <sys/mutex.h>
#include <sys/debug.h>
#include <sys/stream.h>
#include <sys/ksocket.h>
#include <sys/file.h>
#include <sys/taskq.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>
#include <sys/nvme/nvmf_tcp.h>

#include "nvmf_core.h"
#include "nvmf_transport_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nvmf_tcp_capsule;
struct nvmf_tcp_qpair;

/*
 * A data transfer of a command: host data sent for an R2T, host data received
 * by C2H, or controller data received by H2C.
 */
typedef struct nvmf_tcp_command_buffer {
	struct nvmf_tcp_qpair		*tcb_qp;

	struct nvmf_io_request		tcb_io;
	size_t				tcb_data_len;
	size_t				tcb_data_xfered;
	uint32_t			tcb_data_offset;

	volatile uint_t			tcb_refs;
	int				tcb_error;
	/* The callback ran at cancel; tq_rx_buffers.tcbl_lock. */
	boolean_t			tcb_done;

	uint16_t			tcb_cid;
	uint16_t			tcb_ttag;

	list_node_t			tcb_link;

	/* Controller only. */
	struct nvmf_tcp_capsule		*tcb_tc;
} nvmf_tcp_command_buffer_t;

typedef struct nvmf_tcp_command_buffer_list {
	list_t				tcbl_list;
	kmutex_t			tcbl_lock;
} nvmf_tcp_command_buffer_list_t;

typedef struct nvmf_tcp_qpair {
	struct nvmf_qpair		qp;

	/* Our hold on the daemon's open file keeps the socket. */
	file_t				*tq_fp;
	ksocket_t			tq_so;
	boolean_t			tq_started;

	volatile uint_t			tq_refs;	/* one per capsule */
	uint8_t				tq_txpda;
	uint8_t				tq_rxpda;
	boolean_t			tq_header_digests;
	boolean_t			tq_data_digests;
	uint32_t			tq_maxr2t;
	uint32_t			tq_maxh2cdata;	/* controller only */
	uint32_t			tq_max_tx_data;
	uint32_t			tq_max_icd;	/* host only */
	uint32_t			tq_max_rx_icd;	/* controller only */
	uint32_t			tq_max_io;	/* host only */
	uint32_t			tq_max_rx_pdu;
	uint16_t			tq_next_ttag;	/* controller only */
	uint_t				tq_num_ttags;	/* controller only */
	uint_t				tq_active_ttags; /* controller only */
	boolean_t			tq_send_success; /* controller only */

	kmutex_t			tq_rx_lock;
	kcondvar_t			tq_rx_cv;
	boolean_t			tq_rx_shutdown;
	taskq_t				*tq_rx_taskq;

	kmutex_t			tq_tx_lock;
	kcondvar_t			tq_tx_cv;
	kcondvar_t			tq_tx_done_cv;
	boolean_t			tq_tx_draining;
	boolean_t			tq_tx_shutdown;
	boolean_t			tq_tx_exited;
	mblk_t				*tq_tx_pdus_head;
	mblk_t				*tq_tx_pdus_tail;
	list_t				tq_tx_capsules;
	taskq_t				*tq_tx_taskq;

	nvmf_tcp_command_buffer_list_t	tq_tx_buffers;
	nvmf_tcp_command_buffer_list_t	tq_rx_buffers;

	/*
	 * A controller's receive buffer waits on tq_rx_buffers for a transfer
	 * tag and then sits in tq_open_ttags[] under that tag.  Both, and
	 * tc_rx_busy of every capsule, are under tq_rx_buffers.tcbl_lock.
	 */
	nvmf_tcp_command_buffer_t	**tq_open_ttags; /* controller only */
	boolean_t			tq_rx_closed;
	kcondvar_t			tq_cancel_cv;
} nvmf_tcp_qpair_t;

typedef struct nvmf_tcp_rxpdu {
	mblk_t				*rp_mp;
	const nvmf_tcp_common_pdu_hdr_t	*rp_hdr;
	uint32_t			rp_data_len;
	boolean_t			rp_data_digest_mismatch;
} nvmf_tcp_rxpdu_t;

typedef struct nvmf_tcp_capsule {
	struct nvmf_capsule		nc;

	volatile uint_t			tc_refs;

	nvmf_tcp_rxpdu_t		tc_rx_pdu;

	uint32_t			tc_active_r2ts;	/* controller only */
	/* Threads copying into or completing a receive of this capsule. */
	uint_t				tc_rx_busy;	/* controller only */
#ifdef DEBUG
	uint32_t			tc_tx_data_offset; /* controller only */
	uint_t				tc_pending_r2ts;   /* controller only */
#endif

	list_node_t			tc_link;
} nvmf_tcp_capsule_t;

#define	TCAP(nc)	((nvmf_tcp_capsule_t *)(nc))
#define	TQP(qp)		((nvmf_tcp_qpair_t *)(qp))

static inline boolean_t
nvmf_tcp_refcount_release(volatile uint_t *count)
{
	ASSERT3U(*count, >, 0);
	return (atomic_dec_uint_nv(count) == 0);
}

static inline void
nvmf_tcp_refcount_acquire(volatile uint_t *count)
{
	atomic_inc_uint(count);
}

/* The header and data digests are CRC-32C, reflected polynomial. */
#define	NVMF_TCP_CRC32C_POLY	0x82F63B78U
extern uint32_t nvmf_tcp_crc32c_table[256];

/* nvmf_tcp.c */
mblk_t	*capsule_to_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc);
void	tcp_release_capsule(nvmf_tcp_capsule_t *tc);

/* nvmf_tcp_pdu.c */
boolean_t nvmf_tcp_warn_ok(void);
mblk_t	*nvmf_tcp_allocb(size_t len);
void	nvmf_tcp_send_term_req(nvmf_tcp_qpair_t *qp, uint16_t fes,
    uint32_t fei, const void *pdu_hdr, uint_t hlen);
void	nvmf_tcp_report_error(nvmf_tcp_qpair_t *qp, uint16_t fes,
    uint32_t fei, mblk_t *rx_pdu, uint_t hlen);
int	nvmf_tcp_validate_pdu(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu);
void	nvmf_tcp_free_pdu(nvmf_tcp_rxpdu_t *pdu);
mblk_t	*nvmf_tcp_construct_pdu(nvmf_tcp_qpair_t *qp, void *hdr, size_t hlen,
    mblk_t *data, uint32_t data_len);

/* nvmf_tcp_data.c */
nvmf_tcp_command_buffer_t *tcp_alloc_command_buffer(nvmf_tcp_qpair_t *qp,
    const struct nvmf_io_request *io, uint32_t data_offset, size_t data_len,
    uint16_t cid);
void	tcp_release_command_buffer(nvmf_tcp_command_buffer_t *cb);
void	tcp_add_command_buffer(nvmf_tcp_command_buffer_list_t *list,
    nvmf_tcp_command_buffer_t *cb);
void	tcp_purge_command_buffer(nvmf_tcp_command_buffer_list_t *list,
    uint16_t cid, uint16_t ttag);
void	tcp_rx_busy_enter(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc);
void	tcp_rx_busy_exit(nvmf_tcp_qpair_t *qp, nvmf_tcp_capsule_t *tc);
mblk_t	*nvmf_tcp_command_buffer_mblk(nvmf_tcp_command_buffer_t *cb,
    uint32_t data_offset, uint32_t data_len);
int	nvmf_tcp_handle_h2c_data(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu);
int	nvmf_tcp_handle_c2h_data(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu);
int	nvmf_tcp_handle_r2t(nvmf_tcp_qpair_t *qp, nvmf_tcp_rxpdu_t *pdu);
uint8_t	tcp_validate_command_capsule(struct nvmf_capsule *nc);
size_t	tcp_capsule_data_len(const struct nvmf_capsule *nc);
int	tcp_receive_controller_data(struct nvmf_capsule *nc,
    uint32_t data_offset, struct nvmf_io_request *io);
void	tcp_cancel_controller_data(struct nvmf_capsule *nc);
uint_t	tcp_send_controller_data(struct nvmf_capsule *nc, uint32_t data_offset,
    mblk_t *mp, size_t len);

/* nvmf_tcp_conn.c */
void	nvmf_tcp_write_pdu(nvmf_tcp_qpair_t *qp, mblk_t *mp);
void	nvmf_tcp_so_rx(void *arg);
void	nvmf_tcp_so_tx(void *arg);
int	tcp_adopt_socket(uint64_t fd, file_t **fpp, ksocket_t *ksp);
void	tcp_release_socket(nvmf_tcp_qpair_t *qp);

#ifdef __cplusplus
}
#endif

#endif /* _NVMF_TCP_IMPL_H */

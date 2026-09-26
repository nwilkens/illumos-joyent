/*
 * Copyright (c) 2009-2010 Chelsio, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#ifndef _IWC_T4_H
#define	_IWC_T4_H

/*
 * The T4/T5/T6 RDMA queue formats: work queue entries, the completion queue
 * entry and the queue bookkeeping of a kernel QP and CQ.  Adapted from Linux
 * drivers/infiniband/hw/cxgb4/t4.h under the OpenIB license (see
 * README.illumos).  Everything the hardware writes is checked by the code
 * that reads it, not here.
 */

#include <sys/types.h>
#include <sys/byteorder.h>

#include "common/common.h"
#include "firmware/t4fw_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	T4_EQ_ENTRY_SIZE	64
#define	T4_SQ_NUM_SLOTS		5
#define	T4_SQ_NUM_BYTES		(T4_EQ_ENTRY_SIZE * T4_SQ_NUM_SLOTS)
#define	T4_RQ_NUM_SLOTS		2
#define	T4_RQ_NUM_BYTES		(T4_EQ_ENTRY_SIZE * T4_RQ_NUM_SLOTS)
#define	T4_MAX_RECV_SGE		4
#define	T4_STAG_UNSET		0xffffffffU
#define	T4_PAGESIZE_MASK	0xffff000ULL	/* 4KB to 128MB */
#define	T4_RQT_ENTRY_SHIFT	6
#define	T4_MAX_NUM_PD		65536

#define	T4_MAX_SEND_SGE	((T4_SQ_NUM_BYTES - sizeof (struct fw_ri_send_wr) - \
	sizeof (struct fw_ri_isgl)) / sizeof (struct fw_ri_sge))
#define	T4_MAX_WRITE_SGE ((T4_SQ_NUM_BYTES - \
	sizeof (struct fw_ri_rdma_write_wr) - sizeof (struct fw_ri_isgl)) / \
	sizeof (struct fw_ri_sge))
#define	T4_MAX_FR_IMMD	((T4_SQ_NUM_BYTES - sizeof (struct fw_ri_fr_nsmr_wr) - \
	sizeof (struct fw_ri_immd)) & ~31UL)
#define	T4_MAX_FR_IMMD_DEPTH	(T4_MAX_FR_IMMD / sizeof (uint64_t))

typedef struct t4_status_page {
	uint32_t	rsvd1;		/* flit 0: hardware */
	uint16_t	rsvd2;
	uint16_t	qid;
	uint16_t	cidx;
	uint16_t	pidx;
	uint8_t		qp_err;		/* flit 1: software */
	uint8_t		db_off;
	uint8_t		pad[2];
	uint16_t	host_wq_pidx;
	uint16_t	host_cidx;
	uint16_t	host_pidx;
	uint16_t	pad2;
	uint32_t	srqidx;
} t4_status_page_t;

union t4_wr {
	struct fw_ri_res_wr		res;
	struct fw_ri_wr			ri;
	struct fw_ri_rdma_write_wr	write;
	struct fw_ri_send_wr		send;
	struct fw_ri_rdma_read_wr	read;
	struct fw_ri_fr_nsmr_wr		fr;
	struct fw_ri_inv_lstag_wr	inv;
	t4_status_page_t		status;
	uint64_t	flits[T4_EQ_ENTRY_SIZE / sizeof (uint64_t) *
	    T4_SQ_NUM_SLOTS];
};

union t4_recv_wr {
	struct fw_ri_recv_wr		recv;
	t4_status_page_t		status;
	uint64_t	flits[T4_EQ_ENTRY_SIZE / sizeof (uint64_t) *
	    T4_RQ_NUM_SLOTS];
};

/* Completion and asynchronous error status codes. */
#define	T4_ERR_SUCCESS			0x00
#define	T4_ERR_STAG			0x01
#define	T4_ERR_PDID			0x02
#define	T4_ERR_QPID			0x03
#define	T4_ERR_ACCESS			0x04
#define	T4_ERR_WRAP			0x05
#define	T4_ERR_BOUND			0x06
#define	T4_ERR_INVALIDATE_SHARED_MR	0x07
#define	T4_ERR_INVALIDATE_MR_WITH_MW_BOUND 0x08
#define	T4_ERR_ECC			0x09
#define	T4_ERR_ECC_PSTAG		0x0a
#define	T4_ERR_PBL_ADDR_BOUND		0x0b
#define	T4_ERR_SWFLUSH			0x0c
#define	T4_ERR_CRC			0x10
#define	T4_ERR_MARKER			0x11
#define	T4_ERR_PDU_LEN_ERR		0x12
#define	T4_ERR_OUT_OF_RQE		0x13
#define	T4_ERR_DDP_VERSION		0x14
#define	T4_ERR_RDMA_VERSION		0x15
#define	T4_ERR_OPCODE			0x16
#define	T4_ERR_DDP_QUEUE_NUM		0x17
#define	T4_ERR_MSN			0x18
#define	T4_ERR_TBIT			0x19
#define	T4_ERR_MO			0x1a
#define	T4_ERR_MSN_GAP			0x1b
#define	T4_ERR_MSN_RANGE		0x1c
#define	T4_ERR_IRD_OVERFLOW		0x1d
#define	T4_ERR_RQE_ADDR_BOUND		0x1e
#define	T4_ERR_INTERNAL_ERR		0x1f

typedef struct t4_cqe {
	uint32_t	header;
	uint32_t	len;
	union {
		struct {
			uint32_t	stag;
			uint32_t	msn;
		} rcqe;
		struct {
			uint32_t	stag;
			uint16_t	nada2;
			uint16_t	cidx;
		} scqe;
		struct {
			uint32_t	wrid_hi;
			uint32_t	wrid_low;
		} gen;
		struct {
			uint32_t	mo;
			uint32_t	msn;
			uint32_t	imm_data32;
			uint32_t	reserved;
		} imm;
		uint64_t	drain_cookie;
		uint64_t	flits[3];
	} u;
	uint64_t	reserved[3];
	uint64_t	bits_type_ts;
} t4_cqe_t;

#define	S_CQE_QPID	12
#define	M_CQE_QPID	0xfffff
#define	S_CQE_SWCQE	11
#define	S_CQE_DRAIN	10
#define	S_CQE_STATUS	5
#define	M_CQE_STATUS	0x1f
#define	S_CQE_TYPE	4
#define	S_CQE_OPCODE	0
#define	M_CQE_OPCODE	0xf

#define	V_CQE_QPID(x)	((uint32_t)(x) << S_CQE_QPID)
#define	V_CQE_SWCQE(x)	((uint32_t)(x) << S_CQE_SWCQE)
#define	V_CQE_DRAIN(x)	((uint32_t)(x) << S_CQE_DRAIN)
#define	V_CQE_STATUS(x)	((uint32_t)(x) << S_CQE_STATUS)
#define	V_CQE_TYPE(x)	((uint32_t)(x) << S_CQE_TYPE)
#define	V_CQE_OPCODE(x)	((uint32_t)(x) << S_CQE_OPCODE)

#define	CQE_HDR(c)	BE_32((c)->header)
#define	CQE_QPID(c)	((CQE_HDR(c) >> S_CQE_QPID) & M_CQE_QPID)
#define	CQE_SWCQE(c)	((CQE_HDR(c) >> S_CQE_SWCQE) & 1)
#define	CQE_DRAIN(c)	((CQE_HDR(c) >> S_CQE_DRAIN) & 1)
#define	CQE_STATUS(c)	((CQE_HDR(c) >> S_CQE_STATUS) & M_CQE_STATUS)
#define	CQE_TYPE(c)	((CQE_HDR(c) >> S_CQE_TYPE) & 1)
#define	CQE_OPCODE(c)	((CQE_HDR(c) >> S_CQE_OPCODE) & M_CQE_OPCODE)
#define	CQE_SQ(c)	(CQE_TYPE(c) == 1)
#define	CQE_LEN(c)	BE_32((c)->len)
#define	CQE_STAG(c)	BE_32((c)->u.rcqe.stag)
#define	CQE_MSN(c)	BE_32((c)->u.rcqe.msn)
#define	CQE_SQ_IDX(c)	((c)->u.scqe.cidx)
#define	CQE_IMM_DATA(c)	((c)->u.imm.imm_data32)

#define	CQE_SEND_OPCODE(c) (CQE_OPCODE(c) == FW_RI_SEND || \
	CQE_OPCODE(c) == FW_RI_SEND_WITH_SE || \
	CQE_OPCODE(c) == FW_RI_SEND_WITH_INV || \
	CQE_OPCODE(c) == FW_RI_SEND_WITH_SE_INV)

#define	S_CQE_GENBIT	63
#define	CQE_GENBIT(c)	((uint_t)((BE_64((c)->bits_type_ts) >> \
	S_CQE_GENBIT) & 1))
#define	V_CQE_GENBIT(x)	((uint64_t)(x) << S_CQE_GENBIT)

#define	T4_CQE_SIZE	sizeof (t4_cqe_t)

/* The software state of a send queue entry. */
typedef struct t4_swsqe {
	uint64_t	wr_id;
	t4_cqe_t	cqe;
	uint32_t	read_len;
	uint8_t		opcode;
	boolean_t	complete;
	boolean_t	signaled;
	boolean_t	flushed;
	uint16_t	idx;
} t4_swsqe_t;

typedef struct t4_swrqe {
	uint64_t	wr_id;
} t4_swrqe_t;

typedef struct t4_sq {
	union t4_wr	*queue;
	t4_swsqe_t	*sw_sq;
	t4_swsqe_t	*oldest_read;
	caddr_t		bar2_va;
	uint32_t	bar2_qid;
	uint32_t	qid;
	uint16_t	in_use;
	uint16_t	size;
	uint16_t	cidx;
	uint16_t	pidx;
	uint16_t	wq_pidx;
	int32_t		flush_cidx;
} t4_sq_t;

typedef struct t4_rq {
	union t4_recv_wr *queue;
	t4_swrqe_t	*sw_rq;
	caddr_t		bar2_va;
	uint32_t	bar2_qid;
	uint32_t	qid;
	uint32_t	msn;
	uint32_t	rqt_hwaddr;
	uint16_t	rqt_size;
	uint16_t	in_use;
	uint16_t	size;
	uint16_t	cidx;
	uint16_t	pidx;
	uint16_t	wq_pidx;
} t4_rq_t;

typedef struct t4_wq {
	t4_sq_t		sq;
	t4_rq_t		rq;
	boolean_t	flushed;
	uint8_t		*qp_errp;
} t4_wq_t;

static inline uint32_t
t4_sq_avail(const t4_wq_t *wq)
{
	return (wq->sq.size - 1 - wq->sq.in_use);
}

static inline uint32_t
t4_rq_avail(const t4_wq_t *wq)
{
	return (wq->rq.size - 1 - wq->rq.in_use);
}

static inline void
t4_sq_produce(t4_wq_t *wq, uint8_t len16)
{
	wq->sq.in_use++;
	if (++wq->sq.pidx == wq->sq.size)
		wq->sq.pidx = 0;
	wq->sq.wq_pidx += howmany(len16 * 16, T4_EQ_ENTRY_SIZE);
	if (wq->sq.wq_pidx >= wq->sq.size * T4_SQ_NUM_SLOTS)
		wq->sq.wq_pidx %= wq->sq.size * T4_SQ_NUM_SLOTS;
}

static inline void
t4_sq_consume(t4_wq_t *wq)
{
	if (wq->sq.cidx == wq->sq.flush_cidx)
		wq->sq.flush_cidx = -1;
	wq->sq.in_use--;
	if (++wq->sq.cidx == wq->sq.size)
		wq->sq.cidx = 0;
}

static inline void
t4_rq_produce(t4_wq_t *wq, uint8_t len16)
{
	wq->rq.in_use++;
	if (++wq->rq.pidx == wq->rq.size)
		wq->rq.pidx = 0;
	wq->rq.wq_pidx += howmany(len16 * 16, T4_EQ_ENTRY_SIZE);
	if (wq->rq.wq_pidx >= wq->rq.size * T4_RQ_NUM_SLOTS)
		wq->rq.wq_pidx %= wq->rq.size * T4_RQ_NUM_SLOTS;
}

static inline void
t4_rq_consume(t4_wq_t *wq)
{
	wq->rq.in_use--;
	if (++wq->rq.cidx == wq->rq.size)
		wq->rq.cidx = 0;
}

static inline void
t4_set_wq_in_error(t4_wq_t *wq)
{
	*wq->qp_errp = 1;
}

static inline boolean_t
t4_wq_in_error(const t4_wq_t *wq)
{
	return (*wq->qp_errp != 0);
}

/*
 * A CQ: the hardware ring, whose last entry is the status page, and a
 * software ring for completions the driver makes or moves.
 */
typedef struct t4_cq {
	t4_cqe_t	*queue;
	t4_cqe_t	*sw_queue;
	caddr_t		bar2_va;
	uint32_t	bar2_qid;
	uint32_t	cqid;
	uint64_t	bits_type_ts;
	uint16_t	size;		/* hardware entries, no status page */
	uint16_t	cidx;
	uint16_t	sw_pidx;
	uint16_t	sw_cidx;
	uint16_t	sw_in_use;
	uint16_t	cidx_inc;
	uint8_t		gen;
	boolean_t	error;
	boolean_t	armed;
} t4_cq_t;

static inline boolean_t
t4_valid_cqe(const t4_cq_t *cq, const t4_cqe_t *cqe)
{
	return (CQE_GENBIT(cqe) == cq->gen);
}

static inline void
t4_swcq_produce(t4_cq_t *cq)
{
	if (cq->sw_in_use + 1 >= cq->size) {
		cq->error = B_TRUE;
		return;
	}
	cq->sw_in_use++;
	if (++cq->sw_pidx == cq->size)
		cq->sw_pidx = 0;
}

static inline void
t4_swcq_consume(t4_cq_t *cq)
{
	cq->sw_in_use--;
	if (++cq->sw_cidx == cq->size)
		cq->sw_cidx = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _IWC_T4_H */

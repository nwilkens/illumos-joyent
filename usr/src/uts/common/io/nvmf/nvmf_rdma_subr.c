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
 * The checks of the RDMA transport that depend on nothing but their
 * arguments: CM private data, queue sizing and transfer ranges.  The host
 * sends every value here.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>

#include "nvmf_rdma_impl.h"

#define	NR_CMD_BYTES	1024
#define	NR_XFER_BYTES	512
#define	NR_WR_BYTES	128

static uint16_t
nr_le16(const uint8_t *p)
{
	return ((uint16_t)(p[0] | (p[1] << 8)));
}

static void
nr_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/*
 * Check a CONNECT_REQUEST.  RoCE pads the private data to its message's
 * size, so only a short one is refused for its length.
 */
nvmf_rdma_rej_t
nvmf_rdma_req_parse(const void *pdata, size_t len, uint32_t peer_ird,
    const nvmf_rdma_limits_t *lim, nvmf_rdma_req_t *req)
{
	const uint8_t *p = pdata;
	uint32_t entries;

	bzero(req, sizeof (*req));
	if (p == NULL || len < NVMF_RDMA_REQ_LEN)
		return (NVMF_RDMA_REJ_INVALID_LEN);
	if (nr_le16(p) != 0)
		return (NVMF_RDMA_REJ_INVALID_RECFMT);
	req->nrq_qid = nr_le16(p + 2);
	req->nrq_hrqsize = nr_le16(p + 4);
	req->nrq_hsqsize = nr_le16(p + 6);
	req->nrq_cntlid = nr_le16(p + 8);

	if (req->nrq_qid > lim->nrl_max_qid)
		return (NVMF_RDMA_REJ_INVALID_QID);
	entries = (uint32_t)req->nrq_hsqsize + 1;
	if (entries < 2 || entries > (req->nrq_qid == 0 ?
	    lim->nrl_admin_entries : lim->nrl_io_entries) ||
	    entries > NVMF_RDMA_MAX_ENTRIES)
		return (NVMF_RDMA_REJ_INVALID_HSQSIZE);
	if (req->nrq_hrqsize < entries)
		return (NVMF_RDMA_REJ_INVALID_HRQSIZE);
	if (peer_ird == 0)
		return (NVMF_RDMA_REJ_INVALID_IRD);
	return (NVMF_RDMA_OK);
}

void
nvmf_rdma_rep_build(uint8_t *rep, uint16_t crqsize)
{
	bzero(rep, NVMF_RDMA_REP_LEN);
	nr_put_le16(rep + 2, crqsize);
}

void
nvmf_rdma_rej_build(uint8_t *rej, nvmf_rdma_rej_t sts)
{
	bzero(rej, NVMF_RDMA_REJ_LEN);
	nr_put_le16(rej + 2, (uint16_t)sts);
}

/*
 * Size a queue of depth entries with icd bytes of in-capsule data.  The
 * send queue holds a SEND per command context and xfers transfers of
 * rw_wrs entries each; transfers beyond xfers wait for one to finish, so
 * a device with a short queue gets fewer of them rather than a refusal.
 */
nvmf_rdma_rej_t
nvmf_rdma_size_queue(uint32_t depth, uint32_t icd,
    const nvmf_rdma_devlim_t *dl, nvmf_rdma_sizes_t *sz)
{
	uint64_t max_sq, base_sq, xfers, wrs;

	bzero(sz, sizeof (*sz));
	if (depth < 2 || depth > NVMF_RDMA_MAX_ENTRIES)
		return (NVMF_RDMA_REJ_INVALID_HSQSIZE);
	if (icd > NVMF_RDMA_MAX_ICD || dl->ndl_rw_wrs == 0)
		return (NVMF_RDMA_REJ_NO_RESOURCES);

	sz->nrs_depth = depth;
	sz->nrs_cmds = 2 * depth;
	sz->nrs_rq = depth + 1;
	sz->nrs_slot = (uint32_t)P2ROUNDUP(NVMF_RDMA_SQE_LEN + icd, 64);
	if (sz->nrs_rq > dl->ndl_max_qp_wr || sz->nrs_rq >= dl->ndl_max_cqe)
		return (NVMF_RDMA_REJ_INVALID_HSQSIZE);

	wrs = dl->ndl_rw_wrs;
	base_sq = (uint64_t)sz->nrs_cmds + 1;
	max_sq = MIN(dl->ndl_max_qp_wr, dl->ndl_max_cqe - sz->nrs_rq);
	if (base_sq + wrs > max_sq)
		return (NVMF_RDMA_REJ_INVALID_HSQSIZE);
	xfers = MIN(depth, (max_sq - base_sq) / wrs);

	sz->nrs_xfers = (uint32_t)xfers;
	sz->nrs_sq = (uint32_t)(xfers * wrs + base_sq);
	sz->nrs_cq = sz->nrs_rq + sz->nrs_sq;
	sz->nrs_bytes = (uint64_t)depth * sz->nrs_slot +
	    (uint64_t)sz->nrs_cmds * (NVMF_RDMA_CQE_LEN + NR_CMD_BYTES) +
	    xfers * (NR_XFER_BYTES + wrs * NR_WR_BYTES) +
	    (uint64_t)sz->nrs_cq * 32;
	return (NVMF_RDMA_OK);
}

/* len bytes at off lie within total. */
boolean_t
nvmf_rdma_range_ok(uint32_t off, uint32_t len, uint32_t total)
{
	return (len <= total && off <= total - len);
}

uint32_t
nvmf_rdma_cid_hash(uint16_t cid)
{
	return ((cid ^ (cid >> 6)) & (NR_CIDHASH - 1));
}

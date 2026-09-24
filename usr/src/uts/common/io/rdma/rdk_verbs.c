/*
 * Copyright (c) 2004 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2004 Infinicon Corporation.  All rights reserved.
 * Copyright (c) 2004 Intel Corporation.  All rights reserved.
 * Copyright (c) 2004 Topspin Corporation.  All rights reserved.
 * Copyright (c) 2004 Voltaire Corporation.  All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2005, 2006 Cisco Systems.  All rights reserved.
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
 *
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

/*
 * The verbs.  The QP state table, the page walk of rdk_sg_to_pages() and
 * the drain functions are adapted from Linux drivers/infiniband/core/verbs.c
 * under the OpenIB license (see README.illumos); the rest is written for
 * illumos.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/atomic.h>

#include "rdk_impl.h"

static const char *const rdk_wc_status_names[] = {
	[RDK_WC_SUCCESS] = "success",
	[RDK_WC_LOC_LEN_ERR] = "local length error",
	[RDK_WC_LOC_QP_OP_ERR] = "local QP operation error",
	[RDK_WC_LOC_EEC_OP_ERR] = "local EE context operation error",
	[RDK_WC_LOC_PROT_ERR] = "local protection error",
	[RDK_WC_WR_FLUSH_ERR] = "WR flushed",
	[RDK_WC_MW_BIND_ERR] = "memory bind operation error",
	[RDK_WC_BAD_RESP_ERR] = "bad response error",
	[RDK_WC_LOC_ACCESS_ERR] = "local access error",
	[RDK_WC_REM_INV_REQ_ERR] = "remote invalid request error",
	[RDK_WC_REM_ACCESS_ERR] = "remote access error",
	[RDK_WC_REM_OP_ERR] = "remote operation error",
	[RDK_WC_RETRY_EXC_ERR] = "transport retry counter exceeded",
	[RDK_WC_RNR_RETRY_EXC_ERR] = "RNR retry counter exceeded",
	[RDK_WC_LOC_RDD_VIOL_ERR] = "local RDD violation error",
	[RDK_WC_REM_INV_RD_REQ_ERR] = "remote invalid RD request",
	[RDK_WC_REM_ABORT_ERR] = "operation aborted",
	[RDK_WC_INV_EECN_ERR] = "invalid EE context number",
	[RDK_WC_INV_EEC_STATE_ERR] = "invalid EE context state",
	[RDK_WC_FATAL_ERR] = "fatal error",
	[RDK_WC_RESP_TIMEOUT_ERR] = "response timeout error",
	[RDK_WC_GENERAL_ERR] = "general error"
};

const char *
rdk_wc_status_msg(enum rdk_wc_status status)
{
	if ((uint_t)status < ARRAY_SIZE(rdk_wc_status_names) &&
	    rdk_wc_status_names[status] != NULL)
		return (rdk_wc_status_names[status]);
	return ("unrecognized status");
}

static const char *const rdk_event_names[] = {
	[RDK_EVENT_CQ_ERR] = "CQ error",
	[RDK_EVENT_QP_FATAL] = "QP fatal error",
	[RDK_EVENT_QP_REQ_ERR] = "QP request error",
	[RDK_EVENT_QP_ACCESS_ERR] = "QP access error",
	[RDK_EVENT_COMM_EST] = "communication established",
	[RDK_EVENT_SQ_DRAINED] = "send queue drained",
	[RDK_EVENT_PATH_MIG] = "path migration successful",
	[RDK_EVENT_PATH_MIG_ERR] = "path migration error",
	[RDK_EVENT_DEVICE_FATAL] = "device fatal error",
	[RDK_EVENT_PORT_ACTIVE] = "port active",
	[RDK_EVENT_PORT_ERR] = "port error",
	[RDK_EVENT_LID_CHANGE] = "LID change",
	[RDK_EVENT_PKEY_CHANGE] = "P_key change",
	[RDK_EVENT_SM_CHANGE] = "SM change",
	[RDK_EVENT_SRQ_ERR] = "SRQ error",
	[RDK_EVENT_SRQ_LIMIT_REACHED] = "SRQ limit reached",
	[RDK_EVENT_QP_LAST_WQE_REACHED] = "last WQE reached",
	[RDK_EVENT_CLIENT_REREGISTER] = "client reregister",
	[RDK_EVENT_GID_CHANGE] = "GID table change"
};

const char *
rdk_event_msg(enum rdk_event_type ev)
{
	if ((uint_t)ev < ARRAY_SIZE(rdk_event_names) &&
	    rdk_event_names[ev] != NULL)
		return (rdk_event_names[ev]);
	return ("unrecognized event");
}

/*
 * Protection domains.  The provider must offer a local DMA lkey; there is
 * no unsafe global rkey.
 */
int
rdk_alloc_pd(struct rdk_device *dev, uint32_t flags, struct rdk_pd **pdp)
{
	struct rdk_pd *pd;
	int ret;

	*pdp = NULL;
	if (flags != 0 ||
	    (dev->rd_attr.kernel_cap_flags & RDK_KCAP_LOCAL_DMA_LKEY) == 0)
		return (ENOTSUP);
	if ((ret = rdk_obj_hold(dev)) != 0)
		return (ret);

	pd = kmem_zalloc(dev->rd_ops->size_pd, KM_SLEEP);
	pd->device = dev;
	pd->local_dma_lkey = dev->rd_attr.local_dma_lkey;
	if ((ret = dev->rd_ops->alloc_pd(pd)) != 0) {
		kmem_free(pd, dev->rd_ops->size_pd);
		rdk_obj_rele(dev);
		return (ret);
	}
	*pdp = pd;
	return (0);
}

void
rdk_dealloc_pd(struct rdk_pd *pd)
{
	struct rdk_device *dev = pd->device;

	if (pd->usecnt != 0) {
		dev_err(dev->rd_dip, CE_WARN, "!PD freed with %u users; "
		    "leaking it", pd->usecnt);
		return;
	}
	dev->rd_ops->dealloc_pd(pd);
	kmem_free(pd, dev->rd_ops->size_pd);
	rdk_obj_rele(dev);
}

/*
 * Completion queues.
 */
int
rdk_create_cq(struct rdk_device *dev, rdk_comp_handler_t comp,
    void (*event)(struct rdk_event *, void *), void *ctx,
    const struct rdk_cq_init_attr *attr, struct rdk_cq **cqp)
{
	return (rdk_create_cq_poll(dev, comp, event, ctx, attr,
	    RDK_POLL_DIRECT, NULL, cqp));
}

/* The poller is set before the provider can send an event. */
int
rdk_create_cq_poll(struct rdk_device *dev, rdk_comp_handler_t comp,
    void (*event)(struct rdk_event *, void *), void *ctx,
    const struct rdk_cq_init_attr *attr, enum rdk_poll_context poll_ctx,
    struct rdk_cq_poller *poller, struct rdk_cq **cqp)
{
	struct rdk_cq *cq;
	int ret;

	*cqp = NULL;
	if (attr->cqe == 0 || attr->cqe > (uint32_t)dev->rd_attr.max_cqe ||
	    attr->flags != 0)
		return (EINVAL);
	if ((ret = rdk_obj_hold(dev)) != 0)
		return (ret);

	cq = kmem_zalloc(dev->rd_ops->size_cq, KM_SLEEP);
	cq->device = dev;
	cq->comp_handler = comp;
	cq->event_handler = event;
	cq->cq_context = ctx;
	cq->cqe = (int)attr->cqe;
	cq->poll_ctx = poll_ctx;
	cq->poller = poller;
	if ((ret = dev->rd_ops->create_cq(cq, attr)) != 0) {
		kmem_free(cq, dev->rd_ops->size_cq);
		rdk_obj_rele(dev);
		return (ret);
	}
	*cqp = cq;
	return (0);
}

void
rdk_destroy_cq(struct rdk_cq *cq)
{
	struct rdk_device *dev = cq->device;

	if (cq->usecnt != 0) {
		dev_err(dev->rd_dip, CE_WARN, "!CQ freed with %u QPs; "
		    "leaking it", cq->usecnt);
		return;
	}
	dev->rd_ops->destroy_cq(cq);
	kmem_free(cq, dev->rd_ops->size_cq);
	rdk_obj_rele(dev);
}

/*
 * The attributes each QP state transition requires and allows, for the QP
 * types this framework offers.
 */
static const struct {
	boolean_t	valid;
	int		req_param[RDK_QPT_MAX];
	int		opt_param[RDK_QPT_MAX];
} rdk_qp_state_table[RDK_QPS_ERR + 1][RDK_QPS_ERR + 1] = {
	[RDK_QPS_RESET] = {
		[RDK_QPS_RESET] = { .valid = B_TRUE },
		[RDK_QPS_INIT] = {
			.valid = B_TRUE,
			.req_param = {
				[RDK_QPT_UD] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_PORT | RDK_QP_QKEY),
				[RDK_QPT_RC] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_PORT | RDK_QP_ACCESS_FLAGS),
				[RDK_QPT_GSI] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_QKEY),
			}
		},
	},
	[RDK_QPS_INIT] = {
		[RDK_QPS_RESET] = { .valid = B_TRUE },
		[RDK_QPS_ERR] = { .valid = B_TRUE },
		[RDK_QPS_INIT] = {
			.valid = B_TRUE,
			.opt_param = {
				[RDK_QPT_UD] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_PORT | RDK_QP_QKEY),
				[RDK_QPT_RC] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_PORT | RDK_QP_ACCESS_FLAGS),
				[RDK_QPT_GSI] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_QKEY),
			}
		},
		[RDK_QPS_RTR] = {
			.valid = B_TRUE,
			.req_param = {
				[RDK_QPT_RC] = (RDK_QP_AV | RDK_QP_PATH_MTU |
				    RDK_QP_DEST_QPN | RDK_QP_RQ_PSN |
				    RDK_QP_MAX_DEST_RD_ATOMIC |
				    RDK_QP_MIN_RNR_TIMER),
			},
			.opt_param = {
				[RDK_QPT_UD] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_QKEY),
				[RDK_QPT_RC] = (RDK_QP_ALT_PATH |
				    RDK_QP_ACCESS_FLAGS | RDK_QP_PKEY_INDEX),
				[RDK_QPT_GSI] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_QKEY),
			},
		},
	},
	[RDK_QPS_RTR] = {
		[RDK_QPS_RESET] = { .valid = B_TRUE },
		[RDK_QPS_ERR] = { .valid = B_TRUE },
		[RDK_QPS_RTS] = {
			.valid = B_TRUE,
			.req_param = {
				[RDK_QPT_UD] = RDK_QP_SQ_PSN,
				[RDK_QPT_RC] = (RDK_QP_TIMEOUT |
				    RDK_QP_RETRY_CNT | RDK_QP_RNR_RETRY |
				    RDK_QP_SQ_PSN | RDK_QP_MAX_QP_RD_ATOMIC),
				[RDK_QPT_GSI] = RDK_QP_SQ_PSN,
			},
			.opt_param = {
				[RDK_QPT_UD] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
				[RDK_QPT_RC] = (RDK_QP_CUR_STATE |
				    RDK_QP_ALT_PATH | RDK_QP_ACCESS_FLAGS |
				    RDK_QP_MIN_RNR_TIMER |
				    RDK_QP_PATH_MIG_STATE),
				[RDK_QPT_GSI] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
			}
		}
	},
	[RDK_QPS_RTS] = {
		[RDK_QPS_RESET] = { .valid = B_TRUE },
		[RDK_QPS_ERR] = { .valid = B_TRUE },
		[RDK_QPS_RTS] = {
			.valid = B_TRUE,
			.opt_param = {
				[RDK_QPT_UD] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
				[RDK_QPT_RC] = (RDK_QP_CUR_STATE |
				    RDK_QP_ACCESS_FLAGS | RDK_QP_ALT_PATH |
				    RDK_QP_PATH_MIG_STATE |
				    RDK_QP_MIN_RNR_TIMER),
				[RDK_QPT_GSI] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
			}
		},
		[RDK_QPS_SQD] = {
			.valid = B_TRUE,
			.opt_param = {
				[RDK_QPT_UD] = RDK_QP_EN_SQD_ASYNC_NOTIFY,
				[RDK_QPT_RC] = RDK_QP_EN_SQD_ASYNC_NOTIFY,
				[RDK_QPT_GSI] = RDK_QP_EN_SQD_ASYNC_NOTIFY
			}
		},
	},
	[RDK_QPS_SQD] = {
		[RDK_QPS_RESET] = { .valid = B_TRUE },
		[RDK_QPS_ERR] = { .valid = B_TRUE },
		[RDK_QPS_RTS] = {
			.valid = B_TRUE,
			.opt_param = {
				[RDK_QPT_UD] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
				[RDK_QPT_RC] = (RDK_QP_CUR_STATE |
				    RDK_QP_ALT_PATH | RDK_QP_ACCESS_FLAGS |
				    RDK_QP_MIN_RNR_TIMER |
				    RDK_QP_PATH_MIG_STATE),
				[RDK_QPT_GSI] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
			}
		},
		[RDK_QPS_SQD] = {
			.valid = B_TRUE,
			.opt_param = {
				[RDK_QPT_UD] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_QKEY),
				[RDK_QPT_RC] = (RDK_QP_PORT | RDK_QP_AV |
				    RDK_QP_TIMEOUT | RDK_QP_RETRY_CNT |
				    RDK_QP_RNR_RETRY |
				    RDK_QP_MAX_QP_RD_ATOMIC |
				    RDK_QP_MAX_DEST_RD_ATOMIC |
				    RDK_QP_ALT_PATH | RDK_QP_ACCESS_FLAGS |
				    RDK_QP_PKEY_INDEX | RDK_QP_MIN_RNR_TIMER |
				    RDK_QP_PATH_MIG_STATE),
				[RDK_QPT_GSI] = (RDK_QP_PKEY_INDEX |
				    RDK_QP_QKEY),
			}
		}
	},
	[RDK_QPS_SQE] = {
		[RDK_QPS_RESET] = { .valid = B_TRUE },
		[RDK_QPS_ERR] = { .valid = B_TRUE },
		[RDK_QPS_RTS] = {
			.valid = B_TRUE,
			.opt_param = {
				[RDK_QPT_UD] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
				[RDK_QPT_GSI] = (RDK_QP_CUR_STATE |
				    RDK_QP_QKEY),
			}
		}
	},
	[RDK_QPS_ERR] = {
		[RDK_QPS_RESET] = { .valid = B_TRUE },
		[RDK_QPS_ERR] = { .valid = B_TRUE }
	}
};

boolean_t
rdk_modify_qp_is_ok(enum rdk_qp_state cur, enum rdk_qp_state next,
    enum rdk_qp_type type, int mask)
{
	int req, opt;

	if ((uint_t)cur > RDK_QPS_ERR || (uint_t)next > RDK_QPS_ERR ||
	    (uint_t)type >= RDK_QPT_MAX)
		return (B_FALSE);

	if ((mask & RDK_QP_CUR_STATE) != 0 &&
	    cur != RDK_QPS_RTR && cur != RDK_QPS_RTS &&
	    cur != RDK_QPS_SQD && cur != RDK_QPS_SQE)
		return (B_FALSE);

	if (!rdk_qp_state_table[cur][next].valid)
		return (B_FALSE);

	req = rdk_qp_state_table[cur][next].req_param[type];
	opt = rdk_qp_state_table[cur][next].opt_param[type];

	if ((mask & req) != req)
		return (B_FALSE);
	if ((mask & ~(req | opt | RDK_QP_STATE)) != 0)
		return (B_FALSE);
	return (B_TRUE);
}

static void rdk_drain_reap(struct rdk_qp *);

/*
 * Queue pairs.
 */
int
rdk_create_qp(struct rdk_pd *pd, struct rdk_qp_init_attr *init,
    struct rdk_qp **qpp)
{
	struct rdk_device *dev = pd->device;
	struct rdk_qp *qp;
	int ret;

	*qpp = NULL;
	if (init->send_cq == NULL || init->recv_cq == NULL ||
	    init->send_cq->device != dev || init->recv_cq->device != dev)
		return (EINVAL);
	if (init->qp_type != RDK_QPT_RC && init->qp_type != RDK_QPT_UD &&
	    init->qp_type != RDK_QPT_GSI)
		return (ENOTSUP);
	if (init->create_flags != 0 ||
	    (init->sq_sig_type != RDK_SIGNAL_ALL_WR &&
	    init->sq_sig_type != RDK_SIGNAL_REQ_WR))
		return (EINVAL);
	if (init->port_num != 0 && !rdk_port_valid(dev, init->port_num))
		return (EINVAL);
	if ((ret = rdk_obj_hold(dev)) != 0)
		return (ret);

	qp = kmem_zalloc(dev->rd_ops->size_qp, KM_SLEEP);
	qp->device = dev;
	qp->pd = pd;
	qp->send_cq = init->send_cq;
	qp->recv_cq = init->recv_cq;
	qp->event_handler = init->event_handler;
	qp->qp_context = init->qp_context;
	qp->qp_type = init->qp_type;
	qp->port = init->port_num != 0 ? init->port_num : 1;
	if ((ret = dev->rd_ops->create_qp(qp, init)) != 0) {
		kmem_free(qp, dev->rd_ops->size_qp);
		rdk_obj_rele(dev);
		return (ret);
	}
	atomic_inc_32(&pd->usecnt);
	atomic_inc_32(&qp->send_cq->usecnt);
	atomic_inc_32(&qp->recv_cq->usecnt);
	*qpp = qp;
	return (0);
}

int
rdk_modify_qp(struct rdk_qp *qp, struct rdk_qp_attr *attr, int mask)
{
	struct rdk_device *dev = qp->device;
	const struct rdk_gid_attr *old;
	int ret;

	if ((mask & ~RDK_QP_ATTR_STANDARD_BITS) != 0)
		return (EINVAL);
	if ((mask & RDK_QP_PORT) != 0 && !rdk_port_valid(dev, attr->port_num))
		return (EINVAL);
	if ((mask & RDK_QP_PATH_MTU) != 0 &&
	    rdk_mtu_enum_to_int(attr->path_mtu) < 0)
		return (EINVAL);

	if ((mask & RDK_QP_AV) != 0) {
		if (qp->qp_type != RDK_QPT_RC)
			return (EINVAL);
		if ((ret = rdk_resolve_ah_attr(dev, &attr->ah_attr)) != 0)
			return (ret);
	}

	ret = dev->rd_ops->modify_qp(qp, attr, mask);

	if ((mask & RDK_QP_AV) != 0) {
		if (ret == 0) {
			old = qp->av_sgid_attr;
			qp->av_sgid_attr = attr->ah_attr.grh.sgid_attr;
			rdk_put_gid_attr(old);
		} else {
			rdk_put_gid_attr(attr->ah_attr.grh.sgid_attr);
		}
		attr->ah_attr.grh.sgid_attr = NULL;
	}
	return (ret);
}

int
rdk_query_qp(struct rdk_qp *qp, struct rdk_qp_attr *attr, int mask,
    struct rdk_qp_init_attr *init)
{
	bzero(attr, sizeof (*attr));
	bzero(init, sizeof (*init));
	return (qp->device->rd_ops->query_qp(qp, attr, mask, init));
}

void
rdk_destroy_qp(struct rdk_qp *qp)
{
	struct rdk_device *dev = qp->device;

	dev->rd_ops->destroy_qp(qp);
	rdk_cq_barrier(qp->send_cq);
	if (qp->recv_cq != qp->send_cq)
		rdk_cq_barrier(qp->recv_cq);
	rdk_drain_reap(qp);
	rdk_put_gid_attr(qp->av_sgid_attr);
	atomic_dec_32(&qp->pd->usecnt);
	atomic_dec_32(&qp->send_cq->usecnt);
	atomic_dec_32(&qp->recv_cq->usecnt);
	kmem_free(qp, dev->rd_ops->size_qp);
	rdk_obj_rele(dev);
}

/*
 * Memory registration.
 */
int
rdk_alloc_mr(struct rdk_pd *pd, enum rdk_mr_type type, uint32_t max_num_sg,
    struct rdk_mr **mrp)
{
	struct rdk_device *dev = pd->device;
	struct rdk_mr *mr;
	int ret;

	*mrp = NULL;
	if (type != RDK_MR_TYPE_MEM_REG)
		return (ENOTSUP);
	if (max_num_sg == 0 ||
	    max_num_sg > dev->rd_attr.max_fast_reg_page_list_len)
		return (EINVAL);
	if ((ret = rdk_obj_hold(dev)) != 0)
		return (ret);
	if ((ret = dev->rd_ops->alloc_mr(pd, type, max_num_sg, &mr)) != 0) {
		rdk_obj_rele(dev);
		return (ret);
	}
	mr->device = dev;
	mr->pd = pd;
	mr->type = type;
	atomic_inc_32(&pd->usecnt);
	*mrp = mr;
	return (0);
}

/*
 * Map DMA cookies into the MR's page list.  Returns the number of cookies
 * mapped; *offset, if given, is the byte offset into the first cookie on
 * entry and into the first unmapped one on return.
 */
int
rdk_map_mr_sg(struct rdk_mr *mr, const ddi_dma_cookie_t *cookies, uint_t n,
    uint64_t *offset, uint32_t page_size)
{
	struct rdk_device *dev = mr->device;

	if (n == 0 || cookies == NULL || !ISP2(page_size) ||
	    (dev->rd_attr.page_size_cap & page_size) == 0)
		return (-EINVAL);
	mr->page_size = page_size;
	return (dev->rd_ops->map_mr_sg(mr, cookies, n, offset));
}

/*
 * Convert the largest prefix of the cookies that meets the page list rules
 * to pages, calling set_page() for each.  Every cookie but the first must
 * start on a page boundary and every one but the last must end on one.
 */
int
rdk_sg_to_pages(struct rdk_mr *mr, const ddi_dma_cookie_t *cookies, uint_t n,
    uint64_t *offset_p, int (*set_page)(struct rdk_mr *, uint64_t))
{
	uint64_t offset = offset_p != NULL ? *offset_p : 0;
	uint64_t page_mask = ~((uint64_t)mr->page_size - 1);
	uint64_t last_end = 0, last_page_off = 0;
	uint_t i;
	int ret;

	if (n == 0 || offset >= cookies[0].dmac_size)
		return (-EINVAL);

	mr->iova = cookies[0].dmac_laddress + offset;
	mr->length = 0;

	for (i = 0; i < n; i++) {
		uint64_t dma_addr = cookies[i].dmac_laddress + offset;
		uint64_t prev_addr = dma_addr;
		uint64_t dma_len = cookies[i].dmac_size - offset;
		uint64_t end = dma_addr + dma_len;
		uint64_t page_addr = dma_addr & page_mask;

		if (cookies[i].dmac_size <= offset || end < dma_addr)
			break;

		if (i != 0 && (last_page_off != 0 || page_addr != dma_addr)) {
			/* A gap ends the mapping. */
			if (last_end != dma_addr)
				break;
			goto next_page;
		}

		do {
			ret = set_page(mr, page_addr);
			if (ret < 0) {
				/* The first page may start below dma_addr. */
				uint64_t stop = MAX(prev_addr, dma_addr);

				mr->length += stop - dma_addr;
				if (offset_p != NULL) {
					*offset_p = stop -
					    cookies[i].dmac_laddress;
				}
				return (i != 0 || stop != dma_addr ? (int)i :
				    ret);
			}
			prev_addr = page_addr;
next_page:
			page_addr += mr->page_size;
		} while (page_addr < end);

		mr->length += dma_len;
		last_end = end;
		last_page_off = end & ~page_mask;
		offset = 0;
	}

	if (offset_p != NULL)
		*offset_p = 0;
	return ((int)i);
}

/*
 * Returns EIO if the provider could not confirm that the device let go of
 * the memory; the MR is released either way.
 */
int
rdk_dereg_mr(struct rdk_mr *mr)
{
	struct rdk_device *dev = mr->device;
	struct rdk_pd *pd = mr->pd;
	int ret;

	ret = dev->rd_ops->dereg_mr(mr);
	atomic_dec_32(&pd->usecnt);
	rdk_obj_rele(dev);
	return (ret);
}

/*
 * Address handles.
 */
int
rdk_create_ah(struct rdk_pd *pd, struct rdk_ah_attr *attr,
    struct rdk_ah **ahp)
{
	struct rdk_device *dev = pd->device;
	struct rdk_ah *ah;
	int ret;

	*ahp = NULL;
	if ((ret = rdk_resolve_ah_attr(dev, attr)) != 0)
		return (ret);
	if ((ret = rdk_obj_hold(dev)) != 0) {
		rdk_put_gid_attr(attr->grh.sgid_attr);
		attr->grh.sgid_attr = NULL;
		return (ret);
	}

	ah = kmem_zalloc(dev->rd_ops->size_ah, KM_SLEEP);
	ah->device = dev;
	ah->pd = pd;
	ah->type = attr->type;
	ah->sgid_attr = attr->grh.sgid_attr;
	if ((ret = dev->rd_ops->create_ah(ah, attr)) != 0) {
		rdk_put_gid_attr(ah->sgid_attr);
		kmem_free(ah, dev->rd_ops->size_ah);
		rdk_obj_rele(dev);
		attr->grh.sgid_attr = NULL;
		return (ret);
	}
	attr->grh.sgid_attr = NULL;
	atomic_inc_32(&pd->usecnt);
	*ahp = ah;
	return (0);
}

void
rdk_destroy_ah(struct rdk_ah *ah)
{
	struct rdk_device *dev = ah->device;

	dev->rd_ops->destroy_ah(ah);
	rdk_put_gid_attr(ah->sgid_attr);
	atomic_dec_32(&ah->pd->usecnt);
	kmem_free(ah, dev->rd_ops->size_ah);
	rdk_obj_rele(dev);
}

/*
 * Draining: move the QP to the error state, post one more work request and
 * wait until its flushed completion is processed.  Only CQs from
 * rdk_alloc_cq() dispatch the completion to the waiter.  A drain that times
 * out leaves the sentinel on the QP, and destroy releases it once no
 * completion can come.
 */
uint_t rdk_drain_timeout_ms = 10000;

typedef struct rdk_drain_cqe {
	struct rdk_cqe		rdc_cqe;
	kmutex_t		rdc_lock;
	kcondvar_t		rdc_cv;
	boolean_t		rdc_done;
	uint_t			rdc_refs;
	struct rdk_drain_cqe	*rdc_next;	/* on the QP's orphans */
} rdk_drain_cqe_t;

static void rdk_drain_rele(rdk_drain_cqe_t *);

/* The QP's timed-out sentinels, after its completions have stopped. */
static void
rdk_drain_reap(struct rdk_qp *qp)
{
	rdk_drain_cqe_t *d, *next;
	boolean_t undone;

	d = atomic_swap_ptr(&qp->drain_orphans, NULL);
	for (; d != NULL; d = next) {
		next = d->rdc_next;
		mutex_enter(&d->rdc_lock);
		undone = !d->rdc_done;
		d->rdc_done = B_TRUE;
		mutex_exit(&d->rdc_lock);
		if (undone)
			rdk_drain_rele(d);
		rdk_drain_rele(d);
	}
}

static void
rdk_drain_orphan(struct rdk_qp *qp, rdk_drain_cqe_t *d)
{
	void *old;

	mutex_enter(&d->rdc_lock);
	if (d->rdc_done) {
		mutex_exit(&d->rdc_lock);
		return;
	}
	d->rdc_refs++;
	mutex_exit(&d->rdc_lock);
	do {
		old = qp->drain_orphans;
		d->rdc_next = old;
	} while (atomic_cas_ptr(&qp->drain_orphans, old, d) != old);
}

static void
rdk_drain_rele(rdk_drain_cqe_t *d)
{
	boolean_t last;

	mutex_enter(&d->rdc_lock);
	last = --d->rdc_refs == 0;
	mutex_exit(&d->rdc_lock);
	if (last) {
		cv_destroy(&d->rdc_cv);
		mutex_destroy(&d->rdc_lock);
		kmem_free(d, sizeof (*d));
	}
}

static void
rdk_drain_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	rdk_drain_cqe_t *d = (rdk_drain_cqe_t *)(void *)wc->wr_cqe;

	_NOTE(ARGUNUSED(cq));
	mutex_enter(&d->rdc_lock);
	d->rdc_done = B_TRUE;
	cv_broadcast(&d->rdc_cv);
	mutex_exit(&d->rdc_lock);
	rdk_drain_rele(d);
}

static rdk_drain_cqe_t *
rdk_drain_alloc(void)
{
	rdk_drain_cqe_t *d = kmem_zalloc(sizeof (*d), KM_SLEEP);

	d->rdc_cqe.done = rdk_drain_done;
	d->rdc_refs = 1;
	mutex_init(&d->rdc_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&d->rdc_cv, NULL, CV_DRIVER, NULL);
	return (d);
}

static boolean_t
rdk_drain_wait(struct rdk_qp *qp, struct rdk_cq *cq, rdk_drain_cqe_t *d,
    const char *what)
{
	boolean_t done;
	clock_t deadline = ddi_get_lbolt() +
	    drv_usectohz((clock_t)rdk_drain_timeout_ms * MILLISEC);

	mutex_enter(&d->rdc_lock);
	while (!d->rdc_done) {
		if (cq->poll_ctx == RDK_POLL_DIRECT) {
			mutex_exit(&d->rdc_lock);
			(void) rdk_process_cq_direct(cq, -1);
			mutex_enter(&d->rdc_lock);
			if (d->rdc_done)
				break;
		}
		if (cv_timedwait(&d->rdc_cv, &d->rdc_lock,
		    MIN(deadline, ddi_get_lbolt() +
		    drv_usectohz(100 * MILLISEC))) == -1 &&
		    ddi_get_lbolt() >= deadline) {
			dev_err(qp->device->rd_dip, CE_WARN, "!QP %u: the %s "
			    "did not drain in %u ms", qp->qp_num, what,
			    rdk_drain_timeout_ms);
			break;
		}
	}
	done = d->rdc_done;
	mutex_exit(&d->rdc_lock);
	return (done);
}

static int
rdk_drain_to_err(struct rdk_qp *qp, const char *what)
{
	struct rdk_qp_attr attr;
	int ret;

	bzero(&attr, sizeof (attr));
	attr.qp_state = RDK_QPS_ERR;
	if ((ret = rdk_modify_qp(qp, &attr, RDK_QP_STATE)) != 0) {
		dev_err(qp->device->rd_dip, CE_WARN, "!QP %u: failed to drain "
		    "the %s: %d", qp->qp_num, what, ret);
	}
	return (ret);
}

void
rdk_drain_sq(struct rdk_qp *qp)
{
	struct rdk_rdma_wr swr;
	rdk_drain_cqe_t *d;
	int ret;

	if (qp->send_cq->poller == NULL ||
	    rdk_drain_to_err(qp, "send queue") != 0)
		return;

	/*
	 * A datagram QP has no RDMA write to post and needs an AH to send;
	 * moving it to the error state has flushed its send queue.
	 */
	if (qp->qp_type != RDK_QPT_RC)
		return;

	d = rdk_drain_alloc();
	bzero(&swr, sizeof (swr));
	swr.wr.wr_cqe = &d->rdc_cqe;
	swr.wr.opcode = RDK_WR_RDMA_WRITE;
	swr.wr.send_flags = RDK_SEND_SIGNALED;
	d->rdc_refs++;
	if ((ret = rdk_post_send(qp, &swr.wr, NULL)) != 0) {
		d->rdc_refs--;
		dev_err(qp->device->rd_dip, CE_WARN, "!QP %u: failed to drain "
		    "the send queue: %d", qp->qp_num, ret);
	} else if (!rdk_drain_wait(qp, qp->send_cq, d, "send queue")) {
		rdk_drain_orphan(qp, d);
	}
	rdk_drain_rele(d);
}

void
rdk_drain_rq(struct rdk_qp *qp)
{
	struct rdk_recv_wr rwr;
	rdk_drain_cqe_t *d;
	int ret;

	if (qp->recv_cq->poller == NULL ||
	    rdk_drain_to_err(qp, "receive queue") != 0)
		return;

	d = rdk_drain_alloc();
	bzero(&rwr, sizeof (rwr));
	rwr.wr_cqe = &d->rdc_cqe;
	d->rdc_refs++;
	if ((ret = rdk_post_recv(qp, &rwr, NULL)) != 0) {
		d->rdc_refs--;
		dev_err(qp->device->rd_dip, CE_WARN, "!QP %u: failed to drain "
		    "the receive queue: %d", qp->qp_num, ret);
	} else if (!rdk_drain_wait(qp, qp->recv_cq, d, "receive queue")) {
		rdk_drain_orphan(qp, d);
	}
	rdk_drain_rele(d);
}

void
rdk_drain_qp(struct rdk_qp *qp)
{
	rdk_drain_sq(qp);
	rdk_drain_rq(qp);
}

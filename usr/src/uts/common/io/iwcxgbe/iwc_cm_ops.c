/*
 * Copyright (c) 2009-2014 Chelsio, Inc. All rights reserved.
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


/*
 * The provider operations of the rdmak connection manager: connect,
 * accept, reject, listen and disconnect.  Follows Linux cxgb4/cm.c under
 * the OpenIB license.
 */

#include <sys/types.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/strsun.h>
#include <sys/atomic.h>
#include <sys/ethernet.h>
#include <sys/vlan.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "iwc.h"
#include "common/t4_msg.h"

#define	IWC_LISTEN_WAIT_MS	10000

static int
iwc_iw_connect(struct rdk_device *rdev, struct rdk_iw_cm_id *cmid,
    const struct rdk_iw_conn_param *p)
{
	iwc_dev_t *dev = iwc_dev(rdev);
	iwc_t *iwc = dev->d_iwc;
	const uint32_t max = MIN(iwc->iwc_info.tri_vres.trv_max_ordird_qp,
	    IWC_MAX_ORDIRD);
	iwc_qp_t *qp = (iwc_qp_t *)p->qp, *got;
	t4_rdma_act_open_t a;
	iwc_ep_t *ep;
	int ret;

	if (qp == NULL || qp->qp_rdk.device != rdev ||
	    p->pdata_len > IWC_MPA_MAX_ULP_PDATA ||
	    cmid->iw_laddr.sin_family != AF_INET ||
	    cmid->iw_raddr.sin_family != AF_INET)
		return (EINVAL);
	if (iwc->iwc_fatal)
		return (EIO);
	if ((got = iwc_qp_get(iwc, qp->qp_wq.sq.qid)) != qp) {
		if (got != NULL)
			iwc_qp_put(iwc, got);
		return (EINVAL);
	}

	ep = iwc_ep_alloc(iwc, dev);
	ep->ep_laddr = cmid->iw_laddr;
	ep->ep_raddr = cmid->iw_raddr;
	ep->ep_qp = qp;
	ep->ep_ird = MIN(p->ird, max);
	ep->ep_ord = MIN(p->ord, max);
	/* The ready-to-receive read is our first outbound read. */
	if (ep->ep_ord == 0)
		ep->ep_ord = 1;
	ep->ep_attr.ma_initiator = B_TRUE;
	ep->ep_attr.ma_version = 2;
	ep->ep_attr.ma_enhanced = B_TRUE;
	ep->ep_attr.ma_crc = B_TRUE;
	ep->ep_attr.ma_p2p_type = IWC_P2P_READ;
	ep->ep_pdata_len = p->pdata_len;
	if (p->pdata_len != 0)
		bcopy(p->pdata, ep->ep_pdata, p->pdata_len);

	mutex_enter(&ep->ep_lock);
	if ((ret = iwc->iwc_ops->tro_l2t_get(iwc->iwc_peer, ep->ep_port,
	    CPL_L2T_VLAN_NONE, cmid->iw_nh_mac, &ep->ep_l2t)) != 0) {
		ep->ep_l2t = T4_RDMA_TID_NONE;
		goto fail;
	}
	if ((ret = iwc->iwc_ops->tro_atid_alloc(iwc->iwc_peer, ep,
	    &ep->ep_atid)) != 0)
		goto fail;
	iwc_ep_hold(ep);
	ep->ep_flags |= EPF_ATID;

	bzero(&a, sizeof (a));
	a.trao_port = ep->ep_port;
	a.trao_family = AF_INET;
	IN6_INADDR_TO_V4MAPPED(&ep->ep_laddr.sin_addr, &a.trao_laddr);
	IN6_INADDR_TO_V4MAPPED(&ep->ep_raddr.sin_addr, &a.trao_faddr);
	a.trao_lport = ep->ep_laddr.sin_port;
	a.trao_fport = ep->ep_raddr.sin_port;
	a.trao_atid = ep->ep_atid;
	a.trao_l2t = ep->ep_l2t;
	iwc_tcp_opts(ep, iwc_path_mtu(ep, cmid->iw_mtu), &a.trao_opts);
	ep->ep_cmid = cmid;
	ep->ep_flags |= EPF_CM_REF;
	ep->ep_state = IWC_EP_CONNECTING;
	cmid->iw_provider = ep;
	if ((ret = iwc->iwc_ops->tro_act_open(iwc->iwc_peer, &a)) != 0) {
		ep->ep_cmid = NULL;
		ep->ep_flags &= ~EPF_CM_REF;
		cmid->iw_provider = NULL;
		goto fail;
	}
	iwc_ep_deadline(ep, 2 * IWC_MPA_TIMEOUT_MS);
	mutex_exit(&ep->ep_lock);
	/* The ep keeps the QP reference; the base reference is the CM's. */
	return (0);
fail:
	ep->ep_qp = NULL;
	iwc_qp_put(iwc, qp);
	ep->ep_flags |= EPF_FINAL_SENT;
	iwc_ep_release(ep, ret);
	mutex_exit(&ep->ep_lock);
	iwc_ep_rele(ep);
	return (ret);
}

static int
iwc_iw_accept(struct rdk_device *rdev, struct rdk_iw_cm_id *cmid,
    const struct rdk_iw_conn_param *p)
{
	iwc_t *iwc = iwc_of(rdev);
	const uint32_t max = MIN(iwc->iwc_info.tri_vres.trv_max_ordird_qp,
	    IWC_MAX_ORDIRD);
	iwc_ep_t *ep = cmid->iw_provider;
	iwc_qp_t *qp = (iwc_qp_t *)p->qp, *got;
	uint32_t ird, ord;
	int ret;

	if (ep == NULL || qp == NULL || qp->qp_rdk.device != rdev ||
	    p->pdata_len > IWC_MPA_MAX_ULP_PDATA)
		return (EINVAL);
	mutex_enter(&ep->ep_lock);
	if (ep->ep_state != IWC_EP_MPA_REQ_RCVD ||
	    (ep->ep_flags & (EPF_RELEASED | EPF_ABORT_SENT)) != 0) {
		mutex_exit(&ep->ep_lock);
		return (ECONNRESET);
	}
	if ((got = iwc_qp_get(iwc, qp->qp_wq.sq.qid)) != qp) {
		mutex_exit(&ep->ep_lock);
		if (got != NULL)
			iwc_qp_put(iwc, got);
		return (EINVAL);
	}
	ird = MIN(p->ird, max);
	ord = MIN(p->ord, max);
	if (ep->ep_attr.ma_version == 2 && ep->ep_attr.ma_enhanced) {
		/* Relaxed IRD negotiation, as Linux does. */
		if (ord > ep->ep_ird)
			ord = ep->ep_ird;
		if (ird < ep->ep_ord)
			ird = ep->ep_ord;
	}
	ep->ep_ird = ird;
	ep->ep_ord = ord;
	if (ep->ep_attr.ma_p2p_type == IWC_P2P_READ && ep->ep_ird == 0)
		ep->ep_ird = 1;
	ep->ep_qp = qp;
	ep->ep_cmid = cmid;
	ep->ep_deadline = 0;
	if ((ret = iwc_qp_rts(qp, ep)) != 0 ||
	    (ret = iwc_send_mpa(ep, B_TRUE, 0, p->pdata, p->pdata_len)) != 0) {
		/* The framework expects the final event from here on. */
		iwc_ep_abort_locked(ep, ret);
		mutex_exit(&ep->ep_lock);
		return (0);
	}
	ep->ep_state = IWC_EP_FPDU;
	ep->ep_flags |= EPF_UP;
	IWC_STAT(iwc, is_conn_est);
	iwc_ep_event(ep, RDK_IW_EVENT_ESTABLISHED, 0, NULL, 0);
	mutex_exit(&ep->ep_lock);
	return (0);
}

static int
iwc_iw_reject(struct rdk_device *rdev, struct rdk_iw_cm_id *cmid,
    const void *pdata, uint16_t len)
{
	iwc_ep_t *ep = cmid->iw_provider;
	int ret = 0;

	_NOTE(ARGUNUSED(rdev));
	if (ep == NULL)
		return (EINVAL);
	mutex_enter(&ep->ep_lock);
	if (ep->ep_state == IWC_EP_MPA_REQ_RCVD &&
	    (ep->ep_flags & (EPF_RELEASED | EPF_ABORT_SENT)) == 0) {
		if (len > IWC_MPA_MAX_ULP_PDATA ||
		    iwc_send_mpa(ep, B_TRUE, IWC_MPA_REJECT, pdata, len) != 0)
			ret = EIO;
		iwc_ep_abort_locked(ep, ECONNREFUSED);
	}
	mutex_exit(&ep->ep_lock);
	return (ret);
}

static int
iwc_iw_create_listen(struct rdk_device *rdev, struct rdk_iw_cm_id *cmid)
{
	iwc_dev_t *dev = iwc_dev(rdev);
	iwc_t *iwc = dev->d_iwc;
	const clock_t deadline = ddi_get_lbolt() +
	    drv_usectohz(MSEC2NSEC(IWC_LISTEN_WAIT_MS) / 1000);
	t4_rdma_listen_t l;
	iwc_ep_t *ep;
	int ret;

	if (cmid->iw_laddr.sin_family != AF_INET ||
	    cmid->iw_laddr.sin_addr.s_addr == INADDR_ANY ||
	    cmid->iw_laddr.sin_port == 0)
		return (EINVAL);
	if (iwc->iwc_fatal)
		return (EIO);
	ep = iwc_ep_alloc(iwc, dev);
	ep->ep_laddr = cmid->iw_laddr;
	mutex_enter(&ep->ep_lock);
	if ((ret = iwc->iwc_ops->tro_stid_alloc(iwc->iwc_peer, AF_INET, ep,
	    &ep->ep_stid)) != 0) {
		mutex_exit(&ep->ep_lock);
		iwc_ep_rele(ep);
		return (ret);
	}
	iwc_ep_hold(ep);
	ep->ep_flags |= EPF_STID;
	bzero(&l, sizeof (l));
	l.trl_port = ep->ep_port;
	l.trl_family = AF_INET;
	IN6_INADDR_TO_V4MAPPED(&ep->ep_laddr.sin_addr, &l.trl_laddr);
	l.trl_lport = ep->ep_laddr.sin_port;
	l.trl_stid = ep->ep_stid;
	ep->ep_open_done = B_FALSE;
	if ((ret = iwc->iwc_ops->tro_listen(iwc->iwc_peer, &l)) == 0) {
		while (!ep->ep_open_done) {
			if (cv_timedwait(&ep->ep_cv, &ep->ep_lock,
			    deadline) == -1)
				break;
		}
		ret = ep->ep_open_done ? ep->ep_open_status : ETIMEDOUT;
	}
	if (ret != 0) {
		ep->ep_flags &= ~EPF_STID;
		iwc->iwc_ops->tro_stid_free(iwc->iwc_peer, ep->ep_stid);
		mutex_exit(&ep->ep_lock);
		iwc_ep_rele(ep);
		iwc_ep_rele(ep);
		return (ret);
	}
	ep->ep_flags |= EPF_LISTENING;
	ep->ep_state = IWC_EP_LISTEN;
	ep->ep_cmid = cmid;
	cmid->iw_provider = ep;
	mutex_exit(&ep->ep_lock);
	return (0);
}

static void
iwc_iw_destroy_listen(struct rdk_device *rdev, struct rdk_iw_cm_id *cmid)
{
	iwc_t *iwc = iwc_of(rdev);
	iwc_ep_t *ep = cmid->iw_provider;
	const clock_t deadline = ddi_get_lbolt() +
	    drv_usectohz(MSEC2NSEC(IWC_LISTEN_WAIT_MS) / 1000);

	if (ep == NULL)
		return;
	mutex_enter(&ep->ep_lock);
	ep->ep_state = IWC_EP_DEAD;
	ep->ep_cmid = NULL;
	while (ep->ep_embryos != 0)
		cv_wait(&ep->ep_cv, &ep->ep_lock);
	ep->ep_open_done = B_FALSE;
	if (iwc->iwc_ops->tro_unlisten(iwc->iwc_peer, ep->ep_stid) == 0) {
		while (!ep->ep_open_done) {
			if (cv_timedwait(&ep->ep_cv, &ep->ep_lock,
			    deadline) == -1)
				break;
		}
	}
	if ((ep->ep_flags & EPF_STID) != 0) {
		ep->ep_flags &= ~EPF_STID;
		iwc->iwc_ops->tro_stid_free(iwc->iwc_peer, ep->ep_stid);
	}
	mutex_exit(&ep->ep_lock);
	cmid->iw_provider = NULL;
	iwc_ep_rele(ep);
	iwc_ep_rele(ep);
}

static int
iwc_iw_disconnect(struct rdk_device *rdev, struct rdk_iw_cm_id *cmid,
    boolean_t abrupt)
{
	iwc_ep_t *ep = cmid->iw_provider;

	_NOTE(ARGUNUSED(rdev));
	if (ep == NULL)
		return (EINVAL);
	mutex_enter(&ep->ep_lock);
	if (!abrupt && ep->ep_state == IWC_EP_FPDU)
		iwc_ep_close(ep);
	else if (ep->ep_state != IWC_EP_CLOSING || abrupt)
		iwc_ep_abort_locked(ep, abrupt ? ECONNABORTED : 0);
	mutex_exit(&ep->ep_lock);
	return (0);
}

/* The framework is done with the endpoint. */
static void
iwc_iw_release(struct rdk_device *rdev, struct rdk_iw_cm_id *cmid)
{
	iwc_ep_t *ep = cmid->iw_provider;

	_NOTE(ARGUNUSED(rdev));
	if (ep == NULL)
		return;
	mutex_enter(&ep->ep_lock);
	if (ep->ep_cmid == cmid)
		ep->ep_cmid = NULL;
	/* A request the consumer never answered is refused now. */
	if ((ep->ep_flags & EPF_RELEASED) == 0 &&
	    ep->ep_state == IWC_EP_MPA_REQ_RCVD)
		iwc_ep_abort_locked(ep, ECONNREFUSED);
	ep->ep_flags &= ~EPF_CM_REF;
	mutex_exit(&ep->ep_lock);
	cmid->iw_provider = NULL;
	iwc_ep_rele(ep);
}

const struct rdk_iw_cm_ops iwc_iw_ops = {
	.iw_version = RDK_ABI_VERSION,
	.iw_max_pdata = IWC_MPA_MAX_ULP_PDATA,
	.iw_connect = iwc_iw_connect,
	.iw_accept = iwc_iw_accept,
	.iw_reject = iwc_iw_reject,
	.iw_create_listen = iwc_iw_create_listen,
	.iw_destroy_listen = iwc_iw_destroy_listen,
	.iw_disconnect = iwc_iw_disconnect,
	.iw_release = iwc_iw_release
};

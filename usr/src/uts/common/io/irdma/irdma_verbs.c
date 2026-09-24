/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2015 - 2021 Intel Corporation */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * The irdma provider of the rdmak verbs: device and port queries, GIDs, PDs,
 * AHs, DMA buffers, resource numbers and the hardware ARP table.  The logic
 * follows the Linux irdma verbs.c, utils.c and hw.c (see README.illumos);
 * the code is written for illumos.  irdma_cq.c, irdma_qp.c, irdma_post.c,
 * irdma_mr.c and irdma_aeq.c hold the rest.
 *
 * Kernel QPs run in privileged mode, so STAG 0 is the local DMA lkey: it
 * reaches all memory for local access by kernel QPs, and the device refuses
 * it as an rkey.  There is no other all-memory registration.
 *
 * A destroy that cannot issue its control command (the device is being
 * reset or has failed) taints the function; every DMA buffer freed after
 * that goes to ice's quarantine until the reset completes.  That includes
 * consumer buffers from rdk_dma_buf_alloc(), which come from ice too.  A
 * tainted function issues no more verbs commands and reuses no resource
 * number; a command that fails on a healthy one also asks for a reset.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/random.h>
#include <sys/bitmap.h>
#include <sys/strsun.h>

#include "irdma_verbs.h"

/* PBLEs per registration page list, from the HMC's PBLE budget. */
#define	IRDMA_MAX_KMR_PAGES	65536

boolean_t
irdma_healthy(irdma_t *irdma)
{
	return ((irdma->irdma_flags & (IRDMA_F_TAINTED | IRDMA_F_CQP_DEAD)) ==
	    0 && !irdma->irdma_ops->iro_resetting(irdma->irdma_peer));
}

/*
 * A CQP request for op, or NULL when commands cannot be issued; the caller
 * then treats the device as unable to confirm and taints it.
 */
irdma_cqp_req_t *
irdma_vreq(irdma_t *irdma, uint8_t op)
{
	irdma_cqp_req_t *req;

	if (!irdma_healthy(irdma) || (req = irdma_req_alloc(irdma)) == NULL)
		return (NULL);
	req->icr_cmd.cqp_cmd = op;
	return (req);
}

/*
 * A verbs command failed, so the device may still hold what it named.
 * Nothing it touched is reused, and a healthy function asks for a reset.
 */
void
irdma_verbs_uncertain(irdma_t *irdma, const char *what)
{
	if (irdma_healthy(irdma))
		irdma_fatal(irdma, what);
	else
		irdma_taint(irdma);
}

/*
 * Resource numbers.
 */
int
irdma_alloc_rsrc(irdma_t *irdma, ulong_t *map, uint32_t max, uint32_t *num,
    uint32_t *next)
{
	uint32_t i, n;

	mutex_enter(&irdma->irdma_rsrc_lock);
	for (i = 0; i < max; i++) {
		n = (*next + i) % max;
		if (!BT_TEST(map, n)) {
			BT_SET(map, n);
			*num = n;
			*next = (n + 1) % max;
			mutex_exit(&irdma->irdma_rsrc_lock);
			return (0);
		}
	}
	mutex_exit(&irdma->irdma_rsrc_lock);
	return (ENOSPC);
}

/* Once the function is tainted a number stays used until the reset. */
void
irdma_free_rsrc(irdma_t *irdma, ulong_t *map, uint32_t num)
{
	if (!irdma_healthy(irdma))
		return;
	mutex_enter(&irdma->irdma_rsrc_lock);
	BT_CLEAR(map, num);
	mutex_exit(&irdma->irdma_rsrc_lock);
}

/*
 * The hardware ARP table.  Index 0 is reserved.  An entry is keyed by the
 * IPv4 (in ip[0]) or IPv6 address in host order and the MAC, and each QP,
 * AH and GID that uses it holds a reference.  A slot stays claimed from
 * before the device learns of it until the device confirms the delete; a
 * slot whose command failed is never reused.
 */
static int
irdma_arp_find(irdma_t *irdma, const uint32_t *ip, const uint8_t *mac)
{
	irdma_arp_entry_t *e;
	uint32_t i;

	ASSERT(MUTEX_HELD(&irdma->irdma_arp_lock));
	for (i = 1; i < irdma->irdma_arp_size; i++) {
		e = &irdma->irdma_arp_table[i];
		if (e->iae_state == IRDMA_ARP_LIVE &&
		    bcmp(e->iae_ip, ip, sizeof (e->iae_ip)) == 0 &&
		    bcmp(e->iae_mac, mac, ETHERADDRL) == 0)
			return ((int)i);
	}
	return (-1);
}

static int
irdma_arp_cqp(irdma_t *irdma, uint32_t idx, const uint8_t *mac, boolean_t add)
{
	irdma_cqp_req_t *req;
	struct cqp_cmds_info *cmd;

	req = irdma_vreq(irdma, add ? IRDMA_OP_ADD_ARP_CACHE_ENTRY :
	    IRDMA_OP_DELETE_ARP_CACHE_ENTRY);
	if (req == NULL)
		return (EIO);
	cmd = &req->icr_cmd;
	if (add) {
		cmd->in.u.add_arp_cache_entry.info.arp_index = (u16)idx;
		cmd->in.u.add_arp_cache_entry.info.permanent = true;
		bcopy(mac, cmd->in.u.add_arp_cache_entry.info.mac_addr,
		    ETHERADDRL);
		cmd->in.u.add_arp_cache_entry.cqp = &irdma->irdma_cqp;
		cmd->in.u.add_arp_cache_entry.scratch =
		    irdma_req_scratch(irdma, req);
	} else {
		cmd->in.u.del_arp_cache_entry.arp_index = (u16)idx;
		cmd->in.u.del_arp_cache_entry.cqp = &irdma->irdma_cqp;
		cmd->in.u.del_arp_cache_entry.scratch =
		    irdma_req_scratch(irdma, req);
	}
	return (irdma_cqp_exec(irdma, req, NULL));
}

/* Delete a slot the caller has made dying. */
static void
irdma_arp_del_idx(irdma_t *irdma, uint32_t idx)
{
	if (irdma_arp_cqp(irdma, idx, NULL, B_FALSE) != 0) {
		irdma_verbs_uncertain(irdma, "failed to delete an ARP entry");
		return;
	}
	mutex_enter(&irdma->irdma_arp_lock);
	bzero(&irdma->irdma_arp_table[idx], sizeof (irdma_arp_entry_t));
	BT_CLEAR(irdma->irdma_arp_map, idx);
	mutex_exit(&irdma->irdma_arp_lock);
}

/*
 * Return the ARP index for ip and mac with a reference, adding the entry
 * if needed, or -1.  The table lock is not held across the command.
 */
int
irdma_add_arp(irdma_t *irdma, const uint32_t *ip4, boolean_t ipv4,
    const uint8_t *mac)
{
	irdma_arp_entry_t *e;
	uint32_t ip[4] = { 0 };
	uint32_t idx;
	int cur;

	if (ipv4)
		ip[0] = ip4[0];
	else
		bcopy(ip4, ip, sizeof (ip));

	mutex_enter(&irdma->irdma_arp_lock);
	if ((cur = irdma_arp_find(irdma, ip, mac)) >= 0) {
		irdma->irdma_arp_table[cur].iae_refs++;
		mutex_exit(&irdma->irdma_arp_lock);
		return (cur);
	}
	for (idx = 1; idx < irdma->irdma_arp_size; idx++) {
		if (!BT_TEST(irdma->irdma_arp_map, idx))
			break;
	}
	if (idx >= irdma->irdma_arp_size) {
		mutex_exit(&irdma->irdma_arp_lock);
		return (-1);
	}
	BT_SET(irdma->irdma_arp_map, idx);
	e = &irdma->irdma_arp_table[idx];
	bcopy(ip, e->iae_ip, sizeof (ip));
	bcopy(mac, e->iae_mac, ETHERADDRL);
	e->iae_refs = 1;
	e->iae_state = IRDMA_ARP_PENDING;
	mutex_exit(&irdma->irdma_arp_lock);

	if (irdma_arp_cqp(irdma, idx, mac, B_TRUE) != 0) {
		irdma_verbs_uncertain(irdma, "failed to add an ARP entry");
		return (-1);
	}
	mutex_enter(&irdma->irdma_arp_lock);
	e->iae_state = IRDMA_ARP_LIVE;
	mutex_exit(&irdma->irdma_arp_lock);
	return ((int)idx);
}

void
irdma_arp_rele(irdma_t *irdma, uint32_t idx)
{
	irdma_arp_entry_t *e;

	if (idx == 0 || idx >= irdma->irdma_arp_size)
		return;
	e = &irdma->irdma_arp_table[idx];
	mutex_enter(&irdma->irdma_arp_lock);
	VERIFY3U(e->iae_state, ==, IRDMA_ARP_LIVE);
	VERIFY3U(e->iae_refs, >, 0);
	if (--e->iae_refs != 0) {
		mutex_exit(&irdma->irdma_arp_lock);
		return;
	}
	e->iae_state = IRDMA_ARP_DYING;
	mutex_exit(&irdma->irdma_arp_lock);
	irdma_arp_del_idx(irdma, idx);
}

/* The IP of a GID in host order; returns whether it is IPv4. */
static boolean_t
irdma_gid_ip(const rdk_gid_t *gid, uint32_t *ip)
{
	ipaddr_t v4;
	uint_t i;

	bzero(ip, 4 * sizeof (uint32_t));
	if (rdk_gid_to_ipv4(gid, &v4)) {
		ip[0] = ntohl(v4);
		return (B_TRUE);
	}
	for (i = 0; i < 4; i++) {
		uint32_t w;

		bcopy(&gid->raw[i * 4], &w, sizeof (w));
		ip[i] = ntohl(w);
	}
	return (B_FALSE);
}

/*
 * The device and port.
 */
static int
irdma_query_device(struct rdk_device *rdev, struct rdk_device_attr *a)
{
	irdma_t *irdma = IRDMA_DEV(rdev);
	struct irdma_hw_attrs *hw = &irdma->irdma_sc.hw_attrs;
	uint64_t fw = irdma->irdma_sc.feature_info[IRDMA_FEATURE_FW_INFO];
	uint8_t *mac = irdma->irdma_info.iri_mac;

	bzero(a, sizeof (*a));
	a->fw_ver = (FIELD_GET(IRDMA_FW_VER_MAJOR, fw) << 32) |
	    FIELD_GET(IRDMA_FW_VER_MINOR, fw);
	/* EUI-64 from the MAC, as Linux does for the system image GUID. */
	a->sys_image_guid = ((uint64_t)(mac[0] ^ 2) << 56) |
	    ((uint64_t)mac[1] << 48) | ((uint64_t)mac[2] << 40) |
	    (0xfffeULL << 24) | ((uint64_t)mac[3] << 16) |
	    ((uint64_t)mac[4] << 8) | mac[5];
	a->max_mr_size = hw->max_mr_size;
	a->page_size_cap = PAGESIZE;
	a->vendor_id = 0x8086;
	a->max_qp = (int)irdma->irdma_max_qp - IRDMA_FIRST_ID;
	a->max_qp_wr = (int)MIN(hw->max_qp_wr,
	    hw->uk_attrs.max_hw_wq_quanta / 8);
	a->device_cap_flags = RDK_DEVICE_MEM_MGT_EXTENSIONS |
	    RDK_DEVICE_RC_RNR_NAK_GEN;
	a->kernel_cap_flags = RDK_KCAP_LOCAL_DMA_LKEY;
	a->local_dma_lkey = 0;
	a->max_send_sge = (int)hw->uk_attrs.max_hw_wq_frags;
	a->max_recv_sge = (int)hw->uk_attrs.max_hw_wq_frags;
	a->max_sge_rd = (int)hw->uk_attrs.max_hw_read_sges;
	a->max_cq = (int)irdma->irdma_max_cq - IRDMA_FIRST_ID;
	a->max_cqe = (int)MIN(hw->uk_attrs.max_hw_cq_size, IRDMA_MAX_KCQE);
	a->max_mr = (int)irdma->irdma_max_mr - 1;
	a->max_pd = (int)irdma->irdma_max_pd - IRDMA_FIRST_ID;
	a->max_qp_rd_atom = (int)hw->max_hw_ird;
	a->max_qp_init_rd_atom = (int)hw->max_hw_ord;
	a->max_ah = (int)irdma->irdma_max_ah - 1;
	a->max_fast_reg_page_list_len = IRDMA_MAX_KMR_PAGES;
	a->max_inline_data = hw->uk_attrs.max_hw_inline;
	a->max_pkeys = IRDMA_PKEY_TBL_SZ;
	return (0);
}

static int
irdma_query_port(struct rdk_device *rdev, uint32_t port,
    struct rdk_port_attr *a)
{
	irdma_t *irdma = IRDMA_DEV(rdev);

	_NOTE(ARGUNUSED(port));
	a->state = irdma->irdma_link == LINK_STATE_UP ? RDK_PORT_ACTIVE :
	    RDK_PORT_DOWN;
	a->max_mtu = RDK_MTU_4096;
	a->phys_mtu = irdma->irdma_mtu;
	a->active_mtu = rdk_mtu_int_to_enum((int)irdma->irdma_mtu);
	a->gid_tbl_len = IRDMA_GID_TBL_LEN;
	a->max_msg_sz =
	    (uint32_t)MIN(irdma->irdma_sc.hw_attrs.max_hw_outbound_msg_size,
	    UINT32_MAX);
	a->pkey_tbl_len = IRDMA_PKEY_TBL_SZ;
	a->speed = irdma->irdma_speed;
	bcopy(irdma->irdma_info.iri_mac, a->mac, ETHERADDRL);
	return (0);
}

/*
 * A local address goes into the ARP table with our MAC, which is how the
 * device recognizes loopback traffic.  Only the PF's MAC is accepted.
 */
static int
irdma_add_gid(const struct rdk_gid_attr *attr)
{
	irdma_t *irdma = IRDMA_DEV(attr->device);
	uint32_t ip[4];
	boolean_t v4;

	if (bcmp(attr->mac, irdma->irdma_info.iri_mac, ETHERADDRL) != 0 ||
	    attr->vlan_id != RDK_VLAN_NONE)
		return (ENOTSUP);
	v4 = irdma_gid_ip(&attr->gid, ip);
	return (irdma_add_arp(irdma, ip, v4, attr->mac) < 0 ? EIO : 0);
}

static void
irdma_del_gid(const struct rdk_gid_attr *attr)
{
	irdma_t *irdma = IRDMA_DEV(attr->device);
	uint32_t ip[4];
	int idx;

	(void) irdma_gid_ip(&attr->gid, ip);
	mutex_enter(&irdma->irdma_arp_lock);
	idx = irdma_arp_find(irdma, ip, attr->mac);
	mutex_exit(&irdma->irdma_arp_lock);
	if (idx > 0)
		irdma_arp_rele(irdma, (uint32_t)idx);
}

/*
 * Protection domains.
 */
static int
irdma_alloc_pd(struct rdk_pd *rpd)
{
	irdma_t *irdma = IRDMA_DEV(rpd->device);
	irdma_pd_t *pd = IRDMA_PD(rpd);
	uint32_t id;
	int ret;

	ret = irdma_alloc_rsrc(irdma, irdma->irdma_pd_map, irdma->irdma_max_pd,
	    &id, &irdma->irdma_next_pd);
	if (ret != 0)
		return (ret);
	irdma_sc_pd_init(&irdma->irdma_sc, &pd->ipd_sc, id, IRDMA_ABI_VER);
	atomic_inc_32(&irdma->irdma_npds);
	return (0);
}

static void
irdma_dealloc_pd(struct rdk_pd *rpd)
{
	irdma_t *irdma = IRDMA_DEV(rpd->device);

	irdma_free_rsrc(irdma, irdma->irdma_pd_map,
	    IRDMA_PD(rpd)->ipd_sc.pd_id);
	atomic_dec_32(&irdma->irdma_npds);
}

/*
 * Address handles, for UD QPs.
 */
static int
irdma_ah_cqp(irdma_t *irdma, irdma_ah_t *ah, boolean_t create)
{
	irdma_cqp_req_t *req;
	struct cqp_cmds_info *cmd;
	int ret;

	req = irdma_vreq(irdma, create ? IRDMA_OP_AH_CREATE :
	    IRDMA_OP_AH_DESTROY);
	if (req == NULL)
		return (EIO);
	cmd = &req->icr_cmd;
	if (create) {
		cmd->in.u.ah_create.info = ah->iah_sc.ah_info;
		cmd->in.u.ah_create.cqp = &irdma->irdma_cqp;
		cmd->in.u.ah_create.scratch = irdma_req_scratch(irdma, req);
	} else {
		cmd->in.u.ah_destroy.info = ah->iah_sc.ah_info;
		cmd->in.u.ah_destroy.cqp = &irdma->irdma_cqp;
		cmd->in.u.ah_destroy.scratch = irdma_req_scratch(irdma, req);
	}
	ret = irdma_cqp_exec(irdma, req, NULL);
	return (ret);
}

static int
irdma_create_ah(struct rdk_ah *rah, struct rdk_ah_attr *attr)
{
	irdma_t *irdma = IRDMA_DEV(rah->device);
	irdma_ah_t *ah = IRDMA_AH(rah);
	struct irdma_ah_info *info = &ah->iah_sc.ah_info;
	const struct rdk_gid_attr *sgid = attr->grh.sgid_attr;
	uint32_t sip[4], dip[4];
	boolean_t sv4, dv4;
	uint32_t id;
	int arp, ret;

	sv4 = irdma_gid_ip(&sgid->gid, sip);
	dv4 = irdma_gid_ip(&attr->grh.dgid, dip);
	if (sv4 != dv4)
		return (EINVAL);
	/* Multicast needs group support, which is not here. */
	if ((dv4 && (dip[0] >> 28) == 0xe) || attr->grh.dgid.raw[0] == 0xff)
		return (ENOTSUP);

	irdma_sc_init_ah(&irdma->irdma_sc, &ah->iah_sc);
	info->vsi = &irdma->irdma_vsi;
	info->pd_idx = IRDMA_PD(rah->pd)->ipd_sc.pd_id;
	info->flow_label = attr->grh.flow_label;
	info->hop_ttl = attr->grh.hop_limit;
	info->tc_tos = attr->grh.traffic_class;
	info->ipv4_valid = dv4;
	bcopy(dip, info->dest_ip_addr, sizeof (dip));
	bcopy(sip, info->src_ip_addr, sizeof (sip));
	info->do_lpbk = bcmp(sip, dip, sizeof (sip)) == 0;
	bcopy(sgid->mac, info->mac_addr, ETHERADDRL);
	info->vlan_tag = 0;
	info->insert_vlan_tag = false;

	arp = irdma_add_arp(irdma, dip, dv4, attr->roce.dmac);
	if (arp < 0)
		return (EIO);
	info->dst_arpindex = (u32)arp;

	ret = irdma_alloc_rsrc(irdma, irdma->irdma_ah_map, irdma->irdma_max_ah,
	    &id, &irdma->irdma_next_ah);
	if (ret != 0) {
		irdma_arp_rele(irdma, (uint32_t)arp);
		return (ret);
	}
	info->ah_idx = id;
	if ((ret = irdma_ah_cqp(irdma, ah, B_TRUE)) != 0) {
		irdma_verbs_uncertain(irdma, "failed to create an AH");
		irdma_arp_rele(irdma, (uint32_t)arp);
		irdma_free_rsrc(irdma, irdma->irdma_ah_map, id);
		return (ret);
	}
	info->ah_valid = true;
	ah->iah_created = B_TRUE;
	atomic_inc_32(&irdma->irdma_nahs);
	return (0);
}

static void
irdma_destroy_ah(struct rdk_ah *rah)
{
	irdma_t *irdma = IRDMA_DEV(rah->device);
	irdma_ah_t *ah = IRDMA_AH(rah);

	if (!ah->iah_created)
		return;
	if (irdma_ah_cqp(irdma, ah, B_FALSE) != 0)
		irdma_verbs_uncertain(irdma, "failed to destroy an AH");
	irdma_arp_rele(irdma, ah->iah_sc.ah_info.dst_arpindex);
	irdma_free_rsrc(irdma, irdma->irdma_ah_map, ah->iah_sc.ah_info.ah_idx);
	ah->iah_created = B_FALSE;
	atomic_dec_32(&irdma->irdma_nahs);
}

/*
 * DMA buffers for consumers come from ice through the osdep layer, so a
 * buffer freed while the device may still write it is quarantined.
 */
static int
irdma_dma_alloc(struct rdk_device *rdev, size_t len, rdk_dma_buf_t *buf)
{
	irdma_t *irdma = IRDMA_DEV(rdev);
	dma_addr_t pa;
	void *va;

	if (len > IRDMA_DMA_BUF_MAX)
		return (EINVAL);
	va = dma_alloc_coherent(&irdma->irdma_osdev, len, &pa, GFP_KERNEL);
	if (va == NULL)
		return (ENOMEM);
	buf->rdb_va = va;
	buf->rdb_pa = pa;
	buf->rdb_len = len;
	return (0);
}

static void
irdma_dma_free(struct rdk_device *rdev, rdk_dma_buf_t *buf)
{
	irdma_t *irdma = IRDMA_DEV(rdev);

	irdma_osdep_free_consumer(irdma, buf->rdb_va, buf->rdb_pa,
	    buf->rdb_len);
}

static const struct rdk_device_ops irdma_rdk_ops = {
	.version = RDK_ABI_VERSION,
	.query_device = irdma_query_device,
	.query_port = irdma_query_port,
	.add_gid = irdma_add_gid,
	.del_gid = irdma_del_gid,
	.alloc_pd = irdma_alloc_pd,
	.dealloc_pd = irdma_dealloc_pd,
	.create_cq = irdma_create_cq,
	.destroy_cq = irdma_destroy_cq,
	.poll_cq = irdma_poll_cq,
	.req_notify_cq = irdma_req_notify_cq,
	.create_qp = irdma_create_qp,
	.modify_qp = irdma_modify_qp,
	.query_qp = irdma_query_qp,
	.destroy_qp = irdma_destroy_qp,
	.post_send = irdma_post_send,
	.post_recv = irdma_post_recv,
	.alloc_mr = irdma_alloc_mr,
	.map_mr_sg = irdma_map_mr_sg,
	.dereg_mr = irdma_dereg_mr,
	.create_ah = irdma_create_ah,
	.destroy_ah = irdma_destroy_ah,
	.dma_alloc = irdma_dma_alloc,
	.dma_free = irdma_dma_free,
	.size_pd = sizeof (irdma_pd_t),
	.size_cq = sizeof (irdma_cq_t),
	.size_qp = sizeof (irdma_qp_t),
	.size_ah = sizeof (irdma_ah_t)
};

/*
 * Size the resource maps and tables from the committed HMC, once the
 * control plane is up.  The caller holds irdma_cfg_lock.
 */
int
irdma_verbs_init(irdma_t *irdma)
{
	struct irdma_hmc_obj_info *o = irdma->irdma_sc.hmc_info->hmc_obj;
	uint32_t mrbits;
	size_t size;
	uint32_t i;
	char *p;

	ASSERT(MUTEX_HELD(&irdma->irdma_cfg_lock));

	irdma->irdma_max_qp = o[IRDMA_HMC_IW_QP].cnt;
	irdma->irdma_max_cq = o[IRDMA_HMC_IW_CQ].cnt;
	irdma->irdma_max_mr = o[IRDMA_HMC_IW_MR].cnt;
	irdma->irdma_max_ah = o[IRDMA_HMC_IW_FSIAV].cnt;
	irdma->irdma_arp_size = o[IRDMA_HMC_IW_ARP].cnt;
	irdma->irdma_max_pd = irdma->irdma_sc.hw_attrs.max_hw_pds;
	if (irdma->irdma_max_qp <= IRDMA_FIRST_ID ||
	    irdma->irdma_max_cq <= IRDMA_FIRST_ID ||
	    irdma->irdma_max_mr < 2 || irdma->irdma_max_ah < 2 ||
	    irdma->irdma_arp_size < 2 ||
	    irdma->irdma_max_pd <= IRDMA_FIRST_ID ||
	    irdma->irdma_max_pd > (1U << 20) ||
	    irdma->irdma_max_ah > (1U << 20)) {
		irdma_error(irdma, "no room for verbs: qp %u cq %u mr %u "
		    "ah %u arp %u pd %u", irdma->irdma_max_qp,
		    irdma->irdma_max_cq, irdma->irdma_max_mr,
		    irdma->irdma_max_ah, irdma->irdma_arp_size,
		    irdma->irdma_max_pd);
		return (ENOSPC);
	}

	size = BT_SIZEOFMAP(irdma->irdma_max_qp) +
	    BT_SIZEOFMAP(irdma->irdma_max_cq) +
	    BT_SIZEOFMAP(irdma->irdma_max_mr) +
	    BT_SIZEOFMAP(irdma->irdma_max_pd) +
	    BT_SIZEOFMAP(irdma->irdma_max_ah) +
	    BT_SIZEOFMAP(irdma->irdma_arp_size) +
	    sizeof (irdma_qp_t *) * irdma->irdma_max_qp +
	    sizeof (irdma_cq_t *) * irdma->irdma_max_cq +
	    sizeof (irdma_arp_entry_t) * irdma->irdma_arp_size;
	p = kmem_zalloc(size, KM_SLEEP);
	irdma->irdma_rsrc_mem = p;
	irdma->irdma_rsrc_size = size;
	irdma->irdma_qp_map = (ulong_t *)(void *)p;
	p += BT_SIZEOFMAP(irdma->irdma_max_qp);
	irdma->irdma_cq_map = (ulong_t *)(void *)p;
	p += BT_SIZEOFMAP(irdma->irdma_max_cq);
	irdma->irdma_mr_map = (ulong_t *)(void *)p;
	p += BT_SIZEOFMAP(irdma->irdma_max_mr);
	irdma->irdma_pd_map = (ulong_t *)(void *)p;
	p += BT_SIZEOFMAP(irdma->irdma_max_pd);
	irdma->irdma_ah_map = (ulong_t *)(void *)p;
	p += BT_SIZEOFMAP(irdma->irdma_max_ah);
	irdma->irdma_arp_map = (ulong_t *)(void *)p;
	p += BT_SIZEOFMAP(irdma->irdma_arp_size);
	irdma->irdma_qp_table = (irdma_qp_t **)(void *)p;
	p += sizeof (irdma_qp_t *) * irdma->irdma_max_qp;
	irdma->irdma_cq_table = (irdma_cq_t **)(void *)p;
	p += sizeof (irdma_cq_t *) * irdma->irdma_max_cq;
	irdma->irdma_arp_table = (irdma_arp_entry_t *)(void *)p;

	/* Ids 0 to 2 are reserved as in Linux (QP 1 is the GSI QP). */
	for (i = 0; i < IRDMA_FIRST_ID; i++) {
		BT_SET(irdma->irdma_qp_map, i);
		BT_SET(irdma->irdma_cq_map, i);
		BT_SET(irdma->irdma_pd_map, i);
	}
	BT_SET(irdma->irdma_mr_map, 0);
	BT_SET(irdma->irdma_ah_map, 0);
	BT_SET(irdma->irdma_arp_map, 0);
	irdma->irdma_next_qp = irdma->irdma_next_cq = IRDMA_FIRST_ID;
	irdma->irdma_next_pd = IRDMA_FIRST_ID;
	irdma->irdma_next_ah = irdma->irdma_next_arp = 1;
	irdma->irdma_gsi_used = B_FALSE;

	/* The stag index takes at least 14 bits of the 24 above the key. */
	mrbits = 24 - MAX((uint32_t)highbit(irdma->irdma_max_mr - 1), 14);
	irdma->irdma_mr_stagmask = ~(((1U << mrbits) - 1) << (32 - mrbits));

	irdma->irdma_wq = ddi_taskq_create(irdma->irdma_dip, "irdma_wq", 1,
	    TASKQ_DEFAULTPRI, 0);
	if (irdma->irdma_wq == NULL) {
		kmem_free(irdma->irdma_rsrc_mem, irdma->irdma_rsrc_size);
		irdma->irdma_rsrc_mem = NULL;
		return (ENOMEM);
	}
	irdma->irdma_verbs_live = B_TRUE;
	return (0);
}

/*
 * After the device is unregistered, so every object is gone.  The ARP
 * entries of the GIDs were deleted with them.
 */
void
irdma_verbs_fini(irdma_t *irdma)
{
	uint32_t i;

	if (!irdma->irdma_verbs_live)
		return;
	/* The interrupt task checks irdma_verbs_live before the tables. */
	irdma->irdma_verbs_live = B_FALSE;
	membar_producer();
	ddi_taskq_wait(irdma->irdma_taskq);
	ddi_taskq_wait(irdma->irdma_wq);
	ddi_taskq_destroy(irdma->irdma_wq);
	irdma->irdma_wq = NULL;
	for (i = 1; i < irdma->irdma_arp_size; i++) {
		if (irdma->irdma_arp_table[i].iae_state == IRDMA_ARP_LIVE)
			irdma_arp_del_idx(irdma, i);
	}
	kmem_free(irdma->irdma_rsrc_mem, irdma->irdma_rsrc_size);
	irdma->irdma_rsrc_mem = NULL;
}

int
irdma_verbs_register(irdma_t *irdma)
{
	struct rdk_device *rdev = &irdma->irdma_rdk;
	int ret;

	bzero(rdev, sizeof (*rdev));
	(void) snprintf(rdev->rd_name, sizeof (rdev->rd_name), "irdma%d",
	    irdma->irdma_instance);
	rdev->rd_dip = irdma->irdma_dip;
	rdev->rd_ops = &irdma_rdk_ops;
	rdev->rd_phys_port_cnt = 1;
	rdev->rd_node_guid = 0;
	if ((ret = rdk_register_device(rdev)) != 0) {
		irdma_error(irdma, "failed to register with rdmak: %d", ret);
		return (ret);
	}
	rw_enter(&irdma->irdma_rdk_lock, RW_WRITER);
	irdma->irdma_rdk_live = B_TRUE;
	rw_exit(&irdma->irdma_rdk_lock);
	return (0);
}

void
irdma_verbs_unregister(irdma_t *irdma)
{
	rw_enter(&irdma->irdma_rdk_lock, RW_WRITER);
	if (!irdma->irdma_rdk_live) {
		rw_exit(&irdma->irdma_rdk_lock);
		return;
	}
	irdma->irdma_rdk_live = B_FALSE;
	rw_exit(&irdma->irdma_rdk_lock);
	rdk_unregister_device(&irdma->irdma_rdk);
}

/* A device or port event, from thread context. */
void
irdma_verbs_event(irdma_t *irdma, enum rdk_event_type type)
{
	struct rdk_event ev;

	rw_enter(&irdma->irdma_rdk_lock, RW_READER);
	if (irdma->irdma_rdk_live) {
		bzero(&ev, sizeof (ev));
		ev.device = &irdma->irdma_rdk;
		ev.event = type;
		ev.element.port_num = 1;
		rdk_dispatch_event(&ev);
	}
	rw_exit(&irdma->irdma_rdk_lock);
}

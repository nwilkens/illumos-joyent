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
 * rdmak - kernel RDMA verbs framework
 *
 * This file holds the module linkage, device and client registration, the
 * per-port GID tables and asynchronous events.  rdk_verbs.c has the verbs
 * and rdk_cq.c the completion processing for rdk_alloc_cq() consumers.
 *
 * A device counts its live objects (PDs, CQs, QPs, MRs, AHs and DMA
 * buffers).  rdk_unregister_device() stops new objects, calls each client's
 * remove callback, and then waits until the count reaches zero, so the
 * provider tears down only after every consumer is gone.
 *
 * GID tables are static for now: a privileged kernel consumer adds the
 * entries.  An entry that a QP or AH refers to cannot be deleted.
 */

#include <sys/types.h>
#include <sys/modctl.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/disp.h>
#include <netinet/in.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"

/* How long unregister waits between warnings about a leaked object. */
#define	RDK_UNREG_WARN_SEC	10

static kmutex_t rdk_reg_lock;
static list_t rdk_devices;
static list_t rdk_clients;

int
rdk_obj_hold(struct rdk_device *dev)
{
	struct rdk_device_priv *p = dev->rd_priv;
	int ret = 0;

	mutex_enter(&p->rdp_lock);
	if (p->rdp_dying)
		ret = ENXIO;
	else
		p->rdp_nobjs++;
	mutex_exit(&p->rdp_lock);
	return (ret);
}

void
rdk_obj_rele(struct rdk_device *dev)
{
	struct rdk_device_priv *p = dev->rd_priv;

	mutex_enter(&p->rdp_lock);
	VERIFY3U(p->rdp_nobjs, >, 0);
	if (--p->rdp_nobjs == 0)
		cv_broadcast(&p->rdp_cv);
	mutex_exit(&p->rdp_lock);
}

boolean_t
rdk_port_valid(struct rdk_device *dev, uint32_t port)
{
	return (port >= 1 && port <= dev->rd_phys_port_cnt);
}

static rdk_cdata_t *
rdk_cdata_find(struct rdk_device_priv *p, struct rdk_client *client)
{
	rdk_cdata_t *cd;

	ASSERT(MUTEX_HELD(&p->rdp_lock));
	for (cd = list_head(&p->rdp_cdata); cd != NULL;
	    cd = list_next(&p->rdp_cdata, cd)) {
		if (cd->rcd_client == client)
			return (cd);
	}
	return (NULL);
}

/* The caller holds rdk_reg_lock. */
static void
rdk_client_add_one(struct rdk_client *client, struct rdk_device *dev)
{
	struct rdk_device_priv *p = dev->rd_priv;
	rdk_cdata_t *cd;
	int ret;

	ASSERT(MUTEX_HELD(&rdk_reg_lock));
	cd = kmem_zalloc(sizeof (*cd), KM_SLEEP);
	cd->rcd_client = client;
	mutex_enter(&p->rdp_lock);
	list_insert_tail(&p->rdp_cdata, cd);
	mutex_exit(&p->rdp_lock);

	ret = client->add(dev);
	if (ret != 0) {
		dev_err(dev->rd_dip, CE_NOTE, "!RDMA client %s declined %s: %d",
		    client->name, dev->rd_name, ret);
	}

	mutex_enter(&p->rdp_lock);
	cd->rcd_added = (ret == 0);
	if (!cd->rcd_added) {
		list_remove(&p->rdp_cdata, cd);
		kmem_free(cd, sizeof (*cd));
	}
	mutex_exit(&p->rdp_lock);
}

/* The caller holds rdk_reg_lock. */
static void
rdk_client_remove_one(struct rdk_client *client, struct rdk_device *dev)
{
	struct rdk_device_priv *p = dev->rd_priv;
	rdk_cdata_t *cd;
	void *data;

	ASSERT(MUTEX_HELD(&rdk_reg_lock));
	mutex_enter(&p->rdp_lock);
	cd = rdk_cdata_find(p, client);
	if (cd == NULL || !cd->rcd_added) {
		mutex_exit(&p->rdp_lock);
		return;
	}
	data = cd->rcd_data;
	mutex_exit(&p->rdp_lock);

	client->remove(dev, data);

	mutex_enter(&p->rdp_lock);
	list_remove(&p->rdp_cdata, cd);
	mutex_exit(&p->rdp_lock);
	kmem_free(cd, sizeof (*cd));
}

void
rdk_set_client_data(struct rdk_device *dev, struct rdk_client *client,
    void *data)
{
	struct rdk_device_priv *p = dev->rd_priv;
	rdk_cdata_t *cd;

	mutex_enter(&p->rdp_lock);
	if ((cd = rdk_cdata_find(p, client)) != NULL)
		cd->rcd_data = data;
	mutex_exit(&p->rdp_lock);
}

void *
rdk_get_client_data(struct rdk_device *dev, struct rdk_client *client)
{
	struct rdk_device_priv *p = dev->rd_priv;
	rdk_cdata_t *cd;
	void *data = NULL;

	mutex_enter(&p->rdp_lock);
	if ((cd = rdk_cdata_find(p, client)) != NULL)
		data = cd->rcd_data;
	mutex_exit(&p->rdp_lock);
	return (data);
}

static boolean_t
rdk_ops_valid(const struct rdk_device_ops *ops)
{
	return (ops != NULL && ops->version == RDK_ABI_VERSION &&
	    ops->query_device != NULL && ops->query_port != NULL &&
	    ops->add_gid != NULL && ops->del_gid != NULL &&
	    ops->alloc_pd != NULL && ops->dealloc_pd != NULL &&
	    ops->create_cq != NULL && ops->destroy_cq != NULL &&
	    ops->poll_cq != NULL && ops->req_notify_cq != NULL &&
	    ops->create_qp != NULL && ops->modify_qp != NULL &&
	    ops->query_qp != NULL && ops->destroy_qp != NULL &&
	    ops->post_send != NULL && ops->post_recv != NULL &&
	    ops->alloc_mr != NULL && ops->map_mr_sg != NULL &&
	    ops->dereg_mr != NULL && ops->create_ah != NULL &&
	    ops->destroy_ah != NULL && ops->dma_alloc != NULL &&
	    ops->dma_free != NULL &&
	    ops->size_pd >= sizeof (struct rdk_pd) &&
	    ops->size_cq >= sizeof (struct rdk_cq) &&
	    ops->size_qp >= sizeof (struct rdk_qp) &&
	    ops->size_ah >= sizeof (struct rdk_ah));
}

static void
rdk_priv_free(struct rdk_device_priv *p)
{
	list_destroy(&p->rdp_handlers);
	list_destroy(&p->rdp_cdata);
	rw_destroy(&p->rdp_ev_lock);
	mutex_destroy(&p->rdp_gid_lock);
	cv_destroy(&p->rdp_cv);
	mutex_destroy(&p->rdp_lock);
	kmem_free(p, sizeof (*p));
}

int
rdk_register_device(struct rdk_device *dev)
{
	struct rdk_device_priv *p, *o;
	struct rdk_client *client;
	int ret;

	if (dev == NULL || dev->rd_dip == NULL || !rdk_ops_valid(dev->rd_ops) ||
	    dev->rd_phys_port_cnt != 1 || dev->rd_priv != NULL ||
	    strnlen(dev->rd_name, RDK_NAME_MAX) == 0 ||
	    strnlen(dev->rd_name, RDK_NAME_MAX) == RDK_NAME_MAX)
		return (EINVAL);

	bzero(&dev->rd_attr, sizeof (dev->rd_attr));
	if ((ret = dev->rd_ops->query_device(dev, &dev->rd_attr)) != 0)
		return (ret);
	if (dev->rd_num_comp_vectors == 0)
		dev->rd_num_comp_vectors = 1;

	p = kmem_zalloc(sizeof (*p), KM_SLEEP);
	p->rdp_dev = dev;
	mutex_init(&p->rdp_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&p->rdp_cv, NULL, CV_DRIVER, NULL);
	mutex_init(&p->rdp_gid_lock, NULL, MUTEX_DRIVER, NULL);
	rw_init(&p->rdp_ev_lock, NULL, RW_DRIVER, NULL);
	list_create(&p->rdp_cdata, sizeof (rdk_cdata_t),
	    offsetof(rdk_cdata_t, rcd_node));
	list_create(&p->rdp_handlers, sizeof (struct rdk_event_handler),
	    offsetof(struct rdk_event_handler, reh_node));

	mutex_enter(&rdk_reg_lock);
	for (o = list_head(&rdk_devices); o != NULL;
	    o = list_next(&rdk_devices, o)) {
		if (strcmp(o->rdp_dev->rd_name, dev->rd_name) == 0) {
			mutex_exit(&rdk_reg_lock);
			rdk_priv_free(p);
			return (EEXIST);
		}
	}
	dev->rd_priv = p;
	list_insert_tail(&rdk_devices, p);
	for (client = list_head(&rdk_clients); client != NULL;
	    client = list_next(&rdk_clients, client))
		rdk_client_add_one(client, dev);
	mutex_exit(&rdk_reg_lock);
	return (0);
}

/*
 * Waits for every consumer to let go of the device.  A consumer that
 * leaks an object blocks this forever, with a warning every few seconds.
 */
void
rdk_unregister_device(struct rdk_device *dev)
{
	struct rdk_device_priv *p = dev->rd_priv;
	struct rdk_client *client;
	uint_t i;

	if (p == NULL)
		return;

	mutex_enter(&rdk_reg_lock);
	mutex_enter(&p->rdp_lock);
	p->rdp_dying = B_TRUE;
	mutex_exit(&p->rdp_lock);
	list_remove(&rdk_devices, p);

	for (client = list_tail(&rdk_clients); client != NULL;
	    client = list_prev(&rdk_clients, client))
		rdk_client_remove_one(client, dev);

	mutex_enter(&p->rdp_lock);
	while (p->rdp_nobjs != 0) {
		if (cv_reltimedwait(&p->rdp_cv, &p->rdp_lock,
		    SEC_TO_TICK(RDK_UNREG_WARN_SEC), TR_SEC) == -1) {
			dev_err(dev->rd_dip, CE_WARN, "!waiting for %llu RDMA "
			    "objects to be destroyed",
			    (u_longlong_t)p->rdp_nobjs);
		}
	}
	mutex_exit(&p->rdp_lock);
	mutex_exit(&rdk_reg_lock);

	mutex_enter(&p->rdp_gid_lock);
	for (i = 0; i < RDK_GID_TABLE_LEN; i++) {
		rdk_gid_ent_t *e = &p->rdp_gids[i];

		if (!e->rge_valid)
			continue;
		VERIFY0(e->rge_refs);
		dev->rd_ops->del_gid(&e->rge_attr);
		e->rge_valid = B_FALSE;
	}
	mutex_exit(&p->rdp_gid_lock);

	VERIFY(list_is_empty(&p->rdp_handlers));
	VERIFY(list_is_empty(&p->rdp_cdata));
	dev->rd_priv = NULL;
	rdk_priv_free(p);
}

int
rdk_register_client(struct rdk_client *client)
{
	struct rdk_device_priv *p;

	if (client == NULL || client->add == NULL || client->remove == NULL ||
	    client->name == NULL)
		return (EINVAL);

	mutex_enter(&rdk_reg_lock);
	list_insert_tail(&rdk_clients, client);
	for (p = list_head(&rdk_devices); p != NULL;
	    p = list_next(&rdk_devices, p))
		rdk_client_add_one(client, p->rdp_dev);
	mutex_exit(&rdk_reg_lock);
	return (0);
}

void
rdk_unregister_client(struct rdk_client *client)
{
	struct rdk_device_priv *p;

	mutex_enter(&rdk_reg_lock);
	for (p = list_tail(&rdk_devices); p != NULL;
	    p = list_prev(&rdk_devices, p))
		rdk_client_remove_one(client, p->rdp_dev);
	list_remove(&rdk_clients, client);
	mutex_exit(&rdk_reg_lock);
}

/*
 * Asynchronous events.  Handlers run in the provider's thread context and
 * must not register or unregister handlers.
 */
void
rdk_register_event_handler(struct rdk_event_handler *h)
{
	struct rdk_device_priv *p = h->device->rd_priv;

	rw_enter(&p->rdp_ev_lock, RW_WRITER);
	list_insert_tail(&p->rdp_handlers, h);
	rw_exit(&p->rdp_ev_lock);
}

void
rdk_unregister_event_handler(struct rdk_event_handler *h)
{
	struct rdk_device_priv *p = h->device->rd_priv;

	rw_enter(&p->rdp_ev_lock, RW_WRITER);
	list_remove(&p->rdp_handlers, h);
	rw_exit(&p->rdp_ev_lock);
}

void
rdk_dispatch_event(const struct rdk_event *ev)
{
	struct rdk_device_priv *p = ev->device->rd_priv;
	struct rdk_event_handler *h;
	struct rdk_event copy;

	if (p == NULL)
		return;
	rw_enter(&p->rdp_ev_lock, RW_READER);
	for (h = list_head(&p->rdp_handlers); h != NULL;
	    h = list_next(&p->rdp_handlers, h)) {
		copy = *ev;
		h->handler(h, &copy);
	}
	rw_exit(&p->rdp_ev_lock);
}

int
rdk_query_port(struct rdk_device *dev, uint32_t port,
    struct rdk_port_attr *attr)
{
	if (!rdk_port_valid(dev, port))
		return (EINVAL);
	bzero(attr, sizeof (*attr));
	return (dev->rd_ops->query_port(dev, port, attr));
}

/*
 * GIDs.  RoCEv2 carries IPv4 as an IPv4-mapped IPv6 address.
 */
void
rdk_gid_from_ipv4(rdk_gid_t *gid, ipaddr_t ip)
{
	bzero(gid, sizeof (*gid));
	gid->raw[10] = 0xff;
	gid->raw[11] = 0xff;
	bcopy(&ip, &gid->raw[12], sizeof (ip));
}

boolean_t
rdk_gid_to_ipv4(const rdk_gid_t *gid, ipaddr_t *ip)
{
	static const uint8_t prefix[12] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
	};

	if (bcmp(gid->raw, prefix, sizeof (prefix)) != 0)
		return (B_FALSE);
	bcopy(&gid->raw[12], ip, sizeof (*ip));
	return (B_TRUE);
}

static boolean_t
rdk_gid_usable(const rdk_gid_t *gid)
{
	static const rdk_gid_t zero;
	ipaddr_t ip;

	if (bcmp(gid, &zero, sizeof (zero)) == 0)
		return (B_FALSE);
	/* Multicast and unspecified sources are not ours to claim. */
	if (gid->raw[0] == 0xff)
		return (B_FALSE);
	if (rdk_gid_to_ipv4(gid, &ip) &&
	    (ip == INADDR_ANY || (ntohl(ip) >> 28) == 0xe ||
	    ip == htonl(INADDR_BROADCAST)))
		return (B_FALSE);
	return (B_TRUE);
}

/*
 * Add a GID for the port.  An identical entry is reused.  vlan is
 * RDK_VLAN_NONE for untagged traffic; mac is the source MAC for the entry.
 */
int
rdk_add_gid(struct rdk_device *dev, uint32_t port, const rdk_gid_t *gid,
    uint16_t vlan, const uint8_t *mac, uint16_t *indexp)
{
	struct rdk_device_priv *p = dev->rd_priv;
	rdk_gid_ent_t *e, *free = NULL;
	ipaddr_t ip;
	uint_t i;
	int ret;

	if (!rdk_port_valid(dev, port) || !rdk_gid_usable(gid) ||
	    (vlan != RDK_VLAN_NONE && vlan >= 4095) || mac == NULL ||
	    (mac[0] & 0x01) != 0)
		return (EINVAL);

	mutex_enter(&p->rdp_gid_lock);
	mutex_enter(&p->rdp_lock);
	if (p->rdp_dying) {
		mutex_exit(&p->rdp_lock);
		mutex_exit(&p->rdp_gid_lock);
		return (ENXIO);
	}
	for (i = 0; i < RDK_GID_TABLE_LEN; i++) {
		e = &p->rdp_gids[i];
		if (!e->rge_valid) {
			if (free == NULL)
				free = e;
			continue;
		}
		if (e->rge_attr.port_num == port &&
		    bcmp(&e->rge_attr.gid, gid, sizeof (*gid)) == 0 &&
		    e->rge_attr.vlan_id == vlan &&
		    bcmp(e->rge_attr.mac, mac, ETHERADDRL) == 0) {
			e->rge_owners++;
			*indexp = e->rge_attr.index;
			mutex_exit(&p->rdp_lock);
			mutex_exit(&p->rdp_gid_lock);
			return (0);
		}
	}
	mutex_exit(&p->rdp_lock);
	if (free == NULL) {
		mutex_exit(&p->rdp_gid_lock);
		return (ENOSPC);
	}

	e = free;
	bzero(&e->rge_attr, sizeof (e->rge_attr));
	e->rge_attr.device = dev;
	e->rge_attr.gid = *gid;
	e->rge_attr.gid_type = RDK_GID_TYPE_ROCE_UDP_ENCAP;
	e->rge_attr.network_type = rdk_gid_to_ipv4(gid, &ip) ?
	    RDK_NETWORK_IPV4 : RDK_NETWORK_IPV6;
	e->rge_attr.index = (uint16_t)(e - p->rdp_gids);
	e->rge_attr.port_num = port;
	e->rge_attr.vlan_id = vlan;
	bcopy(mac, e->rge_attr.mac, ETHERADDRL);
	ret = dev->rd_ops->add_gid(&e->rge_attr);
	if (ret == 0) {
		mutex_enter(&p->rdp_lock);
		e->rge_refs = 0;
		e->rge_owners = 1;
		e->rge_valid = B_TRUE;
		mutex_exit(&p->rdp_lock);
		*indexp = e->rge_attr.index;
	}
	mutex_exit(&p->rdp_gid_lock);
	return (ret);
}

int
rdk_del_gid(struct rdk_device *dev, uint32_t port, uint16_t index)
{
	struct rdk_device_priv *p = dev->rd_priv;
	rdk_gid_ent_t *e;

	if (!rdk_port_valid(dev, port) || index >= RDK_GID_TABLE_LEN)
		return (EINVAL);

	mutex_enter(&p->rdp_gid_lock);
	e = &p->rdp_gids[index];
	mutex_enter(&p->rdp_lock);
	if (!e->rge_valid || e->rge_attr.port_num != port) {
		mutex_exit(&p->rdp_lock);
		mutex_exit(&p->rdp_gid_lock);
		return (ENOENT);
	}
	if (e->rge_owners > 1) {
		e->rge_owners--;
		mutex_exit(&p->rdp_lock);
		mutex_exit(&p->rdp_gid_lock);
		return (0);
	}
	if (e->rge_refs != 0) {
		mutex_exit(&p->rdp_lock);
		mutex_exit(&p->rdp_gid_lock);
		return (EBUSY);
	}
	e->rge_owners = 0;
	e->rge_valid = B_FALSE;
	mutex_exit(&p->rdp_lock);
	dev->rd_ops->del_gid(&e->rge_attr);
	mutex_exit(&p->rdp_gid_lock);
	return (0);
}

int
rdk_query_gid(struct rdk_device *dev, uint32_t port, uint16_t index,
    rdk_gid_t *gid)
{
	const struct rdk_gid_attr *attr;

	if ((attr = rdk_get_gid_attr(dev, port, index)) == NULL)
		return (ENOENT);
	*gid = attr->gid;
	rdk_put_gid_attr(attr);
	return (0);
}

const struct rdk_gid_attr *
rdk_get_gid_attr(struct rdk_device *dev, uint32_t port, uint16_t index)
{
	struct rdk_device_priv *p = dev->rd_priv;
	rdk_gid_ent_t *e;
	const struct rdk_gid_attr *attr = NULL;

	if (!rdk_port_valid(dev, port) || index >= RDK_GID_TABLE_LEN)
		return (NULL);

	mutex_enter(&p->rdp_lock);
	e = &p->rdp_gids[index];
	if (e->rge_valid && e->rge_attr.port_num == port) {
		e->rge_refs++;
		attr = &e->rge_attr;
	}
	mutex_exit(&p->rdp_lock);
	return (attr);
}

void
rdk_put_gid_attr(const struct rdk_gid_attr *attr)
{
	struct rdk_device_priv *p;
	rdk_gid_ent_t *e;

	if (attr == NULL)
		return;
	p = attr->device->rd_priv;
	e = &p->rdp_gids[attr->index];
	VERIFY3P(&e->rge_attr, ==, attr);
	mutex_enter(&p->rdp_lock);
	VERIFY3U(e->rge_refs, >, 0);
	e->rge_refs--;
	mutex_exit(&p->rdp_lock);
}

/*
 * Fill in the source GID of an address for the provider and check the
 * parts the framework understands.  The caller holds the new reference.
 */
int
rdk_resolve_ah_attr(struct rdk_device *dev, struct rdk_ah_attr *ah)
{
	static const uint8_t zero_mac[ETHERADDRL];
	const struct rdk_gid_attr *sgid;

	if (!rdk_port_valid(dev, ah->port_num) ||
	    ah->type != RDK_AH_ATTR_TYPE_ROCE ||
	    (ah->ah_flags & RDK_AH_GRH) == 0 ||
	    bcmp(ah->roce.dmac, zero_mac, ETHERADDRL) == 0 ||
	    (ah->grh.flow_label & ~0xfffffU) != 0)
		return (EINVAL);

	sgid = rdk_get_gid_attr(dev, ah->port_num, ah->grh.sgid_index);
	if (sgid == NULL)
		return (ENOENT);
	ah->grh.sgid_attr = sgid;
	return (0);
}

int
rdk_dma_buf_alloc(struct rdk_device *dev, size_t len, rdk_dma_buf_t *buf)
{
	int ret;

	bzero(buf, sizeof (*buf));
	if (len == 0)
		return (EINVAL);
	if ((ret = rdk_obj_hold(dev)) != 0)
		return (ret);
	if ((ret = dev->rd_ops->dma_alloc(dev, len, buf)) != 0) {
		rdk_obj_rele(dev);
		return (ret);
	}
	return (0);
}

void
rdk_dma_buf_free(struct rdk_device *dev, rdk_dma_buf_t *buf)
{
	if (buf->rdb_va == NULL)
		return;
	dev->rd_ops->dma_free(dev, buf);
	bzero(buf, sizeof (*buf));
	rdk_obj_rele(dev);
}

/*
 * Module linkage.
 */
static struct modlmisc rdk_modlmisc = {
	.misc_modops = &mod_miscops,
	.misc_linkinfo = "RDMA kernel verbs"
};

static struct modlinkage rdk_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &rdk_modlmisc, NULL }
};

int
_init(void)
{
	int ret;

	mutex_init(&rdk_reg_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&rdk_devices, sizeof (struct rdk_device_priv),
	    offsetof(struct rdk_device_priv, rdp_node));
	list_create(&rdk_clients, sizeof (struct rdk_client),
	    offsetof(struct rdk_client, rc_node));
	if ((ret = rdk_cq_init()) != 0)
		goto fail;
	if ((ret = rdk_cm_init()) != 0) {
		rdk_cq_fini();
		goto fail;
	}
	if ((ret = mod_install(&rdk_modlinkage)) != 0) {
		(void) rdk_cm_fini();
		rdk_cq_fini();
		goto fail;
	}
	return (0);

fail:
	list_destroy(&rdk_clients);
	list_destroy(&rdk_devices);
	mutex_destroy(&rdk_reg_lock);
	return (ret);
}

int
_info(struct modinfo *mi)
{
	return (mod_info(&rdk_modlinkage, mi));
}

int
_fini(void)
{
	int ret;

	mutex_enter(&rdk_reg_lock);
	if (!list_is_empty(&rdk_devices) || list_head(&rdk_clients) !=
	    list_tail(&rdk_clients)) {
		mutex_exit(&rdk_reg_lock);
		return (EBUSY);
	}
	mutex_exit(&rdk_reg_lock);

	if ((ret = rdk_cm_fini()) != 0)
		return (ret);
	if ((ret = mod_remove(&rdk_modlinkage)) != 0) {
		VERIFY0(rdk_cm_init());
		return (ret);
	}
	rdk_cq_fini();
	list_destroy(&rdk_clients);
	list_destroy(&rdk_devices);
	mutex_destroy(&rdk_reg_lock);
	return (0);
}

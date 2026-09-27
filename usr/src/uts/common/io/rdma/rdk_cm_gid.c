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
 * The interfaces of the global zone's IP stack that sit on RDMA device
 * ports, and the RoCEv2 GIDs their addresses give.
 *
 * IP's interface and address events only mark the tables dirty; one task
 * then walks every interface and address and reconciles, so an event lost
 * or reordered costs nothing and the first pass after subscribing is the
 * same code.  Device GID changes, which are firmware commands, run only in
 * that task.
 *
 * An interface is usable when its datalink is a physical link, untagged,
 * and its MAC is a device port's.  VLANs, aggregations, VNICs (over an
 * etherstub or not), IPMP, loopback and tunnels are refused.  Only
 * addresses that are up, not duplicate and belong to the global zone
 * itself (not all-zones ones) give GIDs.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/disp.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/zone.h>
#include <sys/neti.h>
#include <sys/hook.h>
#include <sys/hook_event.h>
#include <sys/dls.h>
#include <sys/dls_mgmt.h>
#include <sys/dlpi.h>
#include <net/if.h>
#include <netinet/in.h>
#include <inet/ip.h>
#include <inet/ip_if.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"

/* How soon a pass runs again when a deletion or an addition must retry. */
uint_t rdk_cm_gid_retry_ms = 1000;

/*
 * rdk_cm_gid_lock is held for a whole pass, device GID changes included;
 * rdk_cm_if_lock is a leaf that covers the interface table for readers.
 * rdk_cm_gid_kick_lock covers only the dirty flag and the task.
 */
static kmutex_t rdk_cm_gid_lock;
static kmutex_t rdk_cm_if_lock;
static kmutex_t rdk_cm_gid_kick_lock;
static kcondvar_t rdk_cm_gid_cv;
static taskq_t *rdk_cm_gid_tq;
static taskq_ent_t rdk_cm_gid_ent;
static boolean_t rdk_cm_gid_dirty;
static boolean_t rdk_cm_gid_running;
static boolean_t rdk_cm_gid_stop;
static uint64_t rdk_cm_gid_passes;
static timeout_id_t rdk_cm_gid_timer;

static rdk_gidtab_t *rdk_cm_gidtab;
static list_t rdk_cm_ifs;		/* rdk_cm_if_lock */

static net_handle_t rdk_cm_neti[2];
static hook_t *rdk_cm_nic_hook[2];

static uint32_t rdk_cm_gid_nospc;	/* rdk_cm_gid_lock */

static int rdk_cm_gid_add(void *, void *, uint32_t, const rdk_gidkey_t *,
    const uint8_t *, uint16_t *);
static int rdk_cm_gid_del(void *, void *, uint32_t, uint16_t);
static void rdk_cm_gid_withdraw(void *, void *, uint32_t, uint16_t,
    boolean_t);

static const rdk_gidtab_ops_t rdk_cm_gidtab_ops = {
	.gto_add = rdk_cm_gid_add,
	.gto_del = rdk_cm_gid_del,
	.gto_withdraw = rdk_cm_gid_withdraw
};

static int
rdk_cm_gid_add(void *arg, void *dev, uint32_t port, const rdk_gidkey_t *key,
    const uint8_t *mac, uint16_t *idx)
{
	rdk_gid_t gid;

	_NOTE(ARGUNUSED(arg));
	bcopy(key->gk_addr, gid.raw, sizeof (gid.raw));
	return (rdk_add_gid(dev, port, &gid, key->gk_vlan, mac, idx));
}

static int
rdk_cm_gid_del(void *arg, void *dev, uint32_t port, uint16_t idx)
{
	_NOTE(ARGUNUSED(arg));
	return (rdk_del_gid(dev, port, idx));
}

/*
 * No new connection may use a withdrawn GID, and the GSI agents drop the
 * cached AHs that hold it, so that the slot can be deleted.
 */
static void
rdk_cm_gid_withdraw(void *arg, void *dev, uint32_t port, uint16_t idx,
    boolean_t on)
{
	_NOTE(ARGUNUSED(arg));
	rdk_gid_withdraw(dev, port, idx, on);
	if (on)
		rdk_cm_roce_gid_withdrawn(dev, port, idx);
}

/*
 * The interface table.
 */
static rdk_cm_if_t *
rdk_cm_if_find_locked(uint_t ifindex)
{
	rdk_cm_if_t *rif;

	ASSERT(MUTEX_HELD(&rdk_cm_if_lock));
	for (rif = list_head(&rdk_cm_ifs); rif != NULL;
	    rif = list_next(&rdk_cm_ifs, rif)) {
		if (rif->rif_ifindex == ifindex)
			return (rif);
	}
	return (NULL);
}

/*
 * What the table knows of an interface: 0 and its MAC and VLAN when it may
 * carry RDMA, or why not.  An interface the table has not seen gets one
 * pass run first.
 */
int
rdk_cm_if_get(uint_t ifindex, uint8_t *mac, uint16_t *vlanp)
{
	rdk_cm_if_t *rif;
	boolean_t synced = B_FALSE;
	int ret;

	for (;;) {
		mutex_enter(&rdk_cm_if_lock);
		if ((rif = rdk_cm_if_find_locked(ifindex)) != NULL) {
			ret = rif->rif_err;
			if (ret == 0) {
				bcopy(rif->rif_mac, mac, ETHERADDRL);
				*vlanp = rif->rif_vlan;
			}
			mutex_exit(&rdk_cm_if_lock);
			return (ret);
		}
		mutex_exit(&rdk_cm_if_lock);
		if (synced)
			return (ENXIO);
		rdk_cm_gid_sync();
		synced = B_TRUE;
	}
}

/*
 * Classify one interface by its datalink.  The dls lookups may call up to
 * dlmgmtd, which is why this runs in the task.
 */
static void
rdk_cm_if_classify(rdk_cm_if_t *rif, ip_stack_t *ipst)
{
	datalink_id_t linkid;
	datalink_class_t class;
	uint32_t media, flags;
	ill_t *ill;

	rif->rif_err = ENXIO;
	rif->rif_vlan = RDK_VLAN_NONE;
	ill = ill_lookup_on_ifindex(rif->rif_ifindex, rif->rif_v6only, ipst);
	if (ill == NULL)
		return;
	if (IS_LOOPBACK(ill) || IS_VNI(ill) || IS_IPMP(ill) ||
	    IS_UNDER_IPMP(ill) || ill->ill_mactype != DL_ETHER ||
	    ill->ill_phys_addr_length != ETHERADDRL ||
	    ill->ill_phys_addr == NULL) {
		rif->rif_err = ENOTSUP;
		ill_refrele(ill);
		return;
	}
	bcopy(ill->ill_phys_addr, rif->rif_mac, ETHERADDRL);
	rif->rif_mtu = ill->ill_mtu;
	ill_refrele(ill);

	if (dls_mgmt_get_linkid_in_zone(rif->rif_name, &linkid,
	    GLOBAL_ZONEID) != 0 ||
	    dls_mgmt_get_linkinfo(linkid, NULL, &class, &media, &flags) != 0) {
		rif->rif_err = EAGAIN;
		return;
	}
	/*
	 * A VLAN link shares the port's MAC, so its class is what tells it
	 * apart.  Its VID would come from dls_devnet_vid(), which dls declares
	 * but does not define; until it does, VLANs are refused.
	 */
	if (class != DATALINK_CLASS_PHYS) {
		rif->rif_err = ENOTSUP;
		return;
	}
	rif->rif_err = 0;
}

/* The device port with this MAC; no hold, rdk_cm_lock is held. */
static rdk_cm_dev_t *
rdk_cm_dev_by_mac_locked(const uint8_t *mac, uint32_t *portp)
{
	rdk_cm_dev_t *cd;
	uint32_t i;

	ASSERT(MUTEX_HELD(&rdk_cm_lock));
	for (cd = list_head(&rdk_cm_devs); cd != NULL;
	    cd = list_next(&rdk_cm_devs, cd)) {
		if (!cd->rcd_added || cd->rcd_removing)
			continue;
		for (i = 0; i < cd->rcd_nports; i++) {
			if (bcmp(cd->rcd_mac[i], mac, ETHERADDRL) == 0) {
				*portp = i + 1;
				return (cd);
			}
		}
	}
	return (NULL);
}

/* Hold the device port whose MAC this is; ENXIO for none. */
int
rdk_cm_dev_by_mac(const uint8_t *mac, rdk_cm_dev_t **cdp, uint32_t *portp)
{
	rdk_cm_dev_t *cd;

	mutex_enter(&rdk_cm_lock);
	if ((cd = rdk_cm_dev_by_mac_locked(mac, portp)) != NULL)
		cd->rcd_ids++;
	mutex_exit(&rdk_cm_lock);
	*cdp = cd;
	return (cd != NULL ? 0 : ENXIO);
}

static rdk_cm_if_t *
rdk_cm_if_seen(list_t *ifs, uint_t ifindex, net_handle_t neti, boolean_t v6)
{
	rdk_cm_if_t *rif;

	for (rif = list_head(ifs); rif != NULL; rif = list_next(ifs, rif)) {
		if (rif->rif_ifindex == ifindex)
			return (rif);
	}
	rif = kmem_zalloc(sizeof (*rif), KM_SLEEP);
	rif->rif_ifindex = ifindex;
	rif->rif_v6only = v6;
	if (net_getifname(neti, ifindex, rif->rif_name,
	    sizeof (rif->rif_name)) != 0)
		rif->rif_name[0] = '\0';
	rif->rif_err = ENXIO;
	list_insert_tail(ifs, rif);
	return (rif);
}

static boolean_t
rdk_cm_lif_usable(net_handle_t neti, phy_if_t phy, lif_if_t lif)
{
	uint64_t flags;
	zoneid_t zid;

	if (net_getlifflags(neti, phy, lif, &flags) != 0 ||
	    (flags & IFF_UP) == 0 ||
	    (flags & (IFF_DUPLICATE | IFF_NOLOCAL | IFF_ANYCAST |
	    IFF_LOOPBACK | IFF_IPMP | IFF_UNNUMBERED)) != 0)
		return (B_FALSE);
	if (net_getlifzone(neti, phy, lif, &zid) != 0 || zid != GLOBAL_ZONEID)
		return (B_FALSE);
	return (B_TRUE);
}

static boolean_t
rdk_cm_addr_gid(boolean_t v6, const struct sockaddr_storage *ss,
    uint8_t *addr)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;
	rdk_gid_t gid;

	if (!v6) {
		if (sin->sin_family != AF_INET ||
		    !rdk_cm_unicast(sin->sin_addr.s_addr))
			return (B_FALSE);
		rdk_gid_from_ipv4(&gid, sin->sin_addr.s_addr);
	} else {
		if (sin6->sin6_family != AF_INET6 ||
		    IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr) ||
		    IN6_IS_ADDR_MULTICAST(&sin6->sin6_addr) ||
		    IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr) ||
		    IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
			return (B_FALSE);
		bcopy(&sin6->sin6_addr, gid.raw, sizeof (gid.raw));
	}
	bcopy(gid.raw, addr, sizeof (gid.raw));
	return (B_TRUE);
}

/* Mark the GIDs of one interface's addresses of one family wanted. */
static void
rdk_cm_gid_want_if(net_handle_t neti, boolean_t v6, const rdk_cm_if_t *rif,
    netstackid_t stack)
{
	net_ifaddr_t type = NA_ADDRESS;
	struct sockaddr_storage ss;
	rdk_cm_dev_t *cd;
	rdk_gidkey_t key;
	uint32_t port;
	lif_if_t lif;

	for (lif = net_lifgetnext(neti, rif->rif_ifindex, 0); lif != 0;
	    lif = net_lifgetnext(neti, rif->rif_ifindex, lif)) {
		if (!rdk_cm_lif_usable(neti, rif->rif_ifindex, lif))
			continue;
		bzero(&ss, sizeof (ss));
		if (net_getlifaddr(neti, rif->rif_ifindex, lif, 1, &type,
		    &ss) != 0)
			continue;
		bzero(&key, sizeof (key));
		if (!rdk_cm_addr_gid(v6, &ss, key.gk_addr))
			continue;
		key.gk_stack = (uint32_t)stack;
		key.gk_ifindex = rif->rif_ifindex;
		key.gk_vlan = rif->rif_vlan;
		key.gk_type = RDK_GID_TYPE_ROCEV2;

		mutex_enter(&rdk_cm_lock);
		cd = rdk_cm_dev_by_mac_locked(rif->rif_mac, &port);
		if (cd != NULL && cd->rcd_roce) {
			(void) rdk_gidtab_want(rdk_cm_gidtab, &key,
			    cd->rcd_dev, port, rif->rif_mac);
		}
		mutex_exit(&rdk_cm_lock);
	}
}

/*
 * One pass.  The walk takes a snapshot through neti; interfaces that come
 * or go meanwhile make another pass.
 */
static void
rdk_cm_gid_pass(void)
{
	rdk_gidtab_stats_t st;
	rdk_cm_if_t *rif;
	netstack_t *ns;
	list_t ifs, gone;
	phy_if_t phy;
	uint_t f;

	if ((ns = netstack_find_by_zoneid(GLOBAL_ZONEID)) == NULL)
		return;
	list_create(&ifs, sizeof (rdk_cm_if_t), offsetof(rdk_cm_if_t,
	    rif_node));
	list_create(&gone, sizeof (rdk_cm_if_t), offsetof(rdk_cm_if_t,
	    rif_node));

	mutex_enter(&rdk_cm_gid_lock);
	for (f = 0; f < 2; f++) {
		if (rdk_cm_neti[f] == NULL)
			continue;
		for (phy = net_phygetnext(rdk_cm_neti[f], 0); phy != 0;
		    phy = net_phygetnext(rdk_cm_neti[f], phy)) {
			rif = rdk_cm_if_seen(&ifs, (uint_t)phy,
			    rdk_cm_neti[f], f == 1);
			if (f == 0)
				rif->rif_v6only = B_FALSE;
		}
	}
	/* The MAC, MTU, link class or IPMP membership may have changed. */
	for (rif = list_head(&ifs); rif != NULL; rif = list_next(&ifs, rif))
		rdk_cm_if_classify(rif, ns->netstack_ip);

	rdk_gidtab_begin(rdk_cm_gidtab);
	for (rif = list_head(&ifs); rif != NULL; rif = list_next(&ifs, rif)) {
		if (rif->rif_err != 0)
			continue;
		for (f = 0; f < 2; f++) {
			if (rdk_cm_neti[f] != NULL) {
				rdk_cm_gid_want_if(rdk_cm_neti[f], f == 1, rif,
				    ns->netstack_stackid);
			}
		}
	}
	rdk_gidtab_end(rdk_cm_gidtab, &st);
	if (rdk_cm_gidtab->gt_dropped != 0) {
		cmn_err(CE_WARN, "!rdk_cm: %u addresses over the %u GID "
		    "entries", rdk_cm_gidtab->gt_dropped, RDK_GIDTAB_MAX);
	}
	if (st.gs_nospc != rdk_cm_gid_nospc && st.gs_nospc != 0) {
		cmn_err(CE_WARN, "!rdk_cm: no GID slot for %u addresses",
		    st.gs_nospc);
	}
	rdk_cm_gid_nospc = st.gs_nospc;

	mutex_enter(&rdk_cm_if_lock);
	list_move_tail(&gone, &rdk_cm_ifs);
	list_move_tail(&rdk_cm_ifs, &ifs);
	rdk_cm_gid_passes++;
	mutex_exit(&rdk_cm_if_lock);
	mutex_exit(&rdk_cm_gid_lock);

	while ((rif = list_remove_head(&gone)) != NULL)
		kmem_free(rif, sizeof (*rif));
	list_destroy(&gone);
	list_destroy(&ifs);
	netstack_rele(ns);
}

static void rdk_cm_gid_kick(void);

static void
rdk_cm_gid_retry(void *arg)
{
	_NOTE(ARGUNUSED(arg));
	mutex_enter(&rdk_cm_gid_kick_lock);
	rdk_cm_gid_timer = 0;
	mutex_exit(&rdk_cm_gid_kick_lock);
	rdk_cm_gid_kick();
}

/* Passes until no event is left; IP's event thread never waits on one. */
static void
rdk_cm_gid_task(void *arg)
{
	boolean_t retry;

	_NOTE(ARGUNUSED(arg));
	mutex_enter(&rdk_cm_gid_kick_lock);
	while (rdk_cm_gid_dirty && !rdk_cm_gid_stop) {
		rdk_cm_gid_dirty = B_FALSE;
		mutex_exit(&rdk_cm_gid_kick_lock);
		rdk_cm_gid_pass();
		mutex_enter(&rdk_cm_gid_lock);
		retry = rdk_gidtab_retry_needed(rdk_cm_gidtab);
		mutex_exit(&rdk_cm_gid_lock);
		mutex_enter(&rdk_cm_gid_kick_lock);
		if (retry && !rdk_cm_gid_stop && rdk_cm_gid_timer == 0) {
			rdk_cm_gid_timer = timeout(rdk_cm_gid_retry, NULL,
			    drv_usectohz((clock_t)rdk_cm_gid_retry_ms *
			    MILLISEC));
		}
	}
	rdk_cm_gid_running = B_FALSE;
	cv_broadcast(&rdk_cm_gid_cv);
	mutex_exit(&rdk_cm_gid_kick_lock);
}

/* Make a pass run soon.  Any context that can take an adaptive mutex. */
static void
rdk_cm_gid_kick(void)
{
	mutex_enter(&rdk_cm_gid_kick_lock);
	rdk_cm_gid_dirty = B_TRUE;
	if (!rdk_cm_gid_running && !rdk_cm_gid_stop) {
		rdk_cm_gid_running = B_TRUE;
		taskq_dispatch_ent(rdk_cm_gid_tq, rdk_cm_gid_task, NULL, 0,
		    &rdk_cm_gid_ent);
	}
	mutex_exit(&rdk_cm_gid_kick_lock);
}

/* Run a pass and wait for it.  Thread context, no CM locks held. */
void
rdk_cm_gid_sync(void)
{
	uint64_t want;

	/* A pass already running may have walked before the change. */
	mutex_enter(&rdk_cm_gid_kick_lock);
	mutex_enter(&rdk_cm_if_lock);
	want = rdk_cm_gid_passes + (rdk_cm_gid_running ? 2 : 1);
	mutex_exit(&rdk_cm_if_lock);
	mutex_exit(&rdk_cm_gid_kick_lock);
	rdk_cm_gid_kick();
	mutex_enter(&rdk_cm_gid_kick_lock);
	for (;;) {
		mutex_enter(&rdk_cm_if_lock);
		if (rdk_cm_gid_passes >= want) {
			mutex_exit(&rdk_cm_if_lock);
			break;
		}
		mutex_exit(&rdk_cm_if_lock);
		if (!rdk_cm_gid_running || rdk_cm_gid_stop)
			break;
		cv_wait(&rdk_cm_gid_cv, &rdk_cm_gid_kick_lock);
	}
	mutex_exit(&rdk_cm_gid_kick_lock);
}

void
rdk_cm_gid_dev_added(void)
{
	rdk_cm_gid_kick();
}

/* The device is going: take its GIDs out before it is unregistered. */
void
rdk_cm_gid_dev_removed(struct rdk_device *dev)
{
	mutex_enter(&rdk_cm_gid_lock);
	rdk_gidtab_forget_dev(rdk_cm_gidtab, dev, B_TRUE);
	mutex_exit(&rdk_cm_gid_lock);
}

/* IP reports interface and address changes in its event taskq. */
static int
rdk_cm_nic_event(hook_event_token_t tok, hook_data_t data, void *arg)
{
	hook_nic_event_t *ne = (hook_nic_event_t *)data;

	_NOTE(ARGUNUSED(tok, arg));
	switch (ne->hne_event) {
	case NE_ADDRESS_CHANGE:
	case NE_LIF_DOWN:
	case NE_DOWN:
	case NE_UNPLUMB:
		rdk_cm_addr_change((uint_t)ne->hne_nic);
		rdk_cm_gid_kick();
		break;
	case NE_PLUMB:
	case NE_UP:
	case NE_LIF_UP:
	case NE_IFINDEX_CHANGE:
		rdk_cm_gid_kick();
		break;
	default:
		break;
	}
	return (0);
}

int
rdk_cm_gid_init(void)
{
	static const char *const fam[2] = { NHF_INET, NHF_INET6 };
	uint_t f;

	mutex_init(&rdk_cm_gid_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&rdk_cm_if_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&rdk_cm_gid_kick_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&rdk_cm_gid_cv, NULL, CV_DRIVER, NULL);
	list_create(&rdk_cm_ifs, sizeof (rdk_cm_if_t),
	    offsetof(rdk_cm_if_t, rif_node));
	rdk_cm_gidtab = kmem_zalloc(sizeof (*rdk_cm_gidtab), KM_SLEEP);
	rdk_gidtab_init(rdk_cm_gidtab, &rdk_cm_gidtab_ops, NULL);
	rdk_cm_gid_stop = B_FALSE;
	rdk_cm_gid_tq = taskq_create("rdk_cm_gid", 1, minclsyspri, 1, 1,
	    TASKQ_PREPOPULATE);

	for (f = 0; f < 2; f++) {
		rdk_cm_neti[f] = net_protocol_lookup(
		    net_zoneidtonetid(GLOBAL_ZONEID), fam[f]);
		if (rdk_cm_neti[f] == NULL)
			continue;
		HOOK_INIT(rdk_cm_nic_hook[f], rdk_cm_nic_event,
		    f == 0 ? "rdmak_cm" : "rdmak_cm6", NULL);
		if (net_hook_register(rdk_cm_neti[f], NH_NIC_EVENTS,
		    rdk_cm_nic_hook[f]) != 0) {
			hook_free(rdk_cm_nic_hook[f]);
			rdk_cm_nic_hook[f] = NULL;
		}
	}
	rdk_cm_gid_kick();
	return (0);
}

/* The devices are gone, so the table holds nothing installed. */
void
rdk_cm_gid_fini(void)
{
	rdk_cm_if_t *rif;
	timeout_id_t tid;
	uint_t f;

	for (f = 0; f < 2; f++) {
		if (rdk_cm_nic_hook[f] != NULL) {
			(void) net_hook_unregister(rdk_cm_neti[f],
			    NH_NIC_EVENTS, rdk_cm_nic_hook[f]);
			hook_free(rdk_cm_nic_hook[f]);
			rdk_cm_nic_hook[f] = NULL;
		}
	}
	mutex_enter(&rdk_cm_gid_kick_lock);
	rdk_cm_gid_stop = B_TRUE;
	tid = rdk_cm_gid_timer;
	rdk_cm_gid_timer = 0;
	mutex_exit(&rdk_cm_gid_kick_lock);
	if (tid != 0)
		(void) untimeout(tid);
	taskq_destroy(rdk_cm_gid_tq);
	for (f = 0; f < 2; f++) {
		if (rdk_cm_neti[f] != NULL) {
			(void) net_protocol_release(rdk_cm_neti[f]);
			rdk_cm_neti[f] = NULL;
		}
	}
	VERIFY0(rdk_cm_gidtab->gt_n);
	kmem_free(rdk_cm_gidtab, sizeof (*rdk_cm_gidtab));
	while ((rif = list_remove_head(&rdk_cm_ifs)) != NULL)
		kmem_free(rif, sizeof (*rif));
	list_destroy(&rdk_cm_ifs);
	cv_destroy(&rdk_cm_gid_cv);
	mutex_destroy(&rdk_cm_gid_kick_lock);
	mutex_destroy(&rdk_cm_if_lock);
	mutex_destroy(&rdk_cm_gid_lock);
}

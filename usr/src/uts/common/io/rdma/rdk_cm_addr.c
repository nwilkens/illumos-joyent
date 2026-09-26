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
 * The connection manager's view of the IP stack: exclusive TCP port
 * reservations, routes and next hops, and address changes.
 *
 * An offloading RNIC takes every segment of the ports it serves before the
 * host sees them, so a port it uses must belong to nobody else in the host
 * stack.  Each reservation is a kernel TCP socket in the ID's netstack,
 * bound with SO_EXCLBIND and without SO_REUSEADDR: while it exists no other
 * socket can bind the port on that address or the wildcard, and a port the
 * host already uses cannot be reserved.  The socket never listens, so a
 * segment for the port that reaches the host gets a reset.  After the chip
 * has used the port the socket stays for the TIME_WAIT interval once the
 * last connection is gone.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <sys/ksocket.h>
#include <sys/zone.h>
#include <sys/neti.h>
#include <sys/hook.h>
#include <sys/hook_event.h>
#include <netinet/in.h>
#include <net/if_dl.h>
#include <inet/ip.h>
#include <inet/ip_if.h>
#include <inet/ip_ire.h>
#include <inet/ip2mac.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"

uint_t rdk_cm_timewait_ms = 60000;

static kmutex_t rdk_cm_resv_lock;
static list_t rdk_cm_tw;		/* reservations in TIME_WAIT */
static timeout_id_t rdk_cm_tw_timer;
static boolean_t rdk_cm_tw_stop;
static taskq_ent_t rdk_cm_tw_ent;
static boolean_t rdk_cm_tw_busy;
static volatile uint_t rdk_cm_ip2mac_out;

static net_handle_t rdk_cm_neti;
static hook_t *rdk_cm_nic_hook;

boolean_t
rdk_cm_unicast(ipaddr_t a)
{
	const uint32_t h = ntohl(a);

	return (a != INADDR_ANY && a != INADDR_BROADCAST && !CLASSD(a) &&
	    (h >> 24) != IN_LOOPBACKNET && (h >> 24) != 0);
}

static void
rdk_cm_resv_close(rdk_cm_resv_t *rr)
{
	(void) ksocket_close(rr->rr_ks, kcred);
	kmem_free(rr, sizeof (*rr));
}

int
rdk_cm_resv_get(cred_t *cr, const struct sockaddr_in *want,
    rdk_cm_resv_t **rrp)
{
	struct sockaddr_in sin, got;
	socklen_t len = sizeof (got);
	rdk_cm_resv_t *rr;
	ksocket_t ks;
	int one = 1, ret;

	*rrp = NULL;
	if (!rdk_cm_unicast(want->sin_addr.s_addr))
		return (EADDRNOTAVAIL);
	if ((ret = ksocket_socket(&ks, AF_INET, SOCK_STREAM, IPPROTO_TCP,
	    KSOCKET_SLEEP, cr)) != 0)
		return (ret);
	if ((ret = ksocket_setsockopt(ks, SOL_SOCKET, SO_EXCLBIND, &one,
	    sizeof (one), cr)) != 0) {
		(void) ksocket_close(ks, cr);
		return (ret);
	}
	bzero(&sin, sizeof (sin));
	sin.sin_family = AF_INET;
	sin.sin_addr = want->sin_addr;
	sin.sin_port = want->sin_port;
	if ((ret = ksocket_bind(ks, (struct sockaddr *)&sin, sizeof (sin),
	    cr)) != 0) {
		(void) ksocket_close(ks, cr);
		return (ret);
	}
	bzero(&got, sizeof (got));
	if ((ret = ksocket_getsockname(ks, (struct sockaddr *)&got, &len,
	    cr)) != 0 || got.sin_family != AF_INET ||
	    got.sin_addr.s_addr != sin.sin_addr.s_addr || got.sin_port == 0 ||
	    (sin.sin_port != 0 && got.sin_port != sin.sin_port)) {
		(void) ksocket_close(ks, cr);
		return (ret != 0 ? ret : EADDRNOTAVAIL);
	}
	rr = kmem_zalloc(sizeof (*rr), KM_SLEEP);
	rr->rr_ks = ks;
	rr->rr_addr = got;
	rr->rr_refs = 1;
	*rrp = rr;
	return (0);
}

void
rdk_cm_resv_hold(rdk_cm_resv_t *rr)
{
	mutex_enter(&rdk_cm_resv_lock);
	VERIFY3U(rr->rr_refs, >, 0);
	rr->rr_refs++;
	mutex_exit(&rdk_cm_resv_lock);
}

/* The chip is about to be told of the port. */
void
rdk_cm_resv_used(rdk_cm_resv_t *rr)
{
	mutex_enter(&rdk_cm_resv_lock);
	rr->rr_used = B_TRUE;
	mutex_exit(&rdk_cm_resv_lock);
}

static void rdk_cm_tw_arm(void);

static void
rdk_cm_tw_task(void *arg)
{
	rdk_cm_resv_t *rr;
	list_t done;
	hrtime_t now = gethrtime();

	_NOTE(ARGUNUSED(arg));
	list_create(&done, sizeof (rdk_cm_resv_t),
	    offsetof(rdk_cm_resv_t, rr_node));
	mutex_enter(&rdk_cm_resv_lock);
	while ((rr = list_head(&rdk_cm_tw)) != NULL &&
	    (rr->rr_expire <= now || rdk_cm_tw_stop)) {
		list_remove(&rdk_cm_tw, rr);
		list_insert_tail(&done, rr);
	}
	rdk_cm_tw_busy = B_FALSE;
	if (!list_is_empty(&rdk_cm_tw))
		rdk_cm_tw_arm();
	mutex_exit(&rdk_cm_resv_lock);
	while ((rr = list_remove_head(&done)) != NULL)
		rdk_cm_resv_close(rr);
	list_destroy(&done);
}

static void
rdk_cm_tw_fire(void *arg)
{
	_NOTE(ARGUNUSED(arg));
	mutex_enter(&rdk_cm_resv_lock);
	rdk_cm_tw_timer = 0;
	if (!rdk_cm_tw_busy) {
		rdk_cm_tw_busy = B_TRUE;
		taskq_dispatch_ent(rdk_cm_taskq, rdk_cm_tw_task, NULL, 0,
		    &rdk_cm_tw_ent);
	}
	mutex_exit(&rdk_cm_resv_lock);
}

/* Fire when the oldest reservation expires.  rdk_cm_resv_lock is held. */
static void
rdk_cm_tw_arm(void)
{
	rdk_cm_resv_t *rr = list_head(&rdk_cm_tw);
	hrtime_t left;

	ASSERT(MUTEX_HELD(&rdk_cm_resv_lock));
	if (rr == NULL || rdk_cm_tw_timer != 0 || rdk_cm_tw_busy)
		return;
	left = MAX(rr->rr_expire - gethrtime(), MSEC2NSEC(10));
	rdk_cm_tw_timer = timeout(rdk_cm_tw_fire, NULL,
	    drv_usectohz(NSEC2USEC(left)));
}

/*
 * Drop a reference.  The last one closes the socket now if the chip never
 * used the port, and after the TIME_WAIT interval otherwise.  Thread
 * context.
 */
void
rdk_cm_resv_rele(rdk_cm_resv_t *rr)
{
	boolean_t now;

	mutex_enter(&rdk_cm_resv_lock);
	VERIFY3U(rr->rr_refs, >, 0);
	if (--rr->rr_refs != 0) {
		mutex_exit(&rdk_cm_resv_lock);
		return;
	}
	now = !rr->rr_used || rdk_cm_timewait_ms == 0;
	if (!now) {
		rr->rr_expire = gethrtime() + MSEC2NSEC(rdk_cm_timewait_ms);
		list_insert_tail(&rdk_cm_tw, rr);
		rdk_cm_tw_arm();
	}
	mutex_exit(&rdk_cm_resv_lock);
	if (now)
		rdk_cm_resv_close(rr);
}

/*
 * The device port whose MAC is the interface's, with a hold on the device.
 * Untagged interfaces of the port only.
 */
static int
rdk_cm_dev_by_ill(const ill_t *ill, rdk_cm_dev_t **cdp, uint32_t *portp)
{
	rdk_cm_dev_t *cd;
	uint32_t i;

	if (ill->ill_phys_addr_length != ETHERADDRL ||
	    ill->ill_phys_addr == NULL)
		return (EADDRNOTAVAIL);
	mutex_enter(&rdk_cm_lock);
	for (cd = list_head(&rdk_cm_devs); cd != NULL;
	    cd = list_next(&rdk_cm_devs, cd)) {
		if (!cd->rcd_added || cd->rcd_removing || cd->rcd_iw == NULL)
			continue;
		for (i = 0; i < cd->rcd_nports; i++) {
			if (bcmp(cd->rcd_mac[i], ill->ill_phys_addr,
			    ETHERADDRL) == 0) {
				cd->rcd_ids++;
				mutex_exit(&rdk_cm_lock);
				*cdp = cd;
				*portp = i + 1;
				return (0);
			}
		}
	}
	mutex_exit(&rdk_cm_lock);
	return (EADDRNOTAVAIL);
}

static boolean_t
rdk_cm_ill_ok(const ill_t *ill)
{
	return (!ill->ill_isv6 && !IS_LOOPBACK(ill) && !IS_VNI(ill) &&
	    !IS_IPMP(ill) && !IS_UNDER_IPMP(ill));
}

static void
rdk_cm_route_fill(const ill_t *ill, ipaddr_t src, uint32_t port,
    struct rdk_cm_route *rt)
{
	rt->rcr_src.sin_family = AF_INET;
	rt->rcr_src.sin_addr.s_addr = src;
	rt->rcr_port = port;
	bcopy(ill->ill_phys_addr, rt->rcr_smac, ETHERADDRL);
	rt->rcr_vlan = RDK_VLAN_NONE;
	rt->rcr_mtu = ill->ill_mtu;
}

/* The device of an address the host has up on one of its interfaces. */
int
rdk_cm_local_dev(cred_t *cr, ipaddr_t addr, rdk_cm_dev_t **cdp,
    uint32_t *portp, uint_t *ifindexp, struct rdk_cm_route *rt)
{
	netstack_t *ns;
	ip_stack_t *ipst;
	ipif_t *ipif;
	ill_t *ill;
	int ret;

	if ((ns = netstack_find_by_cred(cr)) == NULL)
		return (ENXIO);
	ipst = ns->netstack_ip;
	ipif = ipif_lookup_addr_nondup(addr, NULL, crgetzoneid(cr), ipst);
	if (ipif == NULL) {
		netstack_rele(ns);
		return (EADDRNOTAVAIL);
	}
	ill = ipif->ipif_ill;
	if (!rdk_cm_ill_ok(ill)) {
		ret = EADDRNOTAVAIL;
	} else if ((ret = rdk_cm_dev_by_ill(ill, cdp, portp)) == 0) {
		*ifindexp = ill->ill_phyint->phyint_ifindex;
		rdk_cm_route_fill(ill, addr, *portp, rt);
	}
	ipif_refrele(ipif);
	netstack_rele(ns);
	return (ret);
}

/*
 * The egress interface, device and next hop toward dst.  *srcp in:
 * INADDR_ANY to pick the source, or an address up on the egress interface.
 */
int
rdk_cm_route_lookup(cred_t *cr, ipaddr_t dst, ipaddr_t *srcp,
    rdk_cm_dev_t **cdp, uint32_t *portp, uint_t *ifindexp,
    struct rdk_cm_route *rt, ipaddr_t *nhp)
{
	const zoneid_t zoneid = crgetzoneid(cr);
	ipaddr_t setsrc = INADDR_ANY, src;
	netstack_t *ns;
	ip_stack_t *ipst;
	ire_t *ire;
	ill_t *ill = NULL;
	ipif_t *ipif;
	int ret = 0;

	if ((ns = netstack_find_by_cred(cr)) == NULL)
		return (ENXIO);
	ipst = ns->netstack_ip;
	ire = ire_route_recursive_v4(dst, 0, NULL, zoneid, NULL,
	    MATCH_IRE_DSTONLY, IRR_ALLOCATE, 0, ipst, &setsrc, NULL, NULL);
	if ((ire->ire_flags & (RTF_REJECT | RTF_BLACKHOLE)) != 0 ||
	    (ire->ire_type & (IRE_LOCAL | IRE_LOOPBACK | IRE_BROADCAST |
	    IRE_MULTICAST)) != 0 || (ill = ire_nexthop_ill(ire)) == NULL) {
		ret = ENETUNREACH;
		goto out;
	}
	if (!rdk_cm_ill_ok(ill)) {
		ret = ENETUNREACH;
		goto out;
	}
	src = *srcp;
	if (src == INADDR_ANY) {
		if (ip_select_source_v4(ill, setsrc, dst, INADDR_ANY, zoneid,
		    ipst, &src, NULL, NULL) != 0 || !rdk_cm_unicast(src)) {
			ret = EADDRNOTAVAIL;
			goto out;
		}
	} else {
		ipif = ipif_lookup_addr_nondup(src, ill, zoneid, ipst);
		if (ipif == NULL || ipif->ipif_ill != ill) {
			if (ipif != NULL)
				ipif_refrele(ipif);
			ret = EADDRNOTAVAIL;
			goto out;
		}
		ipif_refrele(ipif);
	}
	if ((ret = rdk_cm_dev_by_ill(ill, cdp, portp)) != 0)
		goto out;
	*srcp = src;
	*ifindexp = ill->ill_phyint->phyint_ifindex;
	*nhp = (ire->ire_type & IRE_OFFLINK) != 0 ? ire->ire_gateway_addr :
	    dst;
	rdk_cm_route_fill(ill, src, *portp, rt);
	if (ire->ire_metrics.iulp_mtu != 0)
		rt->rcr_mtu = MIN(rt->rcr_mtu, ire->ire_metrics.iulp_mtu);
out:
	if (ill != NULL)
		ill_refrele(ill);
	ire_refrele(ire);
	netstack_rele(ns);
	return (ret);
}

static void
rdk_cm_rs_rele(rdk_cm_resolve_t *rs)
{
	rdk_cm_id_t *id;
	boolean_t last;

	mutex_enter(&rs->rs_lock);
	VERIFY3U(rs->rs_refs, >, 0);
	last = --rs->rs_refs == 0;
	id = rs->rs_id;
	mutex_exit(&rs->rs_lock);
	if (!last)
		return;
	mutex_destroy(&rs->rs_lock);
	kmem_free(rs, sizeof (*rs));
	if (id != NULL)
		rdk_cm_rele(id);
}

static void
rdk_cm_rs_task(void *arg)
{
	rdk_cm_resolve_t *rs = arg;
	rdk_cm_id_t *id;
	uint32_t gen;
	int err;

	mutex_enter(&rs->rs_lock);
	id = rs->rs_id;
	gen = rs->rs_gen;
	err = rs->rs_err;
	if (id != NULL)
		rdk_cm_hold(id);
	mutex_exit(&rs->rs_lock);
	if (id != NULL) {
		rdk_cm_route_done(id, gen, err, rs->rs_mac);
		rdk_cm_rele(id);
	}
	rdk_cm_rs_rele(rs);
	atomic_dec_uint(&rdk_cm_ip2mac_out);
}

static int
rdk_cm_ip2mac_result(const ip2mac_t *ip2m, uint8_t *mac)
{
	const struct sockaddr_dl *sdl = &ip2m->ip2mac_ha;

	if (ip2m->ip2mac_err != 0)
		return (EHOSTUNREACH);
	if (sdl->sdl_alen != ETHERADDRL || (LLADDR(sdl)[0] & 0x01) != 0)
		return (EHOSTUNREACH);
	bcopy(LLADDR(sdl), mac, ETHERADDRL);
	return (0);
}

/* IP's resolver answered; its context is unknown, so defer to a task. */
static void
rdk_cm_ip2mac_cb(ip2mac_t *ip2m, void *arg)
{
	rdk_cm_resolve_t *rs = arg;

	mutex_enter(&rs->rs_lock);
	rs->rs_done = B_TRUE;
	rs->rs_err = rdk_cm_ip2mac_result(ip2m, rs->rs_mac);
	mutex_exit(&rs->rs_lock);
	taskq_dispatch_ent(rdk_cm_taskq, rdk_cm_rs_task, rs, 0,
	    &rs->rs_tqent);
}

/*
 * Resolve the next hop's MAC for a route operation of generation gen.
 * The answer ends the operation through rdk_cm_route_done().
 */
int
rdk_cm_nexthop(rdk_cm_id_t *id, ipaddr_t nh, uint32_t gen)
{
	rdk_cm_resolve_t *rs;
	ip2mac_t ip2m;
	struct sockaddr_in *sin;
	ip2mac_id_t mid;
	uint8_t mac[ETHERADDRL];
	int err;

	rs = kmem_zalloc(sizeof (*rs), KM_SLEEP);
	mutex_init(&rs->rs_lock, NULL, MUTEX_DRIVER, NULL);
	rs->rs_refs = 2;	/* this call and the callback */
	rs->rs_gen = gen;
	rdk_cm_hold(id);
	rs->rs_id = id;

	bzero(&ip2m, sizeof (ip2m));
	sin = (struct sockaddr_in *)&ip2m.ip2mac_pa;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = nh;
	mutex_enter(&id->rci_lock);
	ip2m.ip2mac_ifindex = id->rci_ifindex;
	id->rci_resolve = rs;
	rs->rs_refs++;		/* rci_resolve */
	mutex_exit(&id->rci_lock);

	atomic_inc_uint(&rdk_cm_ip2mac_out);
	mid = ip2mac(IP2MAC_RESOLVE, &ip2m, rdk_cm_ip2mac_cb, rs,
	    crgetzoneid(id->rci_cred));
	if (ip2m.ip2mac_err == EINPROGRESS && mid != NULL) {
		mutex_enter(&rs->rs_lock);
		rs->rs_ip2mac = mid;
		mutex_exit(&rs->rs_lock);
		rdk_cm_rs_rele(rs);
		return (0);
	}

	/* Answered at once: the callback will not run. */
	atomic_dec_uint(&rdk_cm_ip2mac_out);
	mutex_enter(&rs->rs_lock);
	rs->rs_done = B_TRUE;
	mutex_exit(&rs->rs_lock);
	rdk_cm_rs_rele(rs);
	err = rdk_cm_ip2mac_result(&ip2m, mac);
	rdk_cm_route_done(id, gen, err, mac);
	rdk_cm_resolve_cancel(id);
	rdk_cm_rs_rele(rs);
	return (0);
}

/* Stop waiting for the resolver; a late answer is dropped. */
void
rdk_cm_resolve_cancel(rdk_cm_id_t *id)
{
	rdk_cm_resolve_t *rs;
	ip2mac_id_t mid = NULL;
	rdk_cm_id_t *held;
	boolean_t cb_ref = B_FALSE;

	mutex_enter(&id->rci_lock);
	rs = id->rci_resolve;
	id->rci_resolve = NULL;
	mutex_exit(&id->rci_lock);
	if (rs == NULL)
		return;

	mutex_enter(&rs->rs_lock);
	held = rs->rs_id;
	rs->rs_id = NULL;
	if (!rs->rs_done)
		mid = rs->rs_ip2mac;
	mutex_exit(&rs->rs_lock);
	if (held != NULL)
		rdk_cm_rele(held);

	if (mid != NULL && ip2mac_cancel(mid, crgetzoneid(id->rci_cred)) == 0) {
		mutex_enter(&rs->rs_lock);
		if (!rs->rs_done) {
			rs->rs_done = B_TRUE;
			cb_ref = B_TRUE;
		}
		mutex_exit(&rs->rs_lock);
	}
	if (cb_ref) {
		atomic_dec_uint(&rdk_cm_ip2mac_out);
		rdk_cm_rs_rele(rs);
	}
	rdk_cm_rs_rele(rs);
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
		break;
	default:
		break;
	}
	return (0);
}

int
rdk_cm_addr_init(void)
{
	mutex_init(&rdk_cm_resv_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&rdk_cm_tw, sizeof (rdk_cm_resv_t),
	    offsetof(rdk_cm_resv_t, rr_node));

	rdk_cm_neti = net_protocol_lookup(net_zoneidtonetid(GLOBAL_ZONEID),
	    NHF_INET);
	if (rdk_cm_neti != NULL) {
		HOOK_INIT(rdk_cm_nic_hook, rdk_cm_nic_event, "rdmak_cm",
		    NULL);
		if (net_hook_register(rdk_cm_neti, NH_NIC_EVENTS,
		    rdk_cm_nic_hook) != 0) {
			hook_free(rdk_cm_nic_hook);
			rdk_cm_nic_hook = NULL;
		}
	}
	return (0);
}

/* EBUSY while a reservation waits out TIME_WAIT or IP may call back. */
int
rdk_cm_addr_fini(void)
{
	mutex_enter(&rdk_cm_resv_lock);
	if (!list_is_empty(&rdk_cm_tw) || rdk_cm_tw_busy ||
	    rdk_cm_ip2mac_out != 0) {
		mutex_exit(&rdk_cm_resv_lock);
		return (EBUSY);
	}
	rdk_cm_tw_stop = B_TRUE;
	mutex_exit(&rdk_cm_resv_lock);
	if (rdk_cm_tw_timer != 0)
		(void) untimeout(rdk_cm_tw_timer);

	if (rdk_cm_nic_hook != NULL) {
		(void) net_hook_unregister(rdk_cm_neti, NH_NIC_EVENTS,
		    rdk_cm_nic_hook);
		hook_free(rdk_cm_nic_hook);
		rdk_cm_nic_hook = NULL;
	}
	if (rdk_cm_neti != NULL) {
		(void) net_protocol_release(rdk_cm_neti);
		rdk_cm_neti = NULL;
	}
	list_destroy(&rdk_cm_tw);
	mutex_destroy(&rdk_cm_resv_lock);
	return (0);
}

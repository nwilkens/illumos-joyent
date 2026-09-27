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
 * The connection manager's view of the IP stack: port reservations, routes
 * and next hops.
 *
 * An offloading RNIC takes every segment of the ports it serves before the
 * host sees them, so a port it uses must belong to nobody else in the host
 * stack.  Each iWARP reservation is a kernel TCP socket in the ID's
 * netstack, bound with SO_EXCLBIND and without SO_REUSEADDR: while it
 * exists no other socket can bind the port on that address or the
 * wildcard, and a port the host already uses cannot be reserved.  The
 * socket never listens, so a segment for the port that reaches the host
 * gets a reset.  After the chip has used the port the socket stays for the
 * TIME_WAIT interval once the last connection is gone.
 *
 * RoCE ports are only numbers in the IB CM service ID, which the host TCP
 * stack never sees, so RoCE has its own port space, as the Linux RDMA CM
 * does: an (address, port) pair belongs to one ID and the children of its
 * listener.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <sys/ksocket.h>
#include <sys/zone.h>
#include <sys/random.h>
#include <netinet/in.h>
#include <net/if_dl.h>
#include <inet/ip.h>
#include <inet/ip_if.h>
#include <inet/ip_ire.h>
#include <inet/ip2mac.h>
#include <inet/tunables.h>

#include "rdk_impl.h"
#include "rdk_cm_impl.h"

uint_t rdk_cm_timewait_ms = 60000;

/* The ephemeral range of the RoCE port space (Linux's ip_local_port_range). */
uint_t rdk_cm_roce_port_lo = 32768;
uint_t rdk_cm_roce_port_hi = 60999;

static kmutex_t rdk_cm_resv_lock;
static list_t rdk_cm_tw;		/* reservations in TIME_WAIT */
static avl_tree_t rdk_cm_roce_ports;
static timeout_id_t rdk_cm_tw_timer;
static boolean_t rdk_cm_tw_stop;
static taskq_ent_t rdk_cm_tw_ent;
static boolean_t rdk_cm_tw_busy;
static volatile uint_t rdk_cm_ip2mac_out;

boolean_t
rdk_cm_unicast(ipaddr_t a)
{
	const uint32_t h = ntohl(a);

	return (a != INADDR_ANY && a != INADDR_BROADCAST && !CLASSD(a) &&
	    (h >> 24) != IN_LOOPBACKNET && (h >> 24) != 0 &&
	    (h >> 28) != 0xf);
}

static int
rdk_cm_port_cmp(const void *a, const void *b)
{
	const rdk_cm_resv_t *ra = a, *rb = b;
	uint32_t x = ntohl(ra->rr_addr.sin_addr.s_addr);
	uint32_t y = ntohl(rb->rr_addr.sin_addr.s_addr);

	if (x != y)
		return (x < y ? -1 : 1);
	x = ntohs(ra->rr_addr.sin_port);
	y = ntohs(rb->rr_addr.sin_port);
	if (x != y)
		return (x < y ? -1 : 1);
	return (0);
}

static void
rdk_cm_resv_close(rdk_cm_resv_t *rr)
{
	(void) ksocket_close(rr->rr_ks, kcred);
	kmem_free(rr, sizeof (*rr));
}

static int
rdk_cm_tcp_resv_get(cred_t *cr, const struct sockaddr_in *want,
    rdk_cm_resv_t **rrp)
{
	struct sockaddr_in sin, got;
	socklen_t len = sizeof (got);
	rdk_cm_resv_t *rr;
	ksocket_t ks;
	int one = 1, ret;

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

/* A port of the RoCE space: the one asked for, or a free ephemeral one. */
static int
rdk_cm_roce_resv_get(const struct sockaddr_in *want, rdk_cm_resv_t **rrp)
{
	rdk_cm_resv_t *rr;
	uint32_t lo = rdk_cm_roce_port_lo, hi = rdk_cm_roce_port_hi;
	uint32_t span, start, i;
	avl_index_t where;

	rr = kmem_zalloc(sizeof (*rr), KM_SLEEP);
	rr->rr_addr.sin_family = AF_INET;
	rr->rr_addr.sin_addr = want->sin_addr;
	rr->rr_refs = 1;

	mutex_enter(&rdk_cm_resv_lock);
	if (want->sin_port != 0) {
		rr->rr_addr.sin_port = want->sin_port;
		if (avl_find(&rdk_cm_roce_ports, rr, &where) != NULL) {
			mutex_exit(&rdk_cm_resv_lock);
			kmem_free(rr, sizeof (*rr));
			return (EADDRINUSE);
		}
		avl_insert(&rdk_cm_roce_ports, rr, where);
		mutex_exit(&rdk_cm_resv_lock);
		*rrp = rr;
		return (0);
	}
	if (lo == 0 || hi > UINT16_MAX || lo > hi) {
		lo = 32768;
		hi = 60999;
	}
	span = hi - lo + 1;
	(void) random_get_pseudo_bytes((uint8_t *)&start, sizeof (start));
	for (i = 0; i < span; i++) {
		rr->rr_addr.sin_port = htons((uint16_t)(lo + (start + i) %
		    span));
		if (avl_find(&rdk_cm_roce_ports, rr, &where) == NULL) {
			avl_insert(&rdk_cm_roce_ports, rr, where);
			mutex_exit(&rdk_cm_resv_lock);
			*rrp = rr;
			return (0);
		}
	}
	mutex_exit(&rdk_cm_resv_lock);
	kmem_free(rr, sizeof (*rr));
	return (EADDRNOTAVAIL);
}

int
rdk_cm_resv_get(rdk_cm_dev_t *cd, cred_t *cr, const struct sockaddr_in *want,
    rdk_cm_resv_t **rrp)
{
	*rrp = NULL;
	if (!rdk_cm_unicast(want->sin_addr.s_addr))
		return (EADDRNOTAVAIL);
	if (cd->rcd_roce)
		return (rdk_cm_roce_resv_get(want, rrp));
	return (rdk_cm_tcp_resv_get(cr, want, rrp));
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

/* Make the ID the RoCE listener of its reservation. */
int
rdk_cm_resv_listen(rdk_cm_resv_t *rr, rdk_cm_id_t *id)
{
	int ret = 0;

	mutex_enter(&rdk_cm_resv_lock);
	if (rr->rr_ks != NULL || rr->rr_listener != NULL)
		ret = EADDRINUSE;
	else
		rr->rr_listener = id;
	mutex_exit(&rdk_cm_resv_lock);
	if (ret == 0)
		rdk_cm_hold(id);
	return (ret);
}

void
rdk_cm_resv_unlisten(rdk_cm_resv_t *rr, rdk_cm_id_t *id)
{
	boolean_t mine;

	mutex_enter(&rdk_cm_resv_lock);
	mine = rr->rr_listener == id;
	if (mine)
		rr->rr_listener = NULL;
	mutex_exit(&rdk_cm_resv_lock);
	if (mine)
		rdk_cm_rele(id);
}

/* The listener for a RoCE request to (addr, port), held. */
rdk_cm_id_t *
rdk_cm_roce_listener(ipaddr_t addr, uint16_t port)
{
	rdk_cm_resv_t key, *rr;
	rdk_cm_id_t *id = NULL;

	bzero(&key, sizeof (key));
	key.rr_addr.sin_addr.s_addr = addr;
	key.rr_addr.sin_port = port;
	mutex_enter(&rdk_cm_resv_lock);
	if ((rr = avl_find(&rdk_cm_roce_ports, &key, NULL)) != NULL &&
	    rr->rr_listener != NULL) {
		id = rr->rr_listener;
		rdk_cm_hold(id);
	}
	mutex_exit(&rdk_cm_resv_lock);
	return (id);
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
 * Drop a reference.  The last one frees a RoCE port at once; it closes a
 * host socket now if the chip never used the port, and after the TIME_WAIT
 * interval otherwise.  Thread context.
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
	if (rr->rr_ks == NULL) {
		VERIFY3P(rr->rr_listener, ==, NULL);
		avl_remove(&rdk_cm_roce_ports, rr);
		mutex_exit(&rdk_cm_resv_lock);
		kmem_free(rr, sizeof (*rr));
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
 * The device port an interface sits on, with a hold on the device, from
 * the interface table.  Tagged interfaces are refused for now.
 */
static int
rdk_cm_if_path(uint_t ifindex, rdk_cm_path_t *p)
{
	uint8_t mac[ETHERADDRL];
	uint16_t vlan;
	int ret;

	if ((ret = rdk_cm_if_get(ifindex, mac, &vlan)) != 0)
		return (ret == ENOTSUP ? ENOTSUP : EADDRNOTAVAIL);
	if (vlan != RDK_VLAN_NONE)
		return (ENOTSUP);
	if ((ret = rdk_cm_dev_by_mac(mac, &p->cp_dev, &p->cp_port)) != 0)
		return (EADDRNOTAVAIL);
	p->cp_ifindex = ifindex;
	p->cp_vlan = vlan;
	bcopy(mac, p->cp_smac, ETHERADDRL);
	return (0);
}

static void
rdk_cm_path_rele(rdk_cm_path_t *p)
{
	if (p->cp_dev != NULL) {
		rdk_cm_dev_rele(p->cp_dev);
		p->cp_dev = NULL;
	}
}

void
rdk_cm_path_fill(const rdk_cm_path_t *p, struct rdk_cm_route *rt)
{
	rt->rcr_src.sin_family = AF_INET;
	rt->rcr_src.sin_addr.s_addr = p->cp_src;
	rt->rcr_port = p->cp_port;
	bcopy(p->cp_smac, rt->rcr_smac, ETHERADDRL);
	rt->rcr_vlan = p->cp_vlan;
	rt->rcr_mtu = p->cp_mtu;
}

/* The device of an address the host has up on one of its interfaces. */
int
rdk_cm_local_dev(cred_t *cr, ipaddr_t addr, rdk_cm_path_t *p)
{
	netstack_t *ns;
	ipif_t *ipif;
	uint_t ifindex;
	uint32_t mtu;
	int ret;

	bzero(p, sizeof (*p));
	if ((ns = netstack_find_by_cred(cr)) == NULL)
		return (ENXIO);
	ipif = ipif_lookup_addr_nondup(addr, NULL, crgetzoneid(cr),
	    ns->netstack_ip);
	if (ipif == NULL) {
		netstack_rele(ns);
		return (EADDRNOTAVAIL);
	}
	ifindex = ipif->ipif_ill->ill_phyint->phyint_ifindex;
	mtu = ipif->ipif_ill->ill_mtu;
	ipif_refrele(ipif);
	netstack_rele(ns);
	if ((ret = rdk_cm_if_path(ifindex, p)) != 0)
		return (ret);
	p->cp_src = addr;
	p->cp_mtu = mtu;
	return (0);
}

/*
 * The path toward dst: egress interface and device, source address, next
 * hop and MTU.  src is INADDR_ANY to pick the source, or an address up on
 * the egress interface.  A destination the host itself has, on a RoCE
 * device port, is reached through the device's loopback.
 */
int
rdk_cm_route_lookup(cred_t *cr, ipaddr_t dst, ipaddr_t src, rdk_cm_path_t *p)
{
	const zoneid_t zoneid = crgetzoneid(cr);
	ipaddr_t setsrc = INADDR_ANY;
	netstack_t *ns;
	ip_stack_t *ipst;
	ire_t *ire;
	ill_t *ill = NULL;
	ipif_t *ipif;
	uint_t ifindex;
	boolean_t local;
	int ret = 0;

	bzero(p, sizeof (*p));
	if ((ns = netstack_find_by_cred(cr)) == NULL)
		return (ENXIO);
	ipst = ns->netstack_ip;
	ire = ire_route_recursive_v4(dst, 0, NULL, zoneid, NULL,
	    MATCH_IRE_DSTONLY, IRR_ALLOCATE, 0, ipst, &setsrc, NULL, NULL);
	if (ire == NULL) {
		netstack_rele(ns);
		return (ENETUNREACH);
	}
	local = (ire->ire_type & IRE_LOCAL) != 0;
	if ((ire->ire_flags & (RTF_REJECT | RTF_BLACKHOLE)) != 0 ||
	    (ire->ire_type & (IRE_LOOPBACK | IRE_BROADCAST |
	    IRE_MULTICAST)) != 0) {
		ret = ENETUNREACH;
		goto out;
	}
	if (local) {
		ipif = ipif_lookup_addr_nondup(dst, NULL, zoneid, ipst);
		if (ipif != NULL) {
			ill = ipif->ipif_ill;
			ill_refhold(ill);
			ipif_refrele(ipif);
		}
	} else {
		ill = ire_nexthop_ill(ire);
	}
	if (ill == NULL || IS_LOOPBACK(ill) || IS_IPMP(ill) ||
	    IS_UNDER_IPMP(ill) || ill->ill_isv6) {
		ret = ENETUNREACH;
		goto out;
	}
	ifindex = ill->ill_phyint->phyint_ifindex;
	if (src == INADDR_ANY) {
		if (local) {
			src = dst;
		} else if (ip_select_source_v4(ill, setsrc, dst, INADDR_ANY,
		    zoneid, ipst, &src, NULL, NULL) != 0 ||
		    !rdk_cm_unicast(src)) {
			ret = EADDRNOTAVAIL;
			goto out;
		}
	} else {
		ipif = ipif_lookup_addr_nondup(src, local ? NULL : ill, zoneid,
		    ipst);
		if (ipif == NULL ||
		    ipif->ipif_ill->ill_phyint->phyint_ifindex != ifindex) {
			if (ipif != NULL)
				ipif_refrele(ipif);
			ret = EADDRNOTAVAIL;
			goto out;
		}
		ipif_refrele(ipif);
	}
	if ((ret = rdk_cm_if_path(ifindex, p)) != 0) {
		ret = ret == ENOTSUP ? ENOTSUP : ENETUNREACH;
		goto out;
	}
	if (local && !p->cp_dev->rcd_roce) {
		rdk_cm_path_rele(p);
		ret = ENETUNREACH;
		goto out;
	}
	p->cp_src = src;
	p->cp_local = local;
	p->cp_nexthop = local || (ire->ire_type & IRE_OFFLINK) == 0 ? dst :
	    ire->ire_gateway_addr;
	p->cp_mtu = ill->ill_mtu;
	if (ire->ire_metrics.iulp_mtu != 0)
		p->cp_mtu = MIN(p->cp_mtu, ire->ire_metrics.iulp_mtu);
	p->cp_ttl = (uint8_t)MIN(ipst->ips_ip_def_ttl, UINT8_MAX);
out:
	if (ill != NULL)
		ill_refrele(ill);
	ire_refrele(ire);
	netstack_rele(ns);
	return (ret);
}

/*
 * Next-hop resolution through IP's resolver.
 */
static void
rdk_cm_arp_rele(rdk_cm_arp_t *rp)
{
	const rdk_cm_arp_ops_t *ops;
	void *arg;
	boolean_t last;

	mutex_enter(&rp->rp_lock);
	VERIFY3U(rp->rp_refs, >, 0);
	last = --rp->rp_refs == 0;
	arg = rp->rp_arg;
	ops = rp->rp_ops;
	mutex_exit(&rp->rp_lock);
	if (!last)
		return;
	mutex_destroy(&rp->rp_lock);
	kmem_free(rp, sizeof (*rp));
	if (arg != NULL)
		ops->rao_rele(arg);
}

static void
rdk_cm_arp_task(void *arg)
{
	rdk_cm_arp_t *rp = arg;
	const rdk_cm_arp_ops_t *ops;
	void *owner;
	uint32_t gen;
	int err;

	/* The answer takes over the resolver's hold on the owner. */
	mutex_enter(&rp->rp_lock);
	owner = rp->rp_arg;
	rp->rp_arg = NULL;
	ops = rp->rp_ops;
	gen = rp->rp_gen;
	err = rp->rp_err;
	mutex_exit(&rp->rp_lock);
	if (owner != NULL) {
		ops->rao_done(owner, gen, err, rp->rp_mac);
		ops->rao_rele(owner);
	}
	rdk_cm_arp_rele(rp);
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
	rdk_cm_arp_t *rp = arg;

	mutex_enter(&rp->rp_lock);
	rp->rp_done = B_TRUE;
	rp->rp_err = rdk_cm_ip2mac_result(ip2m, rp->rp_mac);
	mutex_exit(&rp->rp_lock);
	taskq_dispatch_ent(rdk_cm_taskq, rdk_cm_arp_task, rp, 0,
	    &rp->rp_tqent);
}

/*
 * Resolve the MAC of the next hop nh on the interface.  The answer calls
 * rao_done(arg, gen, err, mac), perhaps before this returns; the caller
 * must not hold the locks rao_done takes.  *rpp is the owner's reference.
 */
int
rdk_cm_arp_start(zoneid_t zone, uint_t ifindex, ipaddr_t nh,
    const rdk_cm_arp_ops_t *ops, void *arg, uint32_t gen, rdk_cm_arp_t **rpp)
{
	rdk_cm_arp_t *rp;
	ip2mac_t ip2m;
	struct sockaddr_in *sin;
	ip2mac_id_t mid;
	uint8_t mac[ETHERADDRL];
	int err;

	rp = kmem_zalloc(sizeof (*rp), KM_SLEEP);
	mutex_init(&rp->rp_lock, NULL, MUTEX_DRIVER, NULL);
	rp->rp_refs = 2;	/* the owner and the answer */
	rp->rp_gen = gen;
	rp->rp_zone = zone;
	rp->rp_ops = ops;
	ops->rao_hold(arg);
	rp->rp_arg = arg;
	*rpp = rp;

	bzero(&ip2m, sizeof (ip2m));
	sin = (struct sockaddr_in *)&ip2m.ip2mac_pa;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = nh;
	ip2m.ip2mac_ifindex = ifindex;

	atomic_inc_uint(&rdk_cm_ip2mac_out);
	mid = ip2mac(IP2MAC_RESOLVE, &ip2m, rdk_cm_ip2mac_cb, rp, zone);
	if (ip2m.ip2mac_err == EINPROGRESS && mid != NULL) {
		mutex_enter(&rp->rp_lock);
		rp->rp_ip2mac = mid;
		mutex_exit(&rp->rp_lock);
		return (0);
	}

	/* Answered at once: the callback will not run. */
	atomic_dec_uint(&rdk_cm_ip2mac_out);
	mutex_enter(&rp->rp_lock);
	rp->rp_done = B_TRUE;
	mutex_exit(&rp->rp_lock);
	err = rdk_cm_ip2mac_result(&ip2m, mac);
	ops->rao_done(arg, gen, err, mac);
	rdk_cm_arp_rele(rp);
	return (0);
}

/*
 * The next hop's MAC if the neighbor cache has it now; never starts a
 * resolution, so a packet from an unknown sender cannot make the host ARP.
 */
int
rdk_cm_nexthop_lookup(zoneid_t zone, uint_t ifindex, ipaddr_t nh,
    uint8_t *mac)
{
	ip2mac_t ip2m;
	struct sockaddr_in *sin;

	bzero(&ip2m, sizeof (ip2m));
	sin = (struct sockaddr_in *)&ip2m.ip2mac_pa;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = nh;
	ip2m.ip2mac_ifindex = ifindex;
	(void) ip2mac(IP2MAC_LOOKUP, &ip2m, NULL, NULL, zone);
	return (rdk_cm_ip2mac_result(&ip2m, mac));
}

/* Give back the owner's reference; a later answer is dropped. */
void
rdk_cm_arp_cancel(rdk_cm_arp_t *rp)
{
	const rdk_cm_arp_ops_t *ops;
	ip2mac_id_t mid = NULL;
	void *held;
	boolean_t cb_ref = B_FALSE;

	if (rp == NULL)
		return;
	mutex_enter(&rp->rp_lock);
	held = rp->rp_arg;
	ops = rp->rp_ops;
	rp->rp_arg = NULL;
	if (!rp->rp_done)
		mid = rp->rp_ip2mac;
	mutex_exit(&rp->rp_lock);
	if (held != NULL)
		ops->rao_rele(held);

	if (mid != NULL && ip2mac_cancel(mid, rp->rp_zone) == 0) {
		mutex_enter(&rp->rp_lock);
		if (!rp->rp_done) {
			rp->rp_done = B_TRUE;
			cb_ref = B_TRUE;
		}
		mutex_exit(&rp->rp_lock);
	}
	if (cb_ref) {
		atomic_dec_uint(&rdk_cm_ip2mac_out);
		rdk_cm_arp_rele(rp);
	}
	rdk_cm_arp_rele(rp);
}

/* Stop the ID's route resolution; a late answer is dropped. */
void
rdk_cm_resolve_cancel(rdk_cm_id_t *id)
{
	rdk_cm_arp_t *rp;

	mutex_enter(&id->rci_lock);
	rp = id->rci_resolve;
	id->rci_resolve = NULL;
	mutex_exit(&id->rci_lock);
	rdk_cm_arp_cancel(rp);
}

int
rdk_cm_addr_init(void)
{
	mutex_init(&rdk_cm_resv_lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&rdk_cm_tw, sizeof (rdk_cm_resv_t),
	    offsetof(rdk_cm_resv_t, rr_node));
	avl_create(&rdk_cm_roce_ports, rdk_cm_port_cmp, sizeof (rdk_cm_resv_t),
	    offsetof(rdk_cm_resv_t, rr_avl));
	rdk_cm_tw_stop = B_FALSE;
	return (0);
}

/* EBUSY while a reservation is held or waits, or IP may call back. */
int
rdk_cm_addr_fini(void)
{
	mutex_enter(&rdk_cm_resv_lock);
	if (!list_is_empty(&rdk_cm_tw) || rdk_cm_tw_busy ||
	    rdk_cm_ip2mac_out != 0 || avl_numnodes(&rdk_cm_roce_ports) != 0) {
		mutex_exit(&rdk_cm_resv_lock);
		return (EBUSY);
	}
	rdk_cm_tw_stop = B_TRUE;
	mutex_exit(&rdk_cm_resv_lock);
	if (rdk_cm_tw_timer != 0)
		(void) untimeout(rdk_cm_tw_timer);
	avl_destroy(&rdk_cm_roce_ports);
	list_destroy(&rdk_cm_tw);
	mutex_destroy(&rdk_cm_resv_lock);
	return (0);
}

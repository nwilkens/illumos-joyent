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

#ifndef _RDK_CM_IMPL_H
#define	_RDK_CM_IMPL_H

#include <sys/ksocket.h>
#include <sys/avl.h>
#include <net/if.h>
#include <inet/ip2mac.h>

#include "rdk.h"
#include "rdk_cm_gidtab.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rdk_cm_state {
	RCS_IDLE = 0,
	RCS_BOUND,		/* bind_addr done */
	RCS_ADDR_QUERY,
	RCS_ADDR_RESOLVED,
	RCS_ROUTE_QUERY,
	RCS_ROUTE_RESOLVED,
	RCS_LISTEN,
	RCS_CONNECT,		/* active open with the transport */
	RCS_REQ,		/* request delivered, no decision yet */
	RCS_ACCEPT,		/* passive open with the transport */
	RCS_ESTABLISHED,
	RCS_DISCONNECT,		/* DISCONNECTED delivered */
	RCS_DONE,		/* nothing more will be delivered */
	RCS_REMOVED		/* DEVICE_REMOVAL delivered */
} rdk_cm_state_t;

/* The pending operation, whose end is one event. */
typedef enum rdk_cm_op {
	RCO_NONE = 0,
	RCO_ROUTE,
	RCO_CONNECT,
	RCO_ACCEPT
} rdk_cm_op_t;

/* What a transport tells the core about a connection. */
typedef enum rdk_cm_tev {
	RCT_REPLY = 1,		/* connect or accept ended; status says how */
	RCT_ESTABLISHED,
	RCT_DISCONNECT,
	RCT_CLOSE		/* final */
} rdk_cm_tev_t;

#define	RDK_CM_MAX_PORTS	4

struct rdk_cm_id;
struct rdk_cm_dev;
struct rdk_gsi;
struct rdk_ibconn;

/*
 * A transport backend.  The operations run in thread context without CM
 * locks.  connect and accept that succeed leave a final RCT_CLOSE (or an
 * RCT_REPLY with a nonzero status) owed; release runs once the ID is being
 * destroyed and no operation runs.
 */
typedef struct rdk_cm_tport {
	int	(*ct_listen)(struct rdk_cm_id *);
	void	(*ct_unlisten)(struct rdk_cm_id *);
	int	(*ct_connect)(struct rdk_cm_id *,
	    const struct rdk_cm_conn_param *);
	int	(*ct_accept)(struct rdk_cm_id *,
	    const struct rdk_cm_conn_param *);
	int	(*ct_reject)(struct rdk_cm_id *, const void *, uint16_t);
	void	(*ct_disconnect)(struct rdk_cm_id *, boolean_t);
	void	(*ct_release)(struct rdk_cm_id *);
	uint16_t (*ct_pdata_max)(struct rdk_cm_dev *, enum rdk_cm_msg);
} rdk_cm_tport_t;

/*
 * A device the CM knows: iWARP with its provider's CM operations, or RoCE
 * with a GSI agent per port.
 */
typedef struct rdk_cm_dev {
	list_node_t			rcd_node;
	struct rdk_device		*rcd_dev;
	const struct rdk_iw_cm_ops	*rcd_iw;
	const rdk_cm_tport_t		*rcd_tp;
	boolean_t			rcd_roce;
	uint32_t			rcd_ops;	/* operations running */
	uint32_t			rcd_ids;	/* IDs bound to it */
	boolean_t			rcd_removing;
	boolean_t			rcd_added;	/* client add ran */
	uint32_t			rcd_nports;
	uint8_t				rcd_mac[RDK_CM_MAX_PORTS][ETHERADDRL];
	struct rdk_gsi			*rcd_gsi[RDK_CM_MAX_PORTS];
	uint32_t			rcd_conns;	/* rdk_ibcm_lock */
} rdk_cm_dev_t;

/*
 * A port reservation: an exclusive host TCP port (iWARP, rr_ks set), or a
 * port of the RoCE CM's own space, which the host TCP stack never sees.
 */
typedef struct rdk_cm_resv {
	list_node_t		rr_node;
	avl_node_t		rr_avl;		/* RoCE */
	ksocket_t		rr_ks;
	struct sockaddr_in	rr_addr;
	uint32_t		rr_refs;
	boolean_t		rr_used;	/* the chip has used the port */
	hrtime_t		rr_expire;
	struct rdk_cm_id	*rr_listener;	/* RoCE, held */
} rdk_cm_resv_t;

struct rdk_cm_acl {
	uint32_t	rca_refs;
	uint32_t	rca_n;
	ipaddr_t	rca_addr[];	/* sorted, network order */
};

/* A queued event, with its own copy of the private data. */
typedef struct rdk_cm_qev {
	list_node_t		q_node;
	struct rdk_cm_event	q_ev;
	uint8_t			q_pdata[RDK_CM_PDATA_MAX];
} rdk_cm_qev_t;

/*
 * A next-hop resolution.  The owner holds one reference and gives it back
 * with rdk_cm_arp_cancel(); the answer holds the other and calls rao_done
 * at most once.  rp_arg is held until the answer or the cancel takes it.
 */
typedef struct rdk_cm_arp_ops {
	void	(*rao_done)(void *, uint32_t, int, const uint8_t *);
	void	(*rao_hold)(void *);
	void	(*rao_rele)(void *);
} rdk_cm_arp_ops_t;

typedef struct rdk_cm_arp {
	kmutex_t		rp_lock;
	uint32_t		rp_refs;
	boolean_t		rp_done;	/* callback ran or will not */
	const rdk_cm_arp_ops_t	*rp_ops;
	void			*rp_arg;	/* held; NULL once canceled */
	uint32_t		rp_gen;
	zoneid_t		rp_zone;
	ip2mac_id_t		rp_ip2mac;
	int			rp_err;
	uint8_t			rp_mac[ETHERADDRL];
	taskq_ent_t		rp_tqent;
} rdk_cm_arp_t;

/* An interface of the global zone's IP stack. */
typedef struct rdk_cm_if {
	list_node_t		rif_node;
	uint_t			rif_ifindex;
	boolean_t		rif_v6only;
	char			rif_name[LIFNAMSIZ];
	uint8_t			rif_mac[ETHERADDRL];
	uint16_t		rif_vlan;	/* RDK_VLAN_NONE */
	uint32_t		rif_mtu;
	int			rif_err;	/* 0 when it may carry RDMA */
} rdk_cm_if_t;

/* A path, as route lookup finds it. */
typedef struct rdk_cm_path {
	rdk_cm_dev_t		*cp_dev;	/* held */
	uint32_t		cp_port;
	uint_t			cp_ifindex;
	ipaddr_t		cp_src;
	ipaddr_t		cp_nexthop;
	boolean_t		cp_local;	/* the destination is ours */
	uint16_t		cp_vlan;
	uint32_t		cp_mtu;
	uint8_t			cp_smac[ETHERADDRL];
	uint8_t			cp_ttl;
} rdk_cm_path_t;

/*
 * rci_lock covers the state, the queue and the operation fields.  It is
 * never held across a handler, a transport operation or a ksocket call.
 */
struct rdk_cm_id {
	list_node_t		rci_node;	/* rdk_cm_lock */
	kmutex_t		rci_lock;
	kcondvar_t		rci_cv;
	uint32_t		rci_refs;
	rdk_cm_state_t		rci_state;
	boolean_t		rci_destroying;
	boolean_t		rci_removal;	/* DEVICE_REMOVAL is queued */
	rdk_cm_handler_t	rci_handler;
	void			*rci_ctx;
	cred_t			*rci_cred;
	enum rdk_qp_type	rci_qpt;

	rdk_cm_dev_t		*rci_dev;
	uint32_t		rci_port;
	uint_t			rci_ifindex;
	ipaddr_t		rci_nexthop;
	boolean_t		rci_local;
	uint8_t			rci_ttl;
	struct rdk_cm_route	rci_route;
	rdk_cm_resv_t		*rci_resv;

	rdk_cm_op_t		rci_op;
	uint32_t		rci_opgen;
	boolean_t		rci_canceled;
	int			rci_cancel_err;
	timeout_id_t		rci_timer;
	uint32_t		rci_timer_gen;
	rdk_cm_arp_t		*rci_resolve;

	list_t			rci_events;
	boolean_t		rci_queued;	/* the task is queued or runs */
	kthread_t		*rci_cb_thread;
	taskq_ent_t		rci_tqent;

	boolean_t		rci_tp_ref;	/* the transport owes a final */

	struct rdk_iw_cm_id	rci_iw;
	boolean_t		rci_iw_listen;
	boolean_t		rci_iw_owned;	/* iw_release is owed */
	boolean_t		rci_iw_gone;	/* no more provider calls */
	uint32_t		rci_iw_calls;	/* provider calls running */

	struct rdk_ibconn	*rci_conn;	/* RoCE, held */
	boolean_t		rci_roce_listen;

	rdk_cm_acl_t		*rci_acl;
	uint32_t		rci_backlog;
	uint32_t		rci_pending;	/* admitted, undecided */
	struct rdk_cm_id	*rci_listener;	/* request: held */
	void			*rci_admit;
	boolean_t		rci_decided;
};

/* An admission held for a request, until the consumer decides. */
typedef struct rdk_cm_admit {
	struct rdk_cm_id	*ra_listener;	/* held */
	ipaddr_t		ra_peer;
	boolean_t		ra_done;
} rdk_cm_admit_t;

/*
 * What a transport knows of a new request, to make its child ID.  The
 * child takes cc_admit and the caller's hold on cc_conn.
 */
typedef struct rdk_cm_child {
	struct sockaddr_in	cc_laddr;
	struct sockaddr_in	cc_raddr;
	uint32_t		cc_port;
	const void		*cc_pdata;
	uint16_t		cc_pdata_len;
	uint32_t		cc_ird;
	uint32_t		cc_ord;
	void			*cc_admit;
	void			*cc_iw_provider;
	struct rdk_ibconn	*cc_conn;
	const uint8_t		*cc_dmac;
	uint32_t		cc_mtu;
} rdk_cm_child_t;

extern kmutex_t rdk_cm_lock;
extern list_t rdk_cm_devs;
extern list_t rdk_cm_ids;
extern uint_t rdk_cm_max_pending;
extern taskq_t *rdk_cm_taskq;
extern uint_t rdk_cm_timewait_ms;
extern const rdk_cm_tport_t rdk_cm_iw_tport;
extern const rdk_cm_tport_t rdk_cm_roce_tport;

/* rdk_cm.c */
extern void rdk_cm_hold(struct rdk_cm_id *);
extern void rdk_cm_rele(struct rdk_cm_id *);
extern void rdk_cm_queue(struct rdk_cm_id *, enum rdk_cm_event_type, int,
    const void *, uint16_t);
extern void rdk_cm_queue_locked(struct rdk_cm_id *, rdk_cm_qev_t *);
extern rdk_cm_qev_t *rdk_cm_qev_alloc(enum rdk_cm_event_type, int,
    const void *, uint16_t);
extern void rdk_cm_addr_change(uint_t);
extern void rdk_cm_route_done(void *, uint32_t, int, const uint8_t *);
extern void rdk_cm_conn_event(struct rdk_cm_id *, rdk_cm_tev_t, int,
    uint32_t, const void *, uint16_t, uint32_t, uint32_t);
extern void rdk_cm_final(struct rdk_cm_id *);
extern void rdk_cm_conn_unquota(void);
extern struct rdk_cm_id *rdk_cm_alloc(cred_t *, rdk_cm_handler_t, void *,
    enum rdk_qp_type);
extern int rdk_cm_init(void);
extern int rdk_cm_fini(void);

/* rdk_cm_listen.c */
extern void rdk_cm_dev_rele(rdk_cm_dev_t *);
extern rdk_cm_dev_t *rdk_cm_dev_find_locked(struct rdk_device *);
extern int rdk_cm_client_init(void);
extern void rdk_cm_client_fini(void);
extern int rdk_cm_admit(struct rdk_cm_id *, ipaddr_t, void **);
extern void rdk_cm_unadmit(void *);
extern int rdk_cm_child_new(struct rdk_cm_id *, const rdk_cm_child_t *,
    struct rdk_cm_id **);

extern kcondvar_t rdk_cm_dev_cv;

/* rdk_cm_addr.c */
extern int rdk_cm_resv_get(rdk_cm_dev_t *, cred_t *,
    const struct sockaddr_in *, rdk_cm_resv_t **);
extern void rdk_cm_resv_hold(rdk_cm_resv_t *);
extern void rdk_cm_resv_used(rdk_cm_resv_t *);
extern void rdk_cm_resv_rele(rdk_cm_resv_t *);
extern int rdk_cm_resv_listen(rdk_cm_resv_t *, struct rdk_cm_id *);
extern void rdk_cm_resv_unlisten(rdk_cm_resv_t *, struct rdk_cm_id *);
extern struct rdk_cm_id *rdk_cm_roce_listener(ipaddr_t, uint16_t);
extern int rdk_cm_local_dev(cred_t *, ipaddr_t, rdk_cm_path_t *);
extern int rdk_cm_route_lookup(cred_t *, ipaddr_t, ipaddr_t,
    rdk_cm_path_t *);
extern void rdk_cm_path_fill(const rdk_cm_path_t *, struct rdk_cm_route *);
extern int rdk_cm_arp_start(zoneid_t, uint_t, ipaddr_t,
    const rdk_cm_arp_ops_t *, void *, uint32_t, rdk_cm_arp_t **);
extern void rdk_cm_arp_cancel(rdk_cm_arp_t *);
extern int rdk_cm_nexthop_lookup(zoneid_t, uint_t, ipaddr_t, uint8_t *);
extern void rdk_cm_resolve_cancel(struct rdk_cm_id *);
extern boolean_t rdk_cm_acl_allows(const rdk_cm_acl_t *, ipaddr_t);
extern void rdk_cm_acl_hold(rdk_cm_acl_t *);
extern boolean_t rdk_cm_unicast(ipaddr_t);
extern int rdk_cm_addr_init(void);
extern int rdk_cm_addr_fini(void);

/* rdk_cm_gid.c */
extern int rdk_cm_if_get(uint_t, uint8_t *, uint16_t *);
extern int rdk_cm_dev_by_mac(const uint8_t *, rdk_cm_dev_t **, uint32_t *);
extern void rdk_cm_gid_sync(void);
extern void rdk_cm_gid_dev_added(void);
extern void rdk_cm_gid_dev_removed(struct rdk_device *);
extern int rdk_cm_gid_init(void);
extern void rdk_cm_gid_fini(void);

/* rdk_cm_roce.c */
extern int rdk_cm_roce_dev_add(rdk_cm_dev_t *);
extern void rdk_cm_roce_dev_remove(rdk_cm_dev_t *);
extern void rdk_cm_roce_qp_gone(struct rdk_qp *);
extern void rdk_cm_roce_gid_withdrawn(struct rdk_device *, uint32_t,
    uint16_t);
extern int rdk_cm_roce_init(void);
extern int rdk_cm_roce_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_CM_IMPL_H */

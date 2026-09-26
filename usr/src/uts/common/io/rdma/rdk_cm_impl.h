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
#include <inet/ip2mac.h>

#include "rdk.h"

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
	RCS_CONNECT,		/* active open with the provider */
	RCS_REQ,		/* request delivered, no decision yet */
	RCS_ACCEPT,		/* passive open with the provider */
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

#define	RDK_CM_MAX_PORTS	4

/* A device with its iWARP CM operations and the MACs of its ports. */
typedef struct rdk_cm_dev {
	list_node_t			rcd_node;
	struct rdk_device		*rcd_dev;
	const struct rdk_iw_cm_ops	*rcd_iw;
	uint32_t			rcd_ops;	/* operations running */
	uint32_t			rcd_ids;	/* IDs bound to it */
	boolean_t			rcd_removing;
	boolean_t			rcd_added;	/* client add ran */
	uint32_t			rcd_nports;
	uint8_t				rcd_mac[RDK_CM_MAX_PORTS][ETHERADDRL];
} rdk_cm_dev_t;

/* An exclusive host TCP port reservation. */
typedef struct rdk_cm_resv {
	list_node_t		rr_node;
	ksocket_t		rr_ks;
	struct sockaddr_in	rr_addr;
	uint32_t		rr_refs;
	boolean_t		rr_used;	/* the chip has used the port */
	hrtime_t		rr_expire;
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

/* A route lookup waiting on the neighbor cache. */
typedef struct rdk_cm_resolve {
	kmutex_t		rs_lock;
	uint32_t		rs_refs;
	boolean_t		rs_done;	/* the callback ran or never will */
	struct rdk_cm_id	*rs_id;		/* held; NULL once canceled */
	uint32_t		rs_gen;
	ip2mac_id_t		rs_ip2mac;
	int			rs_err;
	uint8_t			rs_mac[ETHERADDRL];
	taskq_ent_t		rs_tqent;
} rdk_cm_resolve_t;

/*
 * rci_lock covers the state, the queue and the operation fields.  It is
 * never held across a handler, a provider operation or a ksocket call.
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
	struct rdk_cm_route	rci_route;
	rdk_cm_resv_t		*rci_resv;

	rdk_cm_op_t		rci_op;
	uint32_t		rci_opgen;
	boolean_t		rci_canceled;
	int			rci_cancel_err;
	timeout_id_t		rci_timer;
	uint32_t		rci_timer_gen;
	rdk_cm_resolve_t	*rci_resolve;

	list_t			rci_events;
	boolean_t		rci_queued;	/* the task is queued or runs */
	kthread_t		*rci_cb_thread;
	taskq_ent_t		rci_tqent;

	struct rdk_iw_cm_id	rci_iw;
	boolean_t		rci_iw_ref;	/* the provider holds the ID */
	boolean_t		rci_iw_listen;
	boolean_t		rci_iw_owned;	/* iw_release is owed */
	boolean_t		rci_iw_gone;	/* no more provider calls */
	uint32_t		rci_iw_calls;	/* provider operations running */

	rdk_cm_acl_t		*rci_acl;
	uint32_t		rci_backlog;
	uint32_t		rci_pending;	/* admitted, undecided */
	struct rdk_cm_id	*rci_listener;	/* request: held */
	void			*rci_admit;
	boolean_t		rci_decided;
};

/* An admission a provider holds for a SYN. */
typedef struct rdk_cm_admit {
	struct rdk_cm_id	*ra_listener;	/* held */
	ipaddr_t		ra_peer;
	boolean_t		ra_done;
} rdk_cm_admit_t;

extern kmutex_t rdk_cm_lock;
extern list_t rdk_cm_devs;
extern list_t rdk_cm_ids;
extern uint_t rdk_cm_max_pending;
extern taskq_t *rdk_cm_taskq;
extern uint_t rdk_cm_timewait_ms;

/* rdk_cm.c */
extern void rdk_cm_hold(struct rdk_cm_id *);
extern void rdk_cm_rele(struct rdk_cm_id *);
extern void rdk_cm_queue(struct rdk_cm_id *, enum rdk_cm_event_type, int,
    const void *, uint16_t);
extern void rdk_cm_queue_locked(struct rdk_cm_id *, rdk_cm_qev_t *);
extern rdk_cm_qev_t *rdk_cm_qev_alloc(enum rdk_cm_event_type, int,
    const void *, uint16_t);
extern void rdk_cm_addr_change(uint_t);
extern void rdk_cm_route_done(struct rdk_cm_id *, uint32_t, int,
    const uint8_t *);
extern void rdk_cm_conn_event(struct rdk_cm_id *, enum rdk_iw_event_type,
    int, const void *, uint16_t, uint32_t, uint32_t);
extern void rdk_cm_conn_unquota(void);
extern int rdk_cm_init(void);
extern int rdk_cm_fini(void);

/* rdk_cm_iw.c */
extern int rdk_cm_iw_listen(struct rdk_cm_id *);
extern void rdk_cm_iw_unlisten(struct rdk_cm_id *);
extern int rdk_cm_iw_connect(struct rdk_cm_id *,
    const struct rdk_cm_conn_param *);
extern int rdk_cm_iw_accept(struct rdk_cm_id *,
    const struct rdk_cm_conn_param *);
extern int rdk_cm_iw_reject(struct rdk_cm_id *, const void *, uint16_t);
extern void rdk_cm_iw_disconnect(struct rdk_cm_id *, boolean_t);
extern void rdk_cm_iw_wait_final(struct rdk_cm_id *);
extern void rdk_cm_iw_release(struct rdk_cm_id *);
extern void rdk_cm_dev_rele(rdk_cm_dev_t *);
extern void rdk_cm_iw_init(void);
extern void rdk_cm_iw_fini(void);

/* rdk_cm_addr.c */
extern int rdk_cm_resv_get(cred_t *, const struct sockaddr_in *,
    rdk_cm_resv_t **);
extern void rdk_cm_resv_hold(rdk_cm_resv_t *);
extern void rdk_cm_resv_used(rdk_cm_resv_t *);
extern void rdk_cm_resv_rele(rdk_cm_resv_t *);
extern int rdk_cm_local_dev(cred_t *, ipaddr_t, rdk_cm_dev_t **,
    uint32_t *, uint_t *, struct rdk_cm_route *);
extern int rdk_cm_route_lookup(cred_t *, ipaddr_t, ipaddr_t *,
    rdk_cm_dev_t **, uint32_t *, uint_t *, struct rdk_cm_route *,
    ipaddr_t *);
extern int rdk_cm_nexthop(struct rdk_cm_id *, ipaddr_t, uint32_t);
extern void rdk_cm_resolve_cancel(struct rdk_cm_id *);
extern boolean_t rdk_cm_acl_allows(const rdk_cm_acl_t *, ipaddr_t);
extern void rdk_cm_acl_hold(rdk_cm_acl_t *);
extern boolean_t rdk_cm_unicast(ipaddr_t);
extern int rdk_cm_addr_init(void);
extern int rdk_cm_addr_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_CM_IMPL_H */

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

#ifndef _RDK_IMPL_H
#define	_RDK_IMPL_H

#include <sys/rwlock.h>
#include <sys/taskq_impl.h>

#include "rdk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdk_gid_ent {
	struct rdk_gid_attr	rge_attr;
	boolean_t		rge_valid;
	uint32_t		rge_refs;	/* rdp_lock */
} rdk_gid_ent_t;

/* A client's data on one device. */
typedef struct rdk_cdata {
	list_node_t		rcd_node;
	struct rdk_client	*rcd_client;
	void			*rcd_data;
	boolean_t		rcd_added;
} rdk_cdata_t;

/*
 * rdk_reg_lock, a global, serializes device and client registration and is
 * held across the clients' add and remove callbacks.  rdp_gid_lock
 * serializes GID changes with the provider's callbacks.  rdp_lock is a leaf
 * lock.
 */
struct rdk_device_priv {
	struct rdk_device	*rdp_dev;
	list_node_t		rdp_node;
	kmutex_t		rdp_lock;
	kcondvar_t		rdp_cv;
	boolean_t		rdp_dying;
	uint64_t		rdp_nobjs;
	list_t			rdp_cdata;
	krwlock_t		rdp_ev_lock;
	list_t			rdp_handlers;
	kmutex_t		rdp_gid_lock;
	rdk_gid_ent_t		rdp_gids[RDK_GID_TABLE_LEN];
};

extern taskq_t *rdk_cq_taskq;

extern int rdk_obj_hold(struct rdk_device *);
extern void rdk_obj_rele(struct rdk_device *);
extern boolean_t rdk_port_valid(struct rdk_device *, uint32_t);
extern int rdk_resolve_ah_attr(struct rdk_device *, struct rdk_ah_attr *);

extern int rdk_create_cq_poll(struct rdk_device *, rdk_comp_handler_t,
    void (*)(struct rdk_event *, void *), void *,
    const struct rdk_cq_init_attr *, enum rdk_poll_context,
    struct rdk_cq_poller *, struct rdk_cq **);

extern int rdk_cq_init(void);
extern void rdk_cq_fini(void);
extern void rdk_cq_barrier(struct rdk_cq *);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_IMPL_H */

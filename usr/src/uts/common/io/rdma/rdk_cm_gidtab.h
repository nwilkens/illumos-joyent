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

#ifndef _RDK_CM_GIDTAB_H
#define	_RDK_CM_GIDTAB_H

/*
 * The set of RoCEv2 GIDs the host's addresses give, and what is installed
 * in the devices.  A reconcile pass marks each wanted key, then adds what
 * is missing and deletes what is gone.  A deletion the device refuses
 * because a QP or AH still uses the slot stays pending and is retried.
 * Builds in the kernel and on the host for tests.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	RDK_GIDTAB_MAX		256
#define	RDK_GID_TYPE_ROCEV2	2

typedef struct rdk_gidkey {
	uint32_t	gk_stack;	/* netstack id */
	uint32_t	gk_ifindex;
	uint16_t	gk_vlan;	/* 0xffff: untagged */
	uint8_t		gk_type;
	uint8_t		gk_pad;
	uint8_t		gk_addr[16];	/* IPv4-mapped or IPv6 */
} rdk_gidkey_t;

typedef enum rdk_gidstate {
	RGS_FREE = 0,
	RGS_WANT,		/* wanted, not in a device */
	RGS_INSTALLED,
	RGS_STALE		/* installed, no longer wanted */
} rdk_gidstate_t;

typedef struct rdk_gident {
	rdk_gidkey_t	ge_key;
	rdk_gidstate_t	ge_state;
	boolean_t	ge_marked;
	boolean_t	ge_revive;	/* stale, and wanted again */
	void		*ge_dev;	/* where it is or goes */
	uint32_t	ge_port;
	uint8_t		ge_mac[6];
	uint16_t	ge_index;	/* RGS_INSTALLED, RGS_STALE */
	int		ge_err;		/* the device's last refusal */
} rdk_gident_t;

/*
 * gto_withdraw stops new users of an installed entry (B_TRUE) or lets them
 * back (B_FALSE); a stale entry is withdrawn before it is deleted.
 */
typedef struct rdk_gidtab_ops {
	int	(*gto_add)(void *, void *, uint32_t, const rdk_gidkey_t *,
	    const uint8_t *, uint16_t *);
	int	(*gto_del)(void *, void *, uint32_t, uint16_t);
	void	(*gto_withdraw)(void *, void *, uint32_t, uint16_t, boolean_t);
} rdk_gidtab_ops_t;

typedef struct rdk_gidtab {
	const rdk_gidtab_ops_t	*gt_ops;
	void			*gt_arg;
	uint32_t		gt_n;		/* entries not RGS_FREE */
	uint32_t		gt_dropped;	/* keys over RDK_GIDTAB_MAX */
	rdk_gident_t		gt_ents[RDK_GIDTAB_MAX];
} rdk_gidtab_t;

typedef struct rdk_gidtab_stats {
	uint32_t	gs_added;
	uint32_t	gs_deleted;
	uint32_t	gs_add_failed;
	uint32_t	gs_nospc;	/* the device had no room */
	uint32_t	gs_busy;	/* deletions left pending */
	uint32_t	gs_withdrawn;
	uint32_t	gs_revived;
} rdk_gidtab_stats_t;

extern void rdk_gidtab_init(rdk_gidtab_t *, const rdk_gidtab_ops_t *, void *);
extern void rdk_gidtab_begin(rdk_gidtab_t *);
extern int rdk_gidtab_want(rdk_gidtab_t *, const rdk_gidkey_t *, void *,
    uint32_t, const uint8_t *);
extern void rdk_gidtab_end(rdk_gidtab_t *, rdk_gidtab_stats_t *);
extern void rdk_gidtab_forget_dev(rdk_gidtab_t *, void *, boolean_t);
extern boolean_t rdk_gidtab_retry_needed(const rdk_gidtab_t *);
extern int rdk_gidkey_cmp(const rdk_gidkey_t *, const rdk_gidkey_t *);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_CM_GIDTAB_H */

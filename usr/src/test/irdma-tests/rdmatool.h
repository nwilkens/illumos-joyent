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

#ifndef _RDMATOOL_H
#define	_RDMATOOL_H

#include <sys/types.h>
#include <pthread.h>

#include "../../uts/common/io/rdma/rdmat_ioctl.h"

#define	DEVPATH		"/devices/pseudo/rdmat@0:rdmat"
#define	MAGIC		0x52444d54
#define	K_ACK		1
#define	K_RESULT	2
#define	C_FRESH		0x100	/* reopen the server's session */
#define	C_BYE		0x101
#define	C_CPU		0x102	/* the server's busy CPU nanoseconds */
#define	C_TCP		0x103	/* a TCP ping-pong and bulk transfer */
#define	ST_REM_ACCESS	10	/* RDK_WC_REM_ACCESS_ERR */
#define	QPS_ERR		6	/* RDK_QPS_ERR */
#define	QPE_ACCESS	3	/* RDK_EVENT_QP_ACCESS_ERR */

typedef struct tcpreq {
	uint64_t	tr_count;	/* ping-pong round trips */
	uint64_t	tr_bulk;	/* bytes the client then sends */
	uint32_t	tr_size;	/* ping-pong message size */
	uint32_t	tr_pad;
	uint64_t	tr_srv_cpu_ns;	/* out: server busy time in the bulk */
	uint64_t	tr_srv_ns;	/* out: server time from first byte */
} tcpreq_t;

typedef struct msg {
	uint32_t	m_magic;
	uint32_t	m_cmd;
	uint32_t	m_len;
	int32_t		m_err;	/* reply; request: 1 for an async run */
} msg_t;

/* One side of a test: a local session or the server's. */
typedef struct peer {
	const char	*p_name;
	int		p_fd;		/* local session */
	int		p_sock;		/* remote, or -1 */
	rdmat_setup_t	p_setup;
	pthread_t	p_thr;
	int		p_async;
	rdmat_run_t	p_arun;
	int		p_aret;
} peer_t;

extern char *o_dev;
extern uint32_t o_ip;
extern int o_port;
extern uint64_t o_buf_mb;
extern uint32_t o_depth;
extern int o_secs;
extern int failures;
extern uint32_t path_mtu;
extern char *o_server;
extern int o_iwarp;

extern void fatal(const char *, ...);
extern void result(int, const char *, const char *, ...);
extern int open_session(void);
extern int rpc(peer_t *, uint32_t, void *, int, int);
extern int pio(peer_t *, uint32_t, void *);
extern void run_start(peer_t *, const rdmat_run_t *);
extern int run_finish(peer_t *, rdmat_run_t *);
extern int run(peer_t *, rdmat_run_t *);
extern void run_init(rdmat_run_t *, uint32_t, uint32_t, uint32_t);
extern int buf(peer_t *, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
    int64_t *);
extern uint32_t qp_state(peer_t *);
extern int fresh(peer_t *, peer_t *, uint32_t, uint32_t);
extern uint64_t seed_of(const char *, uint64_t);
extern uint64_t cpu_busy_ns(void);
extern uint64_t now_ns(void);

/* rdmatool_iw.c */
extern int iw_pair(peer_t *, peer_t *);
extern int iw_test(peer_t *, peer_t *, const char *);

#endif /* _RDMATOOL_H */

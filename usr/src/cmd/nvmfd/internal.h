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
 * Provenance: ported to illumos from FreeBSD
 * tools/tools/nvmf/nvmfd/internal.h.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef	_NVMFD_INTERNAL_H
#define	_NVMFD_INTERNAL_H

#include <sys/types.h>
#include <sys/ccompile.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <stdint.h>
#include <netdb.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include "nvme_reg.h"

struct controller;
struct nvmf_capsule;
struct nvmf_qpair;

typedef bool handle_command(const struct nvmf_capsule *,
    const nvme_sqe_t *, void *);

/* The whole of a handshake, and any one response, must finish this soon. */
#define	NVMFD_IO_TIMEOUT	10000		/* ms */

enum listen_kind {
	LISTEN_DISCOVERY = 1,
	LISTEN_IO = 2
};

/* One accepted connection, from accept(3SOCKET) until its thread ends. */
typedef struct nvmfd_conn {
	int			nc_fd;
	enum listen_kind	nc_kind;
	bool			nc_session;	/* discovery, past Connect */
	struct sockaddr_storage	nc_peer;
	char			nc_name[NI_MAXHOST];
	struct nvmfd_conn	*nc_next;
} nvmfd_conn_t;

/* A subsystem of the nvmft port provider. */
typedef struct nvmft_subsys {
	char		ns_nqn[NVMF_NQN_FIELD_SIZE];
	uint16_t	ns_portid;
} nvmft_subsys_t;

extern bool data_digests;
extern bool header_digests;
extern bool flow_control_disable;
extern uint32_t maxh2cdata;

/* nvmfd.c */
void	nvmfd_log(int, const char *, ...) __PRINTFLIKE(2);
void	nvmfd_peer_log(const nvmfd_conn_t *, const char *, ...) __PRINTFLIKE(2);
bool	nvmfd_conn_session(nvmfd_conn_t *conn);

/* controller.c */
void	controller_handle_admin_commands(struct controller *c,
    handle_command *cb, void *cb_arg);
struct controller *init_controller(struct nvmf_qpair *qp,
    const nvme_identify_ctrl_t *cdata, uint_t idle_ms);
void	free_controller(struct controller *c);

/* discovery.c */
void	discovery_add_io_listener(int s);
void	discovery_connection(nvmfd_conn_t *conn);

/* io.c */
void	io_connection(nvmfd_conn_t *conn);

/* nvmft.c */
void	init_nvmft(void);
int	nvmft_handoff_qpair(struct nvmf_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data, bool *refusedp);
int	nvmft_subsystems(nvmft_subsys_t **subsp, uint_t *countp,
    uint64_t *genctrp);

#endif /* _NVMFD_INTERNAL_H */

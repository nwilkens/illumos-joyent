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
 *
 * nvmfd is the userland NVMe-over-Fabrics target daemon.  It listens on TCP
 * for both the Discovery controller and the I/O subsystem, accepts Fabrics
 * associations with libnvmf (running the CONNECT handshake in userland), then
 * hands the established association off to the kernel nvmft COMSTAR/STMF port
 * provider via /dev/nvmft/admin.
 *
 * The FreeBSD daemon could optionally serve I/O entirely in userland from a
 * block-device backend (devices.c/ctl.c).  On illumos the namespace/LU data
 * lives behind STMF (sbdadm/stmfadm), so this port drops the userland block
 * backend entirely: nvmfd always hands the I/O association to the kernel.
 * Thus there is no devices.c, and ctl.c (the FreeBSD CTL backend) is replaced
 * by nvmft.c (the STMF handoff).
 */

#ifndef	_NVMFD_INTERNAL_H
#define	_NVMFD_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include "nvme_reg.h"		/* nvme_sqe_t / nvme_cqe_t (driver-private) */

/*
 * The illumos NVMe/Fabrics typedefs (nvme_sqe_t, nvme_identify_ctrl_t,
 * nvmf_fabric_connect_cmd_t, nvmf_fabric_connect_data_t,
 * nvmf_association_params_t) come from the headers above.
 */
struct controller;
struct nvmf_capsule;
struct nvmf_qpair;

typedef bool handle_command(const struct nvmf_capsule *,
    const nvme_sqe_t *, void *);

extern bool data_digests;
extern bool header_digests;
extern bool flow_control_disable;
extern uint32_t maxh2cdata;

/* controller.c */
void	controller_handle_admin_commands(struct controller *c,
    handle_command *cb, void *cb_arg);
struct controller *init_controller(struct nvmf_qpair *qp,
    const nvme_identify_ctrl_t *cdata);
void	free_controller(struct controller *c);

/* discovery.c */
void	init_discovery(void);
void	handle_discovery_socket(int s);
void	discovery_add_io_controller(int s, const char *subnqn);

/* io.c */
void	init_io(const char *subnqn);
void	handle_io_socket(int s);
void	shutdown_io(void);

/* nvmft.c - kernel handoff to the COMSTAR/STMF port provider (replaces ctl.c) */
void	init_nvmft(const char *subnqn,
    const nvmf_association_params_t *params);
void	nvmft_handoff_qpair(struct nvmf_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data);
void	shutdown_nvmft(const char *subnqn);

#endif /* _NVMFD_INTERNAL_H */

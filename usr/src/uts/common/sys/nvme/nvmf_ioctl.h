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
 * Shared user/kernel ioctl contract for the NVMe-over-Fabrics COMSTAR target
 * port provider (nvmft) and the Fabrics host (nvmf).  This header carries only
 * the small, stable ABI that userland needs: the packed-nvlist carrier struct
 * and the ioctl command numbers.  The richer protocol structures stay in
 * <sys/nvme/nvmf.h>; the driver-private defaults stay in the kernel-only
 * io/nvmf/nvmf_core.h, which now includes this header for the carrier.
 *
 * The nvmft target control ioctls (NVMFT_IOC_*) operate on /dev/nvmft/admin.
 * NVMFT_IOC_SUBSYS_* let nvmfadm(8) create and destroy a subsystem
 * stmf_local_port_t (one SubNQN -> one STMF local port) without going through
 * STMF provider configuration.  Once a subsystem port exists, the namespace
 * (LU) mapping is done with the existing sbdadm/stmfadm view-entry path; NVMe
 * NSID == STMF LUN + 1.  NVMFT_IOC_HANDOFF lets the userland target daemon
 * (nvmfd) hand an accepted, CONNECT-negotiated queue pair (socket fd plus
 * transport/CONNECT parameters) to the kernel, replacing FreeBSD's
 * CTL_NVMF_HANDOFF.
 */

#ifndef	_SYS_NVME_NVMF_IOCTL_H
#define	_SYS_NVME_NVMF_IOCTL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Control device exported by the nvmft driver.  The driver creates a single
 * pseudo minor node named "admin" (instance 0).  There is no devfsadm devlink
 * rule, so the stable path used by nvmfd(8) and nvmfadm(8) is the /devices
 * minor node itself.
 */
#define	NVMFT_DRIVER	"nvmft"
#define	NVMFT_DEV	"/devices/pseudo/nvmft@0:admin"

/*
 * (data, size) is the userspace buffer for a packed nvlist.
 *
 * For requests that copyout an nvlist, len is the amount of data copied out to
 * *data.  If size is zero, no data is copied and len is set to the required
 * buffer size.  This is the single carrier for every nvlist-based ioctl in both
 * the host and target control paths.
 */
struct nvmf_ioc_nv {
	void	*data;
	size_t	len;
	size_t	size;
};

/*
 * ioctl command magic.  illumos uses a flat command-number convention for
 * driver ioctls.  The host (/dev/nvmf, /dev/nvmeX) commands keep their FreeBSD
 * ordinals (200-205, defined in io/nvmf/nvmf_core.h); the target port-provider
 * control commands (this header) start at 100 so the two ranges never collide.
 */
#define	NVMF_IOC		(('n' << 8))

/*
 * Operations on /dev/nvmft/admin.
 *
 * NVMFT_IOC_SUBSYS_CREATE  Create and register one subsystem stmf_local_port_t
 *			    for a SubNQN.  Argument: struct nvmf_ioc_nv with a
 *			    packed request nvlist (see below).
 * NVMFT_IOC_SUBSYS_DELETE  Destroy a subsystem by SubNQN.  The port must have
 *			    no active controllers (no connected hosts).
 *			    Argument: struct nvmf_ioc_nv with a packed nvlist
 *			    { subnqn }.
 * NVMFT_IOC_SUBSYS_LIST    Copy out the list of currently registered SubNQNs.
 *			    Argument: struct nvmf_ioc_nv; on return the nvlist
 *			    holds a string array "subnqns".
 */
#define	NVMFT_IOC_SUBSYS_CREATE	(NVMF_IOC | 100)
#define	NVMFT_IOC_SUBSYS_DELETE	(NVMF_IOC | 101)
#define	NVMFT_IOC_SUBSYS_LIST	(NVMF_IOC | 102)

/*
 * NVMFT_IOC_HANDOFF  Hand an established controller queue pair off to the
 *		      kernel.  This is the core of the userland target daemon
 *		      (nvmfd): after accepting a TCP connection and running the
 *		      Fabrics CONNECT handshake with libnvmf, the daemon packs
 *		      the qpair (socket fd + negotiated transport/CONNECT
 *		      parameters) with nvmf_handoff_controller_qpair() and
 *		      issues this ioctl.  Argument: struct nvmf_ioc_nv with a
 *		      packed handoff nvlist (see below).  This replaces FreeBSD's
 *		      CTL_NVMF/CTL_NVMF_HANDOFF on /dev/cam/ctl.
 *
 * The kernel handoff agent unpacks the nvlist, validates the "cmd"/"data"
 * binary sizes, reads the SubNQN from the CONNECT data's nfcd_subnqn field
 * (there is no separate top-level subnqn key in the handoff nvlist) to look up
 * the target subsystem's nvmft_port_t, then dispatches on the CONNECT command's
 * queue id: queue id 0 calls nvmft_handoff_admin_queue(), a nonzero queue id
 * calls nvmft_handoff_io_queue().  On success the kernel transport has taken its
 * own reference on the socket, so the daemon should close its fd (the kernel
 * reference keeps the connection alive until the qpair is torn down); on failure
 * the daemon retains ownership of the fd.
 */
#define	NVMFT_IOC_HANDOFF	(NVMF_IOC | 103)

/*
 * Handoff nvlist keys for NVMFT_IOC_HANDOFF.  The layout is identical to the
 * nvlist produced by libnvmf's nvmf_handoff_controller_qpair() so that the
 * same packer serves the FreeBSD CTL path and this illumos STMF path:
 *
 *   number	trtype		enum nvmf_trtype (e.g. NVMF_TRTYPE_TCP).
 *   nvlist	params		Transport qpair handoff parameters (below).
 *   binary	cmd		struct nvmf_fabric_connect_cmd  (64 bytes).
 *   binary	data		struct nvmf_fabric_connect_data (1024 bytes).
 *
 * The transport-independent keys of the "params" sub-nvlist are:
 *
 *   bool	admin
 *   bool	sq_flow_control
 *   number	qsize
 *   number	sqhd
 *
 * For the TCP transport, "params" additionally carries:
 *
 *   number	fd		Userland socket file descriptor (adopted by
 *				the kernel transport on success).
 *   number	rxpda
 *   number	txpda
 *   bool	header_digests
 *   bool	data_digests
 *   number	maxr2t
 *   number	maxh2cdata
 *   number	max_icd
 */
#define	NVMFT_NV_TRTYPE		"trtype"
#define	NVMFT_NV_PARAMS		"params"
#define	NVMFT_NV_CMD		"cmd"
#define	NVMFT_NV_DATA		"data"

/*
 * Request nvlist keys for NVMFT_IOC_SUBSYS_CREATE.  Only "subnqn" is required;
 * the rest fall back to the Fabrics defaults (NVMF_MAX_IO_ENTRIES, NVMF_IOCCSZ,
 * NVMF_IORCSZ, NVMF_NN) when absent, so the minimal "export one namespace" call
 * is just { subnqn }.
 *
 *   string	subnqn		Subsystem NQN (required).
 *   string	serial		Controller serial number (optional; derived
 *				from the host id when absent).
 *   number	portid		STMF/relative port id (optional, default 1).
 *   number	max_io_qsize	Max I/O queue entries (optional).
 *   number	ioccsz		I/O command capsule size in bytes; reported to
 *				hosts in 16-byte units (optional).
 *   number	iorcsz		I/O response capsule size in bytes; reported to
 *				hosts in 16-byte units (optional).
 *   number	nn		Number of namespaces (optional).
 */
#define	NVMFT_NV_SUBNQN		"subnqn"
#define	NVMFT_NV_SERIAL		"serial"
#define	NVMFT_NV_PORTID		"portid"
#define	NVMFT_NV_MAX_IO_QSIZE	"max_io_qsize"
#define	NVMFT_NV_IOCCSZ		"ioccsz"
#define	NVMFT_NV_IORCSZ		"iorcsz"
#define	NVMFT_NV_NN		"nn"

/* Reply nvlist key for NVMFT_IOC_SUBSYS_LIST: string array of SubNQNs. */
#define	NVMFT_NV_SUBNQNS	"subnqns"

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NVME_NVMF_IOCTL_H */

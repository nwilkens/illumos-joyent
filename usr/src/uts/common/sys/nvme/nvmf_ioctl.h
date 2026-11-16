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
 * io/nvmf/nvmf_core.h.
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
 * The control nodes of the nvmft and nvmf_host drivers, by the links that
 * devfsadm(8) makes for the "admin" and "nvmf" minor nodes.
 */
#define	NVMFT_DRIVER	"nvmft"
#define	NVMFT_DEV	"/dev/nvmft/admin"
#define	NVMF_HOST_DEV	"/dev/nvmf"

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
 * driver ioctls.  The host commands keep their FreeBSD ordinals, from 200;
 * the target port-provider control commands start at 100.
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
 * NVMFT_IOC_SUBSYS_LIST    Copy out the registered subsystems.  Argument:
 *			    struct nvmf_ioc_nv; on return the nvlist holds
 *			    the reply keys below.
 */
#define	NVMFT_IOC_SUBSYS_CREATE	(NVMF_IOC | 100)
#define	NVMFT_IOC_SUBSYS_DELETE	(NVMF_IOC | 101)
#define	NVMFT_IOC_SUBSYS_LIST	(NVMF_IOC | 102)

/*
 * NVMFT_IOC_HANDOFF  Hand a controller queue pair to the kernel.  nvmfd(8)
 *		      accepts the connection, receives the Connect command and
 *		      its data with libnvmf, and packs them with the socket and
 *		      the transport parameters in a handoff nvlist (see below).
 *		      Argument: struct nvmf_ioc_nv.  The subsystem is the one
 *		      that the Connect data names.
 *
 * If the ioctl fails, the kernel has not touched the socket, and the Connect
 * is still the caller's to answer.  If it returns one of the values below, the
 * kernel has its own hold on the socket and has sent the Connect response, so
 * the caller sends nothing more and closes its descriptor.
 */
#define	NVMFT_IOC_HANDOFF	(NVMF_IOC | 103)

#define	NVMFT_HANDOFF_CONNECTED	0	/* the queue is connected */
#define	NVMFT_HANDOFF_REFUSED	1	/* the kernel refused the Connect */

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

/*
 * Reply nvlist keys for NVMFT_IOC_SUBSYS_LIST.  The two arrays have one
 * element for each subsystem, in the same order.  The generation changes
 * whenever a subsystem is created or deleted.
 *
 *   string array	subnqns
 *   uint16 array	portids
 *   number		generation
 */
#define	NVMFT_NV_SUBNQNS	"subnqns"
#define	NVMFT_NV_PORTIDS	"portids"
#define	NVMFT_NV_GENERATION	"generation"

/*
 * Operations on /dev/nvmf, the control node of the nvmf_host driver.  The
 * argument is a struct nvmf_ioc_nv with a packed nvlist, except for the two
 * disconnects, which take none.
 *
 * NVMF_HANDOFF_HOST	  Hand the queue pairs of a new association to the
 *			  kernel.
 * NVMF_DISCONNECT_HOST,
 * NVMF_DISCONNECT_ALL	  Disconnect the association of the node.
 * NVMF_RECONNECT_PARAMS  Copy out the "rparams" of the last handoff.
 * NVMF_RECONNECT_HOST	  Hand over new queue pairs for a lost association.
 * NVMF_CONNECTION_STATUS Copy out the state of the association.
 * NVMF_LIST_CONTROLLER	  Copy out the controller and its namespaces.  This
 *			  one is not a FreeBSD command.
 */
#define	NVMF_HANDOFF_HOST	(NVMF_IOC | 200)
#define	NVMF_DISCONNECT_HOST	(NVMF_IOC | 201)
#define	NVMF_DISCONNECT_ALL	(NVMF_IOC | 202)
#define	NVMF_RECONNECT_PARAMS	(NVMF_IOC | 203)
#define	NVMF_RECONNECT_HOST	(NVMF_IOC | 204)
#define	NVMF_CONNECTION_STATUS	(NVMF_IOC | 205)
#define	NVMF_LIST_CONTROLLER	(NVMF_IOC | 206)

/*
 * A host qpair handoff nvlist has the keys of the "params" nvlist above and
 *
 *   number	sqtail
 *   number	max_icd		(TCP)
 *
 * The fields in the nvlist for NVMF_HANDOFF_HOST and NVMF_RECONNECT_HOST are:
 *
 *   number			trtype
 *   number			kato	(optional)
 *   number			reconnect_delay (optional)
 *   number			controller_loss_timeout (optional)
 *   qpair handoff nvlist	admin
 *   qpair handoff nvlist array	io
 *   binary			cdata	nvme_identify_ctrl_t
 *   NVMF_RECONNECT_PARAMS nvlist	rparams
 *
 * The fields in the nvlist for NVMF_RECONNECT_PARAMS are:
 *
 *   binary			dle	nvmf_discovery_log_page_entry_t
 *   string			hostnqn
 *   number			num_io_queues
 *   number			kato	(optional)
 *   number			reconnect_delay (optional)
 *   number			controller_loss_timeout (optional)
 *   number			io_qsize
 *   bool			sq_flow_control
 *   bool			header_digests	(TCP)
 *   bool			data_digests	(TCP)
 *
 * The fields in the nvlist for NVMF_CONNECTION_STATUS are:
 *
 *   bool			connected
 *   string			state
 *   timespec nvlist		last_disconnect
 *    number			tv_sec
 *    number			tv_nsec
 */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NVME_NVMF_IOCTL_H */

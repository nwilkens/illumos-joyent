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

#ifndef _SYS_NVME_NVMF_RDMA_H
#define	_SYS_NVME_NVMF_RDMA_H

/*
 * The control interface of nvmf_rdma, the NVMe over Fabrics RDMA transport
 * of the nvmft target: /dev/nvmf_rdma, in the global zone, with all
 * privileges.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	NVMF_RDMA_DEV		"/dev/nvmf_rdma"

#define	NVMF_RDMA_IOC		(('N' << 24) | ('R' << 16))
#define	NVMF_RDMA_IOC_LISTEN	(NVMF_RDMA_IOC | 1)
#define	NVMF_RDMA_IOC_UNLISTEN	(NVMF_RDMA_IOC | 2)

#define	NVMF_RDMA_PORT		4420
#define	NVMF_RDMA_MAX_PEERS	64
#define	NVMF_RDMA_MAX_LISTENERS	64
#define	NVMF_RDMA_ICD_DEFAULT	0xffffffffU

/*
 * Listen on one IPv4 address and port for the listed peers only.  The
 * port defaults to 4420; on iWARP it is taken from the host TCP port
 * space, so NVMe/TCP on the same address needs another one (4421).  Zero
 * limits take the defaults.  nrl_id comes back for UNLISTEN.
 */
typedef struct nvmf_rdma_listen {
	uint32_t	nrl_addr;	/* network order */
	uint16_t	nrl_port;	/* host order */
	uint16_t	nrl_npeers;
	uint32_t	nrl_peers[NVMF_RDMA_MAX_PEERS];	/* network order */
	uint32_t	nrl_io_entries;	/* largest I/O queue */
	uint32_t	nrl_max_qid;
	uint32_t	nrl_icd;	/* in-capsule data bytes */
	uint32_t	nrl_id;
} nvmf_rdma_listen_t;

typedef struct nvmf_rdma_unlisten {
	uint32_t	nru_id;
} nvmf_rdma_unlisten_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NVME_NVMF_RDMA_H */

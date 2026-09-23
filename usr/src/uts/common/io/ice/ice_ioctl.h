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
 * Copyright 2026 RackTop Systems, Inc.
 * Copyright 2026 Edgecast Cloud LLC.
 */

#ifndef _ICE_IOCTL_H
#define	_ICE_IOCTL_H

/*
 * Private firmware diagnostic ioctls for ice(4D).  They arrive as I_STR
 * M_IOCTL messages on the link's DLPI stream and reach ice_m_ioctl().  No
 * tool in the gate uses them, so this header stays with the driver.
 *
 * Every command requires the caller to be in the global zone and to hold
 * both {PRIV_SYS_DEVICES} and {PRIV_SYS_CONFIG}: firmware logging and debug
 * data are card-wide, and an exclusive-stack zone that owns the link must not
 * reach them.  Each command takes exactly one structure below; ioc_count must
 * equal its size.
 *
 * ICE_IOC_FWLOG_GET	Read the firmware logging level of one module
 *			(ifc_module) and the delivery flags.
 * ICE_IOC_FWLOG_SET	Set the level of one module, or of every module
 *			with ICE_FWLOG_MODULE_ALL, the minimum number of log
 *			entries per event, and whether firmware sends log
 *			events to this PF.  A device reset clears the setting.
 * ICE_IOC_FWLOG_READ	Take up to ICE_IOC_BUFSZ bytes of queued log event
 *			data.  The driver queues at most ICE_FWLOG_RING_SIZE
 *			bytes; events that do not fit are dropped and counted.
 * ICE_IOC_FWDUMP	Read one block of internal debug data (admin queue
 *			command 0xFF08).  Repeat with the returned next
 *			table and offset until the firmware ends the cluster.
 *			Only the clusters FreeBSD allows are accepted.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	ICE_IOC			(('I' << 24) | ('C' << 16) | ('E' << 8))
#define	ICE_IOC_FWLOG_GET	(ICE_IOC | 0x01)
#define	ICE_IOC_FWLOG_SET	(ICE_IOC | 0x02)
#define	ICE_IOC_FWLOG_READ	(ICE_IOC | 0x03)
#define	ICE_IOC_FWDUMP		(ICE_IOC | 0x04)

/* Largest data block one command moves. */
#define	ICE_IOC_BUFSZ		4096

/* Log event bytes the driver queues for ICE_IOC_FWLOG_READ. */
#define	ICE_FWLOG_RING_SIZE	(64 * 1024)

/* Firmware log modules (ice_aqc_fw_logging_mod) and levels. */
#define	ICE_FWLOG_NMODULES	32
#define	ICE_FWLOG_MODULE_ALL	0xffffffffu
#define	ICE_FWLOG_LEVEL_MAX	4	/* 0 none ... 4 verbose */
#define	ICE_FWLOG_RES_MIN	1
#define	ICE_FWLOG_RES_MAX	128

/* ifc_flags */
#define	ICE_FWLOG_F_ARQ		0x01	/* firmware sends events to this PF */
#define	ICE_FWLOG_F_REGISTERED	0x02	/* GET only: this PF is registered */

typedef struct ice_ioc_fwlog_cfg {
	uint32_t	ifc_module;
	uint32_t	ifc_level;
	uint32_t	ifc_flags;
	uint32_t	ifc_resolution;
} ice_ioc_fwlog_cfg_t;

/* All fields are outputs; ifr_dropped counts events since the last read. */
typedef struct ice_ioc_fwlog_read {
	uint32_t	ifr_len;	/* bytes in ifr_buf */
	uint32_t	ifr_dropped;
	uint8_t		ifr_buf[ICE_IOC_BUFSZ];
} ice_ioc_fwlog_read_t;

/* The next_* fields and ifd_len are outputs. */
typedef struct ice_ioc_fwdump {
	uint32_t	ifd_cluster;
	uint32_t	ifd_table;
	uint32_t	ifd_offset;
	uint32_t	ifd_next_cluster;
	uint32_t	ifd_next_table;
	uint32_t	ifd_next_offset;
	uint32_t	ifd_len;	/* bytes in ifd_buf */
	uint8_t		ifd_buf[ICE_IOC_BUFSZ];
} ice_ioc_fwdump_t;

#ifdef __cplusplus
}
#endif

#endif /* _ICE_IOCTL_H */

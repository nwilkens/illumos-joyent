/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * This file is part of the Chelsio T4 support code.
 *
 * Copyright (C) 2011-2013 Chelsio Communications.  All rights reserved.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the LICENSE file included in this
 * release for licensing terms and conditions.
 */

/*
 * Copyright 2025 Oxide Computer Company
 */

#ifndef __T4NEX_H
#define	__T4NEX_H

#ifdef __cplusplus
extern "C" {
#endif

#define	T4_IOCTL		((('t' << 16) | '4') << 8)
#define	T4_IOCTL_PCIGET32	(T4_IOCTL + 1)
#define	T4_IOCTL_PCIPUT32	(T4_IOCTL + 2)
#define	T4_IOCTL_GET32		(T4_IOCTL + 3)
#define	T4_IOCTL_PUT32		(T4_IOCTL + 4)
#define	T4_IOCTL_REGDUMP	(T4_IOCTL + 5)
#define	T4_IOCTL_DEVLOG		(T4_IOCTL + 6)
#define	T4_IOCTL_LOAD_FW	(T4_IOCTL + 7)
#define	T4_IOCTL_GET_CUDBG	(T4_IOCTL + 8)
#define	T4_IOCTL_OFLD_TEST	(T4_IOCTL + 9)

/*
 * Offload core test operations (T4_IOCTL_OFLD_TEST).  They need
 * PRIV_SYS_NET_CONFIG in the global zone and the rdma-enable property.
 */
typedef enum t4_ofld_test_op {
	T4_OFLD_TEST_OPEN = 1,
	T4_OFLD_TEST_CLOSE,
	T4_OFLD_TEST_LISTEN,
	T4_OFLD_TEST_UNLISTEN,
	T4_OFLD_TEST_CONNECT,
	T4_OFLD_TEST_SEND,
	T4_OFLD_TEST_DISCONNECT,
	T4_OFLD_TEST_ABORT,
	T4_OFLD_TEST_STATUS,
	T4_OFLD_TEST_TPT
} t4_ofld_test_op_t;

#define	T4_OFLD_TEST_NCONN	16

typedef struct t4_ofld_test_conn {
	uint32_t	totc_tid;
	uint32_t	totc_flags;
	uint32_t	totc_snd_isn;
	uint32_t	totc_rcv_isn;
	uint64_t	totc_rx_bytes;
	uint64_t	totc_tx_bytes;
} t4_ofld_test_conn_t;

#define	T4_OFLD_TCF_PASSIVE	0x01
#define	T4_OFLD_TCF_EST		0x02
#define	T4_OFLD_TCF_PEER_FIN	0x04
#define	T4_OFLD_TCF_FIN_SENT	0x08
#define	T4_OFLD_TCF_FIN_ACKED	0x10
#define	T4_OFLD_TCF_ABORTED	0x20
#define	T4_OFLD_TCF_RELEASED	0x40

typedef struct t4_ofld_test {
	uint32_t	tot_op;
	uint32_t	tot_port;
	uint32_t	tot_laddr;	/* IPv4, network order */
	uint32_t	tot_faddr;
	uint16_t	tot_lport;	/* network order */
	uint16_t	tot_fport;
	uint16_t	tot_vlan;	/* 0xfff: untagged */
	uint8_t		tot_dmac[6];
	uint32_t	tot_id;		/* stid or tid, in and out */
	uint32_t	tot_len;
	uint32_t	tot_timeout_ms;
	int32_t		tot_status;	/* CPL status, out */
	uint32_t	tot_snd_isn;
	uint32_t	tot_rcv_isn;
	uint32_t	tot_nconn;
	uint32_t	tot_accepts;
	uint32_t	tot_refused;
	uint32_t	tot_events;
	uint32_t	tot_pad;	/* same layout for ILP32 and LP64 */
	t4_ofld_test_conn_t tot_conn[T4_OFLD_TEST_NCONN];
} t4_ofld_test_t;

enum {
	T4_CTXT_EGRESS,
	T4_CTXT_INGRESS,
	T4_CTXT_FLM
};

struct t4_reg32_cmd {
	uint32_t reg;
	uint32_t value;
};

#define	T4_REGDUMP_SIZE (160 * 1024)
#define	T6_REGDUMP_SIZE (332 * 1024)
#define	T5_REGDUMP_SIZE (332 * 1024)

struct t4_regdump {
	uint32_t version;
	uint32_t len;
	uint32_t data[];
};

struct t4_sge_context {
	uint32_t version;
	uint32_t mem_id;
	uint32_t addr;
	uint32_t len;
	uint8_t  *data;
};

struct t4_mem_range {
	uint32_t addr;
	uint32_t len;
	uint32_t *data;
};

struct t4_tid_info {
	uint32_t len;
	uint32_t *data;
};

struct t4_mbox {
	uint32_t len;
	uint32_t *data;
};

struct t4_cim_la {
	uint32_t len;
	uint32_t *data;
};

struct t4_ibq {
	uint32_t len;
	uint32_t *data;
};

struct t4_edc {
	uint32_t len;
	uint32_t mem;
	uint32_t pos;
	char *data;
};

struct t4_cim_qcfg {
	uint16_t base[14];
	uint16_t size[14];
	uint16_t thres[6];
	uint32_t stat[4 * (6 + 8)];
	uint32_t obq_wr[2 * (8)];
	uint32_t num_obq;
};

#define	T4_DEVLOG_SIZE	32768
struct t4_devlog {
	uint32_t len;
	uint32_t data[0];
};

struct t4_ldfw {
	uint32_t len;
	uint32_t data[0];
};

struct t4_cudbg_dump {
	uint8_t wr_flash;
	uint8_t bitmap[16];
	uint32_t len;
	uint32_t data[0];
};

#ifdef __cplusplus
}
#endif

#endif /* __T4NEX_H */

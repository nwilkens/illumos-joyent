/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * The daemon passes subsystem parameters and handed-off queues to the kernel
 * in nvlists.  The kernel checks the values that size its objects: a host
 * NQN becomes an STMF identifier with a one-byte length, and the admin flag
 * of a queue selects how its commands are handled.
 */
#include "local.h"

#define	CHECK(x) \
	do { \
		if (!(x)) { \
			(void) fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
			abort(); \
		} \
	} while (0)

static nvmf_fabric_connect_cmd_t
cmd_ok(uint16_t qid, uint16_t sqsize)
{
	nvmf_fabric_connect_cmd_t c;

	memset(&c, 0, sizeof (c));
	c.nfcc_opcode = 0x7f;
	c.nfcc_fctype = NVMF_FCTYPE_CONNECT;
	c.nfcc_qid = qid;
	c.nfcc_sqsize = sqsize;
	c.nfcc_kato = qid == 0 ? 120000 : 0;
	return (c);
}

static nvmf_fabric_connect_data_t
data_ok(uint16_t qid)
{
	nvmf_fabric_connect_data_t d;

	memset(&d, 0, sizeof (d));
	d.nfcd_hostid[3] = 7;
	d.nfcd_cntlid = qid == 0 ? NVMF_CNTLID_DYNAMIC : 5;
	strcpy((char *)d.nfcd_subnqn, "nqn.2024-01.com.example:disk0");
	strcpy((char *)d.nfcd_hostnqn, "nqn.2014-08.org.nvmexpress:host");
	return (d);
}

static void
params(void)
{
	const uint64_t sqe = sizeof (nvme_sqe_t), cqe = sizeof (nvme_cqe_t);

	CHECK(nvmft_port_params_valid(1, 1024, sqe + 16384, cqe, 1024));
	CHECK(nvmft_port_params_valid(UINT16_MAX, 2, sqe, cqe, 1));
	CHECK(nvmft_port_params_valid(0, 65536, sqe, cqe, NVMFT_MAX_NSID));

	CHECK(!nvmft_port_params_valid(UINT16_MAX + 1, 1024, sqe, cqe, 1));
	/* A queue size of 0 would make MQES wrap. */
	CHECK(!nvmft_port_params_valid(1, 0, sqe, cqe, 1));
	CHECK(!nvmft_port_params_valid(1, 1, sqe, cqe, 1));
	CHECK(!nvmft_port_params_valid(1, 65537, sqe, cqe, 1));
	CHECK(!nvmft_port_params_valid(1, 1ULL << 32, sqe, cqe, 1));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe - 16, cqe, 1));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe + 8, cqe, 1));
	CHECK(!nvmft_port_params_valid(1, 1024, (1ULL << 32) + sqe, cqe, 1));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe, 0, 1));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe, cqe + 4, 1));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe, 1ULL << 32, 1));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe, cqe, 0));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe, cqe,
	    NVMFT_MAX_NSID + 1));
	CHECK(!nvmft_port_params_valid(1, 1024, sqe, cqe, 1ULL << 32));
}

static void
handoff(void)
{
	nvmf_fabric_connect_cmd_t c = cmd_ok(0, 31);
	nvmf_fabric_connect_data_t d = data_ok(0);

	CHECK(nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 32, &c, &d));
	/* A transport type is not truncated into a valid one. */
	CHECK(!nvmft_handoff_valid((1ULL << 32) | NVMF_TRTYPE_TCP, B_TRUE, 32,
	    &c, &d));
	CHECK(!nvmft_handoff_valid(0x100, B_TRUE, 32, &c, &d));
	/* The admin flag and the Connect's queue ID must agree. */
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_FALSE, 32, &c, &d));
	/* So must the queue size. */
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 64, &c, &d));
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 0, &c, &d));
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 65536 + 32, &c,
	    &d));
	/* A new association has no controller ID yet. */
	d.nfcd_cntlid = 5;
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 32, &c, &d));

	/* A host NQN of 256 bytes would have an identifier length of 0. */
	d = data_ok(0);
	memset(d.nfcd_hostnqn, 'h', sizeof (d.nfcd_hostnqn));
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 32, &c, &d));
	memset(d.nfcd_hostnqn, 0, sizeof (d.nfcd_hostnqn));
	memset(d.nfcd_hostnqn, 'h', NVMF_NQN_MAX_LEN + 1);
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 32, &c, &d));
	d.nfcd_hostnqn[NVMF_NQN_MAX_LEN] = '\0';
	CHECK(nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 32, &c, &d));
	d.nfcd_hostnqn[0] = '\0';
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 32, &c, &d));
	d = data_ok(0);
	memset(d.nfcd_hostid, 0, sizeof (d.nfcd_hostid));
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 32, &c, &d));

	c = cmd_ok(3, 127);
	d = data_ok(3);
	CHECK(nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_FALSE, 128, &c, &d));
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_TRUE, 128, &c, &d));
	d.nfcd_cntlid = NVMF_CNTLID_DYNAMIC;
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_FALSE, 128, &c, &d));
	d = data_ok(3);
	c.nfcc_fctype = NVMF_FCTYPE_PROPERTY_SET;
	CHECK(!nvmft_handoff_valid(NVMF_TRTYPE_TCP, B_FALSE, 128, &c, &d));
}

int
main(void)
{
	params();
	handoff();
	(void) printf("subsystem and handoff checks passed\n");
	return (0);
}

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
 * Run nvmft_connect_cmd_valid() and nvmft_connect_data_valid() against each
 * rule libnvmf's nvmf_accept() applies, then against random capsules checked
 * by an independent model of the same rules.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "connect.h"

#define	MAX_IO	1024
#define	MAX_ADMIN	NVME_MAX_ADMIN_ENTRIES
#define	DATA_LEN	sizeof (nvmf_fabric_connect_data_t)
#define	CMD_OFF(f)	offsetof(nvmf_fabric_connect_cmd_t, f)
#define	DATA_OFF(f)	offsetof(nvmf_fabric_connect_data_t, f)

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
expect_cmd(const nvmf_fabric_connect_cmd_t *c, size_t len, uint16_t qid,
    uint16_t sqsize, int ok, uint8_t sct, uint8_t sc, int invalid,
    uint16_t ipo, int line)
{
	nvmft_connect_status_t st;

	memset(&st, 0xa5, sizeof (st));
	if (nvmft_connect_cmd_valid(c, len, qid, sqsize, MAX_ADMIN, MAX_IO,
	    &st) != ok) {
		fprintf(stderr, "line %d: valid != %d\n", line, ok);
		abort();
	}
	if (ok)
		return;
	if (st.ncs_sct != sct || st.ncs_sc != sc ||
	    st.ncs_invalid != invalid || (invalid && (st.ncs_ipo != ipo ||
	    st.ncs_iattr != B_FALSE))) {
		fprintf(stderr, "line %d: sct %u sc %#x invalid %d ipo %u\n",
		    line, st.ncs_sct, st.ncs_sc, st.ncs_invalid, st.ncs_ipo);
		abort();
	}
}

#define	CMD_OK(c, l, q, s)	expect_cmd(c, l, q, s, 1, 0, 0, 0, 0, __LINE__)
#define	CMD_BAD(c, l, q, s, sct, sc) \
	expect_cmd(c, l, q, s, 0, sct, sc, 0, 0, __LINE__)
#define	CMD_INVAL(c, l, q, s, off) \
	expect_cmd(c, l, q, s, 0, NVME_CQE_SCT_SPECIFIC, \
	    NVMF_FABRIC_SC_INVALID_PARAM, 1, off, __LINE__)

static void
expect_data(const nvmf_fabric_connect_cmd_t *c,
    const nvmf_fabric_connect_data_t *d, int ok, uint16_t ipo, int line)
{
	nvmft_connect_status_t st;

	if (nvmft_connect_data_valid(c, d, &st) != ok) {
		fprintf(stderr, "line %d: data valid != %d\n", line, ok);
		abort();
	}
	if (!ok && (!st.ncs_invalid || !st.ncs_iattr || st.ncs_ipo != ipo ||
	    st.ncs_sct != NVME_CQE_SCT_SPECIFIC ||
	    st.ncs_sc != NVMF_FABRIC_SC_INVALID_PARAM)) {
		fprintf(stderr, "line %d: ipo %u\n", line, st.ncs_ipo);
		abort();
	}
}

#define	DATA_OK(c, d)		expect_data(c, d, 1, 0, __LINE__)
#define	DATA_BAD(c, d, off)	expect_data(c, d, 0, off, __LINE__)

static void
rules(void)
{
	nvmf_fabric_connect_cmd_t c;
	nvmf_fabric_connect_data_t d;

	c = cmd_ok(0, 31);
	CMD_OK(&c, DATA_LEN, 0, 31);
	c.nfcc_opcode = 0x7e;
	CMD_BAD(&c, DATA_LEN, 0, 31, NVME_CQE_SCT_GENERIC,
	    NVME_CQE_SC_GEN_INV_OPC);
	c = cmd_ok(0, 31);
	c.nfcc_fctype = NVMF_FCTYPE_PROPERTY_GET;
	CMD_BAD(&c, DATA_LEN, 0, 31, NVME_CQE_SCT_GENERIC,
	    NVME_CQE_SC_GEN_INV_OPC);
	c = cmd_ok(0, 31);
	c.nfcc_recfmt = 1;
	CMD_BAD(&c, DATA_LEN, 0, 31, NVME_CQE_SCT_SPECIFIC,
	    NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT);

	/* The Connect must repeat what the host told the transport. */
	c = cmd_ok(1, 31);
	CMD_INVAL(&c, DATA_LEN, 0, 31, CMD_OFF(nfcc_qid));
	c = cmd_ok(0, 31);
	CMD_INVAL(&c, DATA_LEN, 1, 31, CMD_OFF(nfcc_qid));
	c = cmd_ok(0, 31);
	CMD_INVAL(&c, DATA_LEN, 0, 30, CMD_OFF(nfcc_sqsize));

	/* Queue size bounds. */
	c = cmd_ok(0, 0);
	CMD_INVAL(&c, DATA_LEN, 0, 0, CMD_OFF(nfcc_sqsize));
	c = cmd_ok(0, 1);
	CMD_OK(&c, DATA_LEN, 0, 1);
	c = cmd_ok(0, MAX_ADMIN - 1);
	CMD_OK(&c, DATA_LEN, 0, MAX_ADMIN - 1);
	c = cmd_ok(0, MAX_ADMIN);
	CMD_INVAL(&c, DATA_LEN, 0, MAX_ADMIN, CMD_OFF(nfcc_sqsize));
	c = cmd_ok(0, 0xffff);
	CMD_INVAL(&c, DATA_LEN, 0, 0xffff, CMD_OFF(nfcc_sqsize));
	c = cmd_ok(3, 0);
	CMD_INVAL(&c, DATA_LEN, 3, 0, CMD_OFF(nfcc_sqsize));
	c = cmd_ok(3, MAX_IO - 1);
	CMD_OK(&c, DATA_LEN, 3, MAX_IO - 1);
	c = cmd_ok(3, MAX_IO);
	CMD_INVAL(&c, DATA_LEN, 3, MAX_IO, CMD_OFF(nfcc_sqsize));
	c = cmd_ok(3, 0xffff);
	CMD_INVAL(&c, DATA_LEN, 3, 0xffff, CMD_OFF(nfcc_sqsize));

	/* KATO is reserved on I/O queues. */
	c = cmd_ok(3, 31);
	c.nfcc_kato = 1;
	CMD_INVAL(&c, DATA_LEN, 3, 31, CMD_OFF(nfcc_kato));

	/* The data must be exactly one Connect data structure. */
	c = cmd_ok(0, 31);
	CMD_INVAL(&c, 0, 0, 31, CMD_OFF(nfcc_sgl1));
	CMD_INVAL(&c, DATA_LEN - 1, 0, 31, CMD_OFF(nfcc_sgl1));
	CMD_INVAL(&c, DATA_LEN + 1, 0, 31, CMD_OFF(nfcc_sgl1));
	CMD_INVAL(&c, (size_t)-1, 0, 31, CMD_OFF(nfcc_sgl1));

	c = cmd_ok(0, 31);
	d = data_ok(0);
	DATA_OK(&c, &d);
	memset(d.nfcd_hostid, 0, sizeof (d.nfcd_hostid));
	DATA_BAD(&c, &d, DATA_OFF(nfcd_hostid));
	d = data_ok(0);
	d.nfcd_cntlid = 1;
	DATA_BAD(&c, &d, DATA_OFF(nfcd_cntlid));
	d.nfcd_cntlid = NVMF_CNTLID_STATIC_ANY;
	DATA_BAD(&c, &d, DATA_OFF(nfcd_cntlid));

	c = cmd_ok(2, 31);
	d = data_ok(2);
	DATA_OK(&c, &d);
	d.nfcd_cntlid = NVMF_CNTLID_STATIC_MAX;
	DATA_OK(&c, &d);
	d.nfcd_cntlid = NVMF_CNTLID_STATIC_MAX + 1;
	DATA_BAD(&c, &d, DATA_OFF(nfcd_cntlid));
	d.nfcd_cntlid = NVMF_CNTLID_DYNAMIC;
	DATA_BAD(&c, &d, DATA_OFF(nfcd_cntlid));

	/* NQNs: empty, longest, one too long, and no NUL at all. */
	d = data_ok(2);
	d.nfcd_subnqn[0] = '\0';
	DATA_BAD(&c, &d, DATA_OFF(nfcd_subnqn));
	d = data_ok(2);
	memset(d.nfcd_subnqn, 'a', NVMF_NQN_MAX_LEN);
	DATA_OK(&c, &d);
	memset(d.nfcd_subnqn, 'a', NVMF_NQN_MAX_LEN + 1);
	DATA_BAD(&c, &d, DATA_OFF(nfcd_subnqn));
	memset(d.nfcd_subnqn, 'a', sizeof (d.nfcd_subnqn));
	DATA_BAD(&c, &d, DATA_OFF(nfcd_subnqn));
	d = data_ok(2);
	memset(d.nfcd_hostnqn, 'b', sizeof (d.nfcd_hostnqn));
	DATA_BAD(&c, &d, DATA_OFF(nfcd_hostnqn));
	d.nfcd_hostnqn[0] = '\0';
	DATA_BAD(&c, &d, DATA_OFF(nfcd_hostnqn));
}

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint64_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (rng);
}

static int
model_cmd(const nvmf_fabric_connect_cmd_t *c, size_t len, uint16_t qid,
    uint16_t sqsize)
{
	uint32_t qsize = (uint32_t)c->nfcc_sqsize + 1;

	if (c->nfcc_opcode != 0x7f || c->nfcc_fctype != 1 ||
	    c->nfcc_recfmt != 0 || c->nfcc_qid != qid ||
	    c->nfcc_sqsize != sqsize)
		return (0);
	if (qid == 0 && (qsize < 2 || qsize > MAX_ADMIN))
		return (0);
	if (qid != 0 && (qsize < 2 || qsize > MAX_IO || c->nfcc_kato != 0))
		return (0);
	return (len == 1024);
}

static int
model_nqn(const uint8_t *p)
{
	size_t n = 0;

	while (n < 256 && p[n] != 0)
		n++;
	return (n >= 1 && n <= 223);
}

static int
model_data(const nvmf_fabric_connect_cmd_t *c,
    const nvmf_fabric_connect_data_t *d)
{
	int i, nz = 0;

	for (i = 0; i < 16; i++)
		nz |= d->nfcd_hostid[i];
	if (nz == 0)
		return (0);
	if (c->nfcc_qid == 0 ? d->nfcd_cntlid != 0xffff :
	    d->nfcd_cntlid > 0xffef)
		return (0);
	return (model_nqn(d->nfcd_subnqn) && model_nqn(d->nfcd_hostnqn));
}

static void
fuzz(unsigned long iters)
{
	unsigned long i, accepted = 0;

	for (i = 0; i < iters; i++) {
		nvmf_fabric_connect_cmd_t c;
		nvmf_fabric_connect_data_t d;
		nvmft_connect_status_t st;
		uint16_t qid, sqsize;
		size_t len, j;
		uint8_t *p;

		/* Start from a valid capsule so the deep checks get reached. */
		qid = (next() & 1) ? 0 : (uint16_t)(next() & 7);
		sqsize = (uint16_t)(next() % 1200);
		c = cmd_ok(qid, sqsize);
		d = data_ok(qid);
		len = (next() & 15) ? DATA_LEN : (size_t)(next() & 2047);
		for (j = next() % 6; j > 0; j--) {
			p = (uint8_t *)&c;
			p[next() % sizeof (c)] = (uint8_t)next();
		}
		for (j = next() % 6; j > 0; j--) {
			p = (uint8_t *)&d;
			p[next() % sizeof (d)] = (next() & 3) ? 0 :
			    (uint8_t)next();
		}
		if (next() % 8 == 0)
			memset(d.nfcd_subnqn, 'x', sizeof (d.nfcd_subnqn));
		if (next() % 8 == 0)
			memset(d.nfcd_hostid, 0, sizeof (d.nfcd_hostid));

		if (nvmft_connect_cmd_valid(&c, len, qid, sqsize, MAX_ADMIN,
		    MAX_IO, &st) != model_cmd(&c, len, qid, sqsize)) {
			fprintf(stderr, "cmd model mismatch at %lu\n", i);
			abort();
		}
		if (nvmft_connect_data_valid(&c, &d, &st) !=
		    model_data(&c, &d)) {
			fprintf(stderr, "data model mismatch at %lu\n", i);
			abort();
		}
		accepted += model_cmd(&c, len, qid, sqsize) &&
		    model_data(&c, &d);
	}
	/* The fuzz must reach the accept path, not only reject. */
	assert(accepted > iters / 50);
	printf("fuzz: %lu capsules, %lu accepted\n", iters, accepted);
}

int
main(void)
{
	rules();
	fuzz(2000000);
	printf("connect checks passed\n");
	return (0);
}

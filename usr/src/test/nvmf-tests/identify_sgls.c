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
 * Build the Identify Controller template as nvmft_port_alloc() does, apply
 * each transport's capabilities as nvmft_update_cdata() does, and read the
 * SGLS dword at its offset in the 4 KiB structure.  NVMe/TCP must advertise
 * what it did before transports had capabilities.
 */
#include "sgls.h"

CTASSERT(sizeof (nvme_identify_ctrl_t) == 4096);

#define	SGLS_SUP_MASK	0x3u
#define	SGLS_KEYED	(1u << 2)
#define	SGLS_OFFSET	(1u << 20)
#define	SGLS_TRANSPORT	(1u << 21)

static uint32_t
sgls(const nvme_identify_ctrl_t *cd)
{
	uint32_t v;

	memcpy(&v, (const uint8_t *)cd + 536, sizeof (v));
	return (v);
}

static uint32_t
advertised(uint32_t caps)
{
	static nvme_identify_ctrl_t cd;

	_nvmf_init_io_controller_data(0, 1024, "serial", "model", "fw",
	    "nqn.2024-01.com.example:t", 1024, 64 + 16384, 16, &cd);
	nvmft_init_sgls(&cd, caps);
	return (sgls(&cd));
}

int
main(void)
{
	static nvme_identify_ctrl_t tmpl;
	uint32_t tcp, v;

	/* The template the port builds, before a transport is known. */
	_nvmf_init_io_controller_data(0, 1024, "serial", "model", "fw",
	    "nqn.2024-01.com.example:t", 1024, 64 + 16384, 16, &tmpl);
	assert(sgls(&tmpl) == (1u | SGLS_OFFSET | SGLS_TRANSPORT));

	/* NVMe/TCP: unchanged. */
	tcp = tcp_caps(NULL);
	assert(advertised(tcp) == sgls(&tmpl));

	/* RDMA with in-capsule data: keyed and offset, no transport SGL. */
	v = advertised(NVMF_QP_CAP_SGL_KEYED | NVMF_QP_CAP_SGL_OFFSET);
	assert(v == (1u | SGLS_KEYED | SGLS_OFFSET));

	/* RDMA without in-capsule data: keyed only. */
	v = advertised(NVMF_QP_CAP_SGL_KEYED | NVMF_QP_CAP_UNORDERED_DATA |
	    NVMF_QP_CAP_ALWAYS_RESPONSE | NVMF_QP_CAP_LU_DBUF);
	assert(v == (1u | SGLS_KEYED));

	/* A transport that states nothing gets nothing beyond NVM support. */
	assert(advertised(0) == 1u);
	assert((advertised(~0u) & ~(SGLS_SUP_MASK | SGLS_KEYED | SGLS_OFFSET |
	    SGLS_TRANSPORT)) == 0);

	printf("identify sgls passed: tcp %#x\n", advertised(tcp));
	return (0);
}

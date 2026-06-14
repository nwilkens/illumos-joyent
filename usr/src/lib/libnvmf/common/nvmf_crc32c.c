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
 * CRC-32C (Castagnoli) for the NVMe/TCP header and data digests.
 *
 * FreeBSD's libnvmf links sys/libkern/gsb_crc32.c and calls calculate_crc32c().
 * illumos has no userland CRC-32C primitive, so this provides the same
 * computation using the generic <sys/crc32.h> table-driven CRC32() macro the
 * rest of the system uses, with a CRC-32C table built once on first use.  This
 * is the userland twin of the kernel transport's nvmf_tcp_crc32c()
 * (uts/common/io/nvmf/nvmf_tcp.c).
 *
 * The reflected CRC-32C polynomial is 0x82F63B78 (the bit reversal of the
 * normal-form 0x1EDC6F41); <sys/crc32.h>'s CRC32() consumes data least
 * significant bit first, so the reflected form is what we feed CRC32_INIT().
 */

#include <sys/types.h>
#include <sys/crc32.h>
#include <pthread.h>

#include "internal.h"

#define	NVMF_CRC32C_POLY	0x82F63B78U

static uint32_t nvmf_crc32c_table[256];
static pthread_once_t nvmf_crc32c_once = PTHREAD_ONCE_INIT;

static void
nvmf_crc32c_build_table(void)
{
	CRC32_INIT(nvmf_crc32c_table, NVMF_CRC32C_POLY);
}

/*
 * Accumulate a running CRC-32C over buf into crc.  Callers seed the digest with
 * 0xffffffff and finalize with a ^ 0xffffffff, matching FreeBSD's
 * calculate_crc32c() convention; see compute_digest() in nvmf_tcp.c.
 */
uint32_t
nvmf_crc32c(uint32_t crc, const void *buf, size_t len)
{
	(void) pthread_once(&nvmf_crc32c_once, nvmf_crc32c_build_table);

	CRC32(crc, buf, len, crc, nvmf_crc32c_table);
	return (crc);
}

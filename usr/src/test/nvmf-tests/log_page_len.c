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
 * A Get Log Page moves (NUMD + 1) * 4 bytes, and the host's SGL must be
 * exactly that long; otherwise the data transfer ends short (NVMF_MORE),
 * which nvmft asserts cannot happen.
 */
#include "logpage.h"

static void
check(uint32_t numd, size_t sgl, uint_t want, size_t want_len)
{
	size_t len = 12345;
	uint_t sc = nvmft_log_page_len(numd, sgl, &len);

	if (sc != want || (sc == NVME_CQE_SC_GEN_SUCCESS && len != want_len)) {
		(void) fprintf(stderr, "numd %#x sgl %zu: sc %#x len %zu\n",
		    numd, sgl, sc, len);
		abort();
	}
}

int
main(void)
{
	check(127, 512, NVME_CQE_SC_GEN_SUCCESS, 512);
	check(0, 4, NVME_CQE_SC_GEN_SUCCESS, 4);
	/* An SGL longer or shorter than the log page. */
	check(0, 8, NVME_CQE_SC_GEN_INV_DSGL_LEN, 0);
	check(127, 4096, NVME_CQE_SC_GEN_INV_DSGL_LEN, 0);
	check(127, 508, NVME_CQE_SC_GEN_INV_DSGL_LEN, 0);
	/* Bounded, and NUMD + 1 must not wrap to an empty transfer. */
	check(NVMFT_MAX_LOGPAGE_LEN / 4 - 1, NVMFT_MAX_LOGPAGE_LEN,
	    NVME_CQE_SC_GEN_SUCCESS, NVMFT_MAX_LOGPAGE_LEN);
	check(NVMFT_MAX_LOGPAGE_LEN / 4, NVMFT_MAX_LOGPAGE_LEN + 4,
	    NVME_CQE_SC_GEN_INV_FLD, 0);
	check(UINT32_MAX, 0, NVME_CQE_SC_GEN_INV_FLD, 0);
	(void) printf("log page length passed\n");
	return (0);
}

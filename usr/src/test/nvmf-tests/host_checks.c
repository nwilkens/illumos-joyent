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
 * Run the nvmf_host checks on values that a target or a handoff supplies:
 * the LBA format, the active namespace list, MDTS, the handoff timeouts, and
 * the choice between sending, holding and failing block I/O.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

static nvme_identify_nsid_t
ns_ok(uint8_t lbads)
{
	nvme_identify_nsid_t d;

	memset(&d, 0, sizeof (d));
	d.id_nsize = 1000;
	d.id_nlbaf = 1;
	d.id_flbas.lba_format = 1;
	d.id_lbaf[1].lbaf_lbads = lbads;
	return (d);
}

static void
test_ns_fmt(void)
{
	nvme_identify_nsid_t d;
	uint64_t nblks;
	uint32_t blksize;
	uint_t lbads;

	for (lbads = 0; lbads < 256; lbads++) {
		d = ns_ok((uint8_t)lbads);
		nblks = 0;
		blksize = 0;
		if (lbads >= 9 && lbads <= 16) {
			assert(nvmf_ns_fmt(&d, &nblks, &blksize, NULL, 1));
			assert(blksize == 1U << lbads);
			assert(nblks == 1000);
		} else {
			assert(!nvmf_ns_fmt(&d, &nblks, &blksize, NULL, 1));
			assert(blksize == 0 && nblks == 0);
		}
	}

	d = ns_ok(12);
	d.id_dps.dp_pinfo = 1;
	assert(!nvmf_ns_fmt(&d, &nblks, &blksize, NULL, 1));

	d = ns_ok(12);
	d.id_lbaf[1].lbaf_ms = 8;
	assert(!nvmf_ns_fmt(&d, &nblks, &blksize, NULL, 1));

	d = ns_ok(12);
	d.id_flbas.lba_format = 2;
	assert(!nvmf_ns_fmt(&d, &nblks, &blksize, NULL, 1));
}

static void
test_nslist(void)
{
	static nvme_identify_nsid_list_t l;
	uint_t i;

	memset(&l, 0, sizeof (l));
	assert(nvmf_nslist_count(&l, 0) == 0);

	l.nl_nsid[0] = 1;
	l.nl_nsid[1] = 2;
	l.nl_nsid[2] = 9;
	assert(nvmf_nslist_count(&l, 0) == 3);
	assert(nvmf_nslist_count(&l, 1) == -1);
	assert(nvmf_nslist_count(&l, 9) == -1);

	l.nl_nsid[2] = 2;
	assert(nvmf_nslist_count(&l, 0) == -1);
	l.nl_nsid[2] = 1;
	assert(nvmf_nslist_count(&l, 0) == -1);
	l.nl_nsid[2] = NVME_NSID_BCAST;
	assert(nvmf_nslist_count(&l, 0) == -1);
	l.nl_nsid[2] = NVME_NSID_BCAST - 1;
	assert(nvmf_nslist_count(&l, 0) == 3);

	/* A full list of one repeated NSID must not look like progress. */
	for (i = 0; i < NVMF_NSLIST_ENTRIES; i++)
		l.nl_nsid[i] = 7;
	assert(nvmf_nslist_count(&l, 0) == -1);

	for (i = 0; i < NVMF_NSLIST_ENTRIES; i++)
		l.nl_nsid[i] = 100 + i;
	assert(nvmf_nslist_count(&l, 99) == (int)NVMF_NSLIST_ENTRIES);
	assert(nvmf_nslist_count(&l, 100) == -1);
}

static void
test_max_xfer(void)
{
	nvme_reg_cap_t cap;
	uint64_t limit = 1024 * 1024;
	uint_t mdts, mpsmin;

	cap.r = 0;
	assert(nvmf_max_xfer(cap.r, 0, 0, limit) == limit);
	assert(nvmf_max_xfer(cap.r, 1, 0, limit) == 8192);
	assert(nvmf_max_xfer(cap.r, 5, 0, limit) == 128 * 1024);
	assert(nvmf_max_xfer(cap.r, 5, 65536, limit) == 65536);
	assert(nvmf_max_xfer(cap.r, 5, 70000, limit) == 69632);
	assert(nvmf_max_xfer(cap.r, 0, 100, limit) == limit);
	assert(nvmf_max_xfer(cap.r, 0, 0, 57344) == 57344);
	assert(nvmf_max_xfer(cap.r, 0, 0, 60000) == 57344);

	/* No MDTS and no page size may turn the limit into zero. */
	for (mpsmin = 0; mpsmin < 16; mpsmin++) {
		cap.r = 0;
		cap.b.cap_mpsmin = mpsmin;
		for (mdts = 0; mdts < 256; mdts++) {
			uint64_t max = nvmf_max_xfer(cap.r, (uint8_t)mdts, 0,
			    limit);

			assert(max != 0 && max <= limit);
			if (mdts != 0 && 12 + mpsmin + mdts < 20)
				assert(max == 1ULL << (12 + mpsmin + mdts));
		}
	}
}

static void
test_timeouts(void)
{
	assert(nvmf_timeouts_valid(0, 0, 0));
	assert(!nvmf_timeouts_valid(1, 0, 0));
	assert(!nvmf_timeouts_valid(999, 0, 0));
	assert(nvmf_timeouts_valid(1000, 10, 600));
	assert(nvmf_timeouts_valid(3600 * 1000, 3600, 7 * 24 * 3600));
	assert(!nvmf_timeouts_valid(3600 * 1000 + 1, 0, 0));
	assert(!nvmf_timeouts_valid(0, 3601, 0));
	assert(!nvmf_timeouts_valid(0, 0, 7 * 24 * 3600 + 1));
	assert(!nvmf_timeouts_valid(UINT64_MAX, 0, 0));
	assert(!nvmf_timeouts_valid(0, 1ULL << 32, 0));
	assert(!nvmf_timeouts_valid(0, 0, 1ULL << 32));
}

static void
test_io_policy(void)
{
	nvmf_sc_state_t s;
	int enabled, fast;

	for (s = NVMF_SC_IDLE; s <= NVMF_SC_DETACHING; s++) {
		for (enabled = 0; enabled < 2; enabled++) {
			for (fast = 0; fast < 2; fast++) {
				nvmf_io_action_t a = nvmf_io_policy(s,
				    enabled, B_FALSE, fast);

				/* A namespace that went away always fails. */
				assert(nvmf_io_policy(s, enabled, B_TRUE,
				    fast) == NVMF_IO_FAIL);

				switch (s) {
				case NVMF_SC_ATTACHING:
				case NVMF_SC_CONNECTED:
					assert(a == (enabled ?
					    NVMF_IO_SUBMIT : NVMF_IO_FAIL));
					break;
				case NVMF_SC_RESETTING:
				case NVMF_SC_LOST:
				case NVMF_SC_RECONNECTING:
					assert(a == (fast ?
					    NVMF_IO_FAIL : NVMF_IO_HOLD));
					break;
				default:
					assert(a == NVMF_IO_FAIL);
					break;
				}
			}
		}
	}
}

int
main(void)
{
	test_ns_fmt();
	test_nslist();
	test_max_xfer();
	test_timeouts();
	test_io_policy();
	printf("host_checks: ok\n");
	return (0);
}

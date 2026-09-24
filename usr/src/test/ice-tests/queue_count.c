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

/* Run the production queue pair sizing against capability combinations. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define	MIN(a, b)		((a) < (b) ? (a) : (b))
#define	MAX(a, b)		((a) > (b) ? (a) : (b))
#define	MAX_RINGS_PER_GROUP	128
#define	DDI_DEV_T_ANY		0

typedef uint32_t u32;

struct ice_hw_common_caps {
	u32 num_rxq, num_txq, num_msix_vectors, rss_table_entry_width;
};

typedef struct ice {
	void *ice_dip;
	unsigned ice_intr_rdma;
	struct {
		struct {
			struct ice_hw_common_caps common_cap;
		} func_caps;
	} ice_hw;
} ice_t;

static int ncpus, boot_max_ncpus, max_ncpus;
static int prop;
static int have_prop;
static int logged;

static void
ice_error(ice_t *ice, const char *fmt, ...)
{
	(void) ice;
	(void) fmt;
	logged++;
}

static int
ddi_prop_get_int(int dev, void *dip, int flags, const char *name, int dflt)
{
	(void) dev;
	(void) dip;
	(void) flags;
	(void) name;
	return (have_prop ? prop : dflt);
}

static uint32_t ice_prop_get_num_queues(ice_t *);
#include "ice_queue_body.h"

static unsigned rdma;

static uint32_t
limit(int cpus, u32 rxq, u32 txq, u32 msix, u32 width, int conf)
{
	ice_t ice = { 0 };

	ice.ice_intr_rdma = rdma;

	ncpus = cpus;
	boot_max_ncpus = -1;
	max_ncpus = 32;
	ice.ice_hw.func_caps.common_cap.num_rxq = rxq;
	ice.ice_hw.func_caps.common_cap.num_txq = txq;
	ice.ice_hw.func_caps.common_cap.num_msix_vectors = msix;
	ice.ice_hw.func_caps.common_cap.rss_table_entry_width = width;
	have_prop = conf >= -1000;
	prop = conf;
	logged = 0;
	return (ice_queue_limit(&ice));
}

int
main(void)
{
	/* Sixteen queue pairs by default, fewer when the CPUs are fewer. */
	assert(limit(6, 256, 256, 1025, 8, -2000) == 6);
	assert(limit(16, 256, 256, 1025, 8, -2000) == 16);
	assert(limit(24, 256, 256, 1025, 8, -2000) == 16);
	assert(limit(512, 1024, 1024, 2048, 11, -2000) == 16);
	assert(logged == 0);
	/* The property raises the ceiling, following the CPUs. */
	assert(limit(24, 256, 256, 1025, 8, 64) == 24);
	assert(limit(64, 256, 256, 1025, 8, 64) == 64);
	/* MAC rx groups hold at most MAX_RINGS_PER_GROUP - 1 rings. */
	assert(limit(512, 1024, 1024, 2048, 11, 127) == 127);
	assert(logged == 0);
	/* Firmware queue and vector limits; vector 0 is not a queue. */
	assert(limit(64, 3, 256, 1025, 8, 127) == 3);
	assert(limit(64, 256, 5, 1025, 8, 127) == 5);
	assert(limit(64, 256, 256, 9, 8, 127) == 8);
	assert(limit(64, 256, 256, 9, 8, -2000) == 8);
	/* A narrow RSS entry bounds the queues it can name. */
	assert(limit(64, 256, 256, 1025, 2, 127) == 4);
	/* Safe mode leaves one queue pair. */
	assert(limit(64, 1, 1, 2, 0, -2000) == 1);
	assert(limit(64, 256, 256, 1025, 8, 5) == 5);
	assert(logged == 0);
	/* An out-of-range property is clamped to [1, 127] and logged. */
	assert(limit(64, 256, 256, 1025, 8, 0) == 1 && logged == 1);
	assert(limit(64, 256, 256, 1025, 8, -7) == 1 && logged == 1);
	assert(limit(64, 256, 256, 1025, 8, 128) == 64 && logged == 1);
	assert(limit(512, 1024, 1024, 2048, 8, 100000) == 127 && logged == 1);
	/* One CPU seen early in boot uses the boot CPU count instead. */
	assert(limit(1, 256, 256, 1025, 8, -2000) == 16);
	assert(limit(1, 256, 256, 1025, 8, 127) == 32);
	/* The RDMA block takes firmware vectors before the queues do. */
	rdma = 2;
	assert(limit(64, 256, 256, 9, 8, 127) == 6);
	assert(limit(64, 256, 256, 1025, 8, -2000) == 16);
	assert(limit(64, 256, 256, 4, 8, 127) == 1);
	rdma = 0;
	(void) puts("PASS: queue pair count defaults to 16 and follows CPUs, "
	    "firmware and MAC limits");
	return (0);
}

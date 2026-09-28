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
 * Run rdk_vector_info() from rdk_device.c: a vector out of range is
 * refused, and a provider without the operation reports nothing known.
 */

#include "rdk_unit.h"
#include "caps_bodies.h"

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		exit(1);						\
	}								\
} while (0)

static void
fake_vector_info(struct rdk_device *dev, uint32_t vec,
    struct rdk_vector_info *vi)
{
	(void) dev;
	vi->rvi_lgrp = 1;
	vi->rvi_cpu = (int32_t)(vec * 2 + 1);
}

int
main(void)
{
	struct rdk_device_ops ops = { .version = RDK_ABI_VERSION };
	struct rdk_device dev;
	struct rdk_vector_info vi;

	memset(&dev, 0, sizeof (dev));
	dev.rd_ops = &ops;
	dev.rd_num_comp_vectors = 4;
	CHECK(rdk_vector_info(&dev, 0, &vi) == 0);
	CHECK(vi.rvi_lgrp == -1 && vi.rvi_cpu == -1);
	ops.vector_info = fake_vector_info;
	CHECK(rdk_vector_info(&dev, 3, &vi) == 0);
	CHECK(vi.rvi_lgrp == 1 && vi.rvi_cpu == 7);
	CHECK(rdk_vector_info(&dev, 4, &vi) == EINVAL);
	CHECK(vi.rvi_lgrp == -1 && vi.rvi_cpu == -1);
	CHECK(rdk_vector_info(&dev, UINT32_MAX, &vi) == EINVAL);
	(void) printf("PASS: completion vector locality\n");
	return (0);
}

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
 * Copyright 2026 MNX Cloud, Inc.
 */

/*
 * Pulls the extracted command queue code together with the device model.
 */

#ifndef _CMDQ_TEST_H
#define	_CMDQ_TEST_H

#define	DEBUG	1

#include "mlxcx_stub.h"
#include <mlxcx_reg.h>
#include "mlxcx_types.h"
#include "mlxcx_min.h"

static uint32_t mlxcx_get32(mlxcx_t *, uintptr_t);
static void mlxcx_put32(mlxcx_t *, uintptr_t, uint32_t);
void mlxcx_cmd_completion(mlxcx_t *, mlxcx_eventq_ent_t *);
void mlxcx_cmd_eq_enable(mlxcx_t *);
void mlxcx_cmd_eq_disable(mlxcx_t *);
void mlxcx_cmd_queue_fini(mlxcx_t *);

#include "mlxcx_cmd_body.h"
#include "mlxcx_extra_body.h"
#include "mlxcx_cmdq_model.h"

static int
test_main(int argc, char **argv, const char *const *names,
    void (*const *funcs)(void))
{
	if (argc != 2)
		stub_fail("usage: %s scenario", argv[0]);
	stub_verbose = getenv("MLXCX_TEST_VERBOSE") != NULL;
	for (uint_t i = 0; names[i] != NULL; i++) {
		if (strcmp(argv[1], names[i]) == 0) {
			funcs[i]();
			(void) printf("ok %s\n", names[i]);
			return (0);
		}
	}
	stub_fail("unknown scenario %s", argv[1]);
	return (1);
}

#endif /* _CMDQ_TEST_H */

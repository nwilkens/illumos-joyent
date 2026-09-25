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
 * Host stand-in for <sys/byteorder.h> so that mlxcx_reg.h builds on a
 * little-endian development host.
 */

#ifndef _MLXCX_TEST_BYTEORDER_H
#define	_MLXCX_TEST_BYTEORDER_H

#include <stdint.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the mlxcx host tests expect a little-endian host"
#endif

#define	BE_16(x)	__builtin_bswap16(x)
#define	BE_32(x)	__builtin_bswap32(x)
#define	BE_64(x)	__builtin_bswap64(x)

#ifndef _BIT_FIELDS_LTOH
#define	_BIT_FIELDS_LTOH	1
#endif

#ifndef __packed
#define	__packed	__attribute__((packed))
#endif

#endif /* _MLXCX_TEST_BYTEORDER_H */

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
 * Provenance: ported to illumos from FreeBSD lib/libnvmf/nvmf_host.c.
 *
 * Original: Copyright (c) 2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * The host's identity: its 16-byte host ID and its NQN.
 */

#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <uuid/uuid.h>
#include <smbios.h>

#include "libnvmf.h"
#include "internal.h"

/*
 * Read the 16-byte SMBIOS system UUID.  This is the illumos analog of FreeBSD's
 * kern.hostuuid sysctl (see the PORT-TODO in the file header).  Returns 0 on
 * success with the raw UUID bytes in uuid[16], or an errno on failure.
 */
static int
nvmf_system_uuid(uint8_t uuid[16])
{
	smbios_hdl_t *shp;
	smbios_system_t sys;
	int err;

	shp = smbios_open(NULL, SMB_VERSION, 0, &err);
	if (shp == NULL)
		return (ENOENT);

	if (smbios_info_system(shp, &sys) == SMB_ERR ||
	    sys.smbs_uuid == NULL || sys.smbs_uuidlen != 16) {
		smbios_close(shp);
		return (ENOENT);
	}

	(void) memcpy(uuid, sys.smbs_uuid, 16);
	smbios_close(shp);
	return (0);
}

int
nvmf_hostid_from_hostuuid(uint8_t hostid[16])
{
	return (nvmf_system_uuid(hostid));
}

int
nvmf_nqn_from_hostuuid(char nqn[NVMF_NQN_MAX_LEN])
{
	uint8_t uuid[16];
	char uuid_str[NVMF_UUID_STRING_LEN + 1];
	int error;

	error = nvmf_system_uuid(uuid);
	if (error != 0)
		return (error);

	uuid_unparse(uuid, uuid_str);

	(void) strlcpy(nqn, NVMF_NQN_UUID_PRE, NVMF_NQN_MAX_LEN);
	(void) strlcat(nqn, uuid_str, NVMF_NQN_MAX_LEN);
	return (0);
}

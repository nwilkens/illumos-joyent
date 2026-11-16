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
 * The host's identity: its 16-byte host ID and its NQN.  Both are kept in
 * /etc/nvme, as text, and are created the first time they are asked for.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <smbios.h>

#include "libnvmf.h"
#include "internal.h"

#define	NVMF_ETC_DIR		"/etc/nvme"
#define	NVMF_HOSTID_FILE	NVMF_ETC_DIR "/hostid"
#define	NVMF_HOSTNQN_FILE	NVMF_ETC_DIR "/hostnqn"

/* The SMBIOS system UUID, unless the firmware left it unset. */
static int
nvmf_system_uuid(uuid_t uuid)
{
	static const uint8_t ones[16] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	smbios_hdl_t *shp;
	smbios_system_t sys;
	int err;

	shp = smbios_open(NULL, SMB_VERSION, 0, &err);
	if (shp == NULL)
		return (ENOENT);

	if (smbios_info_system(shp, &sys) == SMB_ERR ||
	    sys.smbs_uuid == NULL || sys.smbs_uuidlen != sizeof (uuid_t)) {
		smbios_close(shp);
		return (ENOENT);
	}

	(void) memcpy(uuid, sys.smbs_uuid, sizeof (uuid_t));
	smbios_close(shp);
	if (uuid_is_null(uuid) || memcmp(uuid, ones, sizeof (ones)) == 0)
		return (ENOENT);
	return (0);
}

/* Read the first line of a file, without its newline. */
static int
nvmf_load_line(const char *path, char *buf, size_t len)
{
	ssize_t n;
	int error, fd;

	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd == -1)
		return (errno);
	n = read(fd, buf, len - 1);
	error = errno;
	(void) close(fd);
	if (n < 0)
		return (error);
	buf[n] = '\0';
	buf[strcspn(buf, "\r\n")] = '\0';
	return (0);
}

/* Create a file holding one line.  Fails with EEXIST if the file exists. */
static int
nvmf_save_line(const char *path, const char *line)
{
	char tmp[MAXPATHLEN];
	size_t len = strlen(line);
	int error = 0, fd;

	if (mkdir(NVMF_ETC_DIR, 0755) != 0 && errno != EEXIST)
		return (errno);
	(void) snprintf(tmp, sizeof (tmp), "%s.%ld", path, (long)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
	if (fd == -1)
		return (errno);
	if (write(fd, line, len) != (ssize_t)len || write(fd, "\n", 1) != 1 ||
	    fsync(fd) != 0)
		error = errno != 0 ? errno : EIO;
	(void) close(fd);
	if (error == 0 && link(tmp, path) != 0)
		error = errno;
	(void) unlink(tmp);
	return (error);
}

int
nvmf_host_id(uint8_t hostid[16])
{
	char str[UUID_PRINTABLE_STRING_LENGTH + 1];
	uuid_t uuid;
	bool stable;
	int error, tries;

	for (tries = 0; tries < 2; tries++) {
		error = nvmf_load_line(NVMF_HOSTID_FILE, str, sizeof (str));
		if (error == 0) {
			if (uuid_parse(str, uuid) != 0 || uuid_is_null(uuid))
				return (EINVAL);
			(void) memcpy(hostid, uuid, sizeof (uuid_t));
			return (0);
		}
		if (error != ENOENT)
			return (error);

		stable = nvmf_system_uuid(uuid) == 0;
		if (!stable)
			uuid_generate(uuid);
		uuid_unparse(uuid, str);
		error = nvmf_save_line(NVMF_HOSTID_FILE, str);
		if (error == EEXIST)
			continue;

		/* A random ID that was not saved would change on every call. */
		if (error != 0 && !stable)
			return (error);
		(void) memcpy(hostid, uuid, sizeof (uuid_t));
		return (0);
	}
	return (error);
}

int
nvmf_host_nqn(char nqn[NVMF_NQN_FIELD_SIZE])
{
	char str[UUID_PRINTABLE_STRING_LENGTH];
	uuid_t uuid;
	int error, tries;

	for (tries = 0; tries < 2; tries++) {
		error = nvmf_load_line(NVMF_HOSTNQN_FILE, nqn,
		    NVMF_NQN_FIELD_SIZE);
		if (error == 0)
			return (nvmf_nqn_valid(nqn) ? 0 : EINVAL);
		if (error != ENOENT)
			return (error);

		error = nvmf_host_id(uuid);
		if (error != 0)
			return (error);
		uuid_unparse(uuid, str);
		(void) snprintf(nqn, NVMF_NQN_FIELD_SIZE, "%s%s",
		    NVMF_NQN_UUID_PRE, str);

		/* The NQN follows from the host ID, so it need not be saved. */
		if (nvmf_save_line(NVMF_HOSTNQN_FILE, nqn) != EEXIST)
			return (0);
	}
	return (error);
}

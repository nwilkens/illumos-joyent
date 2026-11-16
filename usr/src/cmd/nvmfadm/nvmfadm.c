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
 * nvmfadm: create, delete and list the subsystems of the NVMe over Fabrics
 * target port provider, nvmft.  A subsystem is one STMF target port.  The
 * subsystems are kept in nvmft's STMF provider data, and "nvmfadm restore"
 * creates them again from there when the target service starts.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libnvmf.h>
#include <libnvpair.h>
#include <libstmf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/nvme/nvmf_ioctl.h>

/* The provider data holds one nvlist, of an nvlist for each subsystem. */
#define	NVMFADM_CFG_SUBSYSTEMS	"subsystems"

/* Attempts to save a change before giving up on a busy configuration. */
#define	NVMFADM_CFG_TRIES	5

static void
usage(void)
{
	(void) fprintf(stderr,
	    "usage: nvmfadm create-subsys [-p portid] [-s serial] subnqn\n"
	    "       nvmfadm delete-subsys subnqn\n"
	    "       nvmfadm list-subsys\n"
	    "       nvmfadm restore\n");
	exit(2);
}

static int
nvmft_open(void)
{
	int fd = open(NVMFT_DEV, O_RDWR);

	if (fd == -1)
		err(1, "cannot open %s", NVMFT_DEV);
	return (fd);
}

/* Send a request nvlist.  Returns 0 or an errno. */
static int
nvmft_send(int fd, int cmd, nvlist_t *nvl)
{
	struct nvmf_ioc_nv nv;
	char *packed = NULL;
	size_t len = 0;
	int error = 0;

	if (nvlist_pack(nvl, &packed, &len, NV_ENCODE_NATIVE, 0) != 0)
		return (ENOMEM);

	(void) memset(&nv, 0, sizeof (nv));
	nv.data = packed;
	nv.len = len;
	nv.size = len;
	if (ioctl(fd, cmd, &nv) != 0)
		error = errno;
	free(packed);
	return (error);
}

/* Create a subsystem from its saved or requested properties. */
static int
subsys_create(int fd, const char *subnqn, nvlist_t *props)
{
	nvlist_t *req;
	char *serial;
	uint64_t portid;
	int error;

	if (nvlist_alloc(&req, NV_UNIQUE_NAME, 0) != 0)
		return (ENOMEM);
	error = nvlist_add_string(req, NVMFT_NV_SUBNQN, subnqn);
	if (error == 0 &&
	    nvlist_lookup_string(props, NVMFT_NV_SERIAL, &serial) == 0)
		error = nvlist_add_string(req, NVMFT_NV_SERIAL, serial);
	if (error == 0 &&
	    nvlist_lookup_uint64(props, NVMFT_NV_PORTID, &portid) == 0)
		error = nvlist_add_uint64(req, NVMFT_NV_PORTID, portid);
	if (error == 0)
		error = nvmft_send(fd, NVMFT_IOC_SUBSYS_CREATE, req);
	nvlist_free(req);
	return (error);
}

static int
subsys_delete(int fd, const char *subnqn)
{
	nvlist_t *req;
	int error;

	if (nvlist_alloc(&req, NV_UNIQUE_NAME, 0) != 0)
		return (ENOMEM);
	error = nvlist_add_string(req, NVMFT_NV_SUBNQN, subnqn);
	if (error == 0)
		error = nvmft_send(fd, NVMFT_IOC_SUBSYS_DELETE, req);
	nvlist_free(req);
	return (error);
}

/*
 * Read the saved configuration.  On success *cfgp is never NULL and *subsp
 * points at the subsystem list inside it.
 */
static int
config_load(nvlist_t **cfgp, nvlist_t **subsp, uint64_t *tokenp)
{
	nvlist_t *cfg = NULL, *subs;
	int ret;

	*tokenp = 0;
	ret = stmfGetProviderDataProt(NVMFT_DRIVER, &cfg,
	    STMF_PORT_PROVIDER_TYPE, tokenp);
	if (ret == STMF_ERROR_NOT_FOUND) {
		if (nvlist_alloc(&cfg, NV_UNIQUE_NAME, 0) != 0)
			return (ENOMEM);
	} else if (ret != STMF_STATUS_SUCCESS) {
		return (EIO);
	}

	if (nvlist_lookup_nvlist(cfg, NVMFADM_CFG_SUBSYSTEMS, &subs) != 0) {
		if (nvlist_alloc(&subs, NV_UNIQUE_NAME, 0) != 0) {
			nvlist_free(cfg);
			return (ENOMEM);
		}
		ret = nvlist_add_nvlist(cfg, NVMFADM_CFG_SUBSYSTEMS, subs);
		nvlist_free(subs);
		if (ret != 0 || nvlist_lookup_nvlist(cfg,
		    NVMFADM_CFG_SUBSYSTEMS, &subs) != 0) {
			nvlist_free(cfg);
			return (ENOMEM);
		}
	}

	*cfgp = cfg;
	*subsp = subs;
	return (0);
}

/*
 * Add a subsystem to the saved configuration, or with props NULL remove it.
 * Removing one that is not there fails with ENOENT.
 */
static int
config_update(const char *subnqn, nvlist_t *props)
{
	nvlist_t *cfg, *subs;
	uint64_t token;
	int error, ret, tries;

	for (tries = 0; tries < NVMFADM_CFG_TRIES; tries++) {
		error = config_load(&cfg, &subs, &token);
		if (error != 0)
			return (error);

		if (props != NULL) {
			error = nvlist_add_nvlist(subs, subnqn, props);
		} else if (!nvlist_exists(subs, subnqn)) {
			error = ENOENT;
		} else {
			error = nvlist_remove_all(subs, subnqn);
		}
		if (error != 0) {
			nvlist_free(cfg);
			return (error);
		}

		ret = stmfSetProviderDataProt(NVMFT_DRIVER, cfg,
		    STMF_PORT_PROVIDER_TYPE, &token);
		nvlist_free(cfg);
		if (ret == STMF_STATUS_SUCCESS)
			return (0);

		/* Someone else changed it first: read it again. */
		if (ret != STMF_ERROR_PROV_DATA_STALE)
			return (EIO);
	}
	return (EBUSY);
}

static int
cmd_create_subsys(int argc, char **argv)
{
	nvlist_t *props;
	const char *subnqn;
	char *end;
	unsigned long portid;
	int c, error, fd;

	if (nvlist_alloc(&props, NV_UNIQUE_NAME, 0) != 0)
		errx(1, "out of memory");

	while ((c = getopt(argc, argv, "p:s:")) != -1) {
		switch (c) {
		case 'p':
			errno = 0;
			portid = strtoul(optarg, &end, 0);
			if (errno != 0 || end == optarg || *end != '\0' ||
			    portid > UINT16_MAX)
				errx(2, "invalid port ID %s", optarg);
			if (nvlist_add_uint64(props, NVMFT_NV_PORTID,
			    portid) != 0)
				errx(1, "out of memory");
			break;
		case 's':
			if (nvlist_add_string(props, NVMFT_NV_SERIAL,
			    optarg) != 0)
				errx(1, "out of memory");
			break;
		default:
			usage();
		}
	}
	if (optind != argc - 1)
		usage();
	subnqn = argv[optind];
	if (!nvmf_nqn_valid_strict(subnqn))
		errx(2, "invalid subsystem NQN %s", subnqn);

	fd = nvmft_open();
	error = subsys_create(fd, subnqn, props);
	if (error != 0)
		errc(1, error, "cannot create subsystem %s", subnqn);

	error = config_update(subnqn, props);
	if (error != 0) {
		(void) subsys_delete(fd, subnqn);
		errc(1, error, "cannot save subsystem %s; it was not created",
		    subnqn);
	}

	(void) close(fd);
	nvlist_free(props);
	return (0);
}

static int
cmd_delete_subsys(int argc, char **argv)
{
	const char *subnqn;
	int error, fd, saved;

	if (argc != 2)
		usage();
	subnqn = argv[1];

	fd = nvmft_open();
	error = subsys_delete(fd, subnqn);
	(void) close(fd);
	if (error != 0 && error != ENOENT)
		errc(1, error, "cannot delete subsystem %s", subnqn);

	saved = config_update(subnqn, NULL);
	if (saved != 0 && saved != ENOENT)
		errc(1, saved, "cannot remove saved subsystem %s", subnqn);
	if (error == ENOENT && saved == ENOENT)
		errx(1, "no subsystem %s", subnqn);
	return (0);
}

static int
cmd_list_subsys(int argc)
{
	struct nvmf_ioc_nv nv;
	nvlist_t *reply;
	char **nqns;
	uint_t i, n;
	int fd;

	if (argc != 1)
		usage();

	fd = nvmft_open();
	(void) memset(&nv, 0, sizeof (nv));
	if (ioctl(fd, NVMFT_IOC_SUBSYS_LIST, &nv) != 0)
		err(1, "cannot list subsystems");
	nv.size = nv.len;
	nv.data = malloc(nv.size);
	if (nv.data == NULL)
		errx(1, "out of memory");
	if (ioctl(fd, NVMFT_IOC_SUBSYS_LIST, &nv) != 0)
		err(1, "cannot list subsystems");
	(void) close(fd);

	if (nvlist_unpack(nv.data, nv.len, &reply, 0) != 0)
		errx(1, "cannot read the list of subsystems");
	free(nv.data);

	if (nvlist_lookup_string_array(reply, NVMFT_NV_SUBNQNS, &nqns,
	    &n) == 0) {
		for (i = 0; i < n; i++)
			(void) printf("%s\n", nqns[i]);
	}
	nvlist_free(reply);
	return (0);
}

/* Create every saved subsystem that the port provider does not have. */
static int
cmd_restore(int argc)
{
	nvlist_t *cfg, *props, *subs;
	nvpair_t *pair;
	uint64_t token;
	int error, fd, ret = 0;

	if (argc != 1)
		usage();

	fd = nvmft_open();
	error = config_load(&cfg, &subs, &token);
	if (error != 0)
		errc(1, error, "cannot read the saved subsystems");

	for (pair = nvlist_next_nvpair(subs, NULL); pair != NULL;
	    pair = nvlist_next_nvpair(subs, pair)) {
		if (nvpair_value_nvlist(pair, &props) != 0)
			continue;
		error = subsys_create(fd, nvpair_name(pair), props);
		if (error != 0 && error != EEXIST) {
			warnc(error, "cannot create subsystem %s",
			    nvpair_name(pair));
			ret = 1;
		}
	}

	nvlist_free(cfg);
	(void) close(fd);
	return (ret);
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		usage();

	argc--;
	argv++;
	if (strcmp(argv[0], "create-subsys") == 0)
		return (cmd_create_subsys(argc, argv));
	if (strcmp(argv[0], "delete-subsys") == 0)
		return (cmd_delete_subsys(argc, argv));
	if (strcmp(argv[0], "list-subsys") == 0)
		return (cmd_list_subsys(argc));
	if (strcmp(argv[0], "restore") == 0)
		return (cmd_restore(argc));

	usage();
	return (2);
}

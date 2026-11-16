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
 * The nvmft port provider's control node: the list of subsystems it has and
 * the handoff of a connected queue pair to it.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libnvmf.h>
#include <libnvpair.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/nvme/nvmf_ioctl.h>

#include "internal.h"

static int nvmft_fd = -1;

/*
 * The subsystems as last read from the kernel, sorted.  The generation
 * counts the times a read has differed from the one before.
 */
static pthread_mutex_t nvmft_lock = PTHREAD_MUTEX_INITIALIZER;
static char (*nvmft_nqns)[NVMF_NQN_FIELD_SIZE];
static uint_t nvmft_count;
static uint64_t nvmft_genctr;

static int
nqn_compare(const void *a, const void *b)
{
	return (strcmp(a, b));
}

static int
nvmft_read_subsystems(char (**nqnsp)[NVMF_NQN_FIELD_SIZE], uint_t *countp)
{
	char (*nqns)[NVMF_NQN_FIELD_SIZE] = NULL;
	struct nvmf_ioc_nv nv;
	nvlist_t *nvl;
	char **names;
	uint_t count, i;
	int error, tries;

	/* The list can grow between the call that sizes it and the next. */
	for (tries = 0; ; tries++) {
		(void) memset(&nv, 0, sizeof (nv));
		if (ioctl(nvmft_fd, NVMFT_IOC_SUBSYS_LIST, &nv) != 0)
			return (errno);
		nv.size = nv.len;
		nv.data = malloc(nv.size);
		if (nv.data == NULL)
			return (ENOMEM);
		if (ioctl(nvmft_fd, NVMFT_IOC_SUBSYS_LIST, &nv) == 0)
			break;
		error = errno;
		free(nv.data);
		if (error != EFBIG || tries == 3)
			return (error);
	}

	error = nvlist_unpack(nv.data, nv.len, &nvl, 0);
	free(nv.data);
	if (error != 0)
		return (error);

	count = 0;
	if (nvlist_lookup_string_array(nvl, NVMFT_NV_SUBNQNS, &names,
	    &count) != 0)
		count = 0;
	if (count != 0) {
		nqns = calloc(count, sizeof (*nqns));
		if (nqns == NULL) {
			nvlist_free(nvl);
			return (ENOMEM);
		}
		for (i = 0; i < count; i++)
			(void) strlcpy(nqns[i], names[i], sizeof (nqns[i]));
		qsort(nqns, count, sizeof (*nqns), nqn_compare);
	}
	nvlist_free(nvl);

	*nqnsp = nqns;
	*countp = count;
	return (0);
}

/* Call with nvmft_lock held. */
static int
nvmft_refresh(bool first)
{
	char (*nqns)[NVMF_NQN_FIELD_SIZE];
	uint_t count;
	int error;

	error = nvmft_read_subsystems(&nqns, &count);
	if (error != 0)
		return (error);

	if (count == nvmft_count && (count == 0 ||
	    memcmp(nqns, nvmft_nqns, count * sizeof (*nqns)) == 0)) {
		free(nqns);
		return (0);
	}

	free(nvmft_nqns);
	nvmft_nqns = nqns;
	nvmft_count = count;
	if (!first)
		nvmft_genctr++;
	return (0);
}

void
init_nvmft(void)
{
	int error;

	nvmft_fd = open(NVMFT_DEV, O_RDWR);
	if (nvmft_fd == -1)
		err(1, "Failed to open %s", NVMFT_DEV);

	error = nvmft_refresh(true);
	if (error != 0)
		errc(1, error, "Failed to list subsystems");
}

bool
nvmft_subsystem_exists(const char *subnqn)
{
	bool found;

	(void) pthread_mutex_lock(&nvmft_lock);
	(void) nvmft_refresh(false);
	found = nvmft_count != 0 && bsearch(subnqn, nvmft_nqns, nvmft_count,
	    sizeof (*nvmft_nqns), nqn_compare) != NULL;
	(void) pthread_mutex_unlock(&nvmft_lock);
	return (found);
}

/* A copy of the current subsystem list for the caller to free. */
int
nvmft_subsystems(char (**nqnsp)[NVMF_NQN_FIELD_SIZE], uint_t *countp,
    uint64_t *genctrp)
{
	char (*nqns)[NVMF_NQN_FIELD_SIZE] = NULL;
	int error;

	(void) pthread_mutex_lock(&nvmft_lock);
	error = nvmft_refresh(false);
	if (error == 0 && nvmft_count != 0) {
		nqns = calloc(nvmft_count, sizeof (*nqns));
		if (nqns == NULL) {
			error = ENOMEM;
		} else {
			(void) memcpy(nqns, nvmft_nqns,
			    nvmft_count * sizeof (*nqns));
		}
	}
	if (error == 0) {
		*nqnsp = nqns;
		*countp = nvmft_count;
		*genctrp = nvmft_genctr;
	}
	(void) pthread_mutex_unlock(&nvmft_lock);
	return (error);
}

int
nvmft_handoff_qpair(struct nvmf_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data)
{
	struct nvmf_ioc_nv nv;
	int error;

	error = nvmf_handoff_controller_qpair(qp, cmd, data, &nv);
	if (error != 0)
		return (error);

	if (ioctl(nvmft_fd, NVMFT_IOC_HANDOFF, &nv) != 0)
		error = errno;

	free(nv.data);
	return (error);
}

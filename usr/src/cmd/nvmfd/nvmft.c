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

/* The subsystems and their generation as last read from the kernel, sorted. */
static pthread_mutex_t nvmft_lock = PTHREAD_MUTEX_INITIALIZER;
static nvmft_subsys_t *nvmft_subs;
static uint_t nvmft_count;
static uint64_t nvmft_genctr;

static int
subsys_compare(const void *a, const void *b)
{
	const nvmft_subsys_t *sa = a, *sb = b;

	return (strcmp(sa->ns_nqn, sb->ns_nqn));
}

static int
nvmft_read_subsystems(nvmft_subsys_t **subsp, uint_t *countp,
    uint64_t *genctrp)
{
	nvmft_subsys_t *subs = NULL;
	struct nvmf_ioc_nv nv;
	nvlist_t *nvl;
	char **names;
	uint16_t *portids;
	uint_t count, nportids, i;
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

	if (nvlist_lookup_uint64(nvl, NVMFT_NV_GENERATION, genctrp) != 0) {
		nvlist_free(nvl);
		return (EINVAL);
	}
	count = 0;
	if (nvlist_lookup_string_array(nvl, NVMFT_NV_SUBNQNS, &names,
	    &count) != 0)
		count = 0;
	if (count != 0) {
		if (nvlist_lookup_uint16_array(nvl, NVMFT_NV_PORTIDS, &portids,
		    &nportids) != 0 || nportids != count) {
			nvlist_free(nvl);
			return (EINVAL);
		}
		subs = calloc(count, sizeof (*subs));
		if (subs == NULL) {
			nvlist_free(nvl);
			return (ENOMEM);
		}
		for (i = 0; i < count; i++) {
			(void) strlcpy(subs[i].ns_nqn, names[i],
			    sizeof (subs[i].ns_nqn));
			subs[i].ns_portid = portids[i];
		}
		qsort(subs, count, sizeof (*subs), subsys_compare);
	}
	nvlist_free(nvl);

	*subsp = subs;
	*countp = count;
	return (0);
}

/* Call with nvmft_lock held. */
static int
nvmft_refresh(void)
{
	nvmft_subsys_t *subs;
	uint64_t genctr;
	uint_t count;
	int error;

	error = nvmft_read_subsystems(&subs, &count, &genctr);
	if (error != 0)
		return (error);

	free(nvmft_subs);
	nvmft_subs = subs;
	nvmft_count = count;
	nvmft_genctr = genctr;
	return (0);
}

void
init_nvmft(void)
{
	int error;

	nvmft_fd = open(NVMFT_DEV, O_RDWR);
	if (nvmft_fd == -1)
		err(1, "Failed to open %s", NVMFT_DEV);

	error = nvmft_refresh();
	if (error != 0)
		errc(1, error, "Failed to list subsystems");
}

/*
 * A copy of the current subsystem list for the caller to free, with the
 * kernel's generation of that list.
 */
int
nvmft_subsystems(nvmft_subsys_t **subsp, uint_t *countp, uint64_t *genctrp)
{
	nvmft_subsys_t *subs = NULL;
	int error;

	(void) pthread_mutex_lock(&nvmft_lock);
	error = nvmft_refresh();
	if (error == 0 && nvmft_count != 0) {
		subs = calloc(nvmft_count, sizeof (*subs));
		if (subs == NULL) {
			error = ENOMEM;
		} else {
			(void) memcpy(subs, nvmft_subs,
			    nvmft_count * sizeof (*subs));
		}
	}
	if (error == 0) {
		*subsp = subs;
		*countp = nvmft_count;
		*genctrp = nvmft_genctr;
	}
	(void) pthread_mutex_unlock(&nvmft_lock);
	return (error);
}

/*
 * Hand a queue pair to the kernel.  An error means that the kernel left the
 * socket alone.  Otherwise the kernel has answered the Connect, and
 * *refusedp says whether it refused it.
 */
int
nvmft_handoff_qpair(struct nvmf_qpair *qp,
    const nvmf_fabric_connect_cmd_t *cmd,
    const nvmf_fabric_connect_data_t *data, bool *refusedp)
{
	struct nvmf_ioc_nv nv;
	int error, rv;

	error = nvmf_handoff_controller_qpair(qp, cmd, data, &nv);
	if (error != 0)
		return (error);

	rv = ioctl(nvmft_fd, NVMFT_IOC_HANDOFF, &nv);
	if (rv == -1)
		error = errno;
	else
		*refusedp = (rv == NVMFT_HANDOFF_REFUSED);

	free(nv.data);
	return (error);
}

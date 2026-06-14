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
 * Provenance: ported to illumos from FreeBSD
 * tools/tools/nvmf/nvmfd/discovery.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * The Discovery controller is served entirely in userland: it accepts a
 * Fabrics association, answers IDENTIFY and the Discovery Log Page, and
 * returns the I/O subsystem's transport address so a host can then connect to
 * the I/O controller.  This is unchanged from FreeBSD other than style and the
 * libnvmf header path.
 */

#include <sys/byteorder.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <libnvmf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvme_reg.h"

#include "internal.h"

/*
 * NVMe-oF Discovery Log Page identifier (LID); not yet carried in the illumos
 * register header.  libnvmf defines it locally the same way (FreeBSD:
 * NVME_LOG_DISCOVERY).
 */
#ifndef	NVMF_LOG_DISCOVERY
#define	NVMF_LOG_DISCOVERY	0x70
#endif

/*
 * Maximum admin queue entries advertised to hosts.  Matches the value libnvmf
 * uses for the same purpose (FreeBSD: NVME_MAX_ADMIN_ENTRIES).
 */
#ifndef	NVME_MAX_ADMIN_ENTRIES
#define	NVME_MAX_ADMIN_ENTRIES	4096
#endif

struct io_controller_data {
	nvmf_discovery_log_page_entry_t entry;
	bool wildcard;
};

struct discovery_controller {
	nvmf_discovery_log_page_t *discovery_log;
	size_t discovery_log_len;
	int s;
};

struct discovery_thread_arg {
	struct controller *c;
	struct nvmf_qpair *qp;
	int s;
};

static struct io_controller_data *io_controllers;
static struct nvmf_association *discovery_na;
static u_int num_io_controllers;

static bool
init_discovery_log_entry(nvmf_discovery_log_page_entry_t *entry, int s,
    const char *subnqn)
{
	struct sockaddr_storage ss;
	socklen_t len;
	bool wildcard;

	len = sizeof (ss);
	if (getsockname(s, (struct sockaddr *)&ss, &len) == -1)
		err(1, "getsockname");

	(void) memset(entry, 0, sizeof (*entry));
	entry->ndle_trtype = NVMF_TRTYPE_TCP;
	switch (ss.ss_family) {
	case AF_INET: {
		struct sockaddr_in *sin;

		sin = (struct sockaddr_in *)&ss;
		entry->ndle_adrfam = NVMF_ADRFAM_IPV4;
		(void) snprintf((char *)entry->ndle_trsvcid,
		    sizeof (entry->ndle_trsvcid), "%u", ntohs(sin->sin_port));
		if (inet_ntop(AF_INET, &sin->sin_addr,
		    (char *)entry->ndle_traddr,
		    sizeof (entry->ndle_traddr)) == NULL)
			err(1, "inet_ntop");
		wildcard = (sin->sin_addr.s_addr == htonl(INADDR_ANY));
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sin6;

		sin6 = (struct sockaddr_in6 *)&ss;
		entry->ndle_adrfam = NVMF_ADRFAM_IPV6;
		(void) snprintf((char *)entry->ndle_trsvcid,
		    sizeof (entry->ndle_trsvcid), "%u", ntohs(sin6->sin6_port));
		if (inet_ntop(AF_INET6, &sin6->sin6_addr,
		    (char *)entry->ndle_traddr,
		    sizeof (entry->ndle_traddr)) == NULL)
			err(1, "inet_ntop");
		wildcard = (memcmp(&sin6->sin6_addr, &in6addr_any,
		    sizeof (in6addr_any)) == 0);
		break;
	}
	default:
		errx(1, "Unsupported address family %u", ss.ss_family);
	}
	entry->ndle_subtype = NVMF_SUBTYPE_NVME;
	/*
	 * TREQ bit 2 (the LSB of the reserved field, past the 2-bit secure
	 * channel requirement) advertises "connections shall not use SQ flow
	 * control".  (FreeBSD: entry->treq |= (1 << 2).)
	 */
	if (flow_control_disable)
		entry->ndle_treq.reserved |= 1;
	entry->ndle_portid = LE_16(1);
	entry->ndle_cntlid = LE_16(NVMF_CNTLID_DYNAMIC);
	entry->ndle_asqsz = NVME_MAX_ADMIN_ENTRIES;
	(void) strlcpy((char *)entry->ndle_subnqn, subnqn,
	    sizeof (entry->ndle_subnqn));
	return (wildcard);
}

void
init_discovery(void)
{
	nvmf_association_params_t aparams;

	(void) memset(&aparams, 0, sizeof (aparams));
	aparams.nap_sq_flow_control = false;
	aparams.nap_dynamic_controller_model = true;
	aparams.nap_max_admin_qsize = NVME_MAX_ADMIN_ENTRIES;
	aparams.nap_tcp.pda = 0;
	aparams.nap_tcp.header_digests = header_digests;
	aparams.nap_tcp.data_digests = data_digests;
	aparams.nap_tcp.maxh2cdata = maxh2cdata;
	discovery_na = nvmf_allocate_association(NVMF_TRTYPE_TCP, true,
	    &aparams);
	if (discovery_na == NULL)
		err(1, "Failed to create discovery association");
}

void
discovery_add_io_controller(int s, const char *subnqn)
{
	struct io_controller_data *icd;

	io_controllers = reallocf(io_controllers, (num_io_controllers + 1) *
	    sizeof (*io_controllers));
	if (io_controllers == NULL)
		err(1, "reallocf");

	icd = &io_controllers[num_io_controllers];
	num_io_controllers++;

	icd->wildcard = init_discovery_log_entry(&icd->entry, s, subnqn);
}

static void
build_discovery_log_page(struct discovery_controller *dc)
{
	struct sockaddr_storage ss;
	socklen_t len;
	char traddr[256];
	u_int i, nentries;
	uint8_t adrfam;

	if (dc->discovery_log != NULL)
		return;

	len = sizeof (ss);
	if (getsockname(dc->s, (struct sockaddr *)&ss, &len) == -1) {
		warn("build_discovery_log_page: getsockname");
		return;
	}

	(void) memset(traddr, 0, sizeof (traddr));
	switch (ss.ss_family) {
	case AF_INET: {
		struct sockaddr_in *sin;

		sin = (struct sockaddr_in *)&ss;
		adrfam = NVMF_ADRFAM_IPV4;
		if (inet_ntop(AF_INET, &sin->sin_addr, traddr,
		    sizeof (traddr)) == NULL) {
			warn("build_discovery_log_page: inet_ntop");
			return;
		}
		break;
	}
	case AF_INET6: {
		struct sockaddr_in6 *sin6;

		sin6 = (struct sockaddr_in6 *)&ss;
		adrfam = NVMF_ADRFAM_IPV6;
		if (inet_ntop(AF_INET6, &sin6->sin6_addr, traddr,
		    sizeof (traddr)) == NULL) {
			warn("build_discovery_log_page: inet_ntop");
			return;
		}
		break;
	}
	default:
		assert(false);
	}

	nentries = 0;
	for (i = 0; i < num_io_controllers; i++) {
		if (io_controllers[i].wildcard &&
		    io_controllers[i].entry.ndle_adrfam != adrfam)
			continue;
		nentries++;
	}

	dc->discovery_log_len = sizeof (*dc->discovery_log) +
	    nentries * sizeof (nvmf_discovery_log_page_entry_t);
	dc->discovery_log = calloc(1, dc->discovery_log_len);
	if (dc->discovery_log == NULL)
		err(1, "calloc");
	dc->discovery_log->ndlp_numrec = nentries;
	dc->discovery_log->ndlp_recfmt = 0;
	nentries = 0;
	for (i = 0; i < num_io_controllers; i++) {
		if (io_controllers[i].wildcard &&
		    io_controllers[i].entry.ndle_adrfam != adrfam)
			continue;

		dc->discovery_log->ndlp_entries[nentries] =
		    io_controllers[i].entry;
		if (io_controllers[i].wildcard) {
			(void) memcpy(
			    dc->discovery_log->ndlp_entries[nentries].ndle_traddr,
			    traddr, sizeof (traddr));
		}
		nentries++;
	}
}

static void
handle_get_log_page_command(const struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd, struct discovery_controller *dc)
{
	uint64_t offset;
	uint32_t length;

	switch (nvmf_get_log_page_id(cmd)) {
	case NVMF_LOG_DISCOVERY:
		break;
	default:
		warnx("Unsupported log page %u for discovery controller",
		    nvmf_get_log_page_id(cmd));
		goto error;
	}

	build_discovery_log_page(dc);

	offset = nvmf_get_log_page_offset(cmd);
	if (offset >= dc->discovery_log_len)
		goto error;

	length = nvmf_get_log_page_length(cmd);
	if (length > dc->discovery_log_len - offset)
		length = dc->discovery_log_len - offset;

	(void) nvmf_send_controller_data(nc,
	    (char *)dc->discovery_log + offset, length);
	return;
error:
	(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
}

static bool
discovery_command(const struct nvmf_capsule *nc, const nvme_sqe_t *cmd,
    void *arg)
{
	struct discovery_controller *dc = arg;

	switch (cmd->sqe_opc) {
	case NVME_OPC_GET_LOG_PAGE:
		handle_get_log_page_command(nc, cmd, dc);
		return (true);
	default:
		return (false);
	}
}

static void *
discovery_thread(void *arg)
{
	struct discovery_thread_arg *dta = arg;
	struct discovery_controller dc;

	(void) pthread_detach(pthread_self());

	(void) memset(&dc, 0, sizeof (dc));
	dc.s = dta->s;

	controller_handle_admin_commands(dta->c, discovery_command, &dc);

	free(dc.discovery_log);
	free_controller(dta->c);

	nvmf_free_qpair(dta->qp);

	(void) close(dta->s);
	free(dta);
	return (NULL);
}

void
handle_discovery_socket(int s)
{
	nvmf_fabric_connect_data_t data;
	nvme_identify_ctrl_t cdata;
	struct nvmf_qpair_params qparams;
	struct discovery_thread_arg *dta;
	struct nvmf_capsule *nc;
	struct nvmf_qpair *qp;
	pthread_t thr;
	int error;

	(void) memset(&qparams, 0, sizeof (qparams));
	qparams.nqp_tcp.fd = s;

	nc = NULL;
	qp = nvmf_accept(discovery_na, &qparams, &nc, &data);
	if (qp == NULL) {
		warnx("Failed to create discovery qpair: %s",
		    nvmf_association_error(discovery_na));
		goto error;
	}

	if (strcmp((const char *)data.nfcd_subnqn, NVMF_DISCOVERY_NQN) != 0) {
		warnx("Discovery qpair with invalid SubNQN: %.*s",
		    (int)sizeof (data.nfcd_subnqn), data.nfcd_subnqn);
		nvmf_connect_invalid_parameters(nc, true,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_subnqn));
		goto error;
	}

	/* Just use a controller ID of 1 for all discovery controllers. */
	error = nvmf_finish_accept(nc, 1);
	if (error != 0) {
		warnc(error, "Failed to send CONNECT response");
		goto error;
	}

	nvmf_init_discovery_controller_data(qp, &cdata);

	dta = malloc(sizeof (*dta));
	if (dta == NULL)
		err(1, "malloc");
	dta->qp = qp;
	dta->s = s;
	dta->c = init_controller(qp, &cdata);

	error = pthread_create(&thr, NULL, discovery_thread, dta);
	if (error != 0) {
		warnc(error, "Failed to create discovery thread");
		free_controller(dta->c);
		free(dta);
		goto error;
	}

	nvmf_free_capsule(nc);
	return;

error:
	if (nc != NULL)
		nvmf_free_capsule(nc);
	if (qp != NULL)
		nvmf_free_qpair(qp);
	(void) close(s);
}

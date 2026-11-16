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
 * The discovery controller.  Its log page lists every subsystem the nvmft
 * port provider has, once for each address the I/O port listens on.
 */

#include <sys/byteorder.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <atomic.h>
#include <err.h>
#include <errno.h>
#include <libnvmf.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "internal.h"

#define	NVMFD_LOG_DISCOVERY	0x70
#define	NVMFD_MAX_ADMIN_ENTRIES	4096

/* A host that sets no Keep Alive timeout may stay idle this long. */
#define	NVMFD_DISCOVERY_IDLE	120000		/* ms */

/* Slack on top of the host's Keep Alive timeout, for a late command. */
#define	NVMFD_KATO_GRACE	10000		/* ms */

struct io_listener {
	nvmf_discovery_log_page_entry_t entry;
	bool wildcard;
};

struct discovery_controller {
	nvmf_discovery_log_page_t *discovery_log;
	size_t discovery_log_len;
	int s;
};

static struct io_listener *io_listeners;
static uint_t num_io_listeners;
static volatile uint_t discovery_cntlid;

/*
 * Turn a socket address into a discovery log entry's address family, address
 * and port strings, and say whether it is the wildcard address.
 */
static int
sockaddr_to_traddr(const struct sockaddr_storage *ss, uint8_t *adrfam,
    char *traddr, size_t traddr_len, char *trsvcid, size_t trsvcid_len,
    bool *wildcard)
{
	switch (ss->ss_family) {
	case AF_INET: {
		const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;

		*adrfam = NVMF_ADRFAM_IPV4;
		if (inet_ntop(AF_INET, &sin->sin_addr, traddr, traddr_len) ==
		    NULL)
			return (-1);
		if (trsvcid != NULL)
			(void) snprintf(trsvcid, trsvcid_len, "%u",
			    ntohs(sin->sin_port));
		if (wildcard != NULL)
			*wildcard = (sin->sin_addr.s_addr == htonl(INADDR_ANY));
		return (0);
	}
	case AF_INET6: {
		const struct sockaddr_in6 *sin6 =
		    (const struct sockaddr_in6 *)ss;

		*adrfam = NVMF_ADRFAM_IPV6;
		if (inet_ntop(AF_INET6, &sin6->sin6_addr, traddr, traddr_len) ==
		    NULL)
			return (-1);
		if (trsvcid != NULL)
			(void) snprintf(trsvcid, trsvcid_len, "%u",
			    ntohs(sin6->sin6_port));
		if (wildcard != NULL)
			*wildcard = (memcmp(&sin6->sin6_addr, &in6addr_any,
			    sizeof (in6addr_any)) == 0);
		return (0);
	}
	default:
		return (-1);
	}
}

/* Record an address the I/O port listens on.  Called before any thread. */
void
discovery_add_io_listener(int s)
{
	struct io_listener *il;
	nvmf_discovery_log_page_entry_t *entry;
	struct sockaddr_storage ss;
	socklen_t len;

	io_listeners = reallocf(io_listeners, (num_io_listeners + 1) *
	    sizeof (*io_listeners));
	if (io_listeners == NULL)
		err(1, "reallocf");
	il = &io_listeners[num_io_listeners];
	num_io_listeners++;

	len = sizeof (ss);
	if (getsockname(s, (struct sockaddr *)&ss, &len) == -1)
		err(1, "getsockname");

	entry = &il->entry;
	(void) memset(entry, 0, sizeof (*entry));
	entry->ndle_trtype = NVMF_TRTYPE_TCP;
	if (sockaddr_to_traddr(&ss, &entry->ndle_adrfam,
	    (char *)entry->ndle_traddr, sizeof (entry->ndle_traddr),
	    (char *)entry->ndle_trsvcid, sizeof (entry->ndle_trsvcid),
	    &il->wildcard) != 0)
		errx(1, "Unsupported address family %u", ss.ss_family);
	entry->ndle_subtype = NVMF_SUBTYPE_NVME;

	/* TREQ bit 2: connections shall not use SQ flow control. */
	if (flow_control_disable)
		entry->ndle_treq.reserved |= 1;
	entry->ndle_cntlid = LE_16(NVMF_CNTLID_DYNAMIC);
	entry->ndle_asqsz = LE_16(NVMFD_MAX_ADMIN_ENTRIES);
}

/*
 * Build the log page afresh for each Get Log Page, so that a host which reads
 * it in pieces sees the generation counter move if the subsystems changed.
 */
static int
build_discovery_log_page(struct discovery_controller *dc)
{
	nvmft_subsys_t *subs;
	nvmf_discovery_log_page_entry_t *entry;
	struct sockaddr_storage ss;
	socklen_t len;
	char traddr[NVMF_TRADDR_MAX_LEN];
	uint64_t genctr;
	uint_t count, i, j, nlisteners, nentries;
	uint8_t adrfam;
	int error;

	free(dc->discovery_log);
	dc->discovery_log = NULL;

	len = sizeof (ss);
	if (getsockname(dc->s, (struct sockaddr *)&ss, &len) == -1)
		return (errno);

	(void) memset(traddr, 0, sizeof (traddr));
	if (sockaddr_to_traddr(&ss, &adrfam, traddr, sizeof (traddr),
	    NULL, 0, NULL) != 0)
		return (EAFNOSUPPORT);

	error = nvmft_subsystems(&subs, &count, &genctr);
	if (error != 0)
		return (error);

	/* A wildcard listener is reported at the address the host came to. */
	nlisteners = 0;
	for (i = 0; i < num_io_listeners; i++) {
		if (!io_listeners[i].wildcard ||
		    io_listeners[i].entry.ndle_adrfam == adrfam)
			nlisteners++;
	}

	nentries = count * nlisteners;
	dc->discovery_log_len = sizeof (*dc->discovery_log) +
	    (size_t)nentries * sizeof (nvmf_discovery_log_page_entry_t);
	dc->discovery_log = calloc(1, dc->discovery_log_len);
	if (dc->discovery_log == NULL) {
		free(subs);
		return (ENOMEM);
	}
	dc->discovery_log->ndlp_genctr = LE_64(genctr);
	dc->discovery_log->ndlp_numrec = LE_64((uint64_t)nentries);
	dc->discovery_log->ndlp_recfmt = LE_16(0);

	entry = dc->discovery_log->ndlp_entries;
	for (j = 0; j < count; j++) {
		for (i = 0; i < num_io_listeners; i++) {
			if (io_listeners[i].wildcard &&
			    io_listeners[i].entry.ndle_adrfam != adrfam)
				continue;

			*entry = io_listeners[i].entry;
			if (io_listeners[i].wildcard) {
				(void) memcpy(entry->ndle_traddr, traddr,
				    sizeof (entry->ndle_traddr));
			}
			entry->ndle_portid = LE_16(subs[j].ns_portid);
			(void) strlcpy((char *)entry->ndle_subnqn,
			    subs[j].ns_nqn, sizeof (entry->ndle_subnqn));
			entry++;
		}
	}
	free(subs);
	return (0);
}

static void
handle_get_log_page_command(const struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd, struct discovery_controller *dc)
{
	uint64_t length, offset;

	if (nvmf_get_log_page_id(cmd) != NVMFD_LOG_DISCOVERY) {
		(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
		return;
	}

	if (build_discovery_log_page(dc) != 0) {
		(void) nvmf_send_generic_error(nc,
		    NVME_CQE_SC_GEN_INTERNAL_ERR);
		return;
	}

	offset = nvmf_get_log_page_offset(cmd);
	if (offset >= dc->discovery_log_len) {
		(void) nvmf_send_generic_error(nc, NVME_CQE_SC_GEN_INV_FLD);
		return;
	}

	length = nvmf_get_log_page_length(cmd);
	if (length > dc->discovery_log_len - offset)
		length = dc->discovery_log_len - offset;

	(void) nvmf_send_controller_data(nc,
	    (char *)dc->discovery_log + offset, length);
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

/* Controller IDs 1 to 0xffef, handed out in turn. */
static uint16_t
next_cntlid(void)
{
	return (atomic_inc_uint_nv(&discovery_cntlid) %
	    NVMF_CNTLID_STATIC_MAX + 1);
}

void
discovery_connection(nvmfd_conn_t *conn)
{
	nvmf_association_params_t aparams;
	nvmf_fabric_connect_data_t data;
	const nvmf_fabric_connect_cmd_t *cmd;
	nvme_identify_ctrl_t cdata;
	nvmf_qpair_params_t qparams;
	struct discovery_controller dc;
	struct nvmf_association *na;
	struct nvmf_capsule *nc = NULL;
	struct nvmf_qpair *qp = NULL;
	struct controller *c;
	uint64_t idle_ms;
	int error;

	(void) memset(&aparams, 0, sizeof (aparams));
	aparams.nap_sq_flow_control = false;
	aparams.nap_dynamic_controller_model = true;
	aparams.nap_max_admin_qsize = NVMFD_MAX_ADMIN_ENTRIES;
	aparams.nap_tcp.pda = 0;
	aparams.nap_tcp.header_digests = header_digests;
	aparams.nap_tcp.data_digests = data_digests;
	aparams.nap_tcp.maxh2cdata = maxh2cdata;
	na = nvmf_allocate_association(NVMF_TRTYPE_TCP, true, &aparams);
	if (na == NULL) {
		nvmfd_log(LOG_ERR, "Failed to create discovery association");
		return;
	}

	(void) memset(&qparams, 0, sizeof (qparams));
	qparams.nqp_tcp.fd = conn->nc_fd;
	qparams.nqp_timeout_ms = NVMFD_IO_TIMEOUT;
	qp = nvmf_accept(na, &qparams, &nc, &data);
	if (qp == NULL) {
		nvmfd_peer_log(conn, "Discovery port handshake failed: %s",
		    nvmf_association_error(na) != NULL ?
		    nvmf_association_error(na) : "unknown error");
		goto out;
	}

	if (strcmp((const char *)data.nfcd_subnqn, NVMF_DISCOVERY_NQN) != 0) {
		nvmfd_peer_log(conn, "Connect to the discovery port for "
		    "another subsystem");
		nvmf_connect_invalid_parameters(nc, true,
		    offsetof(nvmf_fabric_connect_data_t, nfcd_subnqn));
		goto out;
	}

	if (!nvmfd_conn_session(conn)) {
		nvmfd_peer_log(conn, "Too many discovery sessions");
		(void) nvmf_send_error(nc, NVME_CQE_SCT_SPECIFIC,
		    NVMF_FABRIC_SC_CONTROLLER_BUSY);
		goto out;
	}

	cmd = nvmf_capsule_sqe(nc);
	idle_ms = LE_32(cmd->nfcc_kato);
	if (idle_ms == 0)
		idle_ms = NVMFD_DISCOVERY_IDLE;
	else
		idle_ms += NVMFD_KATO_GRACE;
	if (idle_ms > UINT32_MAX)
		idle_ms = UINT32_MAX;

	error = nvmf_finish_accept(nc, next_cntlid());
	if (error != 0) {
		nvmfd_peer_log(conn, "Failed to send Connect response: %s",
		    strerror(error));
		goto out;
	}
	nvmf_free_capsule(nc);
	nc = NULL;

	nvmf_init_discovery_controller_data(qp, &cdata);
	c = init_controller(qp, &cdata, (uint_t)idle_ms);
	if (c == NULL)
		goto out;

	(void) memset(&dc, 0, sizeof (dc));
	dc.s = conn->nc_fd;
	controller_handle_admin_commands(c, discovery_command, &dc);
	free(dc.discovery_log);
	free_controller(c);

out:
	if (nc != NULL)
		nvmf_free_capsule(nc);
	if (qp != NULL)
		nvmf_free_qpair(qp);
	nvmf_free_association(na);
}

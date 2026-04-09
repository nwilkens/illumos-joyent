/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * Copyright 2026 Nick Wilkens
 */

/*
 * bhyve control socket — JSON-over-Unix-socket interface for GZ
 * migration agent.
 *
 * The GZ migration agent handles RAM transfer, kernel state, and
 * dirty page tracking directly via /dev/vmm.  This control socket
 * handles only the userspace device state that bhyve manages:
 * viona ring pause/resume and PCI device save/restore.
 *
 * Protocol: newline-delimited JSON requests, newline-delimited JSON
 * responses.  For binary payloads (device state nvlist), a length-
 * prefixed binary blob follows the JSON response.
 *
 * Commands:
 *   {"command":"status"}
 *   {"command":"pause-devices"}
 *   {"command":"export-devices"}
 *   {"command":"import-devices","len":NNN}  + NNN bytes of nvlist
 *   {"command":"resume-devices"}
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include <machine/vmm.h>
#include <machine/vmm_dev.h>
#include <vmmapi.h>
#include <libnvpair.h>

#include "bhyve_control.h"
#include "pci_emul.h"

static struct vmctx	*ctl_ctx;
static int		ctl_ncpus;
static char		*ctl_path;
static int		ctl_listen_fd = -1;
static pthread_t	ctl_thread;
static volatile int	ctl_running;

/* Max JSON line length */
#define	CTL_MAXLINE	4096

/*
 * Simple JSON field extraction.  Finds "key":"value" and returns a
 * malloc'd copy of value, or NULL if not found.  Handles only simple
 * string values (no nesting, no escapes).
 */
static char *
json_get_string(const char *json, const char *key)
{
	char pattern[128];
	const char *p, *start, *end;

	(void) snprintf(pattern, sizeof (pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (p == NULL)
		return (NULL);

	/* Skip past "key" and find the colon + opening quote */
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == ':')
		p++;
	if (*p != '"')
		return (NULL);
	start = ++p;
	end = strchr(start, '"');
	if (end == NULL)
		return (NULL);

	return (strndup(start, end - start));
}

/*
 * Extract a numeric field: "key":NNN
 */
static int
json_get_uint64(const char *json, const char *key, uint64_t *valp)
{
	char pattern[128];
	const char *p;

	(void) snprintf(pattern, sizeof (pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (p == NULL)
		return (-1);

	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == ':')
		p++;

	char *endp;
	*valp = strtoull(p, &endp, 10);
	if (endp == p)
		return (-1);
	return (0);
}

/*
 * Write a full buffer to fd, handling partial writes.
 */
static int
write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0)
			return (-1);
		p += n;
		len -= n;
	}
	return (0);
}

/*
 * Read exactly len bytes from fd.
 */
static int
read_all(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n <= 0)
			return (-1);
		p += n;
		len -= n;
	}
	return (0);
}

static void
send_ok(int fd)
{
	const char *resp = "{\"success\":true}\n";
	(void) write_all(fd, resp, strlen(resp));
}

static void
send_error(int fd, const char *msg)
{
	char buf[256];
	(void) snprintf(buf, sizeof (buf),
	    "{\"success\":false,\"error\":\"%s\"}\n", msg);
	(void) write_all(fd, buf, strlen(buf));
}

/*
 * Handle "status" — return basic VM info.
 */
static void
cmd_status(int fd)
{
	size_t lowmem = vm_get_lowmem_size(ctl_ctx);
	size_t highmem = vm_get_highmem_size(ctl_ctx);
	char buf[256];

	(void) snprintf(buf, sizeof (buf),
	    "{\"success\":true,\"ncpus\":%d,"
	    "\"lowmem\":%zu,\"highmem\":%zu,"
	    "\"pid\":%d}\n",
	    ctl_ncpus, lowmem, highmem, (int)getpid());
	(void) write_all(fd, buf, strlen(buf));
}

/*
 * Handle "pause-devices" — pause viona rings before vCPU pause.
 *
 * This must be called BEFORE VM pause so that kernel ring workers
 * stop consuming avail entries while the guest is frozen.
 */
static void
cmd_pause_devices(int fd)
{
	int rv = pci_pause_devices();
	if (rv == 0)
		send_ok(fd);
	else
		send_error(fd, "pci_pause_devices failed");
}

/*
 * Handle "pause-vm" — pause vCPUs and device timers.
 *
 * Must be called from inside bhyve (not from GZ) so that the
 * vCPU threads properly coordinate their exit from VM_RUN.
 * The GZ agent's VM_PAUSE ioctl can deadlock with subsequent
 * VM_DATA_WRITE because vcpu_lock_one blocks on vCPUs still
 * stuck in VM_RUN.
 */
static void
cmd_pause_vm(int fd)
{
	if (vm_pause_instance(ctl_ctx) != 0) {
		send_error(fd, strerror(errno));
		return;
	}
	send_ok(fd);
}

/*
 * Handle "resume-vm" — resume vCPUs and device timers.
 */
static void
cmd_resume_vm(int fd)
{
	if (vm_resume_instance(ctl_ctx) != 0) {
		send_error(fd, strerror(errno));
		return;
	}
	send_ok(fd);
}

/*
 * Handle "export-devices" — save all userspace device state.
 *
 * Packs PCI config space + device-specific state into an nvlist,
 * then sends it as: JSON header with length, followed by raw bytes.
 *
 * Response: {"success":true,"len":NNN}\n<NNN bytes of packed nvlist>
 */
static void
cmd_export_devices(int fd)
{
	nvlist_t *nvl;
	char *packed = NULL;
	size_t packed_len = 0;
	int rv;

	rv = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0);
	if (rv != 0) {
		send_error(fd, "nvlist_alloc failed");
		return;
	}

	rv = pci_save_all(nvl);
	if (rv != 0) {
		nvlist_free(nvl);
		send_error(fd, "pci_save_all failed");
		return;
	}

	rv = nvlist_pack(nvl, &packed, &packed_len, NV_ENCODE_NATIVE, 0);
	nvlist_free(nvl);
	if (rv != 0) {
		send_error(fd, "nvlist_pack failed");
		return;
	}

	/* Send header + binary payload */
	char hdr[128];
	(void) snprintf(hdr, sizeof (hdr),
	    "{\"success\":true,\"len\":%zu}\n", packed_len);
	if (write_all(fd, hdr, strlen(hdr)) != 0 ||
	    write_all(fd, packed, packed_len) != 0) {
		fprintf(stderr, "ctl: export-devices write failed\n");
	}

	free(packed);
}

/*
 * Handle "import-devices" — restore userspace device state.
 *
 * Reads len bytes of packed nvlist from the socket, unpacks it,
 * and calls pci_restore_all().  This restores viona ring state,
 * virtio-blk queues, PCI config, etc.
 */
static void
cmd_import_devices(int fd, uint64_t len)
{
	if (len == 0 || len > 64 * 1024 * 1024) {
		send_error(fd, "invalid length");
		return;
	}

	char *packed = malloc(len);
	if (packed == NULL) {
		send_error(fd, "malloc failed");
		return;
	}

	if (read_all(fd, packed, len) != 0) {
		free(packed);
		send_error(fd, "read failed");
		return;
	}

	nvlist_t *nvl;
	int rv = nvlist_unpack(packed, len, &nvl, 0);
	free(packed);
	if (rv != 0) {
		send_error(fd, "nvlist_unpack failed");
		return;
	}

	rv = pci_restore_all(nvl);
	nvlist_free(nvl);
	if (rv != 0) {
		send_error(fd, "pci_restore_all failed");
		return;
	}

	send_ok(fd);
}

/*
 * Handle "resume-devices" — kick viona rings after import.
 *
 * Called after the GZ agent has written kernel state and is about
 * to VM_RESUME.  This is a no-op for now since pci_restore_all
 * already kicks rings and injects interrupts.  Kept as a command
 * for future use if we split the kick from restore.
 */
static void
cmd_resume_devices(int fd)
{
	send_ok(fd);
}

/*
 * Handle a single client connection.
 */
static void
handle_client(int cfd)
{
	FILE *fp;
	char line[CTL_MAXLINE];

	fp = fdopen(cfd, "r");
	if (fp == NULL) {
		(void) close(cfd);
		return;
	}

	while (fgets(line, sizeof (line), fp) != NULL) {
		/* Strip trailing newline */
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';

		char *cmd = json_get_string(line, "command");
		if (cmd == NULL) {
			send_error(cfd, "missing command field");
			continue;
		}

		if (strcmp(cmd, "status") == 0) {
			cmd_status(cfd);
		} else if (strcmp(cmd, "pause-devices") == 0) {
			cmd_pause_devices(cfd);
		} else if (strcmp(cmd, "export-devices") == 0) {
			cmd_export_devices(cfd);
		} else if (strcmp(cmd, "import-devices") == 0) {
			uint64_t data_len = 0;
			if (json_get_uint64(line, "len", &data_len) != 0) {
				send_error(cfd, "missing len field");
			} else {
				cmd_import_devices(cfd, data_len);
			}
		} else if (strcmp(cmd, "resume-devices") == 0) {
			cmd_resume_devices(cfd);
		} else if (strcmp(cmd, "pause-vm") == 0) {
			cmd_pause_vm(cfd);
		} else if (strcmp(cmd, "resume-vm") == 0) {
			cmd_resume_vm(cfd);
		} else {
			send_error(cfd, "unknown command");
		}

		free(cmd);
	}

	fclose(fp);
}

/*
 * Listener thread — accepts connections and handles them serially.
 * Only one client at a time (migration is a serialized operation).
 */
static void *
control_thread(void *arg __unused)
{
	sigset_t set;

	/* Block all signals in this thread */
	(void) sigfillset(&set);
	(void) pthread_sigmask(SIG_BLOCK, &set, NULL);

	while (ctl_running) {
		int cfd = accept(ctl_listen_fd, NULL, NULL);
		if (cfd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (!ctl_running)
				break;
			fprintf(stderr, "ctl: accept failed: %s\n",
			    strerror(errno));
			continue;
		}

		handle_client(cfd);
	}

	return (NULL);
}

void
bhyve_control_init(struct vmctx *ctx, int ncpus, const char *path)
{
	struct sockaddr_un saddr;
	mode_t old_umask;
	int fd;

	if (path == NULL)
		return;

	ctl_ctx = ctx;
	ctl_ncpus = ncpus;
	ctl_path = strdup(path);

	/* Remove stale socket */
	(void) unlink(path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "ctl: socket failed: %s\n", strerror(errno));
		return;
	}

	memset(&saddr, 0, sizeof (saddr));
	saddr.sun_family = AF_UNIX;
	(void) strlcpy(saddr.sun_path, path, sizeof (saddr.sun_path));

	/* Restrict to owner-only access */
	old_umask = umask(0177);
	if (bind(fd, (struct sockaddr *)&saddr, sizeof (saddr)) != 0) {
		fprintf(stderr, "ctl: bind %s failed: %s\n",
		    path, strerror(errno));
		umask(old_umask);
		(void) close(fd);
		return;
	}
	umask(old_umask);

	if (listen(fd, 1) != 0) {
		fprintf(stderr, "ctl: listen failed: %s\n", strerror(errno));
		(void) close(fd);
		(void) unlink(path);
		return;
	}

	ctl_listen_fd = fd;
	ctl_running = 1;

	if (pthread_create(&ctl_thread, NULL, control_thread, NULL) != 0) {
		fprintf(stderr, "ctl: pthread_create failed: %s\n",
		    strerror(errno));
		(void) close(fd);
		(void) unlink(path);
		ctl_listen_fd = -1;
		return;
	}

	fprintf(stderr, "ctl: listening on %s\n", path);
}

void
bhyve_control_fini(void)
{
	if (ctl_listen_fd < 0)
		return;

	ctl_running = 0;

	/* Closing the listen fd unblocks accept() */
	(void) close(ctl_listen_fd);
	ctl_listen_fd = -1;

	(void) pthread_join(ctl_thread, NULL);

	if (ctl_path != NULL) {
		(void) unlink(ctl_path);
		free(ctl_path);
		ctl_path = NULL;
	}
}

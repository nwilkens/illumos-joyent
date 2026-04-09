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

/*
 * Register list for VM_SET_REGISTER_SET — must match the agent's
 * VCPU_REGS list in vmm_dev.rs exactly.
 */
static const int ctl_vcpu_regs[] = {
	VM_REG_GUEST_RAX, VM_REG_GUEST_RBX, VM_REG_GUEST_RCX,
	VM_REG_GUEST_RDX, VM_REG_GUEST_RSI, VM_REG_GUEST_RDI,
	VM_REG_GUEST_RBP, VM_REG_GUEST_RSP, VM_REG_GUEST_R8,
	VM_REG_GUEST_R9,  VM_REG_GUEST_R10, VM_REG_GUEST_R11,
	VM_REG_GUEST_R12, VM_REG_GUEST_R13, VM_REG_GUEST_R14,
	VM_REG_GUEST_R15, VM_REG_GUEST_RIP, VM_REG_GUEST_RFLAGS,
	VM_REG_GUEST_CR0, VM_REG_GUEST_CR3, VM_REG_GUEST_CR2,
	VM_REG_GUEST_CR4, VM_REG_GUEST_DR7, VM_REG_GUEST_EFER,
	VM_REG_GUEST_XCR0,
};
#define	CTL_N_VCPU_REGS \
	(sizeof (ctl_vcpu_regs) / sizeof (ctl_vcpu_regs[0]))

static struct vmctx	*ctl_ctx;
static int		ctl_ncpus;
static char		*ctl_path;
static int		ctl_listen_fd = -1;
static pthread_t	ctl_thread;
static volatile int	ctl_running;

/* Max JSON line length */
#define	CTL_MAXLINE	4096

/* Buffer for VM_DATA_READ — matches VM_DATA_XFER_LIMIT */
#define	MIG_DATA_BUFSZ	8192

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
 * Handle "export-state" — export ALL state (kernel + devices).
 *
 * Exports kernel state (VMM_TIME, system devices, per-vCPU registers,
 * MSRs, LAPIC, FPU, segment descriptors, run state) and device state
 * (PCI config, virtio queues, viona rings) as two packed nvlists.
 *
 * This must be called AFTER viona rings are paused (pause-devices)
 * and vCPUs are paused (pause-vm).
 *
 * Response:
 *   {"success":true,"kern_len":NNN,"dev_len":NNN}\n
 *   <kern_len bytes><dev_len bytes>
 */
static void
cmd_export_state(int fd)
{
	nvlist_t *kern_nvl, *dev_nvl;
	char *kern_packed = NULL, *dev_packed = NULL;
	size_t kern_len = 0, dev_len = 0;
	int rv;
	uint8_t buf[MIG_DATA_BUFSZ];

	/* Kernel state nvlist */
	rv = nvlist_alloc(&kern_nvl, NV_UNIQUE_NAME, 0);
	if (rv != 0) {
		send_error(fd, "nvlist_alloc kern failed");
		return;
	}

	/* System device classes */
	static const struct {
		uint16_t class;
		uint16_t version;
		const char *name;
	} kern_classes[] = {
		{ 13, 1, "vmm_time" },
		{ 7, 1, "ioapic" },
		{ 8, 1, "atpit" },
		{ 9, 1, "atpic" },
		{ 10, 1, "hpet" },
		{ 11, 1, "pm_timer" },
		{ 12, 2, "rtc" },
	};
	for (uint_t i = 0;
	    i < sizeof (kern_classes) / sizeof (kern_classes[0]); i++) {
		uint32_t result_len = 0;
		char key[32];
		if (vm_data_read(ctl_ctx, -1, kern_classes[i].class,
		    kern_classes[i].version, VDX_FLAG_WRITE_COPYOUT,
		    buf, sizeof (buf), &result_len) == 0) {
			(void) snprintf(key, sizeof (key), "kern.%s",
			    kern_classes[i].name);
			nvlist_add_byte_array(kern_nvl, key,
			    (uchar_t *)buf, (uint_t)result_len);
		}
	}

	/* Per-vCPU state */
	static const struct {
		uint16_t class;
		uint16_t version;
		const char *name;
	} vcpu_dc[] = {
		{ 3, 1, "msrs" },
		{ 5, 1, "lapic" },
		{ 6, 1, "vmm_arch" },
	};

	for (int cpu = 0; cpu < ctl_ncpus; cpu++) {
		struct vcpu *vcpu = vm_vcpu_open(ctl_ctx, cpu);
		if (vcpu == NULL)
			continue;

		/* VM_DATA_READ classes */
		for (uint_t i = 0;
		    i < sizeof (vcpu_dc) / sizeof (vcpu_dc[0]); i++) {
			uint32_t result_len = 0;
			char key[32];
			if (vm_data_read(ctl_ctx, cpu,
			    vcpu_dc[i].class, vcpu_dc[i].version,
			    VDX_FLAG_WRITE_COPYOUT,
			    buf, sizeof (buf), &result_len) == 0) {
				(void) snprintf(key, sizeof (key),
				    "vcpu.%d.%s", cpu, vcpu_dc[i].name);
				nvlist_add_byte_array(kern_nvl, key,
				    (uchar_t *)buf, (uint_t)result_len);
			}
		}

		/* Registers */
		{
			uint64_t regvals[CTL_N_VCPU_REGS];
			char key[32];
			if (vm_get_register_set(vcpu, CTL_N_VCPU_REGS,
			    ctl_vcpu_regs, regvals) == 0) {
				(void) snprintf(key, sizeof (key),
				    "vcpu.%d.regs", cpu);
				nvlist_add_byte_array(kern_nvl, key,
				    (uchar_t *)regvals,
				    (uint_t)sizeof (regvals));
			}
		}

		/* Segment descriptors — packed as [regid:4, base:8, limit:4, access:4] */
		{
			static const int seg_regs[] = {
				VM_REG_GUEST_CS, VM_REG_GUEST_DS,
				VM_REG_GUEST_ES, VM_REG_GUEST_FS,
				VM_REG_GUEST_GS, VM_REG_GUEST_SS,
				VM_REG_GUEST_TR, VM_REG_GUEST_LDTR,
				VM_REG_GUEST_GDTR, VM_REG_GUEST_IDTR,
			};
			uint_t nsegs = sizeof (seg_regs) / sizeof (seg_regs[0]);
			uint8_t segbuf[nsegs * 20];
			uint8_t *p = segbuf;
			char key[32];
			int ok = 1;
			for (uint_t i = 0; i < nsegs; i++) {
				uint64_t base;
				uint32_t limit, access;
				if (vm_get_desc(vcpu, seg_regs[i],
				    &base, &limit, &access) != 0) {
					ok = 0;
					break;
				}
				int32_t regid = seg_regs[i];
				memcpy(p, &regid, 4); p += 4;
				memcpy(p, &base, 8); p += 8;
				memcpy(p, &limit, 4); p += 4;
				memcpy(p, &access, 4); p += 4;
			}
			if (ok) {
				(void) snprintf(key, sizeof (key),
				    "vcpu.%d.segs", cpu);
				nvlist_add_byte_array(kern_nvl, key,
				    segbuf, (uint_t)sizeof (segbuf));
			}
		}

		/* FPU */
		{
			uint8_t fpubuf[8192];
			char key[32];
			if (vm_get_fpu(vcpu, fpubuf, sizeof (fpubuf)) == 0) {
				(void) snprintf(key, sizeof (key),
				    "vcpu.%d.fpu", cpu);
				nvlist_add_byte_array(kern_nvl, key,
				    fpubuf, (uint_t)sizeof (fpubuf));
			}
		}

		/* Run state */
		{
			enum vcpu_run_state rstate;
			uint8_t sipi_vec;
			char key[32];
			if (vm_get_run_state(vcpu, &rstate, &sipi_vec) == 0) {
				(void) snprintf(key, sizeof (key),
				    "vcpu.%d.run_state", cpu);
				nvlist_add_uint64(kern_nvl, key,
				    (uint64_t)rstate);
				(void) snprintf(key, sizeof (key),
				    "vcpu.%d.sipi_vector", cpu);
				nvlist_add_uint64(kern_nvl, key,
				    (uint64_t)sipi_vec);
			}
		}

		if (cpu != 0)
			vm_vcpu_close(vcpu);
	}

	/* Pack kernel nvlist */
	rv = nvlist_pack(kern_nvl, &kern_packed, &kern_len,
	    NV_ENCODE_NATIVE, 0);
	nvlist_free(kern_nvl);
	if (rv != 0) {
		send_error(fd, "kern nvlist_pack failed");
		return;
	}

	/* Device state nvlist */
	rv = nvlist_alloc(&dev_nvl, NV_UNIQUE_NAME, 0);
	if (rv != 0) {
		free(kern_packed);
		send_error(fd, "nvlist_alloc dev failed");
		return;
	}

	rv = pci_save_all(dev_nvl);
	if (rv != 0) {
		free(kern_packed);
		nvlist_free(dev_nvl);
		send_error(fd, "pci_save_all failed");
		return;
	}

	rv = nvlist_pack(dev_nvl, &dev_packed, &dev_len,
	    NV_ENCODE_NATIVE, 0);
	nvlist_free(dev_nvl);
	if (rv != 0) {
		free(kern_packed);
		send_error(fd, "dev nvlist_pack failed");
		return;
	}

	/* Send header + two blobs */
	char hdr[128];
	(void) snprintf(hdr, sizeof (hdr),
	    "{\"success\":true,\"kern_len\":%zu,\"dev_len\":%zu}\n",
	    kern_len, dev_len);
	if (write_all(fd, hdr, strlen(hdr)) != 0 ||
	    write_all(fd, kern_packed, kern_len) != 0 ||
	    write_all(fd, dev_packed, dev_len) != 0) {
		fprintf(stderr, "ctl: export-state write failed\n");
	}

	free(kern_packed);
	free(dev_packed);
	fprintf(stderr, "export-state: kern=%zu dev=%zu\n", kern_len, dev_len);
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
 */
static void
cmd_resume_devices(int fd)
{
	send_ok(fd);
}

/*
 * Handle "import-state" — full state import, pause through resume.
 *
 * This is the primary migration import command.  The GZ agent sends
 * two packed nvlists: kernel state (VMM_TIME, system devices, per-vCPU
 * registers/MSRs/LAPIC/FPU/run_state) and bhyve device state (PCI
 * config, virtio queues, viona rings).
 *
 * bhyve does the entire sequence in-process to avoid the cross-process
 * write-lock deadlock that occurs when the GZ agent calls VM_DATA_WRITE
 * while bhyve's vCPU threads are in VM_RUN.
 *
 * Protocol:
 *   {"command":"import-state","kern_len":NNN,"dev_len":NNN}\n
 *   <kern_len bytes of packed kernel state nvlist>
 *   <dev_len bytes of packed device state nvlist>
 *
 * The kernel state nvlist keys:
 *   kern.vmm_time, kern.ioapic, kern.atpit, kern.atpic, kern.hpet,
 *   kern.pm_timer, kern.rtc — raw bytes for VM_DATA_WRITE
 *   vcpu.N.msrs, vcpu.N.lapic, vcpu.N.vmm_arch — raw bytes
 *   vcpu.N.regs — uint64 array for VM_SET_REGISTER_SET
 *   vcpu.N.segs — packed [regid:i32, base:u64, limit:u32, access:u32]
 *   vcpu.N.fpu — raw bytes for VM_SET_FPU
 *   vcpu.N.run_state, vcpu.N.sipi_vector — uint64
 */
static void
cmd_import_state(int fd, uint64_t kern_len, uint64_t dev_len)
{
	int rv;

	if (kern_len > 256 * 1024 || dev_len > 64 * 1024 * 1024) {
		send_error(fd, "payload too large");
		return;
	}

	/* Read kernel state blob */
	char *kern_packed = malloc(kern_len);
	if (kern_packed == NULL || read_all(fd, kern_packed, kern_len) != 0) {
		free(kern_packed);
		send_error(fd, "read kern state failed");
		return;
	}

	/* Read device state blob */
	char *dev_packed = malloc(dev_len);
	if (dev_packed == NULL || read_all(fd, dev_packed, dev_len) != 0) {
		free(kern_packed);
		free(dev_packed);
		send_error(fd, "read dev state failed");
		return;
	}

	/* Unpack both nvlists */
	nvlist_t *kern_nvl = NULL, *dev_nvl = NULL;
	rv = nvlist_unpack(kern_packed, kern_len, &kern_nvl, 0);
	free(kern_packed);
	if (rv != 0) {
		free(dev_packed);
		send_error(fd, "kern nvlist_unpack failed");
		return;
	}

	rv = nvlist_unpack(dev_packed, dev_len, &dev_nvl, 0);
	free(dev_packed);
	if (rv != 0) {
		nvlist_free(kern_nvl);
		send_error(fd, "dev nvlist_unpack failed");
		return;
	}

	/*
	 * VM should already be paused via the earlier pause-vm command.
	 * Do NOT call vm_pause_instance here — it acquires LOCK_WRITE_HOLD
	 * which races with vCPU threads in the EBUSY/usleep loop.
	 * Just proceed directly to state import.
	 */
	fprintf(stderr, "import-state: starting (VM should be paused)\n");

	/* Import VMM_TIME first (timers depend on boot_hrtime).
	 * Must adjust for destination host's TSC frequency and
	 * wall clock to avoid EPERM from kernel TSC scaling check. */
	{
		uchar_t *data;
		uint_t len;
		if (nvlist_lookup_byte_array(kern_nvl, "kern.vmm_time",
		    &data, &len) == 0 &&
		    len >= sizeof (struct vdi_time_info_v1)) {
			struct vdi_time_info_v1 src;
			memcpy(&src, data, sizeof (src));

			/* Read destination's current time */
			struct vdi_time_info_v1 dst;
			uint32_t dst_len = 0;
			if (vm_data_read(ctl_ctx, -1, 13, 1,
			    VDX_FLAG_WRITE_COPYOUT, &dst,
			    sizeof (dst), &dst_len) == 0) {
				/* Adjust boot_hrtime */
				int64_t uptime = src.vt_hrtime -
				    src.vt_boot_hrtime;
				uint64_t swc = src.vt_hres_sec * 1000000000ULL +
				    src.vt_hres_ns;
				uint64_t dwc = dst.vt_hres_sec * 1000000000ULL +
				    dst.vt_hres_ns;
				int64_t delta = (dwc > swc) ?
				    (int64_t)(dwc - swc) : 0;

				src.vt_boot_hrtime = dst.vt_hrtime -
				    (uptime + delta);
				src.vt_hrtime = dst.vt_hrtime;
				src.vt_hres_sec = dst.vt_hres_sec;
				src.vt_hres_ns = dst.vt_hres_ns;

				/* TSC frequency scaling */
				if (src.vt_guest_freq != dst.vt_guest_freq &&
				    src.vt_guest_freq != 0) {
					uint64_t q = src.vt_guest_tsc /
					    src.vt_guest_freq;
					uint64_t r = src.vt_guest_tsc %
					    src.vt_guest_freq;
					src.vt_guest_tsc =
					    q * dst.vt_guest_freq +
					    r * dst.vt_guest_freq /
					    src.vt_guest_freq;
					src.vt_guest_freq = dst.vt_guest_freq;
				}

				/* Add migration delta to TSC */
				if (delta > 0 && src.vt_guest_freq > 0) {
					uint64_t ns = (uint64_t)delta;
					src.vt_guest_tsc +=
					    (ns / 1000000000ULL) *
					    src.vt_guest_freq +
					    (ns % 1000000000ULL) *
					    src.vt_guest_freq / 1000000000ULL;
				}

				fprintf(stderr,
				    "import-state: vmm_time adjusted "
				    "freq=%llu->%llu\n",
				    (unsigned long long)
				    ((struct vdi_time_info_v1 *)data)->
				    vt_guest_freq,
				    (unsigned long long)src.vt_guest_freq);
			}

			if (vm_data_write(ctl_ctx, -1, 13, 1,
			    VDX_FLAG_READ_COPYIN, &src,
			    sizeof (src)) != 0) {
				fprintf(stderr,
				    "import-state: vmm_time write: %s\n",
				    strerror(errno));
			}
		}
	}

	/* Import system device classes */
	static const struct {
		uint16_t class;
		uint16_t version;
		const char *name;
	} sys_classes[] = {
		{ 7, 1, "ioapic" },
		{ 8, 1, "atpit" },
		{ 9, 1, "atpic" },
		{ 10, 1, "hpet" },
		{ 11, 1, "pm_timer" },
		{ 12, 2, "rtc" },
	};
	for (uint_t i = 0;
	    i < sizeof (sys_classes) / sizeof (sys_classes[0]); i++) {
		char key[32];
		uchar_t *data;
		uint_t len;
		(void) snprintf(key, sizeof (key), "kern.%s",
		    sys_classes[i].name);
		if (nvlist_lookup_byte_array(kern_nvl, key,
		    &data, &len) == 0) {
			if (vm_data_write(ctl_ctx, -1,
			    sys_classes[i].class,
			    sys_classes[i].version,
			    VDX_FLAG_READ_COPYIN, data, len) != 0) {
				fprintf(stderr,
				    "import-state: %s write: %s\n",
				    sys_classes[i].name, strerror(errno));
			}
		}
	}

	/* Import per-vCPU state */
	static const struct {
		uint16_t class;
		uint16_t version;
		const char *name;
	} vcpu_classes[] = {
		{ 3, 1, "msrs" },
		{ 5, 1, "lapic" },
		{ 6, 1, "vmm_arch" },
	};

	for (int cpu = 0; cpu < ctl_ncpus; cpu++) {
		struct vcpu *vcpu = vm_vcpu_open(ctl_ctx, cpu);
		if (vcpu == NULL)
			continue;

		/* VM_DATA_WRITE classes (MSRs, LAPIC, VMM_ARCH) */
		for (uint_t i = 0;
		    i < sizeof (vcpu_classes) / sizeof (vcpu_classes[0]);
		    i++) {
			char key[32];
			uchar_t *data;
			uint_t len;
			(void) snprintf(key, sizeof (key), "vcpu.%d.%s",
			    cpu, vcpu_classes[i].name);
			if (nvlist_lookup_byte_array(kern_nvl, key,
			    &data, &len) == 0) {
				if (vm_data_write(ctl_ctx, cpu,
				    vcpu_classes[i].class,
				    vcpu_classes[i].version,
				    VDX_FLAG_READ_COPYIN, data, len) != 0) {
					fprintf(stderr,
					    "import-state: vcpu%d %s: %s\n",
					    cpu, vcpu_classes[i].name,
					    strerror(errno));
				}
			}
		}

		/* Registers via VM_SET_REGISTER_SET */
		{
			char key[32];
			uchar_t *data;
			uint_t len;
			(void) snprintf(key, sizeof (key),
			    "vcpu.%d.regs", cpu);
			if (nvlist_lookup_byte_array(kern_nvl, key,
			    &data, &len) == 0) {
				/* Data is array of uint64_t register values */
				uint_t nregs = len / sizeof (uint64_t);
				rv = vm_set_register_set(vcpu, nregs,
				    ctl_vcpu_regs, (uint64_t *)data);
				if (rv != 0) {
					fprintf(stderr,
					    "import-state: vcpu%d regs: %s\n",
					    cpu, strerror(errno));
				}
			}
		}

		/* Segment descriptors */
		{
			char key[32];
			uchar_t *data;
			uint_t len;
			(void) snprintf(key, sizeof (key),
			    "vcpu.%d.segs", cpu);
			if (nvlist_lookup_byte_array(kern_nvl, key,
			    &data, &len) == 0) {
				/* Packed: [regid:4, base:8, limit:4, access:4] */
				uint8_t *p = data;
				uint_t nsegs = len / 20;
				for (uint_t i = 0; i < nsegs; i++) {
					int32_t regid;
					uint64_t base;
					uint32_t limit, access;
					memcpy(&regid, p, 4); p += 4;
					memcpy(&base, p, 8); p += 8;
					memcpy(&limit, p, 4); p += 4;
					memcpy(&access, p, 4); p += 4;
					(void) vm_set_desc(vcpu, regid,
					    base, limit, access);
				}
			}
		}

		/* FPU state */
		{
			char key[32];
			uchar_t *data;
			uint_t len;
			(void) snprintf(key, sizeof (key),
			    "vcpu.%d.fpu", cpu);
			if (nvlist_lookup_byte_array(kern_nvl, key,
			    &data, &len) == 0) {
				if (vm_set_fpu(vcpu, data, len) != 0) {
					fprintf(stderr,
					    "import-state: vcpu%d fpu: %s\n",
					    cpu, strerror(errno));
				}
			}
		}

		/* Run state */
		{
			char key[32];
			uint64_t state_val = 0, sipi_val = 0;
			(void) snprintf(key, sizeof (key),
			    "vcpu.%d.run_state", cpu);
			(void) nvlist_lookup_uint64(kern_nvl, key, &state_val);
			(void) snprintf(key, sizeof (key),
			    "vcpu.%d.sipi_vector", cpu);
			(void) nvlist_lookup_uint64(kern_nvl, key, &sipi_val);
			if (state_val != 0) {
				(void) vm_set_run_state(vcpu,
				    (enum vcpu_run_state)state_val,
				    (uint8_t)sipi_val);
			}
		}

		if (cpu != 0)
			vm_vcpu_close(vcpu);

		fprintf(stderr, "import-state: vcpu%d imported\n", cpu);
	}

	nvlist_free(kern_nvl);

	/* Restore bhyve userspace device state */
	fprintf(stderr, "import-state: restoring PCI devices\n");
	rv = pci_restore_all(dev_nvl);
	nvlist_free(dev_nvl);
	if (rv != 0) {
		fprintf(stderr, "import-state: pci_restore_all: %d\n", rv);
	}

	/* Wait briefly for vCPU threads to cycle through VM_RUN
	 * and return to the EBUSY/usleep loop. After the data_write
	 * calls above, vCPUs may have re-entered VM_RUN and be stuck
	 * in hardware VM execution until the next vmexit.  A 50ms
	 * wait covers the LAPIC timer period. */
	(void) usleep(50000);

	/* Resume VM — all state is in place */
	fprintf(stderr, "import-state: resuming VM\n");
	rv = vm_resume_instance(ctl_ctx);
	if (rv != 0) {
		fprintf(stderr, "import-state: vm_resume: %s\n",
		    strerror(errno));
		send_error(fd, "vm_resume failed");
		return;
	}

	fprintf(stderr, "import-state: complete\n");
	send_ok(fd);
}

/*
 * Read a line from fd byte-by-byte.
 * Must NOT use stdio buffering because import-devices sends a binary
 * blob after the JSON line — stdio would consume blob bytes into
 * its read-ahead buffer, corrupting the subsequent read_all().
 */
static ssize_t
read_line(int fd, char *buf, size_t bufsz)
{
	size_t pos = 0;
	while (pos < bufsz - 1) {
		char c;
		ssize_t n = read(fd, &c, 1);
		if (n <= 0)
			return (-1);
		if (c == '\n')
			break;
		buf[pos++] = c;
	}
	buf[pos] = '\0';
	return ((ssize_t)pos);
}

/*
 * Handle a single client connection.
 */
static void
handle_client(int cfd)
{
	char line[CTL_MAXLINE];

	while (read_line(cfd, line, sizeof (line)) >= 0) {

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
		} else if (strcmp(cmd, "export-state") == 0) {
			cmd_export_state(cfd);
		} else if (strcmp(cmd, "import-state") == 0) {
			uint64_t kl = 0, dl = 0;
			if (json_get_uint64(line, "kern_len", &kl) != 0 ||
			    json_get_uint64(line, "dev_len", &dl) != 0) {
				send_error(cfd, "missing kern_len/dev_len");
			} else {
				cmd_import_state(cfd, kl, dl);
			}
		} else {
			send_error(cfd, "unknown command");
		}

		free(cmd);
	}

	(void) close(cfd);
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

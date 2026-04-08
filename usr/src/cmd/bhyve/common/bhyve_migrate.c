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
 * bhyve VM state checkpoint/export for live migration.
 *
 * Captures all VM state into a packed nvlist that can be written to a
 * file (for testing) or a Unix socket (for production migration via a
 * GZ-side agent).
 *
 * State is captured in a specific order:
 * 1. VMM time data (must be imported first on destination)
 * 2. Per-vCPU state (registers, FPU, LAPIC, VMM arch, run state)
 * 3. System device state (IOAPIC, ATPIT, ATPIC, HPET, PM_TIMER, RTC)
 * 4. Userspace device state (via pci_save_all)
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <machine/vmm.h>
#include <machine/vmm_dev.h>
#include <vmmapi.h>
#include <libnvpair.h>

#include "bhyve_migrate.h"
#include "pci_emul.h"

/* Buffer for VM_DATA_READ/WRITE — VM_DATA_XFER_LIMIT is 8192 */
#define	MIG_DATA_BUFSZ	8192

/*
 * Kernel device data classes to save (system-wide, vcpuid = -1).
 * Order matters for restore — time must come first.
 */
static const struct {
	uint16_t	class;
	uint16_t	version;
	const char	*name;
} kern_dev_classes[] = {
	{ 13 /* VDC_VMM_TIME */,	1,	"vmm_time" },
	{ 7  /* VDC_IOAPIC */,		1,	"ioapic" },
	{ 8  /* VDC_ATPIT */,		1,	"atpit" },
	{ 9  /* VDC_ATPIC */,		1,	"atpic" },
	{ 10 /* VDC_HPET */,		1,	"hpet" },
	{ 11 /* VDC_PM_TIMER */,	1,	"pm_timer" },
	{ 12 /* VDC_RTC */,		2,	"rtc" },
};
#define	N_KERN_DEV_CLASSES \
	(sizeof (kern_dev_classes) / sizeof (kern_dev_classes[0]))

/*
 * Per-vCPU data classes that work via VM_DATA_READ/WRITE.
 *
 * NOTE: VDC_REGISTER and VDC_FPU are NOT implemented via VM_DATA_READ.
 * VDC_REGISTER would panic the kernel. VDC_FPU is explicitly unimplemented.
 * Use VM_GET/SET_REGISTER_SET and VM_GET/SET_FPU ioctls instead.
 */
static const struct {
	uint16_t	class;
	uint16_t	version;
	const char	*name;
} vcpu_classes[] = {
	{ 3 /* VDC_MSR */,		1,	"msrs" },
	{ 5 /* VDC_LAPIC */,		1,	"lapic" },
	{ 6 /* VDC_VMM_ARCH */,	1,	"vmm_arch" },
};
#define	N_VCPU_CLASSES \
	(sizeof (vcpu_classes) / sizeof (vcpu_classes[0]))

/*
 * General registers to save/restore via vm_get/set_register_set.
 * Matches the Rust VMM implementation's VCPU_REGS list.
 */
static const int vcpu_regs[] = {
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
#define	N_VCPU_REGS	(sizeof (vcpu_regs) / sizeof (vcpu_regs[0]))

/*
 * Segment descriptor registers — need base/limit/access via vm_get/set_desc.
 */
static const int vcpu_seg_descs[] = {
	VM_REG_GUEST_CS,  VM_REG_GUEST_DS,  VM_REG_GUEST_ES,
	VM_REG_GUEST_FS,  VM_REG_GUEST_GS,  VM_REG_GUEST_SS,
	VM_REG_GUEST_TR,  VM_REG_GUEST_LDTR,
	VM_REG_GUEST_GDTR, VM_REG_GUEST_IDTR,
};
#define	N_VCPU_SEG_DESCS (sizeof (vcpu_seg_descs) / sizeof (vcpu_seg_descs[0]))

/* FPU buffer size — max 2 pages per kernel limit */
#define	FPU_BUF_SIZE	8192

/*
 * Read kernel device state via VM_DATA_READ.
 * Returns allocated buffer (caller frees) and sets *lenp to actual size.
 * Returns NULL on failure.
 */
static void *
mig_data_read(struct vmctx *ctx, int vcpuid, uint16_t class,
    uint16_t version, uint32_t *lenp)
{
	uint8_t buf[MIG_DATA_BUFSZ];
	uint32_t result_len = 0;

	if (vm_data_read(ctx, vcpuid, class, version,
	    VDX_FLAG_WRITE_COPYOUT, buf, sizeof (buf), &result_len) != 0) {
		fprintf(stderr, "mig: VM_DATA_READ class=%d vcpu=%d: %s\n",
		    class, vcpuid, strerror(errno));
		return (NULL);
	}

	void *data = malloc(result_len);
	if (data == NULL)
		return (NULL);
	memcpy(data, buf, result_len);
	*lenp = result_len;
	return (data);
}

int
bhyve_migrate_export(struct vmctx *ctx, int ncpus, int fd)
{
	nvlist_t *nvl;
	int rv;

	rv = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0);
	if (rv != 0)
		return (-1);

	nvlist_add_uint64(nvl, "version", 1);
	nvlist_add_uint64(nvl, "ncpus", (uint64_t)ncpus);

	/*
	 * 1. Kernel device state (system-wide).
	 *    VMM time is first in the array, ensuring it's saved/restored
	 *    before device timers that depend on boot_hrtime.
	 */
	for (uint_t i = 0; i < N_KERN_DEV_CLASSES; i++) {
		uint32_t len = 0;
		void *data;
		char key[64];

		data = mig_data_read(ctx, -1, kern_dev_classes[i].class,
		    kern_dev_classes[i].version, &len);
		if (data == NULL) {
			fprintf(stderr, "mig: failed to read %s\n",
			    kern_dev_classes[i].name);
			/* Continue — some classes may not be present */
			continue;
		}

		(void) snprintf(key, sizeof (key), "kern.%s",
		    kern_dev_classes[i].name);
		nvlist_add_byte_array(nvl, key, (uchar_t *)data, (uint_t)len);
		free(data);
	}

	/*
	 * 2. Per-vCPU state.
	 */
	for (int cpu = 0; cpu < ncpus; cpu++) {
		nvlist_t *cpu_nvl;
		char key[32];

		rv = nvlist_alloc(&cpu_nvl, NV_UNIQUE_NAME, 0);
		if (rv != 0)
			continue;

		struct vcpu *vcpu = vm_vcpu_open(ctx, cpu);
		if (vcpu == NULL)
			continue;

		/*
		 * 2a. General registers via vm_get_register_set (batch).
		 */
		{
			uint64_t regvals[N_VCPU_REGS];
			if (vm_get_register_set(vcpu, N_VCPU_REGS,
			    vcpu_regs, regvals) == 0) {
				nvlist_add_byte_array(cpu_nvl, "registers",
				    (uchar_t *)regvals,
				    (uint_t)sizeof (regvals));
			} else {
				fprintf(stderr,
				    "mig: vm_get_register_set vcpu%d: %s\n",
				    cpu, strerror(errno));
			}
		}

		/*
		 * 2b. Segment descriptors (base/limit/access).
		 * Packed as: [reg_id:u32, base:u64, limit:u32, access:u32]
		 */
		{
			uint8_t segbuf[N_VCPU_SEG_DESCS * 20];
			uint8_t *p = segbuf;
			int seg_ok = 1;
			for (uint_t i = 0; i < N_VCPU_SEG_DESCS; i++) {
				uint64_t base;
				uint32_t limit, access;
				if (vm_get_desc(vcpu, vcpu_seg_descs[i],
				    &base, &limit, &access) != 0) {
					fprintf(stderr,
					    "mig: vm_get_desc vcpu%d "
					    "reg%d: %s\n",
					    cpu, vcpu_seg_descs[i],
					    strerror(errno));
					seg_ok = 0;
					break;
				}
				uint32_t regid = (uint32_t)vcpu_seg_descs[i];
				memcpy(p, &regid, 4); p += 4;
				memcpy(p, &base, 8); p += 8;
				memcpy(p, &limit, 4); p += 4;
				memcpy(p, &access, 4); p += 4;
			}
			if (seg_ok) {
				nvlist_add_byte_array(cpu_nvl, "seg_descs",
				    segbuf, (uint_t)sizeof (segbuf));
			}
		}

		/*
		 * 2c. FPU state via VM_GET_FPU.
		 */
		{
			uint8_t fpubuf[FPU_BUF_SIZE];
			if (vm_get_fpu(vcpu, fpubuf, sizeof (fpubuf)) == 0) {
				nvlist_add_byte_array(cpu_nvl, "fpu",
				    fpubuf, (uint_t)sizeof (fpubuf));
			} else {
				fprintf(stderr, "mig: vm_get_fpu vcpu%d: %s\n",
				    cpu, strerror(errno));
			}
		}

		/*
		 * 2d. Kernel per-vCPU classes via VM_DATA_READ
		 *     (MSRs, LAPIC, VMM_ARCH).
		 */
		for (uint_t i = 0; i < N_VCPU_CLASSES; i++) {
			uint32_t len = 0;
			void *data;

			data = mig_data_read(ctx, cpu,
			    vcpu_classes[i].class,
			    vcpu_classes[i].version, &len);
			if (data == NULL)
				continue;

			nvlist_add_byte_array(cpu_nvl,
			    vcpu_classes[i].name,
			    (uchar_t *)data, (uint_t)len);
			free(data);
		}

		/* 2e. vCPU run state */
		{
			enum vcpu_run_state rstate;
			uint8_t sipi_vec;
			if (vm_get_run_state(vcpu, &rstate, &sipi_vec) == 0) {
				nvlist_add_uint64(cpu_nvl, "run_state",
				    (uint64_t)rstate);
				nvlist_add_uint64(cpu_nvl, "sipi_vector",
				    (uint64_t)sipi_vec);
			}
		}

		if (cpu != 0)
			vm_vcpu_close(vcpu);

		(void) snprintf(key, sizeof (key), "vcpu.%d", cpu);
		nvlist_add_nvlist(nvl, key, cpu_nvl);
		nvlist_free(cpu_nvl);
	}

	/*
	 * 3. Userspace PCI device state.
	 */
	nvlist_t *pci_nvl;
	rv = nvlist_alloc(&pci_nvl, NV_UNIQUE_NAME, 0);
	if (rv == 0) {
		if (pci_save_all(pci_nvl) != 0) {
			fprintf(stderr, "mig: pci_save_all failed\n");
		}
		nvlist_add_nvlist(nvl, "pci", pci_nvl);
		nvlist_free(pci_nvl);
	}

	/*
	 * Pack and write to fd.
	 */
	char *packed = NULL;
	size_t packed_len = 0;
	rv = nvlist_pack(nvl, &packed, &packed_len, NV_ENCODE_NATIVE, 0);
	nvlist_free(nvl);

	if (rv != 0) {
		fprintf(stderr, "mig: nvlist_pack failed: %s\n",
		    strerror(rv));
		return (-1);
	}

	/* Write length header then data */
	uint64_t hdr = packed_len;
	if (write(fd, &hdr, sizeof (hdr)) != sizeof (hdr) ||
	    write(fd, packed, packed_len) != (ssize_t)packed_len) {
		fprintf(stderr, "mig: write failed: %s\n", strerror(errno));
		free(packed);
		return (-1);
	}

	free(packed);
	fprintf(stderr, "mig: exported %zu bytes of VM state\n", packed_len);
	return (0);
}

int
bhyve_migrate_import(struct vmctx *ctx, int ncpus, int fd)
{
	nvlist_t *nvl;
	char *packed = NULL;
	uint64_t packed_len;
	int rv;

	/* Read length header then data */
	if (read(fd, &packed_len, sizeof (packed_len)) != sizeof (packed_len)) {
		fprintf(stderr, "mig: read header failed\n");
		return (-1);
	}

	/* Sanity check: reject absurdly large payloads (max 256MB) */
	if (packed_len > (256 * 1024 * 1024)) {
		fprintf(stderr, "mig: payload too large: %llu bytes\n",
		    (unsigned long long)packed_len);
		return (-1);
	}

	packed = malloc(packed_len);
	if (packed == NULL)
		return (-1);

	ssize_t total = 0;
	while (total < (ssize_t)packed_len) {
		ssize_t n = read(fd, packed + total, packed_len - total);
		if (n <= 0) {
			free(packed);
			return (-1);
		}
		total += n;
	}

	rv = nvlist_unpack(packed, packed_len, &nvl, 0);
	free(packed);
	if (rv != 0) {
		fprintf(stderr, "mig: nvlist_unpack failed: %s\n",
		    strerror(rv));
		return (-1);
	}

	/*
	 * 1. Restore kernel device state (system-wide).
	 *    VMM time is first — timers depend on correct boot_hrtime.
	 */
	for (uint_t i = 0; i < N_KERN_DEV_CLASSES; i++) {
		char key[64];
		uchar_t *data;
		uint_t len;

		(void) snprintf(key, sizeof (key), "kern.%s",
		    kern_dev_classes[i].name);

		if (nvlist_lookup_byte_array(nvl, key, &data, &len) != 0)
			continue;

		if (vm_data_write(ctx, -1, kern_dev_classes[i].class,
		    kern_dev_classes[i].version,
		    VDX_FLAG_READ_COPYIN, data, len) != 0) {
			fprintf(stderr, "mig: VM_DATA_WRITE %s failed: %s\n",
			    kern_dev_classes[i].name, strerror(errno));
		}
	}

	/*
	 * 2. Restore per-vCPU state.
	 */
	for (int cpu = 0; cpu < ncpus; cpu++) {
		nvlist_t *cpu_nvl;
		char key[32];

		(void) snprintf(key, sizeof (key), "vcpu.%d", cpu);
		if (nvlist_lookup_nvlist(nvl, key, &cpu_nvl) != 0)
			continue;

		struct vcpu *vcpu = vm_vcpu_open(ctx, cpu);
		if (vcpu == NULL)
			continue;

		/* 2a. Restore general registers */
		{
			uchar_t *regdata;
			uint_t reglen;
			if (nvlist_lookup_byte_array(cpu_nvl, "registers",
			    &regdata, &reglen) == 0 &&
			    reglen == sizeof (uint64_t) * N_VCPU_REGS) {
				int rr = vm_set_register_set(vcpu, N_VCPU_REGS,
				    vcpu_regs, (uint64_t *)regdata);
				if (rr != 0) {
					fprintf(stderr,
					    "mig: vm_set_register_set "
					    "vcpu%d failed: %s\n",
					    cpu, strerror(errno));
				} else {
					fprintf(stderr,
					    "mig: vcpu%d registers "
					    "restored (%u regs)\n",
					    cpu, (uint_t)N_VCPU_REGS);
				}
			} else {
				fprintf(stderr,
				    "mig: vcpu%d no register data "
				    "(len=%u expected=%zu)\n",
				    cpu, reglen,
				    sizeof (uint64_t) * N_VCPU_REGS);
			}
		}

		/* 2b. Restore segment descriptors */
		{
			uchar_t *segdata;
			uint_t seglen;
			if (nvlist_lookup_byte_array(cpu_nvl, "seg_descs",
			    &segdata, &seglen) == 0 &&
			    seglen == N_VCPU_SEG_DESCS * 20) {
				uint8_t *p = segdata;
				for (uint_t i = 0; i < N_VCPU_SEG_DESCS; i++) {
					uint32_t regid;
					uint64_t base;
					uint32_t limit, access;
					memcpy(&regid, p, 4); p += 4;
					memcpy(&base, p, 8); p += 8;
					memcpy(&limit, p, 4); p += 4;
					memcpy(&access, p, 4); p += 4;
					/*
					 * Validate regid matches expected
					 * segment register to prevent
					 * arbitrary register writes from
					 * a crafted checkpoint.
					 */
					if ((int)regid != vcpu_seg_descs[i]) {
						fprintf(stderr,
						    "mig: bad seg regid %u "
						    "at index %u\n", regid, i);
						break;
					}
					(void) vm_set_desc(vcpu, (int)regid,
					    base, limit, access);
				}
			}
		}

		/* 2c. Restore FPU state */
		{
			uchar_t *fpudata;
			uint_t fpulen;
			if (nvlist_lookup_byte_array(cpu_nvl, "fpu",
			    &fpudata, &fpulen) == 0 &&
			    fpulen <= FPU_BUF_SIZE) {
				int fr = vm_set_fpu(vcpu, fpudata, fpulen);
				fprintf(stderr, "mig: vcpu%d FPU %s "
				    "(%u bytes)\n", cpu,
				    fr == 0 ? "restored" : "FAILED",
				    fpulen);
			}
		}

		/* 2d. Restore kernel per-vCPU classes (MSRs, LAPIC, VMM_ARCH) */
		for (uint_t i = 0; i < N_VCPU_CLASSES; i++) {
			uchar_t *data;
			uint_t len;

			if (nvlist_lookup_byte_array(cpu_nvl,
			    vcpu_classes[i].name, &data, &len) != 0)
				continue;

			if (vm_data_write(ctx, cpu, vcpu_classes[i].class,
			    vcpu_classes[i].version,
			    VDX_FLAG_READ_COPYIN, data, len) != 0) {
				fprintf(stderr,
				    "mig: VM_DATA_WRITE vcpu%d %s failed: %s\n",
				    cpu, vcpu_classes[i].name,
				    strerror(errno));
			}
		}

		/* 2e. Restore vCPU run state */
		{
			uint64_t val;
			enum vcpu_run_state rstate = VRS_HALT;
			uint8_t sipi_vec = 0;
			if (nvlist_lookup_uint64(cpu_nvl, "run_state",
			    &val) == 0)
				rstate = (enum vcpu_run_state)val;
			if (nvlist_lookup_uint64(cpu_nvl, "sipi_vector",
			    &val) == 0)
				sipi_vec = (uint8_t)val;
			(void) vm_set_run_state(vcpu, rstate, sipi_vec);
		}

		if (cpu != 0)
			vm_vcpu_close(vcpu);
	}

	/*
	 * 3. Restore userspace PCI device state.
	 */
	nvlist_t *pci_nvl;
	if (nvlist_lookup_nvlist(nvl, "pci", &pci_nvl) == 0) {
		if (pci_restore_all(pci_nvl) != 0) {
			fprintf(stderr, "mig: pci_restore_all failed\n");
		}
	}

	nvlist_free(nvl);
	fprintf(stderr, "mig: import complete\n");
	return (0);
}

int
bhyve_checkpoint_to_file(struct vmctx *ctx, int ncpus, const char *path)
{
	int fd, rv;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		fprintf(stderr, "mig: open %s: %s\n", path, strerror(errno));
		return (-1);
	}

	rv = bhyve_migrate_export(ctx, ncpus, fd);
	(void) close(fd);
	return (rv);
}

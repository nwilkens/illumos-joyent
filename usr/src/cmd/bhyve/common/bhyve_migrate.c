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
 * Per-vCPU data classes.
 */
static const struct {
	uint16_t	class;
	uint16_t	version;
	const char	*name;
} vcpu_classes[] = {
	{ 2 /* VDC_REGISTER */,		1,	"registers" },
	{ 3 /* VDC_MSR */,		1,	"msrs" },
	{ 4 /* VDC_FPU */,		1,	"fpu" },
	{ 5 /* VDC_LAPIC */,		1,	"lapic" },
	{ 6 /* VDC_VMM_ARCH */,	1,	"vmm_arch" },
};
#define	N_VCPU_CLASSES \
	(sizeof (vcpu_classes) / sizeof (vcpu_classes[0]))

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

		/* Save vCPU run state */
		struct vcpu *vcpu = vm_vcpu_open(ctx, cpu);
		if (vcpu != NULL) {
			enum vcpu_run_state rstate;
			uint8_t sipi_vec;
			if (vm_get_run_state(vcpu, &rstate, &sipi_vec) == 0) {
				nvlist_add_uint64(cpu_nvl, "run_state",
				    (uint64_t)rstate);
				nvlist_add_uint64(cpu_nvl, "sipi_vector",
				    (uint64_t)sipi_vec);
			}
			if (cpu != 0)
				vm_vcpu_close(vcpu);
		}

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

		/* Restore vCPU run state */
		uint64_t val;
		struct vcpu *vcpu = vm_vcpu_open(ctx, cpu);
		if (vcpu != NULL) {
			enum vcpu_run_state rstate = VRS_HALT;
			uint8_t sipi_vec = 0;
			if (nvlist_lookup_uint64(cpu_nvl, "run_state",
			    &val) == 0)
				rstate = (enum vcpu_run_state)val;
			if (nvlist_lookup_uint64(cpu_nvl, "sipi_vector",
			    &val) == 0)
				sipi_vec = (uint8_t)val;
			(void) vm_set_run_state(vcpu, rstate, sipi_vec);
			if (cpu != 0)
				vm_vcpu_close(vcpu);
		}
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

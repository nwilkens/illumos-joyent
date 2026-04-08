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

#ifndef	_BHYVE_MIGRATE_H_
#define	_BHYVE_MIGRATE_H_

#include <vmmapi.h>

/*
 * bhyve live migration - state export/import.
 *
 * Architecture:
 *   bhyve runs inside a restricted bhyve zone with no inter-host
 *   networking and no ZFS access. Migration is coordinated by a
 *   GZ-side agent that:
 *     - handles TCP between source/destination hosts
 *     - handles ZFS send/recv for disk migration
 *     - bridges to bhyve via a Unix socket
 *
 *   bhyve's role is limited to:
 *     - exporting VM state (kernel devices, vCPU regs, userspace devices)
 *     - importing VM state on the destination
 *
 *   The export/import functions write to/read from a file descriptor,
 *   which can be a file (for testing) or a Unix socket (for production
 *   migration via the GZ agent).
 */

/*
 * Export all VM state to an fd as a packed nvlist.
 * VM must be paused and viona rings must be paused before calling.
 *
 * Captures (in order):
 * 1. VMM time data (must be first for correct timer restoration)
 * 2. Per-vCPU state: registers, FPU, LAPIC, VMM arch, run state
 * 3. Kernel device state: IOAPIC, ATPIT, ATPIC, HPET, PM_TIMER, RTC
 * 4. Userspace device state: via pci_save_all()
 *
 * Returns 0 on success, -1 on failure.
 */
int	bhyve_migrate_export(struct vmctx *ctx, int ncpus, int fd);

/*
 * Import VM state from an fd (packed nvlist).
 * VM must be created and memory allocated before calling.
 *
 * Returns 0 on success, -1 on failure.
 */
int	bhyve_migrate_import(struct vmctx *ctx, int ncpus, int fd);

/*
 * Convenience: export to a file path (for testing/debugging).
 */
int	bhyve_checkpoint_to_file(struct vmctx *ctx, int ncpus,
	    const char *path);

#endif	/* _BHYVE_MIGRATE_H_ */

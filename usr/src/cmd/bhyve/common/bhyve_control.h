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

#ifndef	_BHYVE_CONTROL_H_
#define	_BHYVE_CONTROL_H_

#include <vmmapi.h>

/*
 * bhyve control socket — Unix socket interface for GZ migration agent.
 *
 * Provides a JSON-over-Unix-socket control plane that allows the GZ
 * migration agent to:
 *   - Query VM status
 *   - Pause/resume userspace device rings (viona)
 *   - Export/import userspace device state (PCI config, virtio queues)
 *
 * Kernel state (registers, LAPIC, MSRs, timers) and guest RAM are
 * accessed directly by the GZ agent via /dev/vmm/<name> ioctls.
 */

void	bhyve_control_init(struct vmctx *ctx, int ncpus, const char *path);
void	bhyve_control_fini(void);
void	bhyve_control_wait_import(void);

#endif	/* _BHYVE_CONTROL_H_ */

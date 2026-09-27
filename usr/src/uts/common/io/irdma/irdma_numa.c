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
 * Placement of the RDMA vectors near the device.  illumos keeps no
 * locality for devices, so the driver takes the lgroup whose CPUs read a
 * device register in the least time, if it is clearly the least; the
 * numa_lgrp property can name one instead.  Each vector's interrupt goes to
 * a CPU of that lgroup, one core per vector while cores last.  The vector
 * threads stay unbound unless numa_place asks, since a thread bound to its
 * interrupt's CPU waits behind each interrupt and cut multi-QP throughput
 * by two thirds.
 */

#include "irdma_verbs.h"

#include <sys/cpuvar.h>
#include <sys/pghw.h>
#include <sys/lgrp.h>
#include <sys/ddi_intr.h>
#include <sys/ddi_intr_impl.h>

#define	IRDMA_NUMA_READS	32
#define	IRDMA_NUMA_LGRPS	8

/* numa_place: what follows the chosen CPUs. */
#define	IRDMA_NUMA_INTR		0x1
#define	IRDMA_NUMA_THREAD	0x2

/* The least time of a register read from cp; the caller holds cpu_lock. */
static hrtime_t
irdma_numa_read_ns(irdma_t *irdma, cpu_t *cp)
{
	u32 *reg = irdma->irdma_sc.hw_regs[IRDMA_CQPTAIL];
	hrtime_t best = INT64_MAX, t0, dt;
	uint_t i;

	ASSERT(MUTEX_HELD(&cpu_lock));
	thread_affinity_set(curthread, cp->cpu_id);
	for (i = 0; i < IRDMA_NUMA_READS; i++) {
		t0 = gethrtime();
		(void) readl(reg);
		dt = gethrtime() - t0;
		best = MIN(best, dt);
	}
	thread_affinity_clear(curthread);
	return (best);
}

/*
 * The lgroup to place the vectors in, or LGRP_NONE.  The caller holds
 * cpu_lock.
 */
static lgrp_id_t
irdma_numa_nearest(irdma_t *irdma, char *msg, size_t len)
{
	lgrp_id_t ids[IRDMA_NUMA_LGRPS];
	hrtime_t ns[IRDMA_NUMA_LGRPS];
	uint_t n = 0, i, best = 0, next = 0;
	size_t off = 0;
	cpu_t *cp;

	ASSERT(MUTEX_HELD(&cpu_lock));
	cp = cpu_list;
	do {
		lgrp_id_t id = cp->cpu_lpl->lpl_lgrpid;

		if (!cpu_is_online(cp))
			continue;
		for (i = 0; i < n && ids[i] != id; i++)
			;
		if (i < n || n == IRDMA_NUMA_LGRPS)
			continue;
		ids[n] = id;
		ns[n] = irdma_numa_read_ns(irdma, cp);
		off += snprintf(msg + off, len - MIN(off, len), " %d:%lldns",
		    (int)id, (longlong_t)ns[n]);
		n++;
	} while ((cp = cp->cpu_next) != cpu_list);

	if (n == 1)
		return (ids[0]);
	for (i = 1; i < n; i++) {
		if (ns[i] < ns[best])
			best = i;
	}
	next = best == 0 ? 1 : 0;
	for (i = 0; i < n; i++) {
		if (i != best && ns[i] < ns[next])
			next = i;
	}
	/* A near tie says nothing about where the device is. */
	if (n == 0 || ns[best] * 20 > ns[next] * 19)
		return (LGRP_NONE);
	return (ids[best]);
}

/*
 * Up to max online CPUs of lgroup lg, one per core, from the highest id
 * down, which the LAN queues and the OICR use least.  The caller holds
 * cpu_lock.
 */
static uint_t
irdma_numa_cpus(lgrp_id_t lg, processorid_t *cpus, uint_t max)
{
	id_t cores[IRDMA_MAX_VECTORS];
	uint_t n = 0, pass, i;
	cpu_t *cp;

	ASSERT(MUTEX_HELD(&cpu_lock));
	/* The second pass takes the CPUs of cores already used. */
	for (pass = 0; pass < 2 && n < max; pass++) {
		cp = cpu_list->cpu_prev;
		do {
			id_t core = cp->cpu_physid != NULL ?
			    cp->cpu_physid->cpu_coreid : cp->cpu_id;
			boolean_t used = B_FALSE;

			if (n == max)
				break;
			if (!cpu_is_online(cp) ||
			    cp->cpu_lpl->lpl_lgrpid != lg)
				continue;
			for (i = 0; i < n; i++) {
				if (cpus[i] == cp->cpu_id)
					break;
				if (cores[i] == core)
					used = B_TRUE;
			}
			if (i < n || (pass == 0 && used))
				continue;
			cores[n] = core;
			cpus[n++] = cp->cpu_id;
		} while ((cp = cp->cpu_prev) != cpu_list->cpu_prev);
	}
	return (n);
}

/*
 * Choose the vectors' CPUs and move there what numa_place names.  Nothing
 * changes when no lgroup is known.
 */
void
irdma_numa_place(irdma_t *irdma)
{
	ice_rdma_intr_t *in = &irdma->irdma_intr;
	processorid_t cpus[IRDMA_MAX_VECTORS];
	char msg[128];
	lgrp_id_t lg;
	uint_t n = 0, i;
	int prop, place;

	msg[0] = '\0';
	place = ddi_prop_get_int(DDI_DEV_T_ANY, irdma->irdma_dip,
	    DDI_PROP_DONTPASS, "numa_place", IRDMA_NUMA_INTR);
	if ((place & (IRDMA_NUMA_INTR | IRDMA_NUMA_THREAD)) == 0)
		return;
	prop = ddi_prop_get_int(DDI_DEV_T_ANY, irdma->irdma_dip,
	    DDI_PROP_DONTPASS, "numa_lgrp", -1);
	mutex_enter(&cpu_lock);
	lg = prop >= 0 ? (lgrp_id_t)prop :
	    irdma_numa_nearest(irdma, msg, sizeof (msg));
	if (lg != LGRP_NONE)
		n = irdma_numa_cpus(lg, cpus, irdma->irdma_nvecs);
	mutex_exit(&cpu_lock);

	irdma->irdma_numa_lgrp = n == 0 ? LGRP_NONE : lg;
	if (n == 0) {
		dev_err(irdma->irdma_dip, CE_NOTE, "!RDMA vectors left where "
		    "they are: no lgroup is nearest the device (register "
		    "reads by lgroup:%s)", msg);
		return;
	}
	for (i = 0; i < irdma->irdma_nvecs; i++) {
		irdma_vec_t *iv = &irdma->irdma_vecs[i];
		processorid_t cpu = cpus[i % n];

		if ((place & IRDMA_NUMA_INTR) != 0) {
			if (set_intr_affinity(in->irin_handles[i], cpu) ==
			    DDI_SUCCESS)
				iv->iv_intr_cpu = cpu;
			else
				irdma_error(irdma, "failed to move RDMA vector "
				    "%u to CPU %d", i, cpu);
		}
		if ((place & IRDMA_NUMA_THREAD) == 0)
			continue;
		mutex_enter(&iv->iv_lock);
		iv->iv_cpu = cpu;
		cv_signal(&iv->iv_cv);
		mutex_exit(&iv->iv_lock);
	}
	dev_err(irdma->irdma_dip, CE_NOTE, "!RDMA vectors on lgroup %d, "
	    "CPUs %d to %d (register reads by lgroup:%s)", (int)lg,
	    cpus[n - 1], cpus[0], msg[0] != '\0' ? msg : " not measured");
}

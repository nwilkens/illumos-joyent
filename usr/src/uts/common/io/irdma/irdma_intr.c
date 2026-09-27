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
 * Interrupts and completion event queues.
 *
 * Each RDMA vector ice reserved has a context: a kernel thread that its
 * interrupt handler wakes.  The handler takes only the vector's iv_lock,
 * which is at interrupt priority, records that the vector fired and
 * signals the thread; the vector stays masked until the thread has
 * consumed its queues and enables it again.
 *
 * Vector 0 carries the AEQ and CEQ 0, which holds only the CCQ.  Completion
 * vector i of the rdmak device is CEQ i + 1 on vector i + 1, so each verbs
 * CQ has the CEQ of its comp_vector and its completions are handled in that
 * vector's thread: the thread calls the CQ's completion handler directly.
 * No two CEQs share a vector, as in Linux.  A handler that has more work
 * than it may do in one call asks, with irdma_cq_resched(), to be called
 * again from the same thread after the others have had a turn.
 *
 * A CQ is held under its CEQ's ic_lock, which CQ destroy takes to leave
 * the CEQ, and its handler runs with no driver lock held.
 */

#include "irdma_verbs.h"

#include <sys/ddi_intr.h>
#include <sys/disp.h>
#include <sys/cpuvar.h>

/* Passes a vector makes on its own for entries it finds after the enable. */
#define	IRDMA_CEQ_RECHECKS	4

/*
 * How often an idle completion vector looks at its CEQ, and for how long
 * after its last work.  An entry can sit in the CEQ with no interrupt for
 * it (see irdma_vec_idle()).
 */
#define	IRDMA_VEC_WATCH_US	10000
#define	IRDMA_VEC_WATCH_SEC	1

/* The PF-relative vector number of entry i of the RDMA block. */
uint32_t
irdma_hw_vec(irdma_t *irdma, uint_t i)
{
	return (irdma->irdma_intr.irin_first + i);
}

/* As icrdma_ena_irq(), with the vector's own ITR0 interval. */
void
irdma_vec_enable(irdma_t *irdma, uint_t vec)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	uint32_t val;

	val = FIELD_PREP(IRDMA_GLINT_DYN_CTL_ITR_INDX, 0) |
	    FIELD_PREP(IRDMA_GLINT_DYN_CTL_INTERVAL,
	    irdma->irdma_vecs[vec].iv_itr_us >> 1) |
	    FIELD_PREP(IRDMA_GLINT_DYN_CTL_INTENA, 1) |
	    FIELD_PREP(IRDMA_GLINT_DYN_CTL_CLEARPBA, 1);
	writel(val, dev->hw_regs[IRDMA_GLINT_DYN_CTL] +
	    irdma_hw_vec(irdma, vec));
}

/*
 * Map a completion CEQ to its vector.  The core maps CEQs with no ITR; a
 * completion CEQ uses ITR0, whose interval is the vector's iv_itr_us.
 */
static void
irdma_ceq_cfg(irdma_t *irdma, irdma_ceq_t *ic, boolean_t enable)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	uint32_t val;

	val = FIELD_PREP(IRDMA_GLINT_CEQCTL_CAUSE_ENA, enable) |
	    FIELD_PREP(IRDMA_GLINT_CEQCTL_MSIX_INDX,
	    irdma_hw_vec(irdma, ic->ic_vec->iv_idx)) |
	    FIELD_PREP(IRDMA_GLINT_CEQCTL_ITR_INDX, 0);
	writel(val, dev->hw_regs[IRDMA_GLINT_CEQCTL] + ic->ic_id);
}

/*
 * The vector's interrupt hold: the least that any CQ on its CEQ asked for,
 * so no CQ waits longer than it chose.  The caller holds ic_lock.
 */
void
irdma_ceq_set_itr(irdma_ceq_t *ic)
{
	struct irdma_sc_ceq *ceq = &ic->ic_sc;
	uint16_t us = IRDMA_MAX_CQ_HOLD_US;
	unsigned long flags;
	irdma_cq_t *icq;
	uint32_t i;

	ASSERT(MUTEX_HELD(&ic->ic_lock));
	spin_lock_irqsave(&ceq->req_cq_lock, flags);
	for (i = 0; i < ceq->reg_cq_size; i++) {
		icq = ceq->reg_cq[i]->back_cq;
		if (icq != NULL && !icq->icq_dying)
			us = MIN(us, icq->icq_hold_us);
	}
	if (ceq->reg_cq_size == 0)
		us = 0;
	spin_unlock_irqrestore(&ceq->req_cq_lock, flags);
	ic->ic_vec->iv_itr_us = us;
}

void
irdma_vec_disable(irdma_t *irdma, uint_t vec)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;

	dev->irq_ops->irdma_dis_irq(dev, irdma_hw_vec(irdma, vec));
}

uint_t
irdma_intr(caddr_t arg1, caddr_t arg2)
{
	irdma_vec_t *iv = (irdma_vec_t *)(void *)arg1;

	_NOTE(ARGUNUSED(arg2));
	mutex_enter(&iv->iv_lock);
	if (!iv->iv_off) {
		iv->iv_owed = B_TRUE;
		iv->iv_rechecks = 0;
		iv->iv_intrs++;
		cv_signal(&iv->iv_cv);
	}
	mutex_exit(&iv->iv_lock);
	return (DDI_INTR_CLAIMED);
}

/* Whether the CEQ holds an entry that has not been processed. */
static boolean_t
irdma_ceq_pending(struct irdma_sc_ceq *ceq)
{
	u64 temp;

	get_64bit_val(IRDMA_GET_CURRENT_CEQ_ELEM(ceq), 0, &temp);
	return ((u8)FIELD_GET(IRDMA_CEQE_VALID, temp) == ceq->polarity);
}

/* CEQ 0 names only the CCQ. */
static void
irdma_ceq0_process(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_sc_cq *cq;
	uint32_t n;

	for (n = 0; n < irdma->irdma_ceq0.elem_cnt; n++) {
		mutex_enter(&irdma->irdma_ceq_lock);
		cq = irdma_sc_process_ceq(dev, &irdma->irdma_ceq0);
		mutex_exit(&irdma->irdma_ceq_lock);
		if (cq == NULL)
			break;
		if (cq == &irdma->irdma_ccq)
			irdma_ccq_poll(irdma);
		else
			irdma->irdma_bad_entries++;
	}
}

static void
irdma_ceq_process(irdma_ceq_t *ic)
{
	irdma_t *irdma = ic->ic_vec->iv_irdma;
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_sc_cq *cq;
	irdma_cq_t *icq;
	uint32_t n;

	for (n = 0; n < ic->ic_sc.elem_cnt; n++) {
		icq = NULL;
		mutex_enter(&ic->ic_lock);
		cq = irdma_sc_process_ceq(dev, &ic->ic_sc);
		if (cq != NULL)
			icq = irdma_cq_ceq_hold(ic, cq);
		mutex_exit(&ic->ic_lock);
		if (cq == NULL)
			break;
		if (icq != NULL) {
			ic->ic_events++;
			irdma_cq_ceq_dispatch(icq, B_TRUE);
		}
	}
}

/* Call the handlers of the CQs that asked to be called again. */
static void
irdma_ceq_resched_run(irdma_ceq_t *ic)
{
	irdma_cq_t *icq;
	list_t todo;

	list_create(&todo, sizeof (irdma_cq_t), offsetof(irdma_cq_t,
	    icq_rnode));
	mutex_enter(&ic->ic_lock);
	list_move_tail(&todo, &ic->ic_resched);
	mutex_exit(&ic->ic_lock);
	while ((icq = list_remove_head(&todo)) != NULL) {
		mutex_enter(&ic->ic_lock);
		icq->icq_resched = B_FALSE;
		mutex_exit(&ic->ic_lock);
		irdma_cq_ceq_dispatch(icq, B_FALSE);
	}
	list_destroy(&todo);
}

/* A CQ of ic asked to be called again; the caller holds a reference. */
void
irdma_ceq_kick(irdma_ceq_t *ic)
{
	irdma_vec_t *iv = ic->ic_vec;

	mutex_enter(&iv->iv_lock);
	iv->iv_resched = B_TRUE;
	cv_signal(&iv->iv_cv);
	mutex_exit(&iv->iv_lock);
}

static void
irdma_vec_work(irdma_vec_t *iv, irdma_ceq_t *ic, boolean_t owed,
    boolean_t resched)
{
	irdma_t *irdma = iv->iv_irdma;
	uint32_t progress = irdma->irdma_progress;
	boolean_t again = B_FALSE;

	if (owed) {
		if (iv->iv_ctl && (progress & BIT(IRDMA_STEP_CEQ0)) != 0)
			irdma_ceq0_process(irdma);
		if (iv->iv_ctl && (progress & BIT(IRDMA_STEP_AEQ)) != 0)
			irdma_aeq_process(irdma);
		if (ic != NULL)
			irdma_ceq_process(ic);
		/*
		 * The device latches an event while the vector is off and
		 * fires on the enable; looking again first saves that
		 * interrupt.
		 */
		irdma_vec_enable(irdma, iv->iv_idx);
		if (iv->iv_ctl && (progress & BIT(IRDMA_STEP_CEQ0)) != 0 &&
		    irdma_ceq_pending(&irdma->irdma_ceq0))
			again = B_TRUE;
		if (ic != NULL && irdma_ceq_pending(&ic->ic_sc))
			again = B_TRUE;
		if (again) {
			mutex_enter(&iv->iv_lock);
			if (iv->iv_rechecks++ < IRDMA_CEQ_RECHECKS)
				iv->iv_owed = B_TRUE;
			mutex_exit(&iv->iv_lock);
		}
	}
	if (resched && ic != NULL)
		irdma_ceq_resched_run(ic);
}

static boolean_t
irdma_aeq_pending(struct irdma_sc_aeq *aeq)
{
	u64 temp;

	get_64bit_val(IRDMA_GET_CURRENT_AEQ_ELEM(aeq), 8, &temp);
	return ((u8)FIELD_GET(IRDMA_AEQE_VALID, temp) == aeq->polarity);
}

/* Whether a queue of the vector holds an entry; the thread is idle. */
static boolean_t
irdma_vec_pending(irdma_vec_t *iv, irdma_ceq_t *ic)
{
	irdma_t *irdma = iv->iv_irdma;
	uint32_t progress = irdma->irdma_progress;
	boolean_t pending = B_FALSE;

	if (iv->iv_ctl) {
		if ((progress & BIT(IRDMA_STEP_CEQ0)) != 0) {
			mutex_enter(&irdma->irdma_ceq_lock);
			pending = irdma_ceq_pending(&irdma->irdma_ceq0);
			mutex_exit(&irdma->irdma_ceq_lock);
		}
		if (!pending && (progress & BIT(IRDMA_STEP_AEQ)) != 0)
			pending = irdma_aeq_pending(&irdma->irdma_aeq);
	}
	if (!pending && ic != NULL) {
		mutex_enter(&ic->ic_lock);
		pending = irdma_ceq_pending(&ic->ic_sc);
		mutex_exit(&ic->ic_lock);
	}
	return (pending);
}

/*
 * Sleep until the vector has work.  On E810 an entry has been seen to sit
 * in a CEQ after the device fired the vector with no call of the handler,
 * and a CQ with an event pending never fires again, so a vector that did
 * work in the last IRDMA_VEC_WATCH_SEC looks at its queues every
 * IRDMA_VEC_WATCH_US.  A rescue records whether the vector was still
 * enabled, that is whether the device or the host lost the interrupt.
 */
static void
irdma_vec_idle(irdma_vec_t *iv)
{
	irdma_t *irdma = iv->iv_irdma;
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	irdma_ceq_t *ic;
	boolean_t pending;
	uint32_t ctl = 0;

	ASSERT(MUTEX_HELD(&iv->iv_lock));
	while (!iv->iv_exit && !iv->iv_resched && iv->iv_cpu == iv->iv_bound &&
	    (iv->iv_off || !iv->iv_owed)) {
		ic = iv->iv_ceq;
		if (iv->iv_off || gethrtime() - iv->iv_last >
		    SEC2NSEC(IRDMA_VEC_WATCH_SEC)) {
			cv_wait(&iv->iv_cv, &iv->iv_lock);
			continue;
		}
		if (cv_reltimedwait(&iv->iv_cv, &iv->iv_lock,
		    drv_usectohz(IRDMA_VEC_WATCH_US), TR_CLOCK_TICK) != -1 ||
		    iv->iv_owed || iv->iv_resched || iv->iv_exit ||
		    iv->iv_off || iv->iv_ceq != ic)
			continue;
		/* Busy, so that irdma_vec_barrier() waits before ic goes. */
		iv->iv_busy = B_TRUE;
		mutex_exit(&iv->iv_lock);
		pending = irdma_vec_pending(iv, ic);
		if (pending) {
			ctl = readl(dev->hw_regs[IRDMA_GLINT_DYN_CTL] +
			    irdma_hw_vec(irdma, iv->iv_idx));
		}
		mutex_enter(&iv->iv_lock);
		iv->iv_busy = B_FALSE;
		iv->iv_passes++;
		cv_broadcast(&iv->iv_cv);
		if (!pending)
			continue;
		iv->iv_rescues++;
		if ((ctl & IRDMA_GLINT_DYN_CTL_INTENA) != 0)
			iv->iv_rescues_on++;
		iv->iv_owed = B_TRUE;
	}
}

/* Run the calling vector thread on cpu, or anywhere for -1. */
static void
irdma_vec_bind(irdma_vec_t *iv, processorid_t cpu)
{
	cpu_t *cp;

	mutex_enter(&cpu_lock);
	if (iv->iv_bound != -1)
		thread_affinity_clear(curthread);
	if (cpu != -1 && (cp = cpu_get(cpu)) != NULL && cpu_is_online(cp))
		thread_affinity_set(curthread, cpu);
	else
		cpu = -1;
	mutex_exit(&cpu_lock);
	iv->iv_bound = cpu;
}

/*
 * The vector's context.  Work for CQs that asked to be called again runs
 * even when the vector is off, so their handlers see them go away.
 */
static void
irdma_vec_thread(void *arg)
{
	irdma_vec_t *iv = arg;
	boolean_t owed, resched;
	irdma_ceq_t *ic;
	hrtime_t t0;

	mutex_enter(&iv->iv_lock);
	for (;;) {
		irdma_vec_idle(iv);
		if (iv->iv_exit)
			break;
		if (iv->iv_cpu != iv->iv_bound) {
			processorid_t cpu = iv->iv_cpu;

			mutex_exit(&iv->iv_lock);
			irdma_vec_bind(iv, cpu);
			mutex_enter(&iv->iv_lock);
			iv->iv_cpu = iv->iv_bound;
			continue;
		}
		owed = iv->iv_owed && !iv->iv_off;
		resched = iv->iv_resched;
		iv->iv_owed = iv->iv_resched = B_FALSE;
		ic = iv->iv_ceq;
		iv->iv_busy = B_TRUE;
		mutex_exit(&iv->iv_lock);

		t0 = gethrtime();
		irdma_vec_work(iv, ic, owed, resched);

		mutex_enter(&iv->iv_lock);
		iv->iv_last = gethrtime();
		iv->iv_busy_ns += (uint64_t)(iv->iv_last - t0);
		iv->iv_busy = B_FALSE;
		iv->iv_passes++;
		cv_broadcast(&iv->iv_cv);
	}
	mutex_exit(&iv->iv_lock);
	if (iv->iv_bound != -1)
		irdma_vec_bind(iv, -1);
	thread_exit();
}

/* Wait until the pass the vector's thread is in, if any, has ended. */
void
irdma_vec_barrier(irdma_vec_t *iv)
{
	uint64_t pass;

	mutex_enter(&iv->iv_lock);
	pass = iv->iv_passes;
	while (iv->iv_busy && iv->iv_passes == pass)
		cv_wait(&iv->iv_cv, &iv->iv_lock);
	mutex_exit(&iv->iv_lock);
}

void
irdma_intr_barrier(irdma_t *irdma)
{
	uint_t i;

	for (i = 0; i < irdma->irdma_nvecs; i++)
		irdma_vec_barrier(&irdma->irdma_vecs[i]);
}

/* Take no more interrupt work.  This never waits. */
void
irdma_intr_off(irdma_t *irdma)
{
	irdma_vec_t *iv;
	uint_t i;

	for (i = 0; i < irdma->irdma_nvecs; i++) {
		iv = &irdma->irdma_vecs[i];
		mutex_enter(&iv->iv_lock);
		iv->iv_off = B_TRUE;
		mutex_exit(&iv->iv_lock);
	}
}

/*
 * Stop taking interrupt work and wait for the threads.  The handlers stay
 * registered; nothing enables the vectors again.
 */
void
irdma_intr_quiesce(irdma_t *irdma)
{
	irdma_intr_off(irdma);
	irdma_intr_barrier(irdma);
}

/*
 * The vector locks, at the priority of the vectors, and the threads.  They
 * exist before any handler is added and go only after every handler is
 * removed.
 */
void
irdma_vecs_init(irdma_t *irdma)
{
	ice_rdma_intr_t *in = &irdma->irdma_intr;
	irdma_vec_t *iv;
	kthread_t *t;
	uint_t i;

	for (i = 0; i < in->irin_count; i++) {
		iv = &irdma->irdma_vecs[i];
		iv->iv_irdma = irdma;
		iv->iv_idx = i;
		iv->iv_ctl = i == 0;
		iv->iv_cpu = iv->iv_bound = -1;
		(mutex_init)(&iv->iv_lock, NULL, MUTEX_DRIVER,
		    DDI_INTR_PRI(irdma->irdma_intr.irin_pri));
		cv_init(&iv->iv_cv, NULL, CV_DRIVER, NULL);
		t = thread_create(NULL, 0, irdma_vec_thread, iv, 0, &p0,
		    TS_RUN, maxclsyspri);
		iv->iv_did = t->t_did;
		irdma->irdma_nvecs = i + 1;
	}
}

void
irdma_vecs_fini(irdma_t *irdma)
{
	irdma_vec_t *iv;
	uint_t i;

	for (i = 0; i < irdma->irdma_nvecs; i++) {
		iv = &irdma->irdma_vecs[i];
		mutex_enter(&iv->iv_lock);
		iv->iv_exit = B_TRUE;
		cv_broadcast(&iv->iv_cv);
		mutex_exit(&iv->iv_lock);
		thread_join(iv->iv_did);
		cv_destroy(&iv->iv_cv);
		mutex_destroy(&iv->iv_lock);
	}
	irdma->irdma_nvecs = 0;
}

/*
 * A vector whose handler cannot be removed stays in irdma_intr_mask, and
 * the handler's argument must then never be freed.
 */
static void
irdma_intr_release(irdma_t *irdma)
{
	ice_rdma_intr_t *in = &irdma->irdma_intr;
	uint_t i;
	int rc;

	for (i = 0; i < in->irin_count; i++) {
		if ((irdma->irdma_intr_mask & BIT(i)) == 0)
			continue;
		rc = ddi_intr_disable(in->irin_handles[i]);
		if (rc != DDI_SUCCESS)
			irdma_error(irdma, "failed to disable RDMA vector "
			    "%u: %d", i, rc);
		rc = ddi_intr_remove_handler(in->irin_handles[i]);
		if (rc != DDI_SUCCESS) {
			irdma_error(irdma, "failed to remove the handler of "
			    "RDMA vector %u: %d", i, rc);
			continue;
		}
		irdma->irdma_intr_mask &= ~BIT(i);
	}
}

int
irdma_step_intr(irdma_t *irdma)
{
	ice_rdma_intr_t *in = &irdma->irdma_intr;
	uint_t i;
	int rc;

	for (i = 0; i < in->irin_count; i++) {
		rc = ddi_intr_add_handler(in->irin_handles[i], irdma_intr,
		    (caddr_t)&irdma->irdma_vecs[i], NULL);
		if (rc != DDI_SUCCESS)
			goto fail;
		irdma->irdma_intr_mask |= BIT(i);
		rc = ddi_intr_enable(in->irin_handles[i]);
		if (rc != DDI_SUCCESS)
			goto fail;
	}
	return (0);

fail:
	irdma_error(irdma, "failed to set up RDMA vector %u: %d", i, rc);
	irdma_intr_quiesce(irdma);
	irdma_intr_release(irdma);
	return (EIO);
}

void
irdma_unstep_intr(irdma_t *irdma)
{
	irdma_intr_quiesce(irdma);
	irdma_intr_release(irdma);
}

static int
irdma_ceq_create(irdma_t *irdma, irdma_ceq_t *ic, uint32_t size)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	struct irdma_ceq_init_info info;
	irdma_vec_t *iv = ic->ic_vec;

	ic->ic_mem.size = ALIGN(sizeof (struct irdma_ceqe) * size,
	    IRDMA_CEQ_ALIGNMENT);
	ic->ic_mem.va = dma_alloc_coherent(&irdma->irdma_osdev,
	    ic->ic_mem.size, &ic->ic_mem.pa, GFP_KERNEL);
	ic->ic_reg = kcalloc(size, sizeof (struct irdma_sc_cq *), GFP_KERNEL);
	ic->ic_nreg = size;
	if (ic->ic_mem.va == NULL || ic->ic_reg == NULL)
		return (ENOMEM);

	bzero(&info, sizeof (info));
	info.ceq_id = ic->ic_id;
	info.ceqe_base = ic->ic_mem.va;
	info.ceqe_pa = ic->ic_mem.pa;
	info.elem_cnt = size;
	info.dev = dev;
	info.vsi_idx = irdma->irdma_info.iri_vsi_num;
	info.reg_cq = ic->ic_reg;
	if (irdma_sc_ceq_init(&ic->ic_sc, &info) != 0)
		return (EINVAL);
	if (irdma_cqp_ceq_cmd(dev, &ic->ic_sc, IRDMA_OP_CEQ_CREATE) != 0) {
		/* The device may have made it; keep its memory. */
		irdma_taint(irdma);
		dev->ceq[ic->ic_id] = NULL;
		return (EIO);
	}
	ic->ic_live = B_TRUE;

	irdma_ceq_cfg(irdma, ic, B_TRUE);
	mutex_enter(&iv->iv_lock);
	iv->iv_ceq = ic;
	mutex_exit(&iv->iv_lock);
	irdma_vec_enable(irdma, iv->iv_idx);
	return (0);
}

/* The completion CEQs, one per vector after vector 0. */
int
irdma_step_ceqs(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	uint32_t nvec = irdma->irdma_intr.irin_count, n, i, size;
	irdma_ceq_t *ic;
	int ret;

	n = MIN(nvec - 1, irdma->irdma_comp_limit);
	n = MIN(n, dev->hmc_fpm_misc.max_ceqs - 1);
	if (n == 0) {
		irdma_error(irdma, "no CEQ for completions: %u vectors, %u "
		    "CEQs", nvec, dev->hmc_fpm_misc.max_ceqs);
		return (ENOSPC);
	}
	size = MIN(dev->hmc_info->hmc_obj[IRDMA_HMC_IW_CQ].cnt,
	    dev->hw_attrs.max_hw_ceq_size);
	size = MAX(size, dev->hw_attrs.min_hw_ceq_size);

	irdma->irdma_ceqs = kmem_zalloc(sizeof (irdma_ceq_t) * n, KM_SLEEP);
	irdma->irdma_ceqs_alloc = n;
	for (i = 0; i < n; i++) {
		ic = &irdma->irdma_ceqs[i];
		ic->ic_id = i + 1;
		ic->ic_vec = &irdma->irdma_vecs[i + 1];
		(mutex_init)(&ic->ic_lock, NULL, MUTEX_DRIVER, NULL);
		list_create(&ic->ic_resched, sizeof (irdma_cq_t),
		    offsetof(irdma_cq_t, icq_rnode));
		irdma->irdma_nceqs = i + 1;
		if ((ret = irdma_ceq_create(irdma, ic, size)) != 0) {
			irdma_error(irdma, "CEQ %u create failed: %d",
			    ic->ic_id, ret);
			irdma_unstep_ceqs(irdma);
			return (ret);
		}
	}
	return (0);
}

void
irdma_unstep_ceqs(irdma_t *irdma)
{
	struct irdma_sc_dev *dev = &irdma->irdma_sc;
	irdma_ceq_t *ic;
	irdma_vec_t *iv;
	uint32_t i;
	int ret;

	for (i = irdma->irdma_nceqs; i-- > 0; ) {
		ic = &irdma->irdma_ceqs[i];
		iv = ic->ic_vec;
		if (ic->ic_live) {
			irdma_ceq_cfg(irdma, ic, B_FALSE);
			irdma_vec_disable(irdma, iv->iv_idx);
			mutex_enter(&iv->iv_lock);
			iv->iv_ceq = NULL;
			mutex_exit(&iv->iv_lock);
			irdma_vec_barrier(iv);
			ret = -EIO;
			if (irdma_hw_ok(irdma))
				ret = irdma_cqp_ceq_cmd(dev, &ic->ic_sc,
				    IRDMA_OP_CEQ_DESTROY);
			if (ret != 0)
				irdma_taint(irdma);
			dev->ceq[ic->ic_id] = NULL;
			ic->ic_live = B_FALSE;
		}
		VERIFY(list_is_empty(&ic->ic_resched));
		if (ic->ic_mem.va != NULL) {
			dma_free_coherent(&irdma->irdma_osdev, ic->ic_mem.size,
			    ic->ic_mem.va, ic->ic_mem.pa);
		}
		kfree(ic->ic_reg);
		list_destroy(&ic->ic_resched);
		mutex_destroy(&ic->ic_lock);
	}
	if (irdma->irdma_ceqs != NULL) {
		kmem_free(irdma->irdma_ceqs,
		    sizeof (irdma_ceq_t) * irdma->irdma_ceqs_alloc);
	}
	irdma->irdma_ceqs = NULL;
	irdma->irdma_ceqs_alloc = irdma->irdma_nceqs = 0;
}

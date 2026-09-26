#!/usr/bin/env python3
"""Static checks of the lock and interrupt-priority rules of the RDMA peer.

- The irdma interrupt handler takes only its vector's iv_lock, which is made
  at the priority of the RDMA vectors before any handler is added, and it
  only wakes the vector's thread; it calls no core code.
- The vector threads call CQ handlers with no driver lock held, and a CQ
  that asks to be called again does so without its CEQ lock.
- ice calls into the child (irc_event) only with ir_lock dropped, and never
  from a path that holds ice_rebuild_lock.
- The reset worker takes the child offline and brings it back with
  ice_rebuild_lock dropped; ice detach removes the child before it takes the
  lock.
- The core code never runs from the interrupt handler, so its spinlocks may
  be adaptive mutexes (osdep.h).
"""

import re

from irdma_test import ICE, IRDMA, REPO, body


def calls(text):
    return set(re.findall(r"\b([a-z_][a-z0-9_]*)\(", text))


def main():
    ctl = (IRDMA / "irdma_ctl.c").read_text(encoding="utf-8")
    intr = (IRDMA / "irdma_intr.c").read_text(encoding="utf-8")
    drv = (IRDMA / "irdma.c").read_text(encoding="utf-8")
    osdep = (IRDMA / "osdep.h").read_text(encoding="utf-8")
    peer = (ICE / "ice_rdma.c").read_text(encoding="utf-8")
    ice = (ICE / "ice.c").read_text(encoding="utf-8")

    isr = body(intr, "irdma_intr")
    assert calls(isr) - {"irdma_intr", "_NOTE", "ARGUNUSED"} <= \
        {"mutex_enter", "mutex_exit", "cv_signal"}, calls(isr)
    assert set(re.findall(r"mutex_(?:enter|exit)\(&(\w+->\w+)\)", isr)) \
        == {"iv->iv_lock"}
    assert "irdma->" not in isr

    attach = body(drv, "irdma_attach")
    vinit = body(intr, "irdma_vecs_init")
    init = vinit.index("(mutex_init)(&iv->iv_lock")
    assert "DDI_INTR_PRI(irdma->irdma_intr.irin_pri)" in \
        vinit[init:init + 160]
    assert vinit.index("(mutex_init)(&iv->iv_lock") < \
        vinit.index("thread_create(")
    assert attach.index("iro_intr_get") < \
        attach.index("irdma_vecs_init(irdma)") < \
        attach.index("irdma_ctl_start(irdma)")
    # Handlers are added only by the INTR step, after the locks exist, and
    # the locks and threads go only once no handler is left.
    assert "ddi_intr_add_handler" in body(intr, "irdma_step_intr")
    for text in (attach, ctl, drv):
        assert "ddi_intr_add_handler" not in text
    unsetup = body(drv, "irdma_unsetup")
    assert re.search(r"if \(irdma->irdma_intr_mask == 0\)\n\t+"
                     r"irdma_vecs_fini\(irdma\);", unsetup)
    # iv_lock is a leaf: nothing is taken while it is held.
    for name in ("irdma_vec_thread", "irdma_ceq_kick", "irdma_intr_off",
                 "irdma_vec_barrier", "irdma_ceq_create",
                 "irdma_unstep_ceqs"):
        text = body(intr, name)
        for held in re.findall(r"mutex_enter\(&iv->iv_lock\);([\s\S]*?)"
                               r"mutex_exit\(&iv->iv_lock\);", text):
            assert "mutex_enter" not in held, name

    assert "(mutex_init)(&ic->ic_lock, NULL, MUTEX_DRIVER, NULL);" in intr
    # Every other irdma lock is adaptive.
    for lock in ("irdma_cfg_lock", "irdma_req_lock", "irdma_ccq_lock",
                 "irdma_ws_lock", "irdma_rsrc_lock", "irdma_qptable_lock",
                 "irdma_cqtable_lock", "irdma_arp_lock", "irdma_arp_cmd_lock",
                 "irdma_ceq_lock"):
        assert re.search(rf"\(mutex_init\)\(&irdma->{lock}, NULL, "
                         rf"MUTEX_DRIVER, NULL\);", drv), lock
    assert "spin_lock_init(l)" in osdep and \
        "mutex_init(&(l)->sl_lock, NULL, MUTEX_DRIVER, NULL)" in osdep

    # Consumer completion handlers run from the vector threads with no
    # driver lock held, and never from the interrupt handler.
    for name, event in (("irdma_ceq_process", "B_TRUE"),
                        ("irdma_ceq_resched_run", "B_FALSE")):
        ceq = body(intr, name)
        call = ceq.index(f"irdma_cq_ceq_dispatch(icq, {event});")
        assert ceq.rindex("mutex_exit(&ic->ic_lock);", 0, call) > \
            ceq.rindex("mutex_enter(&ic->ic_lock);", 0, call), name
    cq = (IRDMA / "irdma_cq.c").read_text(encoding="utf-8")
    dispatch = body(cq, "irdma_cq_ceq_dispatch")
    handler = dispatch.index("rcq->comp_handler(rcq, rcq->cq_context);")
    assert dispatch.rindex("mutex_exit(&icq->icq_lock);", 0, handler) > \
        dispatch.rindex("mutex_enter(&icq->icq_lock);", 0, handler)
    assert "comp_handler" not in isr
    # A vector looks at its queues again only after the enable.
    work = body(intr, "irdma_vec_work")
    en = work.index("irdma_vec_enable(irdma, iv->iv_idx);")
    for q in ("irdma_ceq_pending(&irdma->irdma_ceq0)",
              "irdma_ceq_pending(&ic->ic_sc)"):
        assert work.index(q) > en, q
    # The idle watchdog looks at the queues and reads the vector with
    # iv_lock dropped.
    idle = body(intr, "irdma_vec_idle")
    rd = idle.index("readl(")
    seg = idle[idle.rindex("mutex_exit(&iv->iv_lock);", 0, rd):rd]
    assert "irdma_vec_pending(iv, ic)" in seg
    # The only re-entry before the read leaves the loop body at once.
    for m in re.finditer(r"mutex_enter\(&iv->iv_lock\);", seg):
        assert seg[m.end():].lstrip().startswith("continue;")
    # The check counts as busy, so irdma_vec_barrier() waits for it before
    # teardown frees the CEQ.
    pre = idle[:idle.index("irdma_vec_pending(iv, ic)")]
    assert pre.rindex("iv->iv_busy = B_TRUE;") > \
        pre.rindex("iv->iv_ceq != ic)")
    post = idle[idle.index("irdma_vec_pending(iv, ic)"):]
    assert post.index("iv->iv_busy = B_FALSE;") < \
        post.index("iv->iv_owed = B_TRUE;")
    # Only a CEQ entry uses up a CQ's arm.
    assert re.search(r"if \(event\) \{\n\t\tmutex_enter\(&icq->icq_lock\);"
                     r"\n\t\ticq->icq_armed = B_FALSE;", dispatch)
    resched = body(cq, "irdma_cq_resched")
    assert resched.index("irdma_ceq_kick(ic);") > \
        resched.rindex("mutex_exit(&ic->ic_lock);")
    # rdmak calls done() with no poller lock held.
    rdk = (REPO / "usr/src/uts/common/io/rdma/rdk_cq.c").read_text(
        encoding="utf-8")
    proc = body(rdk, "rdk_cq_process")
    done = proc.index("wcs[i].wr_cqe->done(cq, &wcs[i]);")
    assert proc.rindex("mutex_exit(&cp->rcp_lock);", 0, done) > \
        proc.rindex("mutex_enter(&cp->rcp_lock);", 0, done)
    # Waiting work (QP errors, flushes) runs on irdma_wq, not in the task.
    aeq = (IRDMA / "irdma_aeq.c").read_text(encoding="utf-8")
    assert "irdma_modify_qp(" not in aeq and "irdma_cqp_exec(" not in aeq

    deliver = body(peer, "ice_rdma_deliver")
    ev = deliver.index("client->irc_event(arg, ev);")
    assert deliver.rindex("mutex_exit(&ir->ir_lock);", 0, ev) > \
        deliver.rindex("mutex_enter(&ir->ir_lock);", 0, ev)
    assert len(re.findall(r"->irc_event\(", peer)) == 1
    for name in ("ice_rdma_reset_prepare", "ice_rdma_reset_done",
                 "ice_rdma_event_task"):
        assert "ice_rebuild_lock" not in body(peer, name), name

    task = body(ice, "ice_reset_task")
    prep = task.index("ice_rdma_reset_prepare(ice);")
    assert task.rindex("mutex_exit(&ice->ice_rebuild_lock);", 0, prep) > \
        task.rindex("mutex_enter(&ice->ice_rebuild_lock);", 0, prep)
    done = task.index("ice_rdma_reset_done(ice, ok);")
    assert task.rindex("mutex_exit(&ice->ice_rebuild_lock);", 0, done) > \
        task.rindex("mutex_enter(&ice->ice_rebuild_lock);", 0, done)

    detach = body(ice, "ice_detach")
    assert detach.index("ice_rdma_detach(ice)") < \
        detach.index("mutex_enter(&ice->ice_rebuild_lock)")

    # The quarantine is released only past the reset barrier.
    rebuild = body(ice, "ice_rebuild")
    assert rebuild.index("ice_rdma_reset_barrier(ice);") > \
        rebuild.index("ice_fw_state(ice, &fwsm)")
    unconf = body(ice, "ice_unconfigure")
    assert unconf.index("ice_rdma_fini(ice, reset_ok);") > \
        unconf.index("ice_reset(&ice->ice_hw, ICE_RESET_PFR)")

    # Register access takes no lock; the map changes only under its writer
    # lock with the generation odd and the writer not preemptible.
    osd = (IRDMA / "irdma_osdep.c").read_text(encoding="utf-8")
    for name in ("readl", "writel", "irdma_regs_find"):
        assert "mutex_enter" not in body(osd, name), name
        assert "rw_enter" not in body(osd, name), name
    for name in ("irdma_osdep_regs_add", "irdma_osdep_regs_dbs",
                 "irdma_osdep_regs_remove"):
        text = body(osd, name)
        assert text.count("irdma_regs_begin();") == 1, name
        first = text.index("irdma_regs_begin();")
        assert first < text.index("irdma_regs_end();"), name
        assert text.rindex("mutex_enter(&irdma_regs_lock);", 0, first) >= 0
    begin, end = body(osd, "irdma_regs_begin"), body(osd, "irdma_regs_end")
    assert begin.index("kpreempt_disable();") < begin.index("irdma_regs_gen++")
    assert end.index("irdma_regs_gen++") < end.index("kpreempt_enable();")
    find = body(osd, "irdma_regs_find")
    assert "& 1) != 0" in find and "while (gen != irdma_regs_gen)" in find
    # The post path does not ask ice, whose lock every QP would share.
    post = (IRDMA / "irdma_post.c").read_text(encoding="utf-8")
    for name in ("irdma_post_send", "irdma_post_recv"):
        assert "irdma_post_ok(" in body(post, name)
        assert "irdma_healthy(" not in body(post, name)

    # A CQE's QP is checked against the CQ's map under icq_lock, without
    # the QP table lock; destroy leaves the map before the number is freed.
    cqe = body(cq, "irdma_osdep_cqe_qp")
    assert "BT_TEST(icq->icq_qpmap, qp_id)" in cqe
    assert "irdma_qptable_lock" not in cqe
    purge = body(cq, "irdma_cq_purge_qp")
    assert purge.index("mutex_enter(&icq->icq_lock);") < \
        purge.index("BT_CLEAR(icq->icq_qpmap")
    qpc = (IRDMA / "irdma_qp.c").read_text(encoding="utf-8")
    destroy = body(qpc, "irdma_destroy_qp")
    assert destroy.index("irdma_cq_purge_qp(iqp->iqp_scq, iqp);") < \
        destroy.index("irdma_qp_free_num(iqp, rqp->qp_num);")
    create = body(qpc, "irdma_create_qp")
    assert create.index("irdma->irdma_qp_table[num] = iqp;") < \
        create.index("irdma_cq_add_qp(iqp->iqp_scq, num);")

    # A moderation delay is cancelled, waiting for one in progress, before
    # the CQ is freed; the delay hands the poller back with no poller lock.
    free = body(rdk, "rdk_free_cq")
    assert free.index("untimeout_generic(tid, 0);") < \
        free.index("rdk_cq_wait_idle(cp);") < free.rindex("rdk_destroy_cq(cq);")
    assert free.index("cp->rcp_dying = B_TRUE;") < \
        free.index("untimeout_generic(tid, 0);")
    fire = body(rdk, "rdk_cq_mod_fire")
    call = fire.index("resched(cq);")
    assert fire.rindex("mutex_exit(&cp->rcp_lock);", 0, call) > \
        fire.rindex("mutex_enter(&cp->rcp_lock);", 0, call)
    # No delay replaces rcp_mod_tid, which rdk_free_cq() waits on, until
    # the hand-back is over.
    assert fire.rindex("cp->rcp_mod_pending = B_FALSE;") > \
        fire.rindex("&cp->rcp_ent);")
    delay = body(rdk, "rdk_cq_mod_delay")
    assert "cp->rcp_mod_pending ||" in delay
    # Busy polling runs only on a poller it holds.
    begin = body(rdk, "rdk_cq_poll_begin")
    assert "cp->rcp_queued = cp->rcp_busy = B_TRUE;" in begin
    assert "if (cp->rcp_dying || cp->rcp_queued) {" in begin
    # The ITR of a vector is the least any CQ on its CEQ asked for.
    itr = body(intr, "irdma_ceq_set_itr")
    assert "us = MIN(us, icq->icq_hold_us);" in itr

    # Vector placement: CPUs are chosen under cpu_lock, interrupts move
    # with it dropped (set_intr_affinity() takes it), and a vector thread
    # binds only itself, under cpu_lock and without iv_lock.
    numa = (IRDMA / "irdma_numa.c").read_text(encoding="utf-8")
    place = body(numa, "irdma_numa_place")
    assert place.index("mutex_exit(&cpu_lock);") < \
        place.index("set_intr_affinity(")
    # Threads bind only when asked; bound, they lost to the interrupt.
    assert '"numa_place", IRDMA_NUMA_INTR);' in place
    assert place.index("(place & IRDMA_NUMA_THREAD) == 0") < \
        place.index("iv->iv_cpu = cpu;")
    for name in ("irdma_numa_read_ns", "irdma_numa_nearest",
                 "irdma_numa_cpus"):
        assert "ASSERT(MUTEX_HELD(&cpu_lock));" in body(numa, name), name
    bind = body(intr, "irdma_vec_bind")
    assert bind.index("mutex_enter(&cpu_lock);") < \
        bind.index("thread_affinity_set(curthread, cpu);") < \
        bind.index("mutex_exit(&cpu_lock);")
    thr = body(intr, "irdma_vec_thread")
    call = thr.index("irdma_vec_bind(iv, cpu);")
    assert thr.rindex("mutex_exit(&iv->iv_lock);", 0, call) > \
        thr.rindex("mutex_enter(&iv->iv_lock);", 0, call)

    # The ice theory statement records the peer locks.
    assert "ir_cfg_lock" in ice and "ir_lock" in ice
    print("PASS: interrupt priority and peer lock ordering")


if __name__ == "__main__":
    main()

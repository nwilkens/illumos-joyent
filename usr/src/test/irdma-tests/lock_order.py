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

    # The ice theory statement records the peer locks.
    assert "ir_cfg_lock" in ice and "ir_lock" in ice
    print("PASS: interrupt priority and peer lock ordering")


if __name__ == "__main__":
    main()

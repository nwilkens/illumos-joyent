#!/usr/bin/env python3
"""Static checks of the lock and interrupt-priority rules of the RDMA peer.

- The irdma interrupt handler takes only irdma_intr_lock, which is made at
  the priority of the RDMA vectors before any handler is added, and it calls
  no core code.
- ice calls into the child (irc_event) only with ir_lock dropped, and never
  from a path that holds ice_rebuild_lock.
- The reset worker takes the child offline and brings it back with
  ice_rebuild_lock dropped; ice detach removes the child before it takes the
  lock.
- The core code never runs from the interrupt handler, so its spinlocks may
  be adaptive mutexes (osdep.h).
"""

import re

from irdma_test import ICE, IRDMA, body


def calls(text):
    return set(re.findall(r"\b([a-z_][a-z0-9_]*)\(", text))


def main():
    ctl = (IRDMA / "irdma_ctl.c").read_text(encoding="utf-8")
    drv = (IRDMA / "irdma.c").read_text(encoding="utf-8")
    osdep = (IRDMA / "osdep.h").read_text(encoding="utf-8")
    peer = (ICE / "ice_rdma.c").read_text(encoding="utf-8")
    ice = (ICE / "ice.c").read_text(encoding="utf-8")

    isr = body(ctl, "irdma_intr")
    assert calls(isr) - {"irdma_intr"} <= {"mutex_enter", "mutex_exit",
                                            "ddi_taskq_dispatch"}, calls(isr)
    assert set(re.findall(r"mutex_(?:enter|exit)\(&irdma->(\w+)\)", isr)) \
        == {"irdma_intr_lock"}

    attach = body(drv, "irdma_attach")
    init = attach.index("(mutex_init)(&irdma->irdma_intr_lock")
    assert "DDI_INTR_PRI(irdma->irdma_intr.irin_pri)" in \
        attach[init:init + 160]
    assert attach.index("iro_intr_get") < init < \
        attach.index("irdma_ctl_start(irdma)")
    # Handlers are added only by the INTR step, after the lock exists.
    assert "ddi_intr_add_handler" in body(ctl, "irdma_step_intr")
    assert "ddi_intr_add_handler" not in attach

    # Every other irdma lock is adaptive.
    for lock in ("irdma_cfg_lock", "irdma_req_lock", "irdma_ccq_lock",
                 "irdma_ws_lock", "irdma_rsrc_lock", "irdma_qptable_lock",
                 "irdma_cqtable_lock", "irdma_arp_lock", "irdma_ceq_lock"):
        assert re.search(rf"\(mutex_init\)\(&irdma->{lock}, NULL, "
                         rf"MUTEX_DRIVER, NULL\);", drv), lock
    assert "spin_lock_init(l)" in osdep and \
        "mutex_init(&(l)->sl_lock, NULL, MUTEX_DRIVER, NULL)" in osdep

    # Consumer completion handlers run from the interrupt task with no
    # driver lock held, and never from the interrupt handler.
    ceq = body(ctl, "irdma_ceq0_process")
    call = ceq.index("irdma_cq_ceq_dispatch(icq);")
    assert ceq.rindex("mutex_exit(&irdma->irdma_ceq_lock);", 0, call) > \
        ceq.rindex("mutex_enter(&irdma->irdma_ceq_lock);", 0, call)
    cq = (IRDMA / "irdma_cq.c").read_text(encoding="utf-8")
    dispatch = body(cq, "irdma_cq_ceq_dispatch")
    handler = dispatch.index("rcq->comp_handler(rcq, rcq->cq_context);")
    assert dispatch.rindex("mutex_exit(&icq->icq_lock);", 0, handler) > \
        dispatch.rindex("mutex_enter(&icq->icq_lock);", 0, handler)
    assert "comp_handler" not in isr
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

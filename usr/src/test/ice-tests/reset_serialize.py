#!/usr/bin/env python3

"""Check ICE reset serialization, dispatch, and detach-safety invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"
ATTACH_SOURCE = REPO / "usr/src/uts/common/io/ice/ice.c"
GLD_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_gld.c"
INTR_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_intr.c"
RX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_rx.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    assert "kmutex_t\t\tice_rebuild_lock;" in header
    assert "ddi_taskq_t\t\t*ice_reset_taskq;" in header
    assert "boolean_t\t\tice_reset_pending;" in header
    assert "boolean_t\t\tice_detaching;" in header
    assert "ICE_ATTACH_RESET_TASKQ" in header

    # mac start/stop bracket the datapath in the outermost rebuild lock so a
    # rebuild cannot interleave with a plumb, and start goes through the
    # factored ice_start_datapath.
    gld = GLD_SOURCE.read_text(encoding="utf-8")
    start = function(gld, "ice_m_start(void *arg)\n{", "\nstatic void\nice_m_stop")
    assert "mutex_enter(&ice->ice_rebuild_lock)" in start
    assert "mutex_exit(&ice->ice_rebuild_lock)" in start
    assert "ice_start_datapath(ice)" in start
    stop = function(gld, "ice_m_stop(void *arg)\n{", "\nint\nice_promisc_apply")
    assert "mutex_enter(&ice->ice_rebuild_lock)" in stop
    assert "mutex_exit(&ice->ice_rebuild_lock)" in stop

    # Every MAC callback that issues an admin-queue command holds the outermost
    # ice_rebuild_lock across it, so a reset rebuild's ice_deinit_hw/ice_hw_init
    # cannot destroy the control queue underneath the command.
    setmac = function(
        gld,
        "ice_gld_set_mac(ice_t *ice, const uint8_t *addr, boolean_t add)\n{",
        "\n/*\n * Ring callbacks.",
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in setmac
    assert "mutex_exit(&ice->ice_rebuild_lock)" in setmac
    assert "ice_gld_set_mac_locked(ice, addr, add)" in setmac

    promisc = function(
        gld, "ice_m_promisc(void *arg, boolean_t on)\n{", "\nstatic int\nice_m_multicst"
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in promisc
    assert "mutex_exit(&ice->ice_rebuild_lock)" in promisc

    # ice_promisc_apply must NOT take the lock: ice_vsi_rebuild calls it while
    # already holding ice_rebuild_lock.
    apply = function(
        gld, "ice_promisc_apply(ice_t *ice, boolean_t on)\n{", "\nstatic int\nice_m_promisc"
    )
    assert "ice_rebuild_lock" not in apply

    lbset = function(
        gld,
        "ice_loopback_mode_set(ice_t *ice, uint32_t mode)\n{",
        "\nvoid\nice_loopback_fini",
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in lbset
    assert "mutex_exit(&ice->ice_rebuild_lock)" in lbset
    assert "ice_loopback_mode_set_locked(ice, mode)" in lbset

    tinfo = function(
        gld,
        "ice_transceiver_info(void *arg, uint_t id, mac_transceiver_info_t *infop)\n{",
        "\nstatic int\nice_transceiver_read",
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in tinfo
    assert "mutex_exit(&ice->ice_rebuild_lock)" in tinfo

    tread = function(
        gld,
        "ice_transceiver_read(void *arg, uint_t id, uint_t page, void *buf,",
        "\nstatic boolean_t\nice_m_getcapab",
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in tread
    assert "mutex_exit(&ice->ice_rebuild_lock)" in tread

    # ice_m_stat reads MAC statistic registers, not admin-queue commands, so it
    # must NOT serialize on the rebuild lock (stats are hot).
    stat = function(
        gld, "ice_m_stat(void *arg, uint_t stat, uint64_t *val)\n{", "\n/*\n * SFF"
    )
    assert "ice_rebuild_lock" not in stat

    # The MAC handle is cleared under ice_lse_lock so a concurrent async link
    # update reading it under the same lock cannot see a torn handle.
    unreg = gld[gld.index("ice_mac_unregister(ice_t *ice)\n{"):]
    assert "mutex_enter(&ice->ice_lse_lock)" in unreg
    assert unreg.index("mutex_enter(&ice->ice_lse_lock)") < unreg.index(
        "ice->ice_mac_hdl = NULL"
    )

    attach = ATTACH_SOURCE.read_text(encoding="utf-8")

    # The taskq worker consumes the coalesced request, no-ops while detaching,
    # and takes the rebuild lock only after dropping ice_lock (outermost rule).
    task = function(attach, "ice_reset_task(void *arg)\n{", "\n#ifdef DEBUG")
    assert "ice->ice_detaching" in task
    assert "ice_prepare_for_reset(ice)" in task
    assert "ice_rebuild(ice)" in task
    assert task.index("mutex_exit(&ice->ice_lock)") < task.index(
        "mutex_enter(&ice->ice_rebuild_lock)"
    )
    # A prepare that cannot drain outstanding rx loans fails closed terminally
    # instead of proceeding into the rebuild.
    assert "if (!ice_prepare_for_reset(ice))" in task
    assert "ice_reset_set_failed(ice)" in task
    assert task.index("if (!ice_prepare_for_reset(ice))") < task.index(
        "ice_rebuild(ice)"
    )

    # The rebuild clears ICE_ATTACH_HW_INIT around ice_deinit_hw/ice_hw_init so a
    # failure between them does not leave detach to double-deinit the HW.
    rebuild = function(attach, "ice_rebuild(ice_t *ice)\n{", "\nvoid\nice_reset_task")
    clear = rebuild.index("ice_attach_progress &= ~ICE_ATTACH_HW_INIT")
    deinit = rebuild.index("ice_deinit_hw(hw)")
    hw_init = rebuild.index("ice_hw_init(ice)")
    set_bit = rebuild.index("ice_attach_progress |= ICE_ATTACH_HW_INIT")
    assert clear < deinit < hw_init < set_bit

    # ice_prepare_for_reset uses the bounded loan wait, and that wait is a
    # timed condvar so the reset taskq cannot wedge on a lost loan.
    prepare = function(
        attach,
        "ice_prepare_for_reset(ice_t *ice)\n{",
        "\nstatic void\nice_rebuild",
    )
    # The bounded wait is now the only rx stop: mac_stop, the reset path and
    # detach all share it, so no teardown path can wedge on a lost loan.
    assert "ice_rx_stop(ice)" in prepare
    rx = RX_SOURCE.read_text(encoding="utf-8")
    stop = rx[rx.index("ice_rx_stop(ice_t *ice)\n{"):]
    assert "cv_timedwait(&irr->irxr_cv" in stop
    assert "drv_usectohz(ICE_RX_LOAN_WAIT_US)" in stop
    assert "cv_wait(&irr->irxr_cv" not in rx

    # The reset taskq is torn down after the interrupt handlers are removed and
    # before the rings and VSI the rebuild touches are freed; its lock goes too.
    unconf = function(
        attach,
        "ice_unconfigure(ice_t *ice)\n{",
        "\nstatic uint32_t\nice_prop_get_num_queues",
    )
    rem = unconf.index("ice_rem_intr_handlers(ice)")
    destroy = unconf.index("ddi_taskq_destroy(ice->ice_reset_taskq)")
    rings = unconf.index("ice_tx_rings_free(ice)")
    vsi_fini = unconf.index("ice_vsi_fini(ice)")
    assert rem < destroy < rings
    assert destroy < vsi_fini
    assert "mutex_destroy(&ice->ice_rebuild_lock)" in unconf

    # Detach marks the device detaching under the rebuild lock before any
    # admin-queue teardown, so a not-yet-started rebuild becomes a no-op and an
    # in-flight one is waited out before ice_loopback_fini's admin-queue
    # commands and before ice_unconfigure.
    detach = function(
        attach,
        "ice_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)\n{",
        "\nstatic void\nice_reset_set_failed",
    )
    assert "ice->ice_detaching = B_TRUE" in detach
    assert detach.index("ice_detaching = B_TRUE") < detach.index(
        "ice_loopback_fini(ice)"
    )
    assert detach.index("ice_detaching = B_TRUE") < detach.index(
        "ice_unconfigure(ice)"
    )

    # ice_reset_dispatch mirrors the oicr_pending coalescing: one queued task
    # at a time, cleared on dispatch failure.
    intr = INTR_SOURCE.read_text(encoding="utf-8")
    dispatch = function(
        intr,
        "ice_reset_dispatch(ice_t *ice)\n{",
        "\n/*\n * Taskq worker",
    )
    assert "if (!ice->ice_reset_pending)" in dispatch
    assert "ice->ice_reset_pending = B_TRUE" in dispatch
    assert "ddi_taskq_dispatch(ice->ice_reset_taskq, ice_reset_task" in dispatch
    assert "ice->ice_reset_pending = B_FALSE" in dispatch

    # The OICR worker hands an owed rebuild to the reset taskq, never runs it.
    worker = function(
        intr,
        "ice_oicr_task(void *arg)\n{",
        "\nstatic void\nice_intr_oicr_enable",
    )
    assert "ice_reset_dispatch(ice)" in worker

    print("PASS: ice reset serialization and dispatch source invariants")


if __name__ == "__main__":
    main()

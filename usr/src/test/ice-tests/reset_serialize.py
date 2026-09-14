#!/usr/bin/env python3

"""Check ICE reset serialization, dispatch, and detach-safety invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"
ATTACH_SOURCE = REPO / "usr/src/uts/common/io/ice/ice.c"
GLD_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_gld.c"
INTR_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_intr.c"
STATS_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_stats.c"
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
    assert "boolean_t\t\tice_attaching;" in header
    assert "boolean_t\t\tice_detaching;" in header
    assert "ICE_ATTACH_RESET_TASKQ" in header

    # mac start/stop bracket the datapath in the outermost rebuild lock so a
    # rebuild cannot interleave with a plumb, and start goes through the
    # factored ice_start_datapath.
    gld = GLD_SOURCE.read_text(encoding="utf-8")
    filters = (GLD_SOURCE.parent / "ice_filter.c").read_text()
    start = function(gld, "ice_m_start(void *arg)\n{", "\nstatic void\nice_m_stop")
    assert "mutex_enter(&ice->ice_rebuild_lock)" in start
    assert "mutex_exit(&ice->ice_rebuild_lock)" in start
    assert "ice_start_datapath(ice)" in start
    stop = function(gld, "ice_m_stop(void *arg)\n{", "\nstatic int\nice_m_promisc")
    assert "mutex_enter(&ice->ice_rebuild_lock)" in stop
    assert "mutex_exit(&ice->ice_rebuild_lock)" in stop

    # Every MAC callback that issues an admin-queue command holds the outermost
    # ice_rebuild_lock across it, so a reset rebuild's ice_deinit_hw/ice_hw_init
    # cannot destroy the control queue underneath the command.
    setmac = function(
        filters,
        "ice_filters_set_mac(ice_t *ice, const uint8_t *addr, boolean_t add)\n{",
        "\n/*\n * Apply or clear",
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in setmac
    assert "mutex_exit(&ice->ice_rebuild_lock)" in setmac
    assert "ice_filters_set_mac_locked(ice, addr, add)" in setmac

    promisc = function(
        filters, "ice_filters_set_promisc(ice_t *ice, boolean_t on)\n{", "\n/* Reapply accepted policy"
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in promisc
    assert "mutex_exit(&ice->ice_rebuild_lock)" in promisc

    # ice_filters_promisc_apply must NOT take the lock: ice_vsi_rebuild calls it while
    # already holding ice_rebuild_lock.  It asserts the lock instead, and owns
    # ice_promisc_on as the accepted policy used by the rebuild.
    apply = function(
        filters, "ice_filters_promisc_apply(ice_t *ice, boolean_t on)\n{", "\nint\nice_filters_set_promisc"
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" not in apply
    assert "mutex_exit(&ice->ice_rebuild_lock)" not in apply
    assert "ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));" in apply
    assert "ice->ice_promisc_on = prev;" in apply
    assert "ice_filters_recover(ice);" in apply

    # A failed enable rolls the same mask back so a retry is not rejected with
    # ICE_ERR_ALREADY_EXISTS, and the errno is decoded before the rollback
    # overwrites sq_last_status.
    assert apply.count("ice_clear_vsi_promisc(hw, vsi->vi_handle, mask, 0)") == 2
    assert apply.index("ret = ice_status_to_errno(ice, status);") < apply.index(
        "rollback = ice_clear_vsi_promisc("
    )

    setmac_locked = function(
        filters,
        "ice_filters_set_mac_locked(ice_t *ice, const uint8_t *addr, boolean_t add)\n{",
        "\n/*\n * ice_rebuild_lock is the outermost",
    )
    assert setmac_locked.index("int error = ice_status_to_errno(ice, status);") < (
        setmac_locked.index("ice_filters_recover(ice);"))
    assert "return (error);" in setmac_locked

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

    # ice_m_stat reads the port counters through hw->port_info->lport, which a
    # reset rebuild invalidates, so it takes the outermost rebuild lock around
    # the inner stat lock and drops them in reverse.
    stat = function(
        gld, "ice_m_stat(void *arg, uint_t stat, uint64_t *val)\n{", "\n/*\n * SFF"
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in stat
    assert stat.index("mutex_enter(&ice->ice_rebuild_lock)") < stat.index(
        "mutex_enter(&ice->ice_stat_lock)"
    )
    assert stat.index("mutex_exit(&ice->ice_stat_lock)") < stat.index(
        "mutex_exit(&ice->ice_rebuild_lock)"
    )
    # Every path that takes the stat lock takes the rebuild lock first.
    assert stat.count("mutex_enter(&ice->ice_stat_lock)") == stat.count(
        "mutex_enter(&ice->ice_rebuild_lock)"
    )
    assert stat.count("mutex_exit(&ice->ice_stat_lock)") == stat.count(
        "mutex_exit(&ice->ice_rebuild_lock)"
    )

    # Same bracket, same order, for both kstat updaters.
    stats_src = STATS_SOURCE.read_text(encoding="utf-8")
    for signature, following in (
        ("ice_pf_kstat_update(kstat_t *ksp, int rw)\n{", "\nstatic int\nice_vsi_kstat_update"),
        ("ice_vsi_kstat_update(kstat_t *ksp, int rw)\n{", "\nstatic boolean_t\nice_pf_kstat_init"),
    ):
        upd = function(stats_src, signature, following)
        assert "mutex_enter(&ice->ice_rebuild_lock)" in upd, signature
        assert upd.index("mutex_enter(&ice->ice_rebuild_lock)") < upd.index(
            "mutex_enter(&ice->ice_stat_lock)"
        ), signature
        assert upd.index("mutex_exit(&ice->ice_stat_lock)") < upd.index(
            "mutex_exit(&ice->ice_rebuild_lock)"
        ), signature

    # The inverse of B1/B2: the rebuild must never reach for the inner stat
    # lock, which would invert the documented order.
    attach_src = ATTACH_SOURCE.read_text(encoding="utf-8")
    assert "ice_stat_lock" not in function(
        attach_src, "ice_rebuild(ice_t *ice, uint32_t requests)\n{", "\nvoid\nice_reset_task"
    )

    # The MAC handle is cleared under ice_lse_lock so a concurrent async link
    # update reading it under the same lock cannot see a torn handle.
    unreg = gld[gld.index("ice_mac_unregister(ice_t *ice)\n{"):]
    assert "mutex_enter(&ice->ice_lse_lock)" in unreg
    assert unreg.index("mutex_enter(&ice->ice_lse_lock)") < unreg.index(
        "ice->ice_mac_hdl = NULL"
    )

    attach = ATTACH_SOURCE.read_text(encoding="utf-8")

    # The worker claims requests under the lifecycle lock and retains its
    # dispatch ownership until the rebuild finishes. reset_requests.py executes
    # the claim, redispatch, and request-during-rebuild interleavings.
    task = function(attach, "ice_reset_task(void *arg)\n{", "\n#ifdef DEBUG")
    assert "ice->ice_attaching" in task
    assert "ice->ice_detaching" in task
    assert "ice_prepare_for_reset(ice)" in task
    assert "ice_rebuild(ice, requests)" in task
    assert task.index("mutex_enter(&ice->ice_rebuild_lock)") < task.index(
        "ice_reset_take_requests(ice)")
    assert task.index("ice_rebuild(ice, requests)") < task.index(
        "ice->ice_reset_pending = B_FALSE")
    # The prepare cannot fail, so the worker always proceeds into the rebuild.
    # An undrained rx loan is handled where it is recoverable -- ice_rx_start()
    # refuses the surviving pool and the rebuild fails soft -- not by taking the
    # instance terminally offline from here.
    assert "if (!ice_prepare_for_reset(ice))" not in task
    assert "ice_reset_set_failed" not in task
    assert task.index("ice_prepare_for_reset(ice);") < task.index(
        "ice_rebuild(ice, requests)"
    )
    # The terminal state has exactly one source: the rebuild's hardware and
    # firmware failure label.
    assert attach.count("ice_reset_set_failed(ice)") == 1

    # The rebuild never tears the common code down, so ICE_ATTACH_HW_INIT stays
    # set for the life of the instance and detach owns the single teardown.
    rebuild = function(attach, "ice_rebuild(ice_t *ice, uint32_t requests)\n{", "\nvoid\nice_reset_task")
    assert "ICE_ATTACH_HW_INIT" not in rebuild
    unconf_hw = function(
        attach,
        "ice_unconfigure(ice_t *ice)\n{",
        "\nstatic uint32_t\nice_prop_get_num_queues",
    )
    assert "ice->ice_attach_progress & ICE_ATTACH_HW_INIT" in unconf_hw
    assert unconf_hw.count("ice_deinit_hw(&ice->ice_hw)") == 1

    # ice_prepare_for_reset uses the bounded loan wait, and that wait is a
    # timed condvar so the reset taskq cannot wedge on a lost loan.
    prepare = function(
        attach,
        "ice_prepare_for_reset(ice_t *ice)\n{",
        "\nstatic void\nice_rebuild",
    )
    # The bounded wait is shared by mac_stop, the reset path and detach, so no
    # teardown path can wedge on a lost loan.  The reset path takes only the
    # waiting half; the reclaim happens past the reset barrier (see
    # reset_rebuild.py).
    assert "ice_rx_quiesce(ice)" in prepare
    rx = RX_SOURCE.read_text(encoding="utf-8")
    quiesce = function(
        rx,
        "ice_rx_quiesce(ice_t *ice)\n{",
        "\nvoid\nice_rx_reclaim",
    )
    assert "cv_timedwait(&irr->irxr_cv" in quiesce
    assert "drv_usectohz(ICE_RX_LOAN_WAIT_US)" in quiesce
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
    # It must also precede mac_unregister: that frees the mac_impl_t the
    # workers report link state through, and the flag is what fences them.
    # A bound client fails the unregister, so the flag rolls back.
    assert detach.index("ice_detaching = B_TRUE") < detach.index(
        "ice_mac_unregister(ice)"
    )
    assert "ice->ice_detaching = B_FALSE" in detach

    # Both the bounded RX fence and fallible hardware isolation precede
    # unregister.  detach_quiesce.py executes the failure/rollback paths.
    assert detach.index("ice_detaching = B_TRUE") < detach.index(
        "ice_detach_quiesce(ice)")
    assert detach.index("ice_detach_quiesce(ice)") < detach.index(
        "ice_mac_unregister(ice)")
    assert "ice->ice_detaching ||" in start

    # Every gate lift requeues an owed rebuild.  The GRST and fatal-cause
    # latches are one-shot; persistent requests preserve the work. Gate lifts
    # redispatch promptly and the admin periodic retries failed dispatches.
    redispatch = function(
        attach,
        "ice_reset_redispatch(ice_t *ice)\n{",
        "\nstatic int\nice_attach",
    )
    assert "ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock))" in redispatch
    assert "ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ" in redispatch
    assert "ice_reset_dispatch(ice)" in redispatch
    # Terminal failure is not requeued: ice_m_start() refuses to plumb anyway.
    assert "ICE_STATE_RESET_FAILED" in redispatch
    assert redispatch.index("ICE_STATE_RESET_FAILED") < redispatch.index(
        "ice_reset_dispatch(ice)"
    )
    # It must not take ice_rebuild_lock itself: it is called with it held.
    assert "mutex_enter(&ice->ice_rebuild_lock)" not in redispatch

    # Cover every gate-lift site, not just the two known ones.
    lifts = 0
    for flag in ("ice->ice_attaching = B_FALSE", "ice->ice_detaching = B_FALSE"):
        offset = 0
        while True:
            try:
                offset = attach.index(flag, offset)
            except ValueError:
                break
            end = attach.index("mutex_exit(&ice->ice_rebuild_lock)", offset)
            assert "ice_reset_redispatch(ice)" in attach[offset:end], flag
            lifts += 1
            offset += 1
    # The end of attach, and the shared rollback for every detach failure.
    assert lifts == 2

    # Attach arms the interrupts last, as FreeBSD's ice_if_attach_post does:
    # after every step that builds the state a rebuild would free, and still
    # before mac_register, which can drive ice_m_start immediately.
    attach_fn = function(
        attach,
        "ice_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)\n{",
        "\nstatic int\nice_detach",
    )
    enable = attach_fn.index("ice_intr_enable(ice)")
    assert attach_fn.index("ice_stats_init(ice)") < enable
    assert attach_fn.index("ice_vsi_init(ice)") < enable
    assert attach_fn.index("ice_queues_intr_map(ice)") < enable
    assert enable < attach_fn.index("ice_mac_register(ice)")
    assert attach_fn.index("ice_intr_oicr_setup(ice, B_TRUE)") < enable
    assert attach_fn.index("ice_set_link_events(ice)") < attach_fn.index(
        "ice_setup_link(ice)"
    )
    # Final carrier resync follows registration and interrupt setup, under
    # the lifecycle lock and before lifting the attach gate.  This prevents
    # querying or publishing across a concurrent start or reset rebuild.
    resync = attach_fn.rindex("ice_link_status_update(ice)")
    assert enable < resync
    assert attach_fn.index("ice_mac_register(ice)") < resync
    clear = attach_fn.index("ice->ice_attaching = B_FALSE")
    assert resync < clear < attach_fn.index("ICE_STATE_ATTACHED")
    locked = attach_fn.rindex("mutex_enter(&ice->ice_rebuild_lock)", 0, clear)
    unlocked = attach_fn.index("mutex_exit(&ice->ice_rebuild_lock)", clear)
    assert locked < resync < clear < unlocked

    # Defense in depth for a cause latched before the enable: the attaching
    # gate is set before the reset taskq exists and cleared under the rebuild
    # lock only once the instance is fully built.
    assert "ice->ice_attaching = B_TRUE" in attach_fn
    assert attach_fn.index("ice_attaching = B_TRUE") < attach_fn.index(
        "ddi_taskq_create(dip, \"ice_reset\""
    )
    clear = attach_fn.index("ice->ice_attaching = B_FALSE")
    assert attach_fn.index("ice_mac_register(ice)") < clear
    assert clear < attach_fn.index("ICE_STATE_ATTACHED")
    held = attach_fn[
        attach_fn.rindex("mutex_enter(&ice->ice_rebuild_lock)", 0, clear):
        attach_fn.index("mutex_exit(&ice->ice_rebuild_lock)", clear)
    ]
    assert "ice->ice_attaching = B_FALSE" in held

    # ice_reset_dispatch mirrors the oicr_pending coalescing: one queued task
    # at a time, cleared on dispatch failure.
    intr = INTR_SOURCE.read_text(encoding="utf-8")
    dispatch = function(
        intr,
        "ice_reset_dispatch(ice_t *ice)\n{",
        "\nstatic boolean_t\nice_state_testclear",
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
    # It goes through the single ice_reset_redispatch() encapsulation rather
    # than open-coding ice_reset_dispatch(), so the ICE_STATE_RESET_FAILED
    # guard cannot be bypassed: otherwise the admin periodic dispatches a
    # rebuild every tick on a terminally failed instance, forever.
    assert "ice_reset_redispatch(ice)" in worker
    assert "ice_reset_dispatch(ice)" not in worker
    # It runs on its own taskq, so it holds the outermost rebuild lock for the
    # admin-queue drain -- taken only after ice_lock is dropped -- and the
    # reset-state check lives inside it, otherwise the check is a race against
    # ice_prepare_for_reset setting reset_ongoing.
    assert worker.index("mutex_exit(&ice->ice_lock)") < worker.index(
        "mutex_enter(&ice->ice_rebuild_lock)"
    )
    assert worker.index("mutex_enter(&ice->ice_rebuild_lock)") < worker.index(
        "hw->reset_ongoing || (ice->ice_state &"
    )
    assert worker.index("hw->reset_ongoing || (ice->ice_state &") < worker.index(
        "ice_clean_rq_elem(hw, &hw->adminq"
    )
    assert "ice->ice_attaching" in worker
    assert "ice->ice_detaching" in worker
    # ice_oicr_fatal reports the link down through the MAC handle, so it must
    # sit behind the lifetime gate, not one statement ahead of it.
    assert worker.index("ice->ice_detaching") < worker.index(
        "ice_oicr_fatal(ice, cause, mdd)"
    )
    # Every exit from the locked region drops it.
    assert worker.count("mutex_enter(&ice->ice_rebuild_lock)") == 1
    assert worker.count("mutex_exit(&ice->ice_rebuild_lock)") == worker.count(
        "return;"
    ) + 1

    # A terminally failed instance stops touching the control queue and the
    # PHY: ice_rebuild() has already run ice_shutdown_all_ctrlq(), so both the
    # ARQ drain and the unconditional link refresh would fail and log at
    # CE_WARN on every 500ms admin periodic tick, forever.  The bit is only
    # ever set under ice_rebuild_lock, so the test must sit inside it.
    # FreeBSD skips control-queue work on the same bit (if_ice_iflib.c:2449).
    assert "(ice->ice_state & ICE_STATE_RESET_FAILED) != 0" in worker
    assert worker.index("mutex_enter(&ice->ice_rebuild_lock)") < worker.index(
        "ICE_STATE_RESET_FAILED"
    )
    assert worker.index("ICE_STATE_RESET_FAILED") < worker.index(
        "ice_clean_rq_elem(hw, &hw->adminq"
    )
    assert worker.index("ICE_STATE_RESET_FAILED") < worker.rindex(
        "ice_link_status_update_impl(ice, NULL)"
    )

    # ice_reset_redispatch() is the single encapsulation of "an owed rebuild
    # should be requeued", so it must be visible to ice_intr.c and must check
    # the terminal bit before dispatching.
    assert "extern void ice_reset_redispatch(ice_t *);" in header
    attach = ATTACH_SOURCE.read_text(encoding="utf-8")
    assert "\nstatic void\nice_reset_redispatch" not in attach
    redispatch = function(
        attach,
        "ice_reset_redispatch(ice_t *ice)\n{",
        "\nstatic int\nice_attach",
    )
    assert redispatch.index("ICE_STATE_RESET_FAILED") < redispatch.index(
        "ice_reset_dispatch(ice)"
    )

    # The terminal state retires any later requests because they cannot be
    # serviced until reload. Successful rebuilds preserve those requests.
    rebuild = function(
        attach, "ice_rebuild(ice_t *ice, uint32_t requests)\n{", "\n/*\n * Reset taskq worker:"
    )
    failed = rebuild[rebuild.index("\nreset_failed:"):]
    assert "~(ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ)" in failed
    assert failed.index("ICE_STATE_PFR_REQ") < failed.index(
        "ice_reset_set_failed(ice)"
    )

    # Routine driver errors go to syslog only; a recurring hardware fault must
    # not be able to render the console unusable.  Matches i40e_error().
    err = function(attach, "ice_error(ice_t *ice, const char *fmt, ...)\n{", "\nint\n")
    assert 'dev_err(ice->ice_dip, CE_WARN, "!%s", buf)' in err
    assert 'cmn_err(CE_WARN, "!ice: %s", buf)' in err

    print("PASS: ice reset serialization and dispatch source invariants")


if __name__ == "__main__":
    main()

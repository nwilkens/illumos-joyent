#!/usr/bin/env python3

"""Check ICE reset prepare/rebuild source invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
ATTACH_SOURCE = REPO / "usr/src/uts/common/io/ice/ice.c"
VSI_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_vsi.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    attach = ATTACH_SOURCE.read_text(encoding="utf-8")

    # Prepare quiesces the datapath, fences new interrupts, and marks the VSI
    # absent so the rebuild recreates it.  The MAC list is left intact.
    prepare = function(
        attach,
        "ice_prepare_for_reset(ice_t *ice)\n{",
        "\nstatic void\nice_rebuild",
    )
    assert "reset_ongoing = true" in prepare
    assert "ice_tx_stop(ice)" in prepare
    # The reset path uses the bounded loan wait, not the unbounded mac_stop one.
    assert "ice_rx_stop_reset(ice)" in prepare
    assert "ice_rx_stop(ice)" not in prepare
    assert "ice_intr_oicr_disable(ice)" in prepare
    assert "PFINT_OICR_ENA, 0" in prepare
    assert "LINK_STATE_DOWN" in prepare
    assert "vi_added = B_FALSE" in prepare
    # Prepare reports whether loans drained so the caller can fail closed.
    assert "return (drained)" in prepare

    rebuild = function(
        attach,
        "ice_rebuild(ice_t *ice)\n{",
        "\nvoid\nice_reset_task",
    )
    # The rebuild reinitializes only what a reset clears.
    for token in (
        "ice_deinit_hw",
        "ice_hw_init",
        "ice_ddp_load",
        "ice_vsi_rebuild",
        "ice_intr_oicr_setup",
        "ice_queues_intr_map",
        "ice_set_link_events",
        # MAC does not re-drive the per-ring rx start across a reset, so the
        # rebuild must repost buffers and reopen the rings itself.
        "ice_rx_rings_resume",
    ):
        assert token in rebuild, token
    # It must not re-run the one-time attach allocations that survive a reset.
    for token in (
        "ice_alloc_intrs",
        "ice_add_intr_handlers",
        "ice_mac_register",
        "ice_tx_rings_alloc",
        "ice_rx_rings_alloc",
    ):
        assert token not in rebuild, token
    # The admin queue is usable only once reset_ongoing is cleared, and every
    # rebuild step below rides that queue, so it clears before hardware init.
    assert "reset_ongoing = false" in rebuild
    assert rebuild.index("reset_ongoing = false") < rebuild.index("ice_hw_init")
    # A global/core reset can zero the MAC counters; drop both baselines.
    assert "ice_stat_port_loaded = B_FALSE" in rebuild
    assert "ice_stat_vsi_loaded = B_FALSE" in rebuild
    # The reset-owed bits clear after the rebuild steps but before the OICR is
    # re-enabled, so a fresh link-change OICR cannot observe a stale
    # RESET_PENDING/PFR_REQ and dispatch a redundant rebuild.
    owed = rebuild.index("~(ICE_STATE_RESET_PENDING | ICE_STATE_PFR_REQ)")
    assert rebuild.index("ice_hw_init") < owed
    assert rebuild.index("ice_vsi_rebuild") < owed
    assert owed < rebuild.index("ice_intr_oicr_setup")
    # The fail-closed bit clears only after the datapath-restoring steps.
    err = rebuild.index("~ICE_STATE_ERROR")
    assert rebuild.index("ice_vsi_rebuild") < err
    assert rebuild.index("ice_queues_intr_map") < err
    # A datapath restart failure is soft (recoverable), not terminal.
    assert "ice_start_datapath(ice)" in rebuild
    # The terminal path fails closed via the shared helper.
    terminal = rebuild[rebuild.index("reset_failed:"):]
    assert "ice_reset_set_failed(ice)" in terminal
    fail = function(
        attach,
        "ice_reset_set_failed(ice_t *ice)\n{",
        "\nstatic boolean_t\nice_prepare_for_reset",
    )
    assert "ICE_STATE_RESET_FAILED" in fail
    assert "DDI_SERVICE_LOST" in fail

    # ice_vsi_rebuild recreates the VSI, replays the tracked filters, restores
    # RSS, and re-applies promiscuous mode when it was on.
    vsi = VSI_SOURCE.read_text(encoding="utf-8")
    vrebuild = vsi[vsi.index("ice_vsi_rebuild(ice_t *ice)\n{"):]
    assert "ice_vsi_setup(ice)" in vrebuild
    assert "ice_fltr_entry_init(" in vrebuild
    assert "ice_add_mac(hw, &add)" in vrebuild
    assert "ice_rss_setup(ice)" in vrebuild
    assert "ice->ice_promisc_on" in vrebuild
    assert "ice_promisc_apply(ice, B_TRUE)" in vrebuild

    print("PASS: ice reset prepare/rebuild source invariants")


if __name__ == "__main__":
    main()

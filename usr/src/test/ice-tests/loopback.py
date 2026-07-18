#!/usr/bin/env python3

"""Check ICE internal MAC loopback control and isolation invariants."""

import re
from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
ATTACH_SOURCE = REPO / "usr/src/uts/common/io/ice/ice.c"
GLD_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_gld.c"
VSI_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_vsi.c"
INTR_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_intr.c"
RX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_rx.c"
TX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_tx.c"
TOOL_SOURCE = REPO / "usr/src/test/ice-tests/ice_loopback.c"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    gld = GLD_SOURCE.read_text(encoding="utf-8")
    assert "#include <sys/stream.h>" in gld
    assert "#include <sys/strsun.h>" in gld
    assert "MC_IOCTL | MC_GETCAPAB" in gld
    assert ".mc_ioctl = ice_m_ioctl" in gld
    for command in ("LB_GET_INFO_SIZE", "LB_GET_INFO", "LB_GET_MODE", "LB_SET_MODE"):
        assert command in gld

    ioctl = function(
        gld,
        "ice_m_ioctl(void *arg, queue_t *q, mblk_t *mp)\n{",
        "\n/*\n * MAC callbacks.",
    )
    set_mode = ioctl[ioctl.index("case LB_SET_MODE:") : ioctl.index("default:")]
    privilege = set_mode.index("secpolicy_net_config(iocp->ioc_cr, B_FALSE)")
    payload = set_mode.index("ice_loopback_payload", privilege)
    transition = set_mode.index("ice_loopback_mode_set", payload)
    assert privilege < payload < transition

    enable = function(
        gld,
        "ice_loopback_enable(ice_t *ice)\n{",
        "\nstatic int\nice_loopback_disable",
    )
    permit = enable.index("ice_vsi_loopback_set(ice, B_TRUE)")
    mac_enable = enable.index("ice_aq_set_mac_loopback", permit)
    rollback = enable.index("ice_vsi_loopback_set(ice, B_FALSE)", mac_enable)
    assert permit < mac_enable < rollback

    disable = function(
        gld,
        "ice_loopback_disable(ice_t *ice)\n{",
        "\nstatic int\nice_loopback_mode_set",
    )
    mac_disable = disable.index("ice_aq_set_mac_loopback")
    revoke = disable.index("ice_vsi_loopback_set(ice, B_FALSE)", mac_disable)
    rollback = disable.index("ice_aq_set_mac_loopback", revoke)
    assert mac_disable < revoke < rollback

    # The loopback transition logic lives in the _locked routine; the thin
    # ice_loopback_mode_set wrapper brackets it in the outermost rebuild lock.
    setter = function(
        gld,
        "ice_loopback_mode_set_locked(ice_t *ice, uint32_t mode)\n{",
        "\n/*\n * ice_rebuild_lock is the outermost",
    )
    transition = setter.index("ice_loopback_enable(ice)")
    publish = setter.index("ice_link_loopback_update", transition)
    refresh = setter.index("ice_link_status_update", publish)
    lock_enter = setter.index("mutex_enter(&ice->ice_loopback_lock)")
    lock_exit = setter.rindex("mutex_exit(&ice->ice_loopback_lock)")
    assert "ice->ice_lock" not in setter
    assert lock_enter < transition < publish < refresh < lock_exit

    wrapper = function(
        gld,
        "ice_loopback_mode_set(ice_t *ice, uint32_t mode)\n{",
        "\nvoid\nice_loopback_fini",
    )
    assert "mutex_enter(&ice->ice_rebuild_lock)" in wrapper
    assert "ice_loopback_mode_set_locked(ice, mode)" in wrapper

    vsi = VSI_SOURCE.read_text(encoding="utf-8")
    vsi_set = function(
        vsi,
        "ice_vsi_loopback_set(ice_t *ice, boolean_t enable)\n{",
        "\nstatic int\nice_vsi_setup",
    )
    assert "ASSERT(MUTEX_HELD(&ice->ice_loopback_lock))" in vsi_set
    assert "ICE_AQ_VSI_PROP_SW_VALID" in vsi_set
    assert "flags |= ICE_AQ_VSI_SW_FLAG_ALLOW_LB |" in vsi_set
    assert "flags &= ~(ICE_AQ_VSI_SW_FLAG_ALLOW_LB |" in vsi_set
    assert vsi_set.count("ICE_AQ_VSI_SW_FLAG_LOCAL_LB") == 2
    prune_disable = vsi_set.index(
        "flags &= ~ICE_AQ_VSI_SW_FLAG_SRC_PRUNE"
    )
    prune_restore = vsi_set.index(
        "flags |= ICE_AQ_VSI_SW_FLAG_SRC_PRUNE", prune_disable
    )
    assert prune_disable < prune_restore
    update = vsi_set.index("ice_update_vsi")
    cache = vsi_set.index("cached->info.sw_flags = flags", update)
    assert update < cache

    attach = ATTACH_SOURCE.read_text(encoding="utf-8")
    assert "mutex_init(&ice->ice_loopback_lock, NULL, MUTEX_DRIVER, NULL)" in attach
    assert "mutex_destroy(&ice->ice_loopback_lock)" in attach
    detach_start = attach.index(
        "ice_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)\n{"
    )
    detach = attach[detach_start:]
    unregister = detach.index("ice_mac_unregister(ice)")
    cleanup = detach.index("ice_loopback_fini(ice)", unregister)
    assert unregister < cleanup

    intr = INTR_SOURCE.read_text(encoding="utf-8")
    decode = function(
        intr,
        "ice_link_prop_update(ice_t *ice)\n{",
        "\n/*\n * Refresh the cached link state",
    )
    override = decode.index("ice->ice_loopback_mode == ICE_LB_INTERNAL_MAC")
    physical_down = decode.index("(li->link_info & ICE_AQ_LINK_UP) == 0")
    assert override < physical_down

    rx = RX_SOURCE.read_text(encoding="utf-8")
    tx = TX_SOURCE.read_text(encoding="utf-8")
    assert "loopback" not in rx.lower()
    assert "loopback" not in tx.lower()

    tool = TOOL_SOURCE.read_text(encoding="utf-8")
    assert "#include <sys/stropts.h>" in tool
    assert "ioctl(fd, I_STR, &str)" in tool
    assert re.search(r"\bioctl\(fd, LB_", tool) is None
    for command in ("LB_GET_INFO_SIZE", "LB_GET_INFO", "LB_GET_MODE", "LB_SET_MODE"):
        assert f"netlb_ioctl(fd, {command}" in tool


if __name__ == "__main__":
    main()

#!/usr/bin/env python3

"""Check ICE internal MAC loopback control and isolation invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
ATTACH_SOURCE = REPO / "usr/src/uts/common/io/ice/ice.c"
GLD_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_gld.c"
INTR_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_intr.c"
RX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_rx.c"
TX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_tx.c"


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

    setter = function(
        gld,
        "ice_loopback_mode_set(ice_t *ice, uint32_t mode)\n{",
        "\nvoid\nice_loopback_fini",
    )
    aq = setter.index("ice_aq_set_mac_loopback")
    publish = setter.index("ice_link_loopback_update", aq)
    refresh = setter.index("ice_link_status_update", publish)
    assert aq < publish < refresh

    attach = ATTACH_SOURCE.read_text(encoding="utf-8")
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


if __name__ == "__main__":
    main()

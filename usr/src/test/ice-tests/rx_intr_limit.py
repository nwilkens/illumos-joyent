#!/usr/bin/env python3

"""Check the ice rx per-interrupt frame limit source invariants."""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
ICE_HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"
ICE_MAIN = REPO / "usr/src/uts/common/io/ice/ice.c"
RX_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_rx.c"
CONF = REPO / "usr/src/uts/common/io/ice/ice.conf"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    header = ICE_HEADER.read_text(encoding="utf-8")
    assert "#define\tICE_DEF_RX_LIMIT_PER_INTR\t256" in header
    assert "#define\tICE_MIN_RX_LIMIT_PER_INTR\t16" in header
    assert "#define\tICE_MAX_RX_LIMIT_PER_INTR\t4096" in header
    assert "kstat_named_t\t\ticrxs_intr_limit;" in header

    # the property is read with both clamps before the rings are allocated
    main_src = ICE_MAIN.read_text(encoding="utf-8")
    prop = main_src.index('"rx_limit_per_intr"')
    assert main_src.index("limit < ICE_MIN_RX_LIMIT_PER_INTR") > prop
    assert main_src.index("limit > ICE_MAX_RX_LIMIT_PER_INTR") > prop
    assign = main_src.index("ice->ice_rx_limit_per_intr = limit")
    assert assign > prop
    assert assign < main_src.index("ice_rx_rings_alloc(ice)")

    rx = RX_SOURCE.read_text(encoding="utf-8")
    ring_rx = function(
        rx,
        "ice_ring_rx(ice_rx_ring_t *irr, int poll_bytes, boolean_t *limitp)",
        "\nmblk_t *\nice_ring_rx_poll",
    )

    # the cap is hoisted once, disarmed for byte-budgeted polls, and covers
    # nonpositive budgets so no caller can drain unbounded
    hoist = ring_rx.index("cap = (poll_bytes <= 0)")
    check = ring_rx.index("frames >= cap")
    assert hoist < check
    assert "icrxs_intr_limit.value.ui64++" in ring_rx
    # a limit hit is declared only when the next descriptor is actually
    # written back, so an exact-cap burst cannot schedule a software
    # interrupt into an empty ring or overcount the kstat
    peek = ring_rx.index("ICE_RX_FLEX_DESC_STATUS0_DD_S", check)
    assert peek < ring_rx.index("icrxs_intr_limit.value.ui64++")
    assert peek < ring_rx.index("*limitp = B_TRUE")
    # the limit check runs before a frame is pulled, and every non-deferred
    # frame counts, so discards consume budget too
    assert check < ring_rx.index("ice_ring_rx_frame(irr, &total_len, &defer)")
    assert ring_rx.index("frames++") > ring_rx.index("if (defer)")
    assert ring_rx.index("frames++") < ring_rx.index("if (mp == NULL)")

    # mac may legitimately poll with a zero budget (bandwidth-capped SRS at
    # its drop threshold); that must deliver nothing, not assert or drain
    poll = function(rx, "ice_ring_rx_poll(void *arg, int poll_bytes)",
                    "\nboolean_t\nice_rx_ring_intr")
    assert "if (poll_bytes <= 0)" in poll
    assert "ASSERT3S(poll_bytes" not in poll
    assert "ice_ring_rx(irr, poll_bytes, &limit)" in poll

    # the interrupt path reports a limit hit to its caller
    intr = function(rx, "ice_rx_ring_intr(ice_rx_ring_t *irr)",
                    "\nint\nice_ring_rx_intr_enable")
    assert "ice_ring_rx(irr, 0, &limit)" in intr
    assert "return (limit);" in intr

    # the kstat is published under the i40e-compatible name
    assert '"rx_intr_limit"' in rx

    # the tunable is documented at its default in the shipped conf
    conf = CONF.read_text(encoding="utf-8")
    assert "# rx_limit_per_intr=256;" in conf

    print("PASS: ice rx per-interrupt limit source invariants")


if __name__ == "__main__":
    main()

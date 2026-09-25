#!/usr/bin/env python3

"""Check the ice limit-hit software-interrupt re-arm source invariants.

Datasheet 613875 section 9.1.2.6: descriptors already written back are not a
hardware event, so a plain INTENA re-arm after a capped drain would strand the
residue until new traffic arrives.  The ISR must instead fold a software
interrupt into the re-arm, throttled by an ITR so the refire train is paced.
"""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
INTR_SOURCE = REPO / "usr/src/uts/common/io/ice/ice_intr.c"
ICE_HEADER = REPO / "usr/src/uts/common/io/ice/ice.h"


def function(source: str, signature: str, following: str) -> str:
    start = source.index(signature)
    end = source.index(following, start)
    return source[start:end]


def main() -> None:
    header = ICE_HEADER.read_text(encoding="utf-8")
    assert "extern boolean_t ice_rx_ring_intr(ice_rx_ring_t *);" in header

    intr = INTR_SOURCE.read_text(encoding="utf-8")
    queue = function(intr, "ice_intr_queue(ice_t *ice, uint_t vector)",
                     "\nuint_t\nice_intr_msix")

    # the rx drain's limit verdict gates the SWINT bits
    verdict = queue.index("ice_rx_ring_intr(&ice->ice_rxr[idx])")
    swint = queue.index("GLINT_DYN_CTL_SWINT_TRIG_M")
    assert verdict < swint

    # SW_ITR_INDX must be programmed with its enable, throttled by the
    # queues' ITR slot; No-ITR (immediate refire) must not be used here
    assert "GLINT_DYN_CTL_SW_ITR_INDX_ENA_M" in queue
    assert "ICE_ITR_IDX_0 << GLINT_DYN_CTL_SW_ITR_INDX_S" in queue
    assert "ICE_ITR_INDEX_NONE << GLINT_DYN_CTL_SW_ITR_INDX_S" not in queue

    # one re-arm write per invocation: the SWINT bits are folded into the
    # single GLINT_DYN_CTL write rather than issued as a second doorbell
    assert queue.count("wr32(hw, GLINT_DYN_CTL(vector)") == 1
    assert swint < queue.index("wr32(hw, GLINT_DYN_CTL(vector)")

    # the base re-arm uses the one shared re-arm word, which enables and
    # clears in one shot
    assert "ICE_GLINT_DYN_CTL_REARM" in queue
    rearm = function(header, "#define\tICE_GLINT_DYN_CTL_REARM",
                     "\n\ntypedef enum ice_state")
    assert "GLINT_DYN_CTL_INTENA_M" in rearm
    assert "GLINT_DYN_CTL_CLEARPBA_M" in rearm

    print("PASS: ice rx limit-hit SWINT re-arm source invariants")


if __name__ == "__main__":
    main()

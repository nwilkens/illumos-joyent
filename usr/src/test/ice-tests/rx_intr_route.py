#!/usr/bin/env python3
"""Execute the real ICE rx interrupt routing writer across owner interleavings."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_rx.c")
    parser.add_argument("--header", type=Path, default=DRIVER / "ice.h")
    parser.add_argument("--ice-source", type=Path, default=DRIVER / "ice.c")
    args = parser.parse_args()
    source = args.source.read_text()
    fragments = [extract(args.header.read_text(),
                         r"^typedef enum ice_rx_intr_route \{[\s\S]*?"
                         r"^} ice_rx_intr_route_t;", args.header)]
    for name in ("ice_rx_ring_intr_program", "ice_rx_ring_intr_route",
                 "ice_ring_rx_intr_enable", "ice_ring_rx_intr_disable"):
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    run_c(TESTDIR / "rx_intr_route.c",
          {"rx_intr_route_body.h": "\n".join(fragments)})

    # No other writer remains: the lifecycle loops only change ring state.
    assert source.count("wr32(hw, QINT_RQCTL(") == 1
    ice_c = args.ice_source.read_text()
    assert "QINT_RQCTL(" not in ice_c
    # The reset resume must not override MAC's poll mode; MAP arms the rest.
    resume = source[source.index("ice_rx_rings_resume(ice_t *ice)\n{"):]
    resume = resume[:resume.index("\n}\n")]
    assert "ice_ring_rx_intr_enable" not in resume
    assert ice_c.count("ice_rx_ring_intr_route(") == 3
    for how in ("ICE_RX_INTR_MAP", "ICE_RX_INTR_UNMAP",
                "ICE_RX_INTR_DISSOCIATE"):
        assert how in ice_c, how


if __name__ == "__main__":
    main()

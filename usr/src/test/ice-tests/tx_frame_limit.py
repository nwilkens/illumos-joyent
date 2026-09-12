#!/usr/bin/env python3
"""Execute the real ICE TX frame admission rule against MTU and LSO cases."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_tx.c")
    parser.add_argument("--header", type=Path, default=DRIVER / "ice.h")
    args = parser.parse_args()
    fragments = [
        extract(args.header.read_text(),
                r"^typedef struct ice_tx_ctx_t \{[\s\S]*?^} ice_tx_ctx_t;",
                args.header),
        extract(args.source.read_text(),
                r"^static boolean_t\nice_tx_frame_fits\([\s\S]*?^}",
                args.source),
    ]
    run_c(TESTDIR / "tx_frame_limit.c",
          {"tx_frame_limit_body.h": "\n".join(fragments)})

    # The rule sits on the single admission path, after LSO gating, and is
    # accounted separately from other drops.
    tx = args.source.read_text()
    one = tx[tx.index("ice_tx_one(ice_tx_ring_t *itr, mblk_t *mp)\n{"):]
    one = one[:one.index("\n}\n")]
    assert "!ice_tx_frame_fits(ice->ice_mtu, &ctx, msglen)" in one
    assert one.index("!ice->ice_tx_lso_enable") < one.index("ice_tx_frame_fits")
    assert one.index("ice_tx_frame_fits") < one.index("ice_tx_build_tcbs(")
    assert "ictxs_oversize_drops.value.ui64++" in one
    assert '"tx_oversize_drops"' in tx


if __name__ == "__main__":
    main()

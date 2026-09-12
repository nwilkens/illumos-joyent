#!/usr/bin/env python3
"""Exercise the real ICE TX notification fence with pthread handshakes."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_tx.c")
    parser.add_argument("--case", help="run one named scenario")
    args = parser.parse_args()
    source = args.source.read_text()
    header = DRIVER / "ice.h"
    fragments = [extract(header.read_text(),
        r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;", header)]
    for name in ("ice_tx_recycle", "ice_tx_quiesce", "ice_tx_ring_intr"):
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    run_c(TESTDIR / "tx_quiesce.c",
          {"ice_tx_quiesce_body.h": "\n".join(fragments)},
          cflags=("-pthread",), cases=((args.case,) if args.case else (),))


if __name__ == "__main__":
    main()

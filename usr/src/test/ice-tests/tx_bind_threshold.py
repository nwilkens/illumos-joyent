#!/usr/bin/env python3

"""Compile and exercise the actual ICE transmit bind-versus-copy decisions."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_tx.c")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    header = DRIVER / "ice.h"
    types = header.read_text(encoding="utf-8")
    fragments = [extract(source,
                         r"^typedef enum ice_tx_build \{[\s\S]*?^} ice_tx_build_t;",
                         args.source),
                 extract(types,
                         r"^typedef enum ice_tcb_type \{[\s\S]*?^} ice_tcb_type_t;",
                         header)]
    for name in ("ice_dma_buffer", "ice_tx_ctrl_block", "ice_txq_stat"):
        fragments.append(extract(types,
                                 rf"^typedef struct {name} \{{[\s\S]*?^}} {name}_t;",
                                 header))
    for name in ("ICE_TX_SMALL_PKT", "ICE_TX_MIN_LEN", "ICE_TX_MAX_COOKIE"):
        fragments.append(extract(types, rf"^#define\s+{name}\s+[^\n]*", header))
    bodies = []
    for name in ("ice_tx_copy_packet", "ice_tx_build_tcbs"):
        bodies.append(extract(source,
                              rf"^static [\w *]+\n{name}\([\s\S]*?^}}",
                              args.source))

    run_c(TESTDIR / "tx_bind_threshold.c", {
        "ice_tx_types.h": "\n".join(fragments),
        "ice_tx_build.h": "\n".join(bodies),
    })


if __name__ == "__main__":
    main()

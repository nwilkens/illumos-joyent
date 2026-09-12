#!/usr/bin/env python3
"""Compile selected production RX functions against controlled DDI/STREAMS."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c

FUNCTIONS = (
    "ice_rx_alloc_mp", "ice_rcb_alloc", "ice_rcb_free", "ice_rx_recycle",
    "ice_rx_reset_desc", "ice_rx_alloc_rcbs", "ice_rx_free_rcbs",
    "ice_rx_next", "ice_rx_copy", "ice_rx_bind", "ice_rx_discard_frame",
    "ice_rx_vlan_insert", "ice_rx_desc_sync", "ice_ring_rx_frame",
)


def run(test, functions=FUNCTIONS, optional=("ice_rx_desc_sync",)):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_rx.c")
    parser.add_argument("--header", type=Path, default=DRIVER / "ice.h")
    args = parser.parse_args()
    source = args.source.read_text()
    header = args.header.read_text()
    parts = [extract(header, r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;",
                     args.header)]
    for name in ("ICE_RX_BUF_SIZE", "ICE_RX_MAX_DESC"):
        parts.append(extract(header, rf"^#define\s+{name}\s+.*", args.header))
    for name in ("ICE_RX_LOAN_RESERVE", "ICE_RX_COPY_THRESHOLD", "ICE_RX_HEADROOM"):
        # Optional headroom permits the original source to be a failing control.
        if re.search(rf"^#define\s+{name}\s+", source, re.MULTILINE):
            parts.append(extract(source, rf"^#define\s+{name}\s+.*", args.source))
    for name in functions:
        if name in optional and not re.search(rf"^{name}\(", source, re.MULTILINE):
            continue
        parts.append(extract(source,
                             rf"^(?:static )?(?:inline )?[\w *]+\n{name}\([\s\S]*?^}}",
                             args.source))
    run_c(TESTDIR / f"{test}.c", {"rx_functions.h": "\n".join(parts)},
          cflags=("-Wno-unused-function",))

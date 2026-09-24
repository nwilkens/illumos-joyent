#!/usr/bin/env python3
"""Compile selected production RX functions against controlled DDI/STREAMS."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c

FUNCTIONS = (
    "ice_rx_alloc_mp", "ice_rcb_alloc", "ice_rcb_free", "ice_rx_rcb_destroy",
    "ice_rx_reap_drain", "ice_rx_reap", "ice_rx_orphan_return",
    "ice_rx_recycle", "ice_rx_reset_desc",
    "ice_rx_pool_alloc", "ice_rx_pool_sweep", "ice_rx_pool_free",
    "ice_rx_pool_swap", "ice_rx_pool_release", "ice_rx_pool_orphan",
    "ice_rx_pool_retire",
    "ice_rx_next", "ice_rx_copy", "ice_rx_bind", "ice_rx_discard_frame",
    "ice_rx_vlan_insert", "ice_rx_desc_sync", "ice_ring_rx_frame",
    "ice_rx_loan_mode",
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
    for name in ("ICE_RX_BUF_SIZE", "ICE_RX_MAX_DESC", "ICE_RX_LOAN_RESERVE",
                 "ICE_RX_LOAN_RESERVE_MAX", "ICE_RX_ORPHAN_BUDGET",
                 "ICE_RX_ORPHAN_LOWAT"):
        # Older revisions kept the reserve in the source; see below.
        if re.search(rf"^#define\s+{name}\s+", header, re.MULTILINE):
            parts.append(extract(header, rf"^#define\s+{name}\s+.*",
                                 args.header))
    for name in ("ICE_RX_LOAN_RESERVE", "ICE_RX_COPY_THRESHOLD",
                 "ICE_RX_HEADROOM", "ICE_RX_LOAN_WAIT_US",
                 "ICE_RX_ORPHAN_POLL_US"):
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

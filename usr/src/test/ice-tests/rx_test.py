#!/usr/bin/env python3
"""Compile selected production RX functions against controlled DDI/STREAMS."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

TESTDIR = Path(__file__).resolve().parent
DRIVER = TESTDIR.parents[3] / "usr/src/uts/common/io/ice"
FUNCTIONS = (
    "ice_rx_alloc_mp", "ice_rcb_alloc", "ice_rcb_free", "ice_rx_recycle",
    "ice_rx_reset_desc", "ice_rx_alloc_rcbs", "ice_rx_free_rcbs",
    "ice_rx_next", "ice_rx_copy", "ice_rx_bind", "ice_rx_discard_frame",
    "ice_rx_vlan_insert", "ice_ring_rx_frame",
)


def extract(source, pattern, path):
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise ValueError(f"cannot extract {pattern!r} from {path}")
    line = source.count("\n", 0, match.start()) + 1
    return f"#line {line} {json.dumps(str(path))}\n{match.group()}\n"


def run(test, functions=FUNCTIONS):
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
        parts.append(extract(source,
                             rf"^(?:static )?(?:inline )?[\w *]+\n{name}\([\s\S]*?^}}",
                             args.source))
    with tempfile.TemporaryDirectory(prefix="ice-rx-test-") as tmp:
        work = Path(tmp)
        (work / "rx_functions.h").write_text("\n".join(parts))
        binary = work / test
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + ["-std=c99", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-function", "-pedantic", "-I", str(work),
                       str(TESTDIR / f"{test}.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

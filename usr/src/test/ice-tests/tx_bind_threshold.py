#!/usr/bin/env python3

"""Compile and exercise the actual ICE transmit bind-versus-copy decisions."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from terminal_filters import extract


TESTDIR = Path(__file__).resolve().parent
REPO = TESTDIR.parents[3]
DRIVER = REPO / "usr/src/uts/common/io/ice"


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

    with tempfile.TemporaryDirectory(prefix="ice-tx-bind-") as tmp:
        work = Path(tmp)
        (work / "ice_tx_types.h").write_text("\n".join(fragments), encoding="utf-8")
        (work / "ice_tx_build.h").write_text("\n".join(bodies), encoding="utf-8")
        binary = work / "tx_bind_threshold"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(work), str(TESTDIR / "tx_bind_threshold.c"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

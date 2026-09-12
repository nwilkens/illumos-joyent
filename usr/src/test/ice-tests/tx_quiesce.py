#!/usr/bin/env python3
"""Exercise the real ICE TX notification fence with pthread handshakes."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from terminal_filters import DRIVER, TESTDIR, extract


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
    with tempfile.TemporaryDirectory(prefix="ice-tx-quiesce-") as tmp:
        work = Path(tmp)
        (work / "ice_tx_quiesce_body.h").write_text("\n".join(fragments))
        binary = work / "tx_quiesce"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-pthread", "-I", str(work), str(TESTDIR / "tx_quiesce.c"),
            "-o", str(binary)], check=True)
        command = [str(binary)] + ([args.case] if args.case else [])
        subprocess.run(command, check=True, timeout=15)


if __name__ == "__main__":
    main()

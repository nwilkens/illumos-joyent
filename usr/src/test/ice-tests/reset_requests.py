#!/usr/bin/env python3
"""Execute ICE reset dispatch and rebuilding with controlled interleavings."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from terminal_filters import DRIVER, TESTDIR, extract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice.c")
    parser.add_argument("--intr-source", type=Path, default=DRIVER / "ice_intr.c")
    args = parser.parse_args()
    source = args.source.read_text()
    header = DRIVER / "ice.h"
    types = extract(header.read_text(),
        r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;", header)
    fragments = [extract(args.intr_source.read_text(),
        r"^void\nice_reset_dispatch\([\s\S]*?^}", args.intr_source)]
    names = ["ice_reset_redispatch", "ice_reset_take_requests",
             "ice_reset_complete", "ice_rebuild", "ice_reset_task"]
    for name in names:
        if not re.search(rf"^{name}\(", source, re.MULTILINE):
            if name in ("ice_reset_take_requests", "ice_reset_complete"):
                continue
            raise ValueError(f"missing reset function {name}")
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    with tempfile.TemporaryDirectory(prefix="ice-reset-requests-") as tmp:
        work = Path(tmp)
        (work / "ice_reset_types.h").write_text(types)
        (work / "ice_reset_body.h").write_text("\n".join(fragments))
        binary = work / "reset_requests"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-Wno-unused-function", "-I", str(work),
            str(TESTDIR / "reset_requests.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

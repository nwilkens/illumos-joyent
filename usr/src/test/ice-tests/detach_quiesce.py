#!/usr/bin/env python3
"""Execute ICE detach with controlled stop, reset, and unregister failures."""

import argparse
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

from terminal_filters import DRIVER, TESTDIR, extract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice.c")
    parser.add_argument("--gld-source", type=Path, default=DRIVER / "ice_gld.c")
    args = parser.parse_args()
    source = args.source.read_text()
    header = DRIVER / "ice.h"
    enums = header.read_text()
    fragments = []
    for name in ("ice_state", "ice_attach_state"):
        fragments.append(extract(enums,
            rf"^typedef enum {name} \{{[\s\S]*?^}} {name}_t;", header))
    # The FMA observer is a shared boundary for the selected detach body.
    acc = DRIVER / "ice.c"
    fragments.append(extract(acc.read_text(),
        r"^int\nice_check_acc_handle\([\s\S]*?^}", acc))
    # The baseline has no helper; still run its real detach as a failing control.
    names = ["ice_reset_redispatch", "ice_detach"]
    if re.search(r"^ice_detach_quiesce\(", source, re.MULTILINE):
        names.insert(0, "ice_detach_quiesce")
    for name in names:
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    fragments.append(extract(args.gld_source.read_text(),
        r"^static int\nice_m_start\([\s\S]*?^}", args.gld_source))
    with tempfile.TemporaryDirectory(prefix="ice-detach-") as tmp:
        work = Path(tmp)
        (work / "ice_detach_body.h").write_text("\n".join(fragments))
        binary = work / "detach_quiesce"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            # Some stubs are needed only by the old implementation.
            "-Wno-unused-function", "-I", str(work),
            str(TESTDIR / "detach_quiesce.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

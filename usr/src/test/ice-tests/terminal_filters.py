#!/usr/bin/env python3

"""Compile and exercise the real ICE filter callbacks with a stub admin queue."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


TESTDIR = Path(__file__).resolve().parent
REPO = TESTDIR.parents[3]
DRIVER = REPO / "usr/src/uts/common/io/ice"
FUNCTIONS = (
    "ice_gld_fltr_init",
    "ice_gld_find_mac",
    "ice_gld_set_mac_locked",
    "ice_gld_set_mac",
    "ice_group_add_mac",
    "ice_group_remove_mac",
    "ice_promisc_apply",
    "ice_m_promisc",
    "ice_m_multicst",
)


def extract(source: str, pattern: str, path: Path) -> str:
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise ValueError(f"cannot extract {pattern!r} from {path}")
    line = source.count("\n", 0, match.start()) + 1
    return f"#line {line} {json.dumps(str(path))}\n{match.group()}\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_gld.c")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    header = DRIVER / "ice.h"
    fragments = [extract(header.read_text(encoding="utf-8"),
                         r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;",
                         header)]
    # illumos style places only the function's final brace in column zero.
    # Compile each body unchanged; this does not assert its implementation text.
    for name in FUNCTIONS:
        fragments.append(extract(
            source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}",
            args.source,
        ))

    with tempfile.TemporaryDirectory(prefix="ice-terminal-filters-") as tmp:
        work = Path(tmp)
        (work / "ice_filter_callbacks.h").write_text(
            "\n".join(fragments), encoding="utf-8")
        binary = work / "terminal_filters"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(work), str(TESTDIR / "terminal_filters.c"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

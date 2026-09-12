#!/usr/bin/env python3
"""Compare actual ICE MAC requests across GLD, attach, replay, and teardown."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from terminal_filters import DRIVER, TESTDIR, callback_fragments, extract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gld-source", type=Path, default=DRIVER / "ice_gld.c")
    parser.add_argument("--vsi-source", type=Path, default=DRIVER / "ice_vsi.c")
    parser.add_argument("--scenario", choices=("requests", "rebuild_invalid",
        "rebuild_number", "rebuild_context", "rebuild_scheduler", "attach_failures"))
    args = parser.parse_args()
    callbacks = callback_fragments(args.gld_source, args.vsi_source)
    source = args.vsi_source.read_text()
    fragments = [extract(source,
        r"^static const uint8_t ice_bcast_addr[\s\S]*?^};", args.vsi_source)]
    if re.search(r"^ice_gld_fltr_init\(", args.gld_source.read_text(),
                 re.MULTILINE):
        fragments.append(extract(source,
            r"^(?:static )?void\nice_fltr_entry_init\([\s\S]*?^}", args.vsi_source))
    for name in ("ice_mac_filter_track", "ice_vsi_teardown",
                 "ice_vsi_setup", "ice_add_mac_filters", "ice_vsi_init",
                 "ice_vsi_rebuild"):
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.vsi_source))
    with tempfile.TemporaryDirectory(prefix="ice-filter-requests-") as tmp:
        work = Path(tmp)
        (work / "ice_filter_callbacks.h").write_text("\n".join(callbacks))
        (work / "ice_vsi_filter_bodies.h").write_text("\n".join(fragments))
        binary = work / "filter_requests"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(work), str(TESTDIR / "filter_requests.c"),
            "-o", str(binary)], check=True)
        scenarios = [args.scenario] if args.scenario else ("requests",
            "rebuild_invalid", "rebuild_number", "rebuild_context",
            "rebuild_scheduler", "attach_failures")
        for scenario in scenarios:
            subprocess.run([str(binary), scenario], check=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Execute ICE reset dispatch and rebuilding with controlled interleavings."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


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
    run_c(TESTDIR / "reset_requests.c", {
        "ice_reset_types.h": types,
        "ice_reset_body.h": "\n".join(fragments),
    }, cflags=("-Wno-unused-function",))


if __name__ == "__main__":
    main()

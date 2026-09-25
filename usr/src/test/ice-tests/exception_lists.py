#!/usr/bin/env python3
"""Check that pbchk and nightly skip the vendored core and DDP package."""

import fnmatch
from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
LISTS = REPO / "exception_lists"
ICE = "usr/src/uts/common/io/ice"

# Each style or license check must skip every vendored file.  The glue must
# stay subject to the checks, so no pattern may match it.
REQUIRED = {
    "copyright": ("core", "firmware"),
    "cstyle": ("core",),
    "hdrchk": ("core",),
    "wscheck": ("core", "firmware/ice.pkg"),
    "keywords": ("firmware/ice.pkg",),
    "utf8check": ("firmware/ice.pkg",),
}


def patterns(name):
    lines = (LISTS / name).read_text().splitlines()
    return [line.split("#")[0].strip() for line in lines
            if line.strip() and not line.startswith("#")]


def excluded(pats, rel):
    return any(fnmatch.fnmatch(rel, p) or rel.startswith(p.rstrip("/") + "/")
               or rel == p for p in pats)


def main():
    root = REPO / ICE
    for name, areas in REQUIRED.items():
        pats = patterns(name)
        for area in areas:
            target = root / area
            files = [target] if target.is_file() else \
                [p for p in target.rglob("*") if p.is_file()]
            assert files, f"{area} has no files"
            for path in files:
                rel = str(path.relative_to(REPO))
                assert excluded(pats, rel), f"{name} does not skip {rel}"
        for path in root.glob("*.[ch]"):
            rel = str(path.relative_to(REPO))
            assert not excluded(pats, rel), f"{name} skips glue file {rel}"
    print("PASS: exception lists skip vendored ice code and keep the glue")


if __name__ == "__main__":
    main()

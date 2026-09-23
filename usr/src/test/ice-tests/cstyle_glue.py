#!/usr/bin/env python3
"""Run the illumos C style checker over the driver's own sources."""

from pathlib import Path
import shutil
import subprocess
import sys


REPO = Path(__file__).resolve().parents[4]
GLUE_DIR = REPO / "usr/src/uts/common/io/ice"
CSTYLE = REPO / "usr/src/tools/scripts/cstyle.pl"
MDB = REPO / "usr/src/cmd/mdb/common/modules/ice"


def main():
    perl = shutil.which("perl")
    if perl is None:
        print("SKIP: perl is not available")
        return 0
    # core/ is vendored Intel code and follows its own style.
    files = sorted(GLUE_DIR.glob("*.[ch]"))
    files += sorted(MDB.glob("*.[ch]"))
    assert files, "no glue sources found"
    result = subprocess.run([perl, str(CSTYLE), "-pP", *map(str, files)],
                            capture_output=True, text=True, timeout=120)
    if result.returncode != 0 or result.stdout.strip():
        print(result.stdout, end="")
        print(result.stderr, end="")
        return 1
    print(f"PASS: {len(files)} glue files are cstyle -pP clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

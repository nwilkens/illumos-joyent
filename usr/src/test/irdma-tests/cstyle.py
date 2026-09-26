#!/usr/bin/env python3
"""Run the illumos C style checker over the irdma driver and its tests."""

import shutil
import subprocess
import sys

from irdma_test import IRDMA, REPO, TESTDIR


def main():
    perl = shutil.which("perl")
    if perl is None:
        print("SKIP: perl is not available")
        return 0
    # core/ is the imported Linux code and keeps its own style.
    files = sorted(IRDMA.glob("*.[ch]")) + sorted(IRDMA.glob("linux/*.h"))
    files += sorted(TESTDIR.glob("*.[ch]"))
    files += sorted((REPO / "usr/src/uts/common/io/rdma").glob("*.[ch]"))
    files += [REPO / "usr/src/uts/common/io/ice/ice_rdma.h",
              REPO / "usr/src/uts/common/io/ice/ice_rdma_impl.h"]
    result = subprocess.run([perl, str(REPO / "usr/src/tools/scripts/cstyle.pl"),
                             "-pP", *map(str, files)],
                            capture_output=True, text=True, timeout=120)
    if result.returncode != 0 or result.stdout.strip():
        print(result.stdout, end="")
        print(result.stderr, end="")
        return 1
    print(f"PASS: {len(files)} files are cstyle -pP clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

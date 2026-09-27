#!/usr/bin/env python3
"""Run the portable irdma and ice RDMA peer checks; no NIC is needed."""

import argparse
from pathlib import Path
import subprocess
import sys

TESTS = (
    "core_provenance.py",
    "cqe_checks.py",
    "cqp_requests.py",
    "cstyle.py",
    "dma_quarantine.py",
    "fpm_checks.py",
    "ice_qsets.py",
    "license_notices.py",
    "lock_order.py",
    "rdk_locks.py",
    "rdk_teardown.py",
    "rdk_verbs.py",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("tests", nargs="*", metavar="TEST")
    args = parser.parse_args()
    if args.list:
        print("\n".join(TESTS))
        return 0
    for name in args.tests:
        if name not in TESTS:
            parser.error(f"unknown test {name!r}; use --list")
    here = Path(__file__).resolve().parent
    names = args.tests or TESTS
    passed = 0
    for name in names:
        result = subprocess.run([sys.executable, "-B", str(here / name)],
                                capture_output=True, text=True, timeout=120)
        if result.returncode == 0:
            passed += 1
            print(f"PASS {name}", flush=True)
        else:
            print(result.stdout, end="")
            print(result.stderr, end="")
            print(f"FAIL {name} (exit {result.returncode})", flush=True)
    print(f"{passed}/{len(names)} passed", flush=True)
    return 0 if passed == len(names) else 1


if __name__ == "__main__":
    sys.exit(main())

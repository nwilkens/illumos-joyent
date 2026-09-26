#!/usr/bin/env python3
"""Run the rdma connection manager and iWARP provider checks; no NIC or
illumos kernel is needed, only Python 3 and a C99 compiler."""

import argparse
from pathlib import Path
import subprocess
import sys


TESTS = (
    "acl_units.py",
    "cm_locks.py",
    "iwc_cqe.py",
    "iwc_intr.py",
    "mpa_parse.py",
)


def run_suite(testdir, names=TESTS, timeout=600):
    passed = 0
    for name in names:
        try:
            result = subprocess.run([sys.executable, "-B", str(testdir / name)],
                                    capture_output=True, text=True,
                                    timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f"TIMEOUT {name} ({timeout}s)", flush=True)
            continue
        if result.returncode == 0:
            passed += 1
            print(f"PASS {name}", flush=True)
        else:
            print(result.stdout, end="")
            print(result.stderr, end="")
            print(f"FAIL {name} (exit {result.returncode})", flush=True)
    print(f"{passed}/{len(names)} passed", flush=True)
    return 0 if passed == len(names) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="list the tests")
    parser.add_argument("tests", nargs="*", metavar="TEST",
                        help="selected script names (default: all)")
    args = parser.parse_args()
    for name in args.tests:
        if name not in TESTS:
            parser.error(f"unknown test {name!r}; use --list")
    if args.list:
        print("\n".join(TESTS))
        return 0
    return run_suite(Path(__file__).resolve().parent, args.tests or TESTS)


if __name__ == "__main__":
    sys.exit(main())

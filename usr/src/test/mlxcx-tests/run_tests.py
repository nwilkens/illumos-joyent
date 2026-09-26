#!/usr/bin/env python3
"""Run the portable mlxcx suite; no NIC or illumos kernel is needed."""

import argparse
from pathlib import Path
import subprocess
import sys


TESTS = (
    "cmdq_geometry.py",
    "cmdq_completion.py",
    "cmdq_abandon.py",
)


def run_suite(testdir, names, extra, timeout=300):
    passed = 0
    for name in names:
        try:
            result = subprocess.run([sys.executable, "-B",
                                     str(testdir / name), *extra],
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
    parser.add_argument("--list", action="store_true",
                        help="list runnable tests")
    parser.add_argument("--source-dir", type=Path,
                        help="mlxcx source directory to test")
    parser.add_argument("tests", nargs="*", metavar="TEST",
                        help="selected script names (default: all)")
    args = parser.parse_args()
    for name in args.tests:
        if name not in TESTS:
            parser.error(f"unknown test {name!r}; use --list")
    if args.list:
        print("\n".join(TESTS))
        return 0
    extra = ["--source-dir", str(args.source_dir)] if args.source_dir else []
    return run_suite(Path(__file__).resolve().parent, args.tests or TESTS,
                     extra)


if __name__ == "__main__":
    sys.exit(main())

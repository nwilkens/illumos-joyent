#!/usr/bin/env python3
"""Run the portable nvmf and nvmft checks; no NIC or illumos kernel is needed."""

import argparse
from pathlib import Path
import subprocess
import sys

TESTS = (
    "adopt_lifecycle.py",
    "connect_checks.py",
    "send_once.py",
    "sgl_decode.py",
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
        try:
            result = subprocess.run([sys.executable, "-B", str(here / name)],
                                    capture_output=True, text=True,
                                    timeout=300)
        except subprocess.TimeoutExpired:
            print(f"TIMEOUT {name}", flush=True)
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


if __name__ == "__main__":
    sys.exit(main())

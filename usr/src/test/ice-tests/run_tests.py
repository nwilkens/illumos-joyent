#!/usr/bin/env python3
"""Run the explicit portable ICE suite; no NIC or illumos kernel is needed."""

import argparse
from pathlib import Path
import subprocess
import sys


# Support modules and hardware acceptance programs are deliberately absent.
TESTS = (
    "runner_checks.py",
    "admin_interrupt.py",
    "detach_quiesce.py",
    "dma_lifetime.py",
    "filter_requests.py",
    "fma_dma.py",
    "hw_stats.py",
    "jumbo_copy.py",
    "jumbo_rx.py",
    "link_operational.py",
    "link_speed_caps.py",
    "link_state.py",
    "loan_wait.py",
    "loopback.py",
    "lso.py",
    "lso_context.py",
    "mac_filter.py",
    "pool_locks.py",
    "reset_loan_escalation.py",
    "reset_oicr.py",
    "reset_rebuild.py",
    "reset_requests.py",
    "reset_serialize.py",
    "rss.py",
    "rx_checksum.py",
    "rx_dma_faults.py",
    "rx_double_start.py",
    "rx_intr_limit.py",
    "rx_intr_rearm.py",
    "rx_layout.py",
    "safe_mode.py",
    "stale_comments.py",
    "terminal_filters.py",
    "tx_bind_threshold.py",
    "tx_blocked.py",
    "tx_doorbell.py",
    "tx_frame_limit.py",
    "tx_quiesce.py",
    "vlan_rx.py",
    "vsi_replay.py",
    "vsi_stats.py",
    "vsi_tx_vlan.py",
)


def run_suite(testdir, names=TESTS, timeout=60):
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
    parser.add_argument("--list", action="store_true", help="list runnable tests")
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

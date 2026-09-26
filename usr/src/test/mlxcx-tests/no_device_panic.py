#!/usr/bin/env python3
"""No assertion or panic in the command, EQ and page paths trusts the device.

This is a source check. In each function of those paths it marks as device
supplied every expression that reads a register, a command queue entry, a
command output, an event queue entry or a returned page address, and every
local variable assigned from one, and fails if any VERIFY, ASSERT or
mlxcx_panic() uses them. A variable that an if statement compares with a
bound counts as checked, and so do values derived from it. The check does
not follow values across calls; the runtime checks cover the call chains
that matter. Queue geometry is left out because attach checks it.

A *_DESTROYED state bit is set only when firmware accepts a DESTROY command,
and *_STARTED is cleared only when it accepts a stop, so both are
device-controlled too. No VERIFY or ASSERT anywhere in the driver may require
a DESTROYED bit, and no destroy function may assert STARTED is clear.
"""

import re
import sys

from c_test import source_parser


# Every function in mlxcx_cmd.c is on the command path.
PATHS = {
    "mlxcx_cmd.c": None,
    "mlxcx_intr.c": ("mlxcx_intr_async", "mlxcx_intr_n", "mlxcx_eq_next",
                     "mlxcx_arm_eq", "mlxcx_update_eq",
                     "mlxcx_give_pages_once", "mlxcx_take_pages_once",
                     "mlxcx_pages_task", "mlxcx_link_state_task",
                     "mlxcx_update_link_state"),
    "mlxcx_ring.c": None,
    "mlxcx.c": ("mlxcx_give_pages", "mlxcx_init_pages",
                "mlxcx_pages_returned", "mlxcx_teardown_pages",
                "mlxcx_teardown_flow_table", "mlxcx_uar_put32",
                "mlxcx_uar_put64", "mlxcx_regs_map", "mlxcx_teardown"),
}

SOURCES = re.compile(
    r"from_be(?:16|24|32|64)\(|get_bits(?:8|16|32|64)\(|"
    r"mlxcx_get(?:16|32|64)\(|\bmce_\w+|\bmleqe_\w+|\bmled_\w+|"
    r"\bmlxo_\w+|\bmco_\w+|\bmlcqe_\w+|\bmlp_npages\b|\bpas\[")

CHECKS = re.compile(r"\b(VERIFY\w*|ASSERT\w*|mlxcx_panic)\s*\(")
ASSIGN = re.compile(r"\b(\w+)(?:\[[^\]]*\])?\s*(?:[-+|&]?=)(?!=)([^;]*);")


def functions(text):
    for match in re.finditer(r"^(?:static )?[A-Za-z_][\w \t\*]*\n(\w+)\("
                             r"[\s\S]*?\n}\n", text, re.MULTILINE):
        yield match.group(1), match.start(), match.group()


def strip(body):
    body = re.sub(r"/\*[\s\S]*?\*/", " ", body)
    body = re.sub(r'"(?:\\.|[^"\\])*"', '""', body)
    return body


def call_args(body, start):
    depth, i = 0, start
    while i < len(body):
        if body[i] == "(":
            depth += 1
        elif body[i] == ")":
            depth -= 1
            if depth == 0:
                return body[start + 1:i]
        i += 1
    return body[start:]


def without_sizeof(text):
    out, i = [], 0
    for match in re.finditer(r"\bsizeof\s*\(", text):
        if match.start() < i:
            continue
        out.append(text[i:match.start()])
        i = match.end() - 1 + len(call_args(text, match.end() - 1)) + 2
    out.append(text[i:])
    return "".join(out)


def bounded_names(body):
    names = set()
    for cond in re.finditer(r"\bif\s*\(", body):
        text = call_args(body, cond.end() - 1)
        for match in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:>=|<=|>|<)(?![<>])",
                                 text):
            names.add(match.group(1))
    return names


def tainted_names(body):
    names = set()
    bounded = bounded_names(body)
    while True:
        before = len(names)
        for match in ASSIGN.finditer(body):
            lhs, rhs = match.group(1), without_sizeof(match.group(2))
            if lhs in bounded:
                continue
            if SOURCES.search(rhs) or any(
                    re.search(rf"\b{re.escape(n)}\b", rhs) for n in names):
                names.add(lhs)
        if len(names) == before:
            return names


def scan(path, names):
    text = path.read_text(encoding="utf-8")
    found = []
    seen = set()
    for name, start, body in functions(text):
        if names is not None and name not in names:
            continue
        seen.add(name)
        clean = strip(body)
        tainted = tainted_names(clean)
        for check in CHECKS.finditer(clean):
            args = without_sizeof(call_args(clean, check.end() - 1))
            line = text.count("\n", 0, start) + clean.count(
                "\n", 0, check.start()) + 1
            what = check.group(1)
            if what == "mlxcx_panic":
                found.append(f"{path.name}:{line}: {name}: mlxcx_panic()")
                continue
            hits = sorted(n for n in tainted
                          if re.search(rf"\b{re.escape(n)}\b", args))
            if SOURCES.search(args) or hits:
                found.append(f"{path.name}:{line}: {name}: {what}("
                             f"{' '.join(args.split())}) uses device data")
    return found, len(seen)


LIFECYCLE_FILES = ("mlxcx.c", "mlxcx_cmd.c", "mlxcx_intr.c", "mlxcx_ring.c",
                   "mlxcx_gld.c")


def scan_lifecycle(path):
    text = path.read_text(encoding="utf-8")
    found = []
    for name, start, body in functions(text):
        clean = strip(body)
        for check in re.finditer(r"\b(VERIFY0?|ASSERT0?)\s*\(", clean):
            args = call_args(clean, check.end() - 1)
            negated = check.group(1).endswith("0") or "!" in args
            started = (negated and "destroy" in name and
                       "_STARTED" in args)
            if started or ("_DESTROYED" in args and not negated):
                line = text.count("\n", 0, start) + clean.count(
                    "\n", 0, check.start()) + 1
                found.append(f"{path.name}:{line}: {name}: "
                             f"{check.group(1)}({' '.join(args.split())}) "
                             "needs firmware to accept a command")
    return found


def main():
    args = source_parser(__doc__).parse_args()
    found, scanned = [], 0
    for file, names in PATHS.items():
        hits, count = scan(args.source_dir / file, names)
        found += hits
        scanned += count
    for file in LIFECYCLE_FILES:
        found += scan_lifecycle(args.source_dir / file)
    if scanned < 100:
        sys.exit(f"FAIL: scanned only {scanned} functions")
    if found:
        print("\n".join(found))
        sys.exit(f"FAIL: {len(found)} device-reachable panics")
    print(f"ok no device-reachable panics in {scanned} functions")


if __name__ == "__main__":
    main()

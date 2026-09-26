#!/usr/bin/env python3
"""Check that the connection manager calls out without its locks.

Provider operations call back into rdk_iw_cm_event(), which takes the ID
lock, and consumer handlers may call any rdk_cm operation.  So no rdk_cm
function may reach a provider operation, a consumer handler, a socket call
or an ARP resolution while it holds an ID lock or one of the global CM
locks.
"""

import re
import sys

from cm_test import CXGBE_TESTS, RDMA

sys.path.insert(0, str(CXGBE_TESTS))
import intr_locks as il  # noqa: E402
from c_src import calls, functions, strip  # noqa: E402

FILES = ("rdk_cm.c", "rdk_cm_iw.c", "rdk_cm_addr.c")
MUTEX_INIT = re.compile(r"mutex_init\s*\(\s*&[^,]*?(\w+)\s*,")
OUTSIDE = re.compile(r"^(iw_\w+|handler|ksocket_\w+|ip2mac\w*|"
                     r"rdk_iw_cm_event|taskq_wait\w*)$")


def callers_of_outside(bodies):
    reach = {n for n, b in bodies.items() if any(OUTSIDE.match(c) for c in
                                                 calls(b))}
    changed = True
    while changed:
        changed = False
        for name, body in bodies.items():
            if name not in reach and calls(body) & reach:
                reach.add(name)
                changed = True
    return reach


def check(texts):
    locks = set()
    bodies = {}
    for text in texts.values():
        locks.update(MUTEX_INIT.findall(strip(text)))
        bodies.update(functions(text))
    reach = callers_of_outside(bodies)
    bad = []
    for name, text in texts.items():
        for func, body in sorted(functions(text).items()):
            for held, callee, _ in il.held_calls(body, locks):
                if held and (OUTSIDE.match(callee) or callee in reach):
                    bad.append(f"{name}: {func}() calls {callee}() holding "
                               f"{held}")
    return locks, bad


def main():
    texts = {f: (RDMA / f).read_text(encoding="utf-8") for f in FILES}
    locks, bad = check(texts)
    if bad:
        print("\n".join(bad))
        return 1
    anchor = ("\tid->rci_destroying = B_TRUE;\n\tmutex_exit(&id->rci_lock);\n"
              "\trdk_cm_destroy_common(id, B_FALSE);\n")
    if texts["rdk_cm.c"].count(anchor) != 1:
        print("mutation anchor not found")
        return 1
    mutated = dict(texts)
    mutated["rdk_cm.c"] = texts["rdk_cm.c"].replace(
        anchor, "\tid->rci_destroying = B_TRUE;\n"
        "\trdk_cm_iw_disconnect(id, B_TRUE);\n\tmutex_exit(&id->rci_lock);\n"
        "\trdk_cm_destroy_common(id, B_FALSE);\n")
    _, found = check(mutated)
    if not any("rdk_cm_iw_disconnect" in f for f in found):
        print(f"missed a provider call under the ID lock: {found}")
        return 1
    print(f"PASS: no call out of rdk_cm holding "
          f"{', '.join(sorted(locks))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

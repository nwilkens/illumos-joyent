#!/usr/bin/env python3
"""Check that rdmak calls out of its locks.

Consumer callbacks (done(), a teardown's function, event handlers and
releases) may call any verb, and provider operations may call back into
rdmak, so no function of the completion, teardown or RDMA READ/WRITE code
may reach one of them while it holds a lock it initializes, and no
teardown or free may wait under one.  A mutation that moves a done() call
under the poller lock must be caught.
"""

import re
import sys

from irdma_test import REPO

sys.path.insert(0, str(REPO / "usr/src/test/cxgbe-tests"))
import intr_locks as il  # noqa: E402
from c_src import calls, functions, strip  # noqa: E402

RDMA = REPO / "usr/src/uts/common/io/rdma"
FILES = ("rdk_quiesce.c", "rdk_cq.c", "rdk_rw.c")
MUTEX_INIT = re.compile(r"mutex_init\s*\(\s*&[^,]*?(\w+)\s*,")
CALLBACKS = {"done", "rtd_func", "handler", "release", "comp_handler",
             "event_handler", "resched", "modify", "func"}
WAITS = {"taskq_wait", "taskq_destroy", "untimeout_generic", "rdk_free_cq",
         "rdk_destroy_cq", "rdk_destroy_qp", "rdk_dereg_mr", "rdk_drain_qp",
         "rdk_teardown_wait", "rdk_poll_cq", "rdk_req_notify_cq",
         "rdk_post_send", "rdk_post_recv", "rdk_map_mr_sg"}


def provider_ops():
    hdr = (RDMA / "rdk.h").read_text(encoding="utf-8")
    ops = hdr[hdr.index("struct rdk_device_ops {"):]
    ops = ops[:ops.index("\n};\n")]
    return set(re.findall(r"\(\*(\w+)\)", ops))


def check(texts):
    outside = CALLBACKS | WAITS | provider_ops()
    locks, bodies = set(), {}
    for text in texts.values():
        locks.update(MUTEX_INIT.findall(strip(text)))
        bodies.update(functions(text))
    reach = {n for n, b in bodies.items() if calls(b) & outside}
    changed = True
    while changed:
        changed = False
        for name, text in bodies.items():
            if name not in reach and calls(text) & reach:
                reach.add(name)
                changed = True
    bad = []
    for name, text in texts.items():
        for func, text in sorted(functions(text).items()):
            for held, callee, _ in il.held_calls(text, locks):
                if held and (callee in outside or callee in reach):
                    bad.append(f"{name}: {func}() calls {callee}() holding "
                               f"{', '.join(held)}")
    return locks, bad


def main():
    texts = {f: (RDMA / f).read_text(encoding="utf-8") for f in FILES
             if (RDMA / f).exists()}
    locks, bad = check(texts)
    if bad:
        print("\n".join(bad))
        return 1
    anchor = ("\t\tfor (i = 0; i < n; i++) {\n"
              "\t\t\tif (wcs[i].wr_cqe != NULL)\n"
              "\t\t\t\twcs[i].wr_cqe->done(cq, &wcs[i]);\n")
    if texts["rdk_cq.c"].count(anchor) != 1:
        print("mutation anchor not found")
        return 1
    mutated = dict(texts)
    mutated["rdk_cq.c"] = texts["rdk_cq.c"].replace(
        anchor, "\t\tmutex_enter(&cp->rcp_lock);\n" + anchor.replace(
            "&wcs[i]);\n", "&wcs[i]);\n\t\t\tmutex_exit(&cp->rcp_lock);\n"))
    _, found = check(mutated)
    if not any("calls done() holding rcp_lock" in f for f in found):
        print(f"missed a done() call under rcp_lock: {found}")
        return 1
    print(f"PASS: {', '.join(sorted(texts))} call out without "
          f"{', '.join(sorted(locks))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

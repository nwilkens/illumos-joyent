#!/usr/bin/env python3
"""Run teardown from callbacks through the whole of rdk_quiesce.c,
rdk_cq.c and rdk_verbs.c on real threads, and check that a free from a
callback that waited for the pollers is caught."""

import sys

from rdk_host import HostFailure, run

FILES = ("rdk_quiesce.c", "rdk_cq.c", "rdk_verbs.c")


def static_checks():
    from irdma_test import REPO, body
    io = REPO / "usr/src/uts/common/io"
    cq = (io / "rdma/rdk_cq.c").read_text(encoding="utf-8")
    proc = body(cq, "rdk_cq_process")
    # The runner covers the whole call, not each batch.
    assert proc.count("cp->rcp_runner = curthread;") == 1
    assert proc.count("cp->rcp_runner = NULL;") == 1
    assert proc.index("for (;;)") < proc.index("cp->rcp_runner = NULL;")
    assert proc.index("cp->rcp_runner = curthread;") < proc.index("for (;;)")
    # Providers call handlers only through the marking upcalls.
    for path in ("irdma/irdma_cq.c", "irdma/irdma_qp.c", "iwcxgbe/iwc_cq.c",
                 "iwcxgbe/iwc_qp.c"):
        text = (io / path).read_text(encoding="utf-8")
        assert "->comp_handler(" not in text and \
            ".comp_handler(" not in text, path
        assert "->event_handler(" not in text and \
            ".event_handler(" not in text, path


def main():
    static_checks()
    try:
        print(run("rdk_teardown.c", FILES), end="")
    except HostFailure as error:
        print(error)
        return 1
    # rdk_free_cq() that waits in a callback must hang or fail the run.
    defer = ("\tif (rdk_in_callback()) {\n"
             "\t\ttaskq_dispatch_ent(rdk_td_taskq, rdk_free_cq_task, cq, 0,\n"
             "\t\t    &cp->rcp_free_ent);\n"
             "\t\treturn;\n\t}\n")
    mutated = {"rdk_cq.c": ((defer, ""),)}
    try:
        run("rdk_teardown.c", FILES, replace=mutated, args=("5",))
    except HostFailure as error:
        if "--verbose" in sys.argv:
            print(error)
        print("PASS: a free that waits in a callback is caught")
        return 0
    print("FAIL: a free that waits in a callback went unnoticed")
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Detach drains the async and page taskqs before it frees what they use,
and keeps packet buffers that hardware may use until TEARDOWN_HCA."""

import re
import sys

from c_test import CTestFailure, function, source_parser


def position(body, needle):
    index = body.find(needle)
    if index < 0:
        sys.exit(f"FAIL: mlxcx_teardown() has no {needle!r}")
    return index


def main():
    args = source_parser(__doc__).parse_args()
    try:
        body = function(args.source_dir / "mlxcx.c", "mlxcx_teardown")
    except CTestFailure as error:
        sys.exit(f"FAIL: {error}")
    stop = position(body, "mlxcx_intr_disable(mlxp)")
    drains = [position(body, f"taskq_destroy(mlxp->{tq})")
              for tq in ("mlx_pages_tq", "mlx_async_tq")]
    params = position(body, "mutex_destroy(&mlxp->mlx_npages_req[i]"
                      ".mla_mtx)")
    users = {name: position(body, f"{name}(mlxp)")
             for name in ("mlxcx_teardown_ports", "mlxcx_teardown_pages",
                          "mlxcx_teardown_eqs", "mlxcx_cmd_queue_fini")}
    failures = []
    if not all(stop < drain for drain in drains):
        failures.append("a taskq is drained before interrupts stop")
    if not all(drain < params for drain in drains):
        failures.append("page request mutexes die before the taskqs")
    for name, used in users.items():
        if not all(drain < used for drain in drains):
            failures.append(f"{name} runs before the taskqs are drained")
    hca = position(body, "mlxcx_cmd_teardown_hca(mlxp)")
    calls = [m.start() for m in re.finditer(r"mlxcx_teardown_bufs\(mlxp\)",
                                           body)]
    early = [c for c in calls if c < hca]
    if any("list_is_empty(&mlxp->mlx_quarantine_bufs)" not in
           body[max(0, c - 200):c] for c in early):
        failures.append("packet buffers are torn down before TEARDOWN_HCA "
                        "while hardware may still use some")
    if not any(c > hca for c in calls) or \
            "mlxcx_orphan_bufs(mlxp)" not in body[hca:]:
        failures.append("quarantined packet buffers are neither freed nor "
                        "orphaned after TEARDOWN_HCA")
    if failures:
        sys.exit("FAIL: " + "; ".join(failures))
    print("ok teardown order")


if __name__ == "__main__":
    main()

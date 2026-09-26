#!/usr/bin/env python3
"""Queue memory whose CREATE timed out or whose DESTROY failed survives
until TEARDOWN_HCA succeeds."""

import re

from c_test import (TESTDIR, CTestFailure, function, mlxcx_types,
                    optional_function, run_c, scenarios, source_parser,
                    typedef)


NAMES = ("failed-wq", "failed-cq", "failed-eq", "destroyed", "leak",
         "stuck-rq", "stuck-sq", "create-timeout")

CREATES = {
    "mlxcx_cmd_create_eq": "MLXCX_EQ_CREATE_UNSURE",
    "mlxcx_cmd_create_cq": "MLXCX_CQ_CREATE_UNSURE",
    "mlxcx_cmd_create_rq": "MLXCX_WQ_CREATE_UNSURE",
    "mlxcx_cmd_create_sq": "MLXCX_WQ_CREATE_UNSURE",
}


def create_check(src):
    """Each queue CREATE records a timeout, the only uncertain outcome."""
    for name, flag in CREATES.items():
        body = function(src / "mlxcx_cmd.c", name)
        if not re.search(r"mlcmd_status == MLXCX_CMD_R_TIMEOUT\)\s*{?\s*"
                         rf"[^;]*{flag}", body):
            raise SystemExit(f"FAIL: {name} does not record a timed out "
                             "create")


def main():
    args = source_parser(__doc__).parse_args()
    src = args.source_dir
    header = src / "mlxcx.h"
    types = "\n".join(typedef(header, name) for name in (
        "mlxcx_eventq_state_t", "mlxcx_completionq_state_t",
        "mlxcx_workq_state_t", "mlxcx_workq_type_t",
        "mlxcx_dma_quarantine_t"))
    helpers = "".join(optional_function(src / "mlxcx.c", name) for name in (
        "mlxcx_dma_quarantine", "mlxcx_dma_quarantine_free",
        "mlxcx_dma_quarantine_fini"))
    if not typedef(header, "mlxcx_dma_quarantine_t"):
        types += ("\ntypedef struct { list_node_t mdq_node; } "
                  "mlxcx_dma_quarantine_t;\n")
    unsure = "CREATE_UNSURE" in header.read_text(encoding="utf-8")
    body = "\n".join([
        "#define\tHAVE_QUARANTINE\t1\n" if helpers else "",
        "#define\tHAVE_UNSURE\t1\n" if unsure else "",
        helpers,
        function(src / "mlxcx_ring.c", "mlxcx_wq_rele_dma"),
        function(src / "mlxcx_ring.c", "mlxcx_cq_rele_dma"),
        function(src / "mlxcx_ring.c", "mlxcx_wq_teardown"),
        function(src / "mlxcx.c", "mlxcx_eq_rele_dma"),
    ])
    if args.scenario is None or "create-timeout" in args.scenario:
        create_check(src)
    try:
        run_c(TESTDIR / "quarantine.c", {
            "mlxcx_types.h": mlxcx_types(src),
            "mlxcx_queue_types.h": types,
            "mlxcx_quarantine_body.h": body,
        }, src, cases=scenarios(args, NAMES))
    except CTestFailure as error:
        raise SystemExit(f"FAIL quarantine.c: {error}") from None


if __name__ == "__main__":
    main()

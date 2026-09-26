#!/usr/bin/env python3
"""Queue memory survives a failed DESTROY until TEARDOWN_HCA succeeds."""

from c_test import (TESTDIR, CTestFailure, function, mlxcx_types,
                    optional_function, run_c, scenarios, source_parser,
                    typedef)


NAMES = ("failed-wq", "failed-cq", "failed-eq", "destroyed", "leak",
         "stuck-rq", "stuck-sq")


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
    body = "\n".join([
        "#define\tHAVE_QUARANTINE\t1\n" if helpers else "",
        helpers,
        function(src / "mlxcx_ring.c", "mlxcx_wq_rele_dma"),
        function(src / "mlxcx_ring.c", "mlxcx_cq_rele_dma"),
        function(src / "mlxcx_ring.c", "mlxcx_wq_teardown"),
        function(src / "mlxcx.c", "mlxcx_eq_rele_dma"),
    ])
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

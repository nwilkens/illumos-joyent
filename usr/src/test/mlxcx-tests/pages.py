"""Shared assembly of the mlxcx page code under the page command model."""

from c_test import (TESTDIR, CTestFailure, function, mlxcx_types,
                    optional_function, run_c, scenarios, source_parser,
                    typedef)


def page_source(srcdir):
    main = srcdir / "mlxcx.c"
    intr = srcdir / "mlxcx_intr.c"
    parts = [
        function(main, "mlxcx_page_compare"),
        optional_function(main, "mlxcx_pages_returned"),
        function(main, "mlxcx_teardown_pages"),
        function(main, "mlxcx_give_pages"),
        function(main, "mlxcx_init_pages"),
        function(intr, "mlxcx_give_pages_once"),
        function(intr, "mlxcx_take_pages_once"),
        function(intr, "mlxcx_pages_task"),
        function(intr, "mlxcx_intr_async"),
    ]
    return "\n".join(parts)


def run(doc, test, names):
    args = source_parser(doc).parse_args()
    headers = {
        "mlxcx_types.h": mlxcx_types(args.source_dir),
        "mlxcx_eq_types.h": typedef(args.source_dir / "mlxcx.h",
                                    "mlxcx_eventq_state_t"),
        "mlxcx_pages_body.h": page_source(args.source_dir),
    }
    try:
        run_c(TESTDIR / test, headers, args.source_dir,
              cases=scenarios(args, names))
    except CTestFailure as error:
        raise SystemExit(f"FAIL {test}: {error}") from None

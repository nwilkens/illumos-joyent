#!/usr/bin/env python3
"""A TX group whose ring setup failed part way tears down cleanly."""

from c_test import (TESTDIR, CTestFailure, define, function, mlxcx_types,
                    optional_function, run_c, scenarios, source_parser,
                    typedef)


NAMES = ("sq-fails", "sq-times-out")


def main():
    args = source_parser(__doc__).parse_args()
    src = args.source_dir
    header = src / "mlxcx.h"
    types = [typedef(header, name) for name in (
        "mlxcx_eventq_state_t", "mlxcx_completionq_state_t",
        "mlxcx_workq_state_t", "mlxcx_workq_type_t", "mlxcx_eventq_type_t",
        "mlxcx_tis_state_t", "mlxcx_group_type_t", "mlxcx_group_state_t",
        "mlxcx_dma_quarantine_t")]
    types += [define(header, name) for name in (
        "MLXCX_CQ_HWM_GAP", "MLXCX_CQ_LWM_GAP", "MLXCX_WQ_HWM_GAP",
        "MLXCX_WQ_LWM_GAP")]
    ring = src / "mlxcx_ring.c"
    body = [optional_function(src / "mlxcx.c", name) for name in (
        "mlxcx_dma_quarantine", "mlxcx_dma_quarantine_free",
        "mlxcx_dma_quarantine_fini")]
    body += [function(ring, name) for name in (
        "mlxcx_wq_rele_dma", "mlxcx_wq_teardown", "mlxcx_sq_setup",
        "mlxcx_teardown_tx_group", "mlxcx_tx_group_setup")]
    try:
        run_c(TESTDIR / "groups.c", {
            "mlxcx_types.h": mlxcx_types(src),
            "mlxcx_group_types.h": "\n".join(types),
            "mlxcx_groups_body.h": "\n".join(body),
        }, src, cases=scenarios(args, NAMES))
    except CTestFailure as error:
        raise SystemExit(f"FAIL groups.c: {error}") from None


if __name__ == "__main__":
    main()

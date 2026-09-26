#!/usr/bin/env python3
"""Packet buffers of a work queue that did not stop outlive TEARDOWN_HCA."""

import re

from c_test import (TESTDIR, CTestFailure, function, mlxcx_types,
                    optional_function, run_c, scenarios, source_parser,
                    typedef)


NAMES = ("rx-stuck", "rx-stuck-leak", "tx-stuck", "orphan-unload",
         "orphan-reattach", "long-name")


def main():
    args = source_parser(__doc__).parse_args()
    src = args.source_dir
    header = src / "mlxcx.h"
    ring = src / "mlxcx_ring.c"
    main_c = src / "mlxcx.c"
    types = "\n".join(typedef(header, name) for name in (
        "mlxcx_completionq_state_t", "mlxcx_workq_state_t",
        "mlxcx_workq_type_t", "mlxcx_dma_quarantine_t"))
    have = "mlxcx_cq_quarantine_bufs" in ring.read_text(encoding="utf-8")
    parts = ["#define\tHAVE_BUF_QUARANTINE\t1\n" if have else ""]
    orphans = re.search(r"^static volatile uint(?:_t|64_t) mlxcx_orphans;$",
                        main_c.read_text(encoding="utf-8"), re.M)
    parts.append(orphans.group() + "\n" if orphans else "")
    parts += [function(main_c, name) for name in (
        "mlxcx_bufs_cache_constr", "mlxcx_bufs_cache_destr",
        "mlxcx_mlbs_create")]
    parts.append(re.sub(r"^inline void", "static inline void",
                        function(ring, "mlxcx_bufshard_adjust_total"),
                        flags=re.M))
    parts += [optional_function(ring, name) for name in (
        "mlxcx_buf_free", "mlxcx_buf_unshard", "mlxcx_cq_quarantine_bufs")]
    parts += [function(ring, name) for name in (
        "mlxcx_buf_mp_return", "mlxcx_buf_create", "mlxcx_buf_create_foreign",
        "mlxcx_buf_take", "mlxcx_buf_take_foreign", "mlxcx_buf_loan",
        "mlxcx_buf_return_chain", "mlxcx_buf_return", "mlxcx_buf_destroy",
        "mlxcx_shard_draining", "mlxcx_wq_rele_dma", "mlxcx_wq_teardown")]
    parts.append(optional_function(ring, "mlxcx_buf_quarantine_free"))
    parts += [function(main_c, name) for name in (
        "mlxcx_mlbs_teardown", "mlxcx_teardown_bufs", "mlxcx_setup_bufs")]
    parts += [optional_function(main_c, name) for name in (
        "mlxcx_dma_quarantine", "mlxcx_dma_quarantine_free",
        "mlxcx_orphan_bufs", "_fini")]
    try:
        run_c(TESTDIR / "bufs.c", {
            "mlxcx_types.h": mlxcx_types(src),
            "mlxcx_bufs_types.h": types,
            "mlxcx_bufs_body.h": "\n".join(parts),
        }, src, cases=scenarios(args, NAMES))
    except CTestFailure as error:
        raise SystemExit(f"FAIL bufs.c: {error}") from None


if __name__ == "__main__":
    main()

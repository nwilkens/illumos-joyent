#!/usr/bin/env python3
"""Run the qset operations of the ice RDMA peer interface."""

from irdma_test import ICE, TESTDIR, function, run_c


def main():
    src = ICE / "ice_rdma_ops.c"
    parts = [function(src, name) for name in (
        "ice_rdma_qset_find", "ice_rdma_tc_valid", "ice_rdma_op_qset_add",
        "ice_rdma_op_qset_del")]
    run_c(TESTDIR / "ice_qsets.c", {"qset_bodies.h": "\n".join(parts)},
          cflags=("-Wno-unused-function",))


if __name__ == "__main__":
    main()

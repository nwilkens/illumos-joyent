#!/usr/bin/env python3
"""Check the capabilities rdmak reports: READ_WITH_INV only where the
provider takes it, no inline data where there is none, the READ sink SGE
limit, and completion vector locality."""

import sys

from irdma_test import IRDMA, REPO, body, function
from rdk_host import HostFailure, run

RDMA = REPO / "usr/src/uts/common/io/rdma"
IWC = REPO / "usr/src/uts/common/io/iwcxgbe"


def text(path):
    return path.read_text(encoding="utf-8")


def main():
    bodies = function(RDMA / "rdk_device.c", "rdk_vector_info")
    try:
        print(run("rdk_caps.c", (), extra={"caps_bodies.h": bodies}), end="")
    except HostFailure as error:
        print(error)
        return 1

    verbs = text(IRDMA / "irdma_verbs.c")
    query = body(verbs, "irdma_query_device")
    assert "RDK_KCAP_READ_WITH_INV" in query
    assert "a->max_sge_rd = (int)hw->uk_attrs.max_hw_read_sges;" in query
    assert "a->max_inline_data = hw->uk_attrs.max_hw_inline;" in query
    assert ".vector_info = irdma_vector_info," in verbs
    info = body(verbs, "irdma_vector_info")
    assert "if (vec >= irdma->irdma_nceqs)" in info
    assert "ic_vec->iv_intr_cpu" in info
    numa = body(text(IRDMA / "irdma_numa.c"), "irdma_numa_place")
    assert numa.index("DDI_SUCCESS)\n\t\t\t\tiv->iv_intr_cpu = cpu;") > 0
    post = body(text(IRDMA / "irdma_post.c"), "irdma_post_send")
    assert "wr->opcode == RDK_WR_RDMA_READ_WITH_INV" in post

    iwc = text(IWC / "iwc.c")
    query = body(iwc, "iwc_query_device")
    assert "RDK_KCAP_READ_WITH_INV" not in query
    assert "a->max_sge_rd = 1;" in query
    assert "a->max_inline_data = 0;" in query
    qp = text(IWC / "iwc_qp.c")
    assert "(wr->send_flags & RDK_SEND_INLINE) != 0) {" in \
        body(qp, "iwc_post_send")
    assert "if (wr->num_sge < 0 || wr->num_sge > 1)" in \
        body(qp, "iwc_build_read")
    assert "case RDK_WR_RDMA_READ_WITH_INV" not in qp
    print("PASS: capabilities of irdma and iwcxgbe")
    return 0


if __name__ == "__main__":
    sys.exit(main())

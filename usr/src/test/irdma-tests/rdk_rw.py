#!/usr/bin/env python3
"""Run rdk_rw.c on every transfer shape against a model of the device and
the peer, and check that the model catches a LOCAL_INV without its fence
and a registration wider than the transfer."""

import re
import sys

from irdma_test import REPO, body
from rdk_host import HostFailure, run

RDMA = REPO / "usr/src/uts/common/io/rdma"

FILES = ("rdk_quiesce.c", "rdk_cq.c", "rdk_verbs.c", "rdk_rw.c")
MUTATIONS = (
    ("an unfenced LOCAL_INV",
     "\t\t\tw->wr.send_flags = RDK_SEND_FENCE;\n", ""),
    ("a registration past the transfer",
     "\tctx->rw_ck[n - 1].dmac_size -= have - len;\n", ""),
    ("a key that is not rotated",
     "\t\trdk_update_fast_reg_key(mr, (uint8_t)rdk_inc_rkey(mr->rkey));\n",
     ""),
)


def rdmat_checks():
    """rdmat keeps a run's contexts and MRs while the QP may complete them."""
    run_c = (RDMA / "rdmat_run.c").read_text(encoding="utf-8")
    rw_c = (RDMA / "rdmat_rw.c").read_text(encoding="utf-8")
    teardown = body(run_c, "rdmat_teardown")
    assert teardown.index("rdk_destroy_qp(tq->tq_qp);") < \
        teardown.index("rdmat_rw_free(tq->tq_rw);")
    assert "tq->tq_bmr != NULL || tq->tq_rw != NULL" in body(run_c,
                                                           "rdmat_run")
    rwrun = body(rw_c, "rdmat_rw_run")
    assert re.search(r"ret2 = tq->tq_send_done < tq->tq_posted;[\s\S]*"
                     r"tq->tq_rw = rw;", rwrun)
    post = body(rw_c, "rdmat_rw_post")
    assert post.index("tq->tq_posted++;") < post.index("rdk_rw_post(")


def main():
    rdmat_checks()
    count = sys.argv[1] if len(sys.argv) > 1 else "20000"
    try:
        print(run("rdk_rw_chain.c", FILES, args=(count,), timeout=600),
              end="")
    except HostFailure as error:
        print(error)
        return 1
    for what, old, new in MUTATIONS:
        try:
            run("rdk_rw_chain.c", FILES, args=("3000",),
                replace={"rdk_rw.c": ((old, new),)})
        except HostFailure:
            print(f"PASS: the model catches {what}")
            continue
        print(f"FAIL: the model missed {what}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

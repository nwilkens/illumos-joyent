#!/usr/bin/env python3
"""Run the CQP request matching against stale and forged completions."""

import re

from irdma_test import IRDMA, TESTDIR, body, function, run_c


def main():
    ctl = IRDMA / "irdma_ctl.c"
    parts = [function(ctl, name) for name in (
        "irdma_req_scratch", "irdma_req_lookup", "irdma_req_free",
        "irdma_req_complete")]
    run_c(TESTDIR / "cqp_requests.c", {"req_bodies.h": "\n".join(parts)},
          cflags=("-Wno-unused-function",))

    text = ctl.read_text(encoding="utf-8")
    # The core's CCQ reader bounds the WQE index and ignores the entry's
    # CQP pointer; the completion path uses only the scratch value.
    core = (IRDMA / "core/ctrl.c").read_text(encoding="utf-8")
    cqe = body(core, "int irdma_sc_ccq_get_cqe_info")
    assert "cqp = ccq->dev->cqp;" in cqe
    assert cqe.index("if (wqe_idx >= cqp->sq_size)") < \
        cqe.index("cqp->scratch_array[wqe_idx]")
    assert "(struct irdma_sc_cqp *)(unsigned long)qp_ctx" not in cqe
    # A request lives in its slot until completion or abandonment.
    exec_ = body(text, "irdma_cqp_exec")
    assert "IRDMA_REQ_ABANDONED" in exec_
    assert re.search(r"mutex_exit\(&irdma->irdma_req_lock\);\n\s*"
                     r"irdma_ccq_poll\(irdma\);", exec_)


if __name__ == "__main__":
    main()

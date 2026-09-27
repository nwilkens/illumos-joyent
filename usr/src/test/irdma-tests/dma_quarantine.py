#!/usr/bin/env python3
"""Run the rdmak DMA quarantine contract with irdma, and check that
iwcxgbe taints on every destroy the adapter did not confirm."""

import re

from irdma_test import IRDMA, REPO, TESTDIR, body, function, run_c
from c_test import CTestFailure  # noqa: E402

RDMA = REPO / "usr/src/uts/common/io/rdma"
IWC = REPO / "usr/src/uts/common/io/iwcxgbe"


def iwc_checks():
    mem = (IWC / "iwc_mem.c").read_text(encoding="utf-8")
    qp = (IWC / "iwc_qp.c").read_text(encoding="utf-8")
    cq = (IWC / "iwc_cq.c").read_text(encoding="utf-8")
    main = (IWC / "iwc.c").read_text(encoding="utf-8")
    # Each path that keeps adapter state it could not release taints.
    for text, name in ((mem, "iwc_dereg_mr"), (mem, "iwc_alloc_mr"),
                       (qp, "iwc_destroy_qp"), (cq, "iwc_destroy_cq")):
        fn = body(text, name)
        assert "iwc_taint(iwc);" in fn, name
    for text in (qp, cq):
        for m in re.finditer(r"IWC_STAT\(iwc, is_quar\);", text):
            assert "iwc_taint(iwc);" in text[m.end():m.end() + 40]
    assert "leaking it\",\n\t\t    mr->mr_stag, ret);\n\t\tiwc_taint(iwc);" \
        in body(mem, "iwc_dereg_mr")
    fatal = main[main.index("case T4_RDMA_EV_FATAL:"):]
    assert fatal.index("iwc_taint(iwc);") < fatal.index("break;")
    free = body(mem, "iwc_dma_free")
    assert "!iwc->iwc_tainted" in free and "!rdk_device_tainted(rdev)" in free
    taint = body(main, "iwc_taint")
    assert "rdk_device_taint(&iwc->iwc_dev[i].d_rdk);" in taint


def main():
    dev = RDMA / "rdk_device.c"
    verbs = RDMA / "rdk_verbs.c"
    osdep = IRDMA / "irdma_osdep.c"
    impl = (IRDMA / "irdma_impl.h").read_text(encoding="utf-8")
    flagdefs = "\n".join(re.findall(r"^#define\tIRDMA_F_\w+\t+0x[0-9a-f]+",
                                     impl, re.M)) + "\n"
    parts = [function(dev, n) for n in ("rdk_device_taint",
                                        "rdk_device_tainted",
                                        "rdk_dma_release",
                                        "rdk_dma_buf_free")]
    parts.append(function(verbs, "rdk_dereg_mr"))
    parts.append(function(IRDMA / "irdma_verbs.c", "irdma_healthy"))
    parts += [function(osdep, n) for n in ("irdma_taint", "irdma_dma_free",
                                           "irdma_osdep_free_consumer")]
    flags = ("-Wno-unused-function", "-Wno-unused-parameter")
    run_c(TESTDIR / "dma_quarantine.c", {"quar_bodies.h": "\n".join(parts),
                                         "quar_flags.h": flagdefs},
          cflags=flags)

    # The run must fail if irdma stops passing its taint to rdmak.
    anchor = "\trdk_device_taint(&irdma->irdma_rdk);\n"
    mutated = [p.replace(anchor, "") for p in parts]
    assert mutated != parts
    try:
        run_c(TESTDIR / "dma_quarantine.c",
              {"quar_bodies.h": "\n".join(mutated),
               "quar_flags.h": flagdefs}, cflags=flags)
    except CTestFailure:
        pass
    else:
        raise AssertionError("a lost irdma taint went unnoticed")

    # irdma copies its taint into a freshly zeroed rdmak device.
    reg = body((IRDMA / "irdma_verbs.c").read_text(encoding="utf-8"),
               "irdma_verbs_register")
    assert reg.index("bzero(rdev") < reg.index("rdk_device_taint(rdev)") < \
        reg.index("rdk_register_device(rdev)")
    iwc_checks()
    print("PASS: DMA quarantine contract in rdmak, irdma and iwcxgbe")


if __name__ == "__main__":
    main()

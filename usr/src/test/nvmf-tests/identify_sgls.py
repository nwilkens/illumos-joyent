#!/usr/bin/env python3
"""Check the Identify SGLS field that nvmft advertises for each transport."""

from common_h import BASE
from nvmf_test import (NVMF, NVME_H, NVMFT, TESTDIR, define, function, run_c,
                       typedef)


def main():
    public = NVMF.parents[1] / "sys/nvme/nvmf_transport.h"
    subr = NVMFT / "nvmft_subr.c"
    parts = [BASE, "#include <stdio.h>\n#include <assert.h>\n"
             "struct nvmf_qpair;\n"
             "#define\tbcopy(s, d, n)\tmemcpy((d), (s), (n))\n"
             "#define\tbzero(p, n)\tmemset((p), 0, (n))\n"]
    for name in ("NVME_CNTRLTYPE_IO", "NVME_SGL_SUP_UNALIGN", "NVME_SERIAL_SZ",
                 "NVME_MODEL_SZ", "NVME_FWVER_SZ"):
        parts.append(define(NVME_H, name))
    parts.append(define(NVMFT / "nvmft_var.h", "NVMFT_VER_1_4"))
    text = public.read_text(encoding="utf-8")
    for line in text.splitlines():
        if line.startswith("#define\tNVMF_QP_CAP_"):
            parts.append(line + "\n")
    parts.append("#pragma pack(1)\n")
    for name in ("nvme_uint128_t", "nvme_idctl_qes_t", "nvme_idctl_psd_t",
                 "nvme_identify_ctrl_t"):
        parts.append(typedef(NVME_H, name))
    parts.append("#pragma pack()\n")
    for name in ("nvmf_strpad", "_nvmf_init_io_controller_data",
                 "nvmft_init_sgls"):
        parts.append(function(subr, name))
    parts.append(function(NVMF / "nvmf_tcp.c", "tcp_caps"))
    run_c(TESTDIR / "identify_sgls.c", {"sgls.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

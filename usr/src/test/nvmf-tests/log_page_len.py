#!/usr/bin/env python3
"""Check the Get Log Page length that nvmft takes from a host's NUMD."""

from common_h import BASE
from nvmf_test import NVME_H, NVMFT, TESTDIR, define, function, run_c


def main():
    parts = [BASE, "#include <stdio.h>\n#include <stdlib.h>\n"]
    for name in ("NVME_CQE_SC_GEN_SUCCESS", "NVME_CQE_SC_GEN_INV_FLD",
                 "NVME_CQE_SC_GEN_INV_DSGL_LEN"):
        parts.append(define(NVME_H, name))
    parts.append(define(NVMFT / "nvmft_var.h", "NVMFT_MAX_LOGPAGE_LEN"))
    parts.append(function(NVMFT / "nvmft_controller.c",
                          "nvmft_log_page_len"))
    run_c(TESTDIR / "log_page_len.c", {"logpage.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

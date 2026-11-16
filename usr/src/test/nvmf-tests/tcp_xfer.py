#!/usr/bin/env python3
"""Run the whole TCP transport against socketpairs, with pthreads as workers."""

import re

from common_h import KSHIM
from nvmf_test import (NVMF, NVMF_H, NVME_H, NVME_REG_H, TESTDIR, UTS, define,
                       function, run_c, struct, typedef)


def body(path):
    """A whole source file without its #include lines."""
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"^#include[^\n]*\n", "", text, flags=re.MULTILINE)
    return f'#line 1 "{path}"\n' + text


def main():
    core = NVMF / "nvmf_transport.c"
    crc = UTS / "sys/crc32.h"
    parts = [KSHIM, (TESTDIR / "tcp_xfer_shim.h").read_text(encoding="utf-8")]
    for name in ("NVME_CQE_SC_GEN_SUCCESS", "NVME_CQE_SC_GEN_INV_FLD",
                 "NVME_CQE_SC_GEN_INV_DSGL_LEN", "NVME_CQE_SC_GEN_INV_SGL_DESC",
                 "NVME_CQE_SC_GEN_INV_SGL_OFF", "NVME_CQE_SC_GEN_INTERNAL_ERR",
                 "NVME_CQE_SC_GEN_TRANSIENT"):
        parts.append(define(NVME_H, name))
    parts.append(typedef(NVME_H, "nvme_cqe_sf_t"))
    for name in ("nvme_sgl_t", "nvme_sqe_t", "nvme_cqe_t"):
        parts.append(typedef(NVME_REG_H, name))
    for name in ("CRC32_INIT", "CRC32"):
        parts.append(define(crc, name))
    parts.append(body(NVMF_H))
    parts.append(body(UTS / "sys/nvme/nvmf_transport.h"))
    parts.append(body(UTS / "sys/nvme/nvmf_tcp.h"))
    parts.append(body(NVMF / "nvmf_transport_internal.h"))
    parts.append(struct(core, "nvmf_transport"))
    for name in ("nvmf_transport_rele", "nvmf_allocate_command",
                 "nvmf_allocate_response", "nvmf_free_capsule",
                 "nvmf_capsule_append_data", "nvmf_receive_controller_data",
                 "nvmf_abort_capsule_data", "nvmf_cancel_controller_data",
                 "nvmf_validate_command_capsule",
                 "nvmf_memdesc_copy", "nvmf_memdesc_copyin",
                 "nvmf_memdesc_copyout"):
        parts.append(function(core, name))
    parts.append(body(NVMF / "nvmf_tcp.c"))
    run_c(TESTDIR / "tcp_xfer.c", {"tcp_xfer.h": "\n".join(parts)},
          timeout=240)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Check what nvmft requires of the daemon: subsystem parameters, and the
handoff of a queue whose Connect the daemon accepted."""

from common_h import BASE
from nvmf_test import (NVMF, NVMF_H, NVME_H, NVME_REG_H, NVMFT, TESTDIR,
                       define, function, run_c, typedef)


def main():
    subr = NVMFT / "nvmft_subr.c"
    internal = NVMF / "nvmf_transport_internal.h"
    var_h = NVMFT / "nvmft_var.h"
    parts = [BASE, "#include <stdio.h>\n#include <stdlib.h>\n"]
    for name in ("NVMF_NQN_FIELD_SIZE", "NVMF_NQN_MAX_LEN",
                 "NVMF_CNTLID_DYNAMIC", "NVMF_CNTLID_STATIC_ANY",
                 "NVMF_CNTLID_STATIC_MAX"):
        parts.append(define(NVMF_H, name))
    for name in ("NVME_CQE_SCT_GENERIC", "NVME_CQE_SCT_SPECIFIC",
                 "NVME_CQE_SC_GEN_INV_OPC"):
        parts.append(define(NVME_H, name))
    for name in ("NVME_MIN_ADMIN_ENTRIES", "NVME_MAX_ADMIN_ENTRIES",
                 "NVME_MIN_IO_ENTRIES", "NVME_MAX_IO_ENTRIES"):
        parts.append(define(internal, name))
    for name in ("NVMFT_OPC_FABRICS", "NVMFT_MAX_LUN", "NVMFT_MAX_NSID"):
        parts.append(define(var_h, name))
    parts.append(typedef(NVMF_H, "nvmf_trtype_t"))
    parts.append(typedef(NVME_H, "nvme_cqe_sf_t"))
    for name in ("nvme_sgl_t", "nvme_sqe_t", "nvme_cqe_t"):
        parts.append(typedef(NVME_REG_H, name))
    parts.append("#pragma pack(1)\n")
    for name in ("nvmf_sgl_descriptor_t", "nvmf_fabric_cmd_type_t",
                 "nvmf_fabric_cmd_status_code_t",
                 "nvmf_fabric_connect_data_t", "nvmf_fabric_connect_cmd_t"):
        parts.append(typedef(NVMF_H, name))
    parts.append("#pragma pack()\n")
    parts.append(typedef(var_h, "nvmft_connect_status_t"))
    for name in ("nvmf_nqn_valid", "nvmft_connect_invalid",
                 "nvmft_connect_fail", "nvmft_connect_cmd_valid",
                 "nvmft_connect_data_valid"):
        parts.append(function(subr, name))
    for name in ("nvmft_port_params_valid", "nvmft_handoff_valid"):
        parts.append(function(NVMFT / "nvmft.c", name))
    run_c(TESTDIR / "local_checks.c", {"local.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

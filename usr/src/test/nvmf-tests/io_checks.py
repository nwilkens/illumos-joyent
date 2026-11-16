#!/usr/bin/env python3
"""Check what nvmft takes from a host's I/O command, and the NVMe status it
gives for an LU's SCSI status."""

from common_h import BASE
from nvmf_test import NVME_H, NVME_REG_H, NVMFT, TESTDIR, UTS, define, \
    function, run_c


def main():
    io = NVMFT / "nvmft_io.c"
    status_h = UTS / "sys/scsi/generic/status.h"
    sense_h = UTS / "sys/scsi/generic/sense.h"
    parts = [BASE, "#include <stdio.h>\n#include <stdlib.h>\n"]
    for name in ("NVME_CQE_SCT_GENERIC", "NVME_CQE_SCT_INTEGRITY",
                 "NVME_CQE_SC_GEN_SUCCESS", "NVME_CQE_SC_GEN_INV_OPC",
                 "NVME_CQE_SC_GEN_INV_FLD", "NVME_CQE_SC_GEN_INTERNAL_ERR",
                 "NVME_CQE_SC_GEN_ABORT_REQUEST", "NVME_CQE_SC_GEN_INV_NS",
                 "NVME_CQE_SC_GEN_INV_DSGL_LEN", "NVME_CQE_SC_GEN_NS_RDONLY",
                 "NVME_CQE_SC_GEN_CMD_INTR", "NVME_CQE_SC_GEN_NVM_LBA_RANGE",
                 "NVME_CQE_SC_GEN_NVM_NS_NOTRDY",
                 "NVME_CQE_SC_GEN_NVM_RSV_CNFLCT", "NVME_CQE_SC_INT_NVM_WRITE",
                 "NVME_CQE_SC_INT_NVM_READ", "NVME_CQE_SC_INT_NVM_COMPARE"):
        parts.append(define(NVME_H, name))
    for name in ("NVME_OPC_NVM_FLUSH", "NVME_OPC_NVM_WRITE",
                 "NVME_OPC_NVM_READ", "NVME_OPC_NVM_COMPARE",
                 "NVME_OPC_NVM_WRITE_ZERO", "NVME_OPC_NVM_DSET_MGMT"):
        parts.append(define(NVME_REG_H, name))
    for name in ("STATUS_GOOD", "STATUS_CHECK", "STATUS_BUSY",
                 "STATUS_RESERVATION_CONFLICT", "STATUS_QFULL",
                 "STATUS_ACA_ACTIVE", "STATUS_TASK_ABORT"):
        parts.append(define(status_h, name))
    for name in ("KEY_NO_SENSE", "KEY_RECOVERABLE_ERROR", "KEY_NOT_READY",
                 "KEY_MEDIUM_ERROR", "KEY_HARDWARE_ERROR",
                 "KEY_ILLEGAL_REQUEST", "KEY_UNIT_ATTENTION",
                 "KEY_WRITE_PROTECT", "KEY_ABORTED_COMMAND", "KEY_MISCOMPARE"):
        parts.append(define(sense_h, name))
    parts.append(define(NVMFT / "nvmft_var.h", "NVMFT_MAX_LUN"))
    parts.append(function(io, "nvmft_check_io_command"))
    parts.append(function(io, "nvmft_scsi_to_nvme"))
    run_c(TESTDIR / "io_checks.c", {"io.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

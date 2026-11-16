#!/usr/bin/env python3
"""Run the nvmf_host checks on target and handoff supplied values."""

from common_h import BASE
from nvmf_test import NVME_H, NVME_REG_H, TESTDIR, UTS, define, function, \
    run_c, typedef

HOST = UTS / "io/nvmf_host"


def main():
    parts = [BASE, """
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	P2ALIGN(x, align)	((x) & -(align))
#define	CE_WARN	1
typedef struct dev_info dev_info_t;
#define	dev_err(...)	((void)0)
"""]
    for name in ("NVME_IDENTIFY_BUFSIZE", "NVME_MAX_LBAF", "NVME_NSID_BCAST"):
        parts.append(define(NVME_H, name))
    for name in ("nvme_uint128_t", "nvme_idns_lbaf_t", "nvme_identify_nsid_t",
                 "nvme_identify_nsid_list_t"):
        parts.append(typedef(NVME_H, name))
    parts.append(typedef(NVME_REG_H, "nvme_reg_cap_t"))
    for name in ("nvmf_sc_state_t", "nvmf_io_action_t"):
        parts.append(typedef(HOST / "nvmf_var.h", name))

    for name in ("NVMF_LBADS_MIN", "NVMF_LBADS_MAX"):
        parts.append(define(HOST / "nvmf_ns.c", name))
    parts.append(function(HOST / "nvmf_ns.c", "nvmf_ns_fmt"))

    parts.append(define(HOST / "nvmf_scan.c", "NVMF_NSLIST_ENTRIES"))
    parts.append(function(HOST / "nvmf_scan.c", "nvmf_nslist_count"))

    parts.append(define(HOST / "nvmf_conn.c", "NVME_MPS_SHIFT"))
    for name in ("nvmf_io_policy", "nvmf_max_xfer"):
        parts.append(function(HOST / "nvmf_conn.c", name))

    for name in ("NVMF_KATO_MIN", "NVMF_KATO_MAX", "NVMF_RECONNECT_DELAY_MAX",
                 "NVMF_LOSS_TIMEOUT_MAX"):
        parts.append(define(HOST / "nvmf_ioctl.c", name))
    parts.append(function(HOST / "nvmf_ioctl.c", "nvmf_timeouts_valid"))

    run_c(TESTDIR / "host_checks.c", {"host.h": "\n".join(parts)})

    # A capsule the transport refused has its data completed by the host,
    # while the request is still held against a queue pair shutdown.
    dispatch = function(HOST / "nvmf_qpair.c", "nvmf_dispatch_command")
    sent = dispatch.index("nvmf_transmit_capsule(nc)")
    done = dispatch.find("nvmf_abort_capsule_data(nc, error)", sent)
    if done == -1 or done > dispatch.index("qp->allocating--", sent):
        raise SystemExit("FAIL: a failed transmit leaves the data "
                         "completion of the request pending")


if __name__ == "__main__":
    main()

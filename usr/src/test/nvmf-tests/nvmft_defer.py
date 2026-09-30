#!/usr/bin/env python3
"""nvmft marks the commands it answers after freeing their capsules: the
Connect, and an Asynchronous Event Request it accepted.  An RDMA transport
keeps only those for a late response."""

import re
import sys

from nvmf_test import NVMFT, function


def main():
    fails = 0
    finish = function(NVMFT / "nvmft_qpair.c", "nvmft_connect_finish")
    m = re.search(r"nvmf_capsule_defer_response\(qp->qp_connect_nc\);\s*"
                  r"nvmf_free_capsule\(qp->qp_connect_nc\);", finish)
    if m is None:
        print("FAIL: the Connect is not deferred before its capsule is freed")
        fails += 1
    admin = function(NVMFT / "nvmft_controller.c", "nvmft_handle_admin_command")
    aer = admin[admin.index("case NVME_OPC_ASYNC_EVENT:"):]
    aer = aer[:aer.index("break;")]
    accept, _, after = aer.partition("} else {")
    if ("nvmf_capsule_defer_response" in accept or
            "nvmf_capsule_defer_response(nc);" not in after.split("}")[0]):
        print("FAIL: only an accepted AER is deferred")
        fails += 1
    if fails == 0:
        print("nvmft defers the Connect and accepted AERs")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

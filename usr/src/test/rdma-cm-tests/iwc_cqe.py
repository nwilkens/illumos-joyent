#!/usr/bin/env python3
"""Check that iwcxgbe takes a CQE's origin from the ring it read it from.

The SWCQE and DRAIN bits are the driver's own, and a drain CQE carries a
work request cookie the consumer's completion handler is called through.
A hardware CQE with either bit set must be refused before its fields are
used, and nothing may decide from the SWCQE bit whether a CQE came from
hardware.
"""

import re
import sys

from cm_test import CXGBE_TESTS, IWC

sys.path.insert(0, str(CXGBE_TESTS))
from c_src import functions  # noqa: E402

# Where the bits may be read, and why.
ALLOWED = {
    "iwc_hw_cqe_ok": "refuses them on a hardware CQE",
    "iwc_read_req_cqe": "copies the origin into a local CQE",
    "iwc_cqe_completes_wr": "reads the software queue only",
    "iwc_poll_one_qp": "reads DRAIN after the origin check",
}


def check(text):
    bodies = functions(text)
    bad = []
    for name, body in bodies.items():
        if re.search(r"\bCQE_(SWCQE|DRAIN)\(", body) and name not in ALLOWED:
            bad.append(f"{name}() reads a driver CQE bit")
    poll = bodies.get("iwc_poll_one_qp", "")
    if "CQE_SWCQE(" in poll:
        bad.append("iwc_poll_one_qp() takes the origin from the CQE")
    one = bodies.get("iwc_poll_one", "")
    if not re.search(r"!sw && !iwc_hw_cqe_ok\(hw\)", one):
        bad.append("iwc_poll_one() does not refuse driver bits from "
                   "hardware")
    flush = bodies.get("iwc_flush_hw_cq", "")
    if "iwc_hw_cqe_ok(hw)" not in flush:
        bad.append("iwc_flush_hw_cq() does not refuse driver bits from "
                   "hardware")
    return bad


def main():
    text = (IWC / "iwc_cq.c").read_text(encoding="utf-8")
    bad = check(text)
    if bad:
        print("\n".join(bad))
        return 1
    mutated = re.sub(r"\(!sw && !iwc_hw_cqe_ok\(hw\)\) \|\|\n\s*", "",
                     text, count=1)
    if not check(mutated):
        print("missed the removal of the hardware CQE check")
        return 1
    print("PASS: CQE origin comes from the ring, and hardware CQEs with "
          "driver bits are refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Execute the queue pair sizing and check its callers and ceiling."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_hw.c")
    args = parser.parse_args()
    source = args.source.read_text()
    header = (DRIVER / "ice.h").read_text()
    fragments = [extract(header, rf"^#define\t{name}\t.*$", DRIVER / "ice.h")
                 for name in ("ICE_MAX_QUEUES", "ICE_DEF_QUEUES")]
    for name in ("ice_queue_limit", "ice_prop_get_num_queues"):
        fragments.append(extract(source,
            rf"^static uint32_t\n{name}\([\s\S]*?^}}", args.source))
    run_c(TESTDIR / "queue_count.c", {"ice_queue_body.h": "\n".join(fragments)})

    # The old fixed cap and power-of-two rounding are gone.
    for code in (header, source, (DRIVER / "ice_vsi.c").read_text()):
        assert "ICE_MAX_INTR_QUEUES" not in code
    assert "ice_ilog2" not in source
    # The vector grant can still lower the count, never raise it; the RDMA
    # block is not a queue vector.
    assert re.search(r"ice->ice_nqueues = \(uint16_t\)MIN\(nreq,\s*"
                     r"\(uint32_t\)actual - 1 - ice->ice_intr_rdma\);",
                     source)


if __name__ == "__main__":
    main()

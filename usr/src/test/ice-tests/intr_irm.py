#!/usr/bin/env python3
"""Execute MSI-X sizing against the APIX limit for drivers without IRM."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_hw.c")
    args = parser.parse_args()
    source = args.source.read_text()
    names = ("ice_prop_get_num_queues", "ice_queue_limit", "ice_intr_cb",
             "ice_intr_cb_fini", "ice_free_intrs", "ice_alloc_intrs")
    bodies = [extract(source, rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}",
                      args.source) for name in names]
    run_c(TESTDIR / "intr_irm.c", {"ice_intr_irm.h": "\n".join(bodies)},
          cflags=("-Wno-unused-parameter",))

    # The callback exists before the first vector count is read.
    alloc = source[source.index("\nice_alloc_intrs(ice_t *ice)\n{"):]
    alloc = alloc[:alloc.index("\n}\n")]
    assert alloc.index("ddi_cb_register(") < \
        alloc.index("ddi_intr_get_navail(") < alloc.index("ddi_intr_alloc(")


if __name__ == "__main__":
    main()

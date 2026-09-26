#!/usr/bin/env python3
"""Execute MSI-X sizing and interrupt resource management reclaims and offers."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def bodies(path, names):
    source = path.read_text()
    return [extract(source, rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}",
                    path) for name in names]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_hw.c")
    args = parser.parse_args()
    source = args.source.read_text()
    header = DRIVER / "ice.h"
    types = [extract(header.read_text(), pattern, header) for pattern in (
        r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;",
        r"^typedef enum ice_attach_state \{[\s\S]*?^} ice_attach_state_t;",
        r"^#define\tICE_RDMA_FIRST_VECTOR\t[\s\S]*?"
        r"\(uint_t\)\(v\) < ICE_INTR_LAN_FIRST\(ice\)\)$")]
    parts = [extract(source, rf"^#define\tICE_RDMA_{n}_VECTORS\t.*$",
                     args.source) for n in ("DEF", "MIN", "MAX")]
    parts += bodies(args.source, ("ice_rdma_vectors",
                                  "ice_prop_get_num_queues", "ice_queue_limit",
                                 "ice_intr_cb", "ice_intr_cb_fini",
                                 "ice_free_intrs", "ice_alloc_intrs",
                                 "ice_rem_intr_handlers",
                                 "ice_add_intr_handlers"))
    parts += bodies(DRIVER / "ice_intr.c", ("ice_ring_vector",
                                            "ice_rx_intr_limit",
                                            "ice_intr_rings_map",
                                            "ice_intr_queue",
                                            "ice_intr_enable",
                                            "ice_intr_disable"))
    parts += bodies(DRIVER / "ice.c", ("ice_intr_adjust_locked",
                                       "ice_intr_adjust"))
    run_c(TESTDIR / "intr_irm.c", {"ice_intr_irm_types.h": "\n".join(types),
                                   "ice_intr_irm.h": "\n".join(parts)},
          cflags=("-Wno-unused-parameter",))

    # The callback exists before the first vector count is read.
    alloc = source[source.index("\nice_alloc_intrs(ice_t *ice)\n{"):]
    alloc = alloc[:alloc.index("\n}\n")]
    assert alloc.index("ddi_cb_register(") < \
        alloc.index("ddi_intr_get_navail(") < alloc.index("ddi_intr_alloc(")

    # The lifecycle lock outlives the callback registration.
    ice = (DRIVER / "ice.c").read_text()
    attach = ice[ice.index("\nice_attach(dev_info_t *dip"):]
    assert attach.index("mutex_init(&ice->ice_rebuild_lock") < \
        attach.index("ice_alloc_intrs(ice)")
    unconf = ice[ice.index("\nice_unconfigure(ice_t *ice)\n{"):]
    unconf = unconf[:unconf.index("\n}\n")]
    assert unconf.index("ice_free_intrs(ice)") < \
        unconf.index("mutex_destroy(&ice->ice_rebuild_lock)")
    detach = ice[ice.index("\nice_detach(dev_info_t *dip"):]
    assert detach.index("ice->ice_irm_busy") < \
        detach.index("ice->ice_detaching = B_TRUE")


if __name__ == "__main__":
    main()

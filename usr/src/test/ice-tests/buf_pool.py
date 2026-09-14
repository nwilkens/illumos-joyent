#!/usr/bin/env python3
"""Execute TX pool lifetime and allocation-failure paths with DDI boundaries."""

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    header = DRIVER / "ice.h"
    source = DRIVER / "ice_dma.c"
    types = header.read_text()
    dma = source.read_text()
    fragments = [extract(types,
                         r"^typedef struct ice_dma_buffer \{[\s\S]*?^} ice_dma_buffer_t;",
                         header)]
    fragments.append(extract(types,
                             r"^typedef struct ice_buf_pool \{[\s\S]*?^} ice_buf_pool_t;",
                             header))
    members = types.split("/* Shared TX copy-buffer pools. */", 1)[1].split(
        "/* DDP firmware. */", 1)[0]
    fragments.append("typedef struct ice {\n"
                     "unsigned ice_num_txr;\n"
                     "struct { unsigned itxr_size; } *ice_txr;\n"
                     "boolean_t ice_tx_lso_enable;\n" + members + "} ice_t;\n")
    names = ("ice_buf_pool_fini", "ice_buf_pool_init", "ice_buf_pool_alloc",
             "ice_buf_alloc", "ice_lso_buf_alloc", "ice_small_buf_alloc",
             "ice_buf_free", "ice_buf_init", "ice_buf_fini")
    bodies = [extract(dma, rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", source)
              for name in names]
    run_c(TESTDIR / "buf_pool.c", {"ice_pool_types.h": "\n".join(fragments),
                                  "ice_pool_code.h": "\n".join(bodies)})


if __name__ == "__main__":
    main()

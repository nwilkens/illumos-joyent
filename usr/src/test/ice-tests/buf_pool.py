#!/usr/bin/env python3
"""Execute TX pool sizing, lifetime and the deferred LSO allocation."""

import re

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    header = DRIVER / "ice.h"
    source = DRIVER / "ice_dma.c"
    types = header.read_text()
    dma = source.read_text()
    fragments = [extract(types,
                         r"^typedef struct ice_dma_buffer \{[\s\S]*?^} ice_dma_buffer_t;",
                         header),
                 extract(types,
                         r"^typedef struct ice_buf_pool \{[\s\S]*?^} ice_buf_pool_t;",
                         header),
                 extract(types,
                         r"^typedef enum ice_tx_lso_state \{[\s\S]*?^} ice_tx_lso_state_t;",
                         header)]
    fragments += [extract(types, rf"^#define\t{name}\t.*$", header)
                  for name in re.findall(r"^#define\t(ICE_TX_\w+_BUFS_\w+)\t",
                                         types, re.MULTILINE)]
    names = ("ice_buf_pool_fini", "ice_buf_pool_init", "ice_buf_pool_alloc",
             "ice_buf_alloc", "ice_lso_buf_alloc", "ice_small_buf_alloc",
             "ice_buf_free", "ice_tx_pool_bufs", "ice_buf_init",
             "ice_tx_lso_fini", "ice_tx_lso_task", "ice_buf_fini")
    bodies = [extract(dma, rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", source)
              for name in names]
    tx_source = DRIVER / "ice_tx.c"
    tx = tx_source.read_text()
    bodies.append(extract(tx,
        r"^typedef enum ice_tx_build \{[\s\S]*?^} ice_tx_build_t;", tx_source))
    bodies.append(extract(tx,
        r"^static ice_tx_build_t\nice_tx_lso_resources\([\s\S]*?^}",
        tx_source))
    run_c(TESTDIR / "buf_pool.c", {"ice_pool_types.h": "\n".join(fragments),
                                  "ice_pool_code.h": "\n".join(bodies)})

    # Pool sizes come from the ring count and caps, never the descriptors.
    init = dma[dma.index("\nice_buf_init(ice_t *ice)"):]
    init = init[:init.index("\n}\n")]
    assert "itxr_size" not in init and "ice_tx_ring_size" not in init
    assert "ice_lso_pool" not in init and "ICE_TX_LSO_BUFS" not in init


if __name__ == "__main__":
    main()

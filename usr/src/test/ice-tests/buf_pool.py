#!/usr/bin/env python3
"""Execute TX pool sizing, lifetime and the LSO allocation at MAC start."""

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
                         header)]
    fragments += [extract(types, rf"^#define\t{name}\t.*$", header)
                  for name in re.findall(r"^#define\t(ICE_TX_\w+_BUFS_\w+)\t",
                                         types, re.MULTILINE)]
    fragments.append(extract(types, r"^#define\tICE_TX_SMALL_ALIGN\t.*$",
                             header))
    names = ("ice_buf_pool_fini", "ice_buf_pool_init", "ice_buf_take",
             "ice_buf_put", "ice_tx_pool_bufs", "ice_buf_init",
             "ice_tx_lso_fini", "ice_tx_lso_alloc", "ice_tx_lso_free",
             "ice_buf_fini")
    bodies = [extract(dma, rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", source)
              for name in names]
    run_c(TESTDIR / "buf_pool.c", {"ice_pool_types.h": "\n".join(fragments),
                                  "ice_pool_code.h": "\n".join(bodies)})

    # Pool sizes come from the ring count and caps, never the descriptors.
    init = dma[dma.index("\nice_buf_init(ice_t *ice)"):]
    init = init[:init.index("\n}\n")]
    assert "itxr_size" not in init and "ice_tx_ring_size" not in init
    assert "ice_lso_pool" not in init and "ICE_TX_LSO_BUFS" not in init

    # No deferred allocation remains: the TX path never allocates, and MAC
    # start does it before any ring opens.
    tx = (DRIVER / "ice_tx.c").read_text()
    for text in (dma, tx, types):
        assert "ice_tx_taskq" not in text and "lso_state" not in text
    assert "taskq" not in dma and "taskq" not in tx


if __name__ == "__main__":
    main()

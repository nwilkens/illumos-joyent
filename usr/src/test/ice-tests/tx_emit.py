#!/usr/bin/env python3
"""Exercise actual ICE TX descriptor emission and completion ownership."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, extract_file, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_tx.c")
    args = parser.parse_args()
    header = DRIVER / "ice.h"
    desc = DRIVER / "core/ice_lan_tx_rx.h"
    source = args.source.read_text(encoding="utf-8")
    types = [extract_file(header,
        r"^typedef enum ice_tcb_type \{[\s\S]*?^} ice_tcb_type_t;"),
        extract_file(header,
        r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;"),
        extract_file(desc, r"^struct ice_tx_desc \{[\s\S]*?"
        r"^enum ice_tx_ctx_desc_cmd_bits \{[\s\S]*?^};")]
    for name in ("ice_dma_buffer", "ice_tx_ctrl_block", "ice_tx_ctx_t"):
        alias = name if name.endswith("_t") else name + "_t"
        types.append(extract_file(header,
            rf"^typedef struct {name} \{{[\s\S]*?^}} {alias};"))
    for name in ("ICE_DMA_PA", "ICE_TX_MAX_BUFSZ"):
        types.append(extract_file(header,
            rf"^#define\s+{name}(?:\([^\n]*|\s+[^\n]*)"))
    types.append(extract(source,
        r"^#define\s+ICE_TX_QW1_DTYPE_DONE_M\s+[^\n]*", args.source))

    names = ["ice_tx_ring_next", "ice_tcb_free", "ice_tx_sync_descs",
             "ice_tx_write_desc", "ice_tx_write_ctx_desc", "ice_tx_sync_tcb",
             "ice_tx_emit", "ice_tx_desc_done", "ice_tx_recycle"]
    # The same behavioral cases also run before the bind-handle consolidation.
    if re.search(r"^ice_tcb_bind_handle\(", source, re.MULTILINE):
        names.insert(0, "ice_tcb_bind_handle")
    bodies = [extract(source,
        rf"^static (?:inline )?[\w *]+\n{name}\([\s\S]*?^}}",
        args.source) for name in names]
    run_c(TESTDIR / "tx_emit.c", {
        "ice_tx_emit_types.h": "\n".join(types),
        "ice_tx_emit_body.h": "\n".join(bodies),
    })


if __name__ == "__main__":
    main()

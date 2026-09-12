#!/usr/bin/env python3

"""Compile and exercise the actual ICE transmit offload validation."""

import argparse
from pathlib import Path

from c_test import DRIVER, REPO, TESTDIR, extract_file, run_c


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_tx.c")
    args = parser.parse_args()
    header = DRIVER / "ice.h"
    desc = DRIVER / "core/ice_lan_tx_rx.h"
    mac = REPO / "usr/src/uts/common/sys/mac_provider.h"
    fragments = [
        extract_file(header, r"^typedef struct ice_tx_ctx_t \{[\s\S]*?^} ice_tx_ctx_t;"),
        extract_file(args.source,
                r"^typedef enum ice_tx_build \{[\s\S]*?^} ice_tx_build_t;"),
        extract_file(mac, r"^typedef enum mac_ether_offload_flags \{[\s\S]*?"
                r"^} mac_ether_offload_info_t;"),
        extract_file(desc, r"^enum ice_tx_desc_cmd_bits \{[\s\S]*?"
                r"^#define ICE_TXD_QW1_TX_BUF_SZ_S[^\n]*"),
    ]
    definitions = (
        (header, ("ICE_TX_LSO_MIN_MSS", "ICE_TX_LSO_MAX_HDRLEN", "ICE_LSO_MAXLEN")),
        (desc, ("ICE_TXD_CTX_MAX_MSS",)),
        (DRIVER / "core/ice_defs.h", ("ICE_BYTES_PER_WORD", "ICE_BYTES_PER_DWORD")),
        (REPO / "usr/src/uts/common/sys/pattr.h",
         ("HCK_IPV4_HDRCKSUM", "HCK_PARTIALCKSUM", "HW_LSO")),
    )
    for path, names in definitions:
        for name in names:
            fragments.append(extract_file(path, rf"^#define\s+{name}\s+[^\n]*"))

    body = extract_file(args.source,
                   r"^static ice_tx_build_t\nice_tx_context\([\s\S]*?^}")
    # Permit testing the reviewed baseline, which also passed the MTU owner.
    legacy = "ice_tx_context(ice_t *ice," in body
    call = "ice_tx_context(&ice, mp, ctx)" if legacy else "ice_tx_context(mp, ctx)"
    run_c(TESTDIR / "lso_context.c", {
        "ice_tx_types.h": "\n".join(fragments),
        "ice_tx_context.h": body,
    }, cflags=(f"-DCONTEXT(mp,ctx)={call}",))


if __name__ == "__main__":
    main()

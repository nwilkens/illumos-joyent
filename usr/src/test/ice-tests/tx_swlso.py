#!/usr/bin/env python3
"""Run the actual software LSO fallback and check how the send path uses it."""

import argparse
from pathlib import Path

from c_test import DRIVER, REPO, TESTDIR, extract, extract_file, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_tx.c")
    args = parser.parse_args()
    tx = args.source.read_text()
    defs = []
    for name in ("HCK_IPV4_HDRCKSUM", "HCK_IPV4_HDRCKSUM_OK", "HCK_FULLCKSUM",
                 "HCK_FULLCKSUM_OK", "HW_LSO"):
        defs.append(extract_file(REPO / "usr/src/uts/common/sys/pattr.h",
                                 rf"^#define\s+{name}\s+[^\n]*"))
    defs.append(extract_file(REPO / "usr/src/uts/common/sys/mac.h",
                             r"^typedef enum mac_emul \{[\s\S]*?^} mac_emul_t;"))
    defs.append(extract_file(REPO / "usr/src/uts/common/sys/mac.h",
                             r"^#define\s+MAC_HWCKSUM_EMULS\s+[^\n]*"))
    body = extract(tx, r"^static mblk_t \*\nice_tx_swlso\([\s\S]*?^}",
                   args.source)
    run_c(TESTDIR / "tx_swlso.c", {"tx_swlso_defs.h": "\n".join(defs),
                                   "tx_swlso_body.h": body})

    # The send loop hands such a packet, still unconsumed, to the fallback and
    # sends the segments before the rest of the chain.
    loop = tx[tx.index("\nice_ring_tx(void *arg, mblk_t *mp)\n{"):]
    loop = loop[:loop.index("\n}\n")]
    assert "ice_tx_swlso(itr, mp, next)" in loop
    assert "res == ICE_TX_ONE_SWLSO" in loop
    one = tx[tx.index("\nice_tx_one(ice_tx_ring_t *itr, mblk_t *mp)\n{"):]
    one = one[:one.index("\n}\n")]
    # The MTU and LSO-enable checks apply before the fallback is chosen.
    assert one.index("!ice->ice_tx_lso_enable") < \
        one.index("return (ICE_TX_ONE_SWLSO);")
    assert one.index("ice_tx_frame_fits(") < \
        one.index("return (ICE_TX_ONE_SWLSO);")
    assert one.index("return (ICE_TX_ONE_SWLSO);") < one.index("freemsg(mp)")
    print("PASS: the send path segments LSO without checksum offload")


if __name__ == "__main__":
    main()

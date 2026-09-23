#!/usr/bin/env python3
"""Execute the receive checksum verdict and its per-ring counters."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_rx.c")
    args = parser.parse_args()
    header = DRIVER / "ice.h"
    desc = DRIVER / "core/ice_lan_tx_rx.h"
    desc_text = desc.read_text()
    types = [extract(desc_text, r"^struct ice_rx_ptype_decoded \{[\s\S]*?^};",
                     desc)]
    for name in ("ice_rx_ptype_outer_ip", "ice_rx_ptype_outer_ip_ver",
                 "ice_rx_ptype_tunnel_type", "ice_rx_ptype_inner_prot",
                 "ice_rx_flex_desc_status_error_0_bits"):
        types.append(extract(desc_text, rf"^enum {name} \{{[\s\S]*?^}};", desc))
    types.append(extract(header.read_text(),
        r"^typedef struct ice_rxq_stat \{[\s\S]*?^} ice_rxq_stat_t;", header))
    body = extract(args.source.read_text(),
        r"^static void\nice_rx_hcksum\([\s\S]*?^}", args.source)
    run_c(TESTDIR / "rx_hcksum.c", {"ice_rx_types.h": "\n".join(types),
                                     "ice_rx_hcksum.h": body})

    # Every counter is published under a kstat name.
    rx = args.source.read_text()
    fields = re.findall(r"kstat_named_t\s+(icrxs_hck_\w+);",
                        header.read_text())
    assert len(fields) == 9, fields
    for field in fields:
        assert f"&rxs->{field}, \"rx_{field[6:]}\"" in rx, field
    tx = (DRIVER / "ice_tx.c").read_text()
    for field in re.findall(r"kstat_named_t\s+(ictxs_(?:hck|lso)_\w+);",
                            header.read_text()):
        assert f"&txs->{field}, \"tx_{field[6:]}\"" in tx, field


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Execute ICE lifecycle admission, startup, stop, and resource ownership."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice.c")
    parser.add_argument("--gld-source", type=Path, default=DRIVER / "ice_gld.c")
    args = parser.parse_args()
    header = DRIVER / "ice.h"
    mac = DRIVER.parents[1] / "sys/mac.h"
    definitions = [extract(header.read_text(),
        r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;", header),
        extract(mac.read_text(),
        r"^typedef enum \{\n\s+LINK_STATE_UNKNOWN[\s\S]*?^} link_state_t;", mac)]
    bodies = []
    # Old revisions keep the lifecycle implementation in the MAC adapter.
    moved = re.search(r"^ice_start\(", args.source.read_text(), re.MULTILINE)
    owner = args.source if moved else args.gld_source
    groups = [
        (DRIVER / "ice_intr.c", ("ice_link_state_effective",
                                 "ice_link_state_set", "ice_link_state_publish")),
        (DRIVER / "ice_tx.c", ("ice_tx_stop",)),
        (DRIVER / "ice_rx.c", ("ice_rx_stop",)),
        (owner, ("ice_start_datapath",)),
    ]
    if moved:
        groups.append((owner, ("ice_start", "ice_stop")))
    groups.append((args.gld_source, ("ice_m_start", "ice_m_stop")))
    for path, names in groups:
        for name in names:
            bodies.append(extract(path.read_text(),
                rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", path))
    run_c(TESTDIR / "lifecycle_api.c", {
        "ice_lifecycle_types.h": "\n".join(definitions),
        "ice_lifecycle_bodies.h": "\n".join(bodies),
    })


if __name__ == "__main__":
    main()

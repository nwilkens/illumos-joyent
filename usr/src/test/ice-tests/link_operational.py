#!/usr/bin/env python3
"""Execute ICE carrier updates, operational publication, and MAC startup."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_intr.c")
    parser.add_argument("--gld-source", type=Path, default=DRIVER / "ice_gld.c")
    args = parser.parse_args()
    source = args.source.read_text()
    header = DRIVER / "ice.h"
    mac = DRIVER.parents[1] / "sys/mac.h"
    adminq = DRIVER / "core/ice_adminq_cmd.h"
    types = [extract(header.read_text(),
        r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;", header),
        extract(mac.read_text(), r"^typedef enum \{\n\s+LINK_STATE_UNKNOWN[\s\S]*?^} link_fec_t;", mac),
        extract(mac.read_text(), r"^typedef enum \{\n\s+MAC_PROP_PRIVATE[\s\S]*?^} mac_prop_id_t;", mac)]
    types.append(extract(header.read_text(), r"^#define\s+ICE_LB_NONE[^\n]*\n[^\n]*", header))
    definitions = adminq.read_text()
    for match in re.finditer(r"^#define\s+ICE_AQ_LINK_(?:UP\s|PAUSE_[TR]X\s|SPEED_[A-Z0-9]+\s)[^\n]*", definitions, re.MULTILINE):
        types.append(match.group() + "\n")
    bodies = []
    for name in ("ice_link_state_effective", "ice_link_state_set", "ice_link_report",
                 "ice_link_state_publish", "ice_link_prop_update", "ice_link_loopback_update"):
        if name == "ice_link_state_effective" and not re.search(rf"^{name}\(", source, re.MULTILINE):
            continue
        bodies.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    for name in ("ice_m_start", "ice_m_getprop"):
        bodies.append(extract(args.gld_source.read_text(),
            rf"^static int\n{name}\([\s\S]*?^}}", args.gld_source))
    run_c(TESTDIR / "link_operational.c", {
        "ice_link_types.h": "\n".join(types),
        "ice_link_body.h": "\n".join(bodies),
    })


if __name__ == "__main__":
    main()

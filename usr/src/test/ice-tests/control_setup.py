#!/usr/bin/env python3
"""Execute ICE link-event and RSS setup with controlled firmware boundaries."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


def define(path, name):
    return extract(path.read_text(),
        rf"^#define[ \t]+{name}\b(?:[^\n]*\\\n)*[^\n]*", path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--intr-source", type=Path, default=DRIVER / "ice_intr.c")
    parser.add_argument("--vsi-source", type=Path, default=DRIVER / "ice_vsi.c")
    args = parser.parse_args()
    header = DRIVER / "ice.h"
    adminq = DRIVER / "core/ice_adminq_cmd.h"
    flow = DRIVER / "core/ice_flow.h"
    types = DRIVER / "core/ice_type.h"
    definitions = [define(header, "ICE_AQ_LINK_EVENT_MASK_DEFINED")]
    for name in re.findall(r"^#define\s+(ICE_AQ_LINK_EVENT_\w+)",
                           adminq.read_text(), re.MULTILINE):
        definitions.append(define(adminq, name))
    for name in ("ICE_AQC_GET_SET_RSS_KEY_DATA_RSS_KEY_SIZE",
                 "ICE_AQC_GET_SET_RSS_KEY_DATA_HASH_KEY_SIZE"):
        definitions.append(define(adminq, name))
    for kind, name in (("struct", "ice_aqc_get_set_rss_keys"),
                       ("enum", "ice_lut_type"), ("enum", "ice_lut_size")):
        definitions.append(extract(adminq.read_text(),
            rf"^{kind} {name} \{{[\s\S]*?^}};", adminq))
    definitions.append(extract(types.read_text(),
        r"^struct ice_aq_get_set_rss_lut_params \{[\s\S]*?^};", types))
    for kind, name in (("enum", "ice_flow_field"),
                       ("enum", "ice_rss_cfg_hdr_type"),
                       ("struct", "ice_rss_hash_cfg")):
        definitions.append(extract(flow.read_text(),
            rf"^{kind} {name} \{{[\s\S]*?^}};", flow))
    # Only the four headers needed here; the full enum has a non-C99 value.
    definitions.append("enum {")
    for name in ("IPV4", "IPV6", "TCP", "UDP"):
        definitions.append(extract(flow.read_text(),
            rf"^\s+ICE_FLOW_SEG_HDR_{name}\s*=[^\n]*", flow))
    definitions.append("};")
    for name in ("ICE_FLOW_HASH_IPV4", "ICE_FLOW_HASH_IPV6",
                 "ICE_FLOW_HASH_TCP_PORT", "ICE_FLOW_HASH_UDP_PORT",
                 "ICE_HASH_TCP_IPV4", "ICE_HASH_UDP_IPV4",
                 "ICE_HASH_TCP_IPV6", "ICE_HASH_UDP_IPV6"):
        definitions.append(define(flow, name))
    vsi = args.vsi_source.read_text()
    bodies = [extract(vsi,
        r"^static const struct \{[^}]*} ice_rss_flows\[\] = \{[\s\S]*?^};",
        args.vsi_source)]
    for path, name in ((args.intr_source, "ice_set_link_events"),
                       (args.vsi_source, "ice_rss_setup")):
        bodies.append(extract(path.read_text(),
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", path))
    run_c(TESTDIR / "control_setup.c", {
        "ice_control_types.h": "\n".join(definitions),
        "ice_control_bodies.h": "\n".join(bodies),
    })


if __name__ == "__main__":
    main()

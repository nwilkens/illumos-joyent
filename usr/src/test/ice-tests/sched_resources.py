#!/usr/bin/env python3
"""Execute scheduler resource admission without constructing a scheduler tree."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=DRIVER / "core/ice_sched.c")
    parser.add_argument("--scenario", choices=("levels", "fanout", "valid",
                                               "failures", "cached"))
    args = parser.parse_args()
    adminq = DRIVER / "core/ice_adminq_cmd.h"
    sched = DRIVER / "core/ice_sched.h"
    status = DRIVER / "core/ice_status.h"
    definitions = [extract(adminq.read_text(),
        r"^#define ICE_AQC_TOPO_MAX_LEVEL_NUM[^\n]*", adminq)]
    for name in ("ICE_SCHED_5_LAYERS", "ICE_SCHED_9_LAYERS"):
        definitions.append(extract(sched.read_text(),
            rf"^#define {name}[^\n]*", sched))
    for name in ("ice_aqc_generic_sched_props", "ice_aqc_layer_props",
                 "ice_aqc_query_txsched_res_resp"):
        definitions.append(extract(adminq.read_text(),
            rf"^struct {name} \{{[\s\S]*?^}};", adminq))
    definitions.append(extract(status.read_text(),
        r"^enum ice_status \{[\s\S]*?^};", status))
    body = extract(args.source.read_text(),
        r"^int ice_sched_query_res_alloc\([\s\S]*?^}", args.source)
    cases = (args.scenario,) if args.scenario else (
        "levels", "fanout", "valid", "failures", "cached")
    run_c(TESTDIR / "sched_resources.c", {
        "ice_sched_resource_types.h": "\n".join(definitions),
        "ice_sched_resource_body.h": body,
    }, cases=tuple((case,) for case in cases))


if __name__ == "__main__":
    main()

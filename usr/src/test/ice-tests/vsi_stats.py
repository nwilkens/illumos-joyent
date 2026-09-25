#!/usr/bin/env python3
"""Exercise VSI statistics bounds before counter reads and clear writes."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c

CASES = ("zero", "last", "limit", "maximum", "missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_stats.c")
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    header = DRIVER / "ice.h"
    types = DRIVER / "core/ice_type.h"
    registers = DRIVER / "core/ice_hw_autogen.h"
    fragments = [extract(header.read_text(), r"^#define\s+ICE_MAX_VSI\s+.*",
                         header),
                 extract(types.read_text(),
                         r"^struct ice_eth_stats \{[\s\S]*?^};", types)]
    for name in ("GLV_GORCL", "GLV_UPRCL", "GLV_MPRCL", "GLV_BPRCL",
                 "GLV_RDPC", "GLV_GOTCL", "GLV_UPTCL", "GLV_MPTCL",
                 "GLV_BPTCL", "GLV_TEPC"):
        fragments.append(extract(registers.read_text(),
            rf"^#define {name}\([^\n]+", registers))
    function = extract(args.source.read_text(),
        r"^(?:static )?void\nice_stats_update_vsi\([\s\S]*?^}", args.source)
    run_c(TESTDIR / "vsi_stats.c", {
        "vsi_stats_types.h": "\n".join(fragments),
        "vsi_stats_function.h": function,
    }, cases=[(case,) for case in ([args.case] if args.case else CASES)])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Execute ICE statistics reads, cache refresh and reset baseline ownership."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_stats.c")
    args = parser.parse_args()
    types = DRIVER / "core/ice_type.h"
    registers = DRIVER / "core/ice_hw_autogen.h"
    common = DRIVER / "core/ice_common.c"
    mac = DRIVER.parents[1] / "sys/mac.h"
    ether = mac.with_name("mac_ether.h")
    definitions = []
    for name in ("MAC_STAT_MIN", "MACTYPE_STAT_MIN"):
        definitions.append(extract(mac.read_text(),
            rf"^#define\s+{name}\s+[^\n]*", mac))
    for path, name in ((mac, "mac_driver_stat"), (ether, "ether_stat")):
        definitions.append(extract(path.read_text(),
            rf"^enum {name} \{{[\s\S]*?^}};", path))
    for name in ("ice_eth_stats", "ice_hw_port_stats"):
        definitions.append(extract(types.read_text(),
            rf"^struct {name} \{{[\s\S]*?^}};", types))
    for name in ("GORCL", "UPRCL", "MPRCL", "BPRCL", "GOTCL", "UPTCL",
                 "MPTCL", "BPTCL", "TDOLD", "LXONRXC", "LXOFFRXC",
                 "LXONTXC", "LXOFFTXC", "CRCERRS", "ILLERRC", "MLFC",
                 "MRFC", "RLEC", "RUC", "RFC", "ROC", "RJC"):
        definitions.append(extract(registers.read_text(),
            rf"^#define GLPRT_{name}\([^\n]+", registers))
    definitions.append(extract(args.source.read_text(),
        r"^#define\s+ICE_STATS_MIN_UPDATE_NS\s+[^\n]*", args.source))
    bodies = []
    for path, names in ((common, ("ice_stat_update40", "ice_stat_update32")),
                        (args.source, ("ice_stats_update_port", "ice_stats_read",
                                       "ice_stats_reset", "ice_stats_check_acc"))):
        for name in names:
            bodies.append(extract(path.read_text(),
                rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", path))
    run_c(TESTDIR / "stats_read.c", {
        "ice_stats_read_types.h": "\n".join(definitions),
        "ice_stats_read_bodies.h": "\n".join(bodies),
    })


if __name__ == "__main__":
    main()

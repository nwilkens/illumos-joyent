#!/usr/bin/env python3
"""Compare actual ICE MAC requests across GLD, attach, replay, and teardown."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c
from terminal_filters import callback_fragments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gld-source", type=Path, default=DRIVER / "ice_gld.c")
    parser.add_argument("--vsi-source", type=Path, default=DRIVER / "ice_vsi.c")
    parser.add_argument("--filter-source", type=Path, default=DRIVER / "ice_filter.c")
    parser.add_argument("--scenario", choices=("requests", "rebuild_invalid",
        "rebuild_number", "rebuild_context", "rebuild_scheduler",
        "attach_failures", "recovery_replay"))
    args = parser.parse_args()
    callbacks = callback_fragments(args.gld_source, args.vsi_source, args.filter_source)
    source = args.vsi_source.read_text()
    modern = "ice_filters_init(ice)" in source
    policy = args.filter_source.read_text() if modern else source
    policy_path = args.filter_source if modern else args.vsi_source
    fragments = [extract(policy,
        r"^static const uint8_t ice_bcast_addr[\s\S]*?^};", policy_path)]
    if re.search(r"^ice_gld_fltr_init\(", args.gld_source.read_text(),
                 re.MULTILINE):
        fragments.append(extract(source,
            r"^(?:static )?void\nice_fltr_entry_init\([\s\S]*?^}", args.vsi_source))
    if modern:
        names = ("ice_mac_filter_track", "ice_filters_init", "ice_filters_setup",
                 "ice_filters_fini", "ice_filters_replay")
        for name in names:
            fragments.append(extract(policy,
                rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", policy_path))
        names = ("ice_vsi_teardown", "ice_vsi_setup", "ice_vsi_init", "ice_vsi_rebuild")
    else:
        fragments.append("#define ice_filters_setup ice_add_mac_filters")
        names = ("ice_mac_filter_track", "ice_vsi_teardown", "ice_vsi_setup",
                 "ice_add_mac_filters", "ice_vsi_init", "ice_vsi_rebuild")
    for name in names:
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.vsi_source))
    scenarios = [args.scenario] if args.scenario else ("requests",
        "rebuild_invalid", "rebuild_number", "rebuild_context",
        "rebuild_scheduler", "attach_failures", "recovery_replay")
    run_c(TESTDIR / "filter_requests.c", {
        "ice_filter_callbacks.h": "\n".join(callbacks),
        "ice_vsi_filter_bodies.h": "\n".join(fragments),
    }, cflags=("-Wno-unused-function",),
        cases=tuple((scenario,) for scenario in scenarios))


if __name__ == "__main__":
    main()

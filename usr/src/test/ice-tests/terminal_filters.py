#!/usr/bin/env python3

"""Compile and exercise the real ICE filter callbacks with a stub admin queue."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


FUNCTIONS = (
    "ice_gld_find_mac",
    "ice_gld_set_mac_locked",
    "ice_gld_set_mac",
    "ice_group_add_mac",
    "ice_group_remove_mac",
    "ice_promisc_apply",
    "ice_m_promisc",
    "ice_m_multicst",
)


def callback_fragments(gld_source: Path, vsi_source: Path,
                       filter_source: Path = DRIVER / "ice_filter.c") -> list[str]:
    source = gld_source.read_text(encoding="utf-8")
    header = DRIVER / "ice.h"
    fragments = [extract(header.read_text(encoding="utf-8"),
                         r"^typedef enum ice_state \{[\s\S]*?^} ice_state_t;",
                         header)]
    if "ice_filters_set_mac(" in source:
        policy = filter_source.read_text(encoding="utf-8")
        names = ("ice_fltr_entry_init", "ice_filter_find_mac",
                 "ice_filters_blocked", "ice_filters_recover",
                 "ice_filters_set_mac_locked", "ice_filters_set_mac",
                 "ice_filters_promisc_apply", "ice_filters_set_promisc",
                 "ice_filters_replay_promisc")
        for name in names:
            fragments.append(extract(policy,
                rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", filter_source))
        for name in ("ice_group_add_mac", "ice_group_remove_mac",
                     "ice_m_promisc", "ice_m_multicst"):
            fragments.append(extract(source,
                rf"^static [\w *]+\n{name}\([\s\S]*?^}}", gld_source))
        return fragments
    # Keep old source selections usable as behavioral controls.
    fragments += ["#define ice_filters_set_mac ice_gld_set_mac",
                  "#define ice_filters_replay_promisc(i) ice_promisc_apply(i, B_TRUE)"]
    # Older GLD revisions have a local constructor; current callbacks share VSI's.
    if re.search(r"^ice_gld_fltr_init\(", source, re.MULTILINE):
        fragments.append(extract(source,
            r"^static void\nice_gld_fltr_init\([\s\S]*?^}", gld_source))
    else:
        fragments.append(extract(vsi_source.read_text(encoding="utf-8"),
            r"^void\nice_fltr_entry_init\([\s\S]*?^}", vsi_source))
    # Recovery helpers did not exist in older source used as negative controls.
    for name in ("ice_gld_filters_blocked", "ice_gld_filter_recover"):
        if re.search(rf"^{name}\(", source, re.MULTILINE):
            fragments.append(extract(source,
                rf"^static [\w *]+\n{name}\([\s\S]*?^}}", gld_source))
    # Compile each body unchanged; this does not assert its implementation text.
    for name in FUNCTIONS:
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", gld_source))
    return fragments


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_gld.c")
    parser.add_argument("--vsi-source", type=Path, default=DRIVER / "ice_vsi.c")
    parser.add_argument("--filter-source", type=Path, default=DRIVER / "ice_filter.c")
    args = parser.parse_args()
    fragments = callback_fragments(args.source, args.vsi_source, args.filter_source)

    run_c(TESTDIR / "terminal_filters.c",
          {"ice_filter_callbacks.h": "\n".join(fragments)},
          cflags=("-Wno-unused-function",))


if __name__ == "__main__":
    main()

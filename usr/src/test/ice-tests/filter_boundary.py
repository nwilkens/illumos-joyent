#!/usr/bin/env python3
"""Enforce the filter module's ownership boundary in the illumos glue."""

import re

from c_test import DRIVER


def code(path):
    text = path.read_text()
    return re.sub(r"/\*[\s\S]*?\*/|//[^\n]*", "", text)


def main():
    private = re.compile(
        r"\b(?:vi_macs|vi_mac_lock|ice_promisc_on|ice_mac_filter_t|"
        r"ice_fltr_list_entry|ice_fltr_entry_init|ice_add_mac|ice_remove_mac|"
        r"ice_set_vsi_promisc|ice_clear_vsi_promisc)\b")
    for path in DRIVER.glob("*.c"):
        if path.name == "ice_filter.c":
            continue
        match = private.search(code(path))
        assert match is None, f"{path.name} reaches into filter ownership: {match[0]}"
    header = code(DRIVER / "ice.h")
    assert "ice_fltr_entry_init" not in header
    assert "ice_mac_filter_t" not in header
    assert "ice_promisc_apply" not in header
    print("PASS: filter requests and accepted policy have one module owner")


if __name__ == "__main__":
    main()

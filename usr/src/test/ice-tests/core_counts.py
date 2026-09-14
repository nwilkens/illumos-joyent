#!/usr/bin/env python3

"""Check that firmware-supplied counts in the imported core are bounded.

Each guard is marked with an ``illumos:`` comment because it diverges from the
vendor import; a core refresh must carry or replace every one of them.
"""

from pathlib import Path


REPO = Path(__file__).resolve().parents[4]
CORE = REPO / "usr/src/uts/common/io/ice/core"

GUARDS = {
    "ice_common.c": (
        "cap_count > ICE_AQ_MAX_BUF_LEN / sizeof(struct ice_aqc_list_caps_elem)",
    ),
    "ice_switch.c": (
        "num_elems > ICE_SW_CFG_MAX_BUF_LEN / sizeof(*rbuf)",
    ),
    "ice_ddp_common.c": (
        "LE32_TO_CPU(pkg_info->count) > ICE_PKG_CNT",
        "LE32_TO_CPU(pkg->count) > ICE_PKG_CNT",
        "sizeof(*meta)",
    ),
    "ice_sched.c": (
        "num_elems < 1 || num_elems > ICE_AQC_TOPO_MAX_LEVEL_NUM",
        "parent->num_children >= hw->max_children[parent->tx_sched_layer]",
        "LE16_TO_CPU(buf->sched_props.logical_levels) >\n"
        "\t    ICE_AQC_TOPO_MAX_LEVEL_NUM",
    ),
}


def main() -> None:
    for name, guards in GUARDS.items():
        source = (CORE / name).read_text(encoding="utf-8")
        for guard in guards:
            assert guard in source, (name, guard)
        # Every guard sits next to its marker so a refresh can find it.
        assert source.count("/* illumos:") >= len(guards), name
    # The capability guard applies to both discovery paths.
    common = (CORE / "ice_common.c").read_text(encoding="utf-8")
    assert common.count(GUARDS["ice_common.c"][0]) == 2

    print("PASS: imported core bounds firmware-supplied counts")


if __name__ == "__main__":
    main()

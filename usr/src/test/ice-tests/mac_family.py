#!/usr/bin/env python3
"""Check device ID aliases and the per-family decisions of the driver."""

import argparse
from pathlib import Path
import re
import struct

from c_test import DRIVER, REPO, TESTDIR, extract, run_c


CORE = DRIVER / "core"
MANIFEST = REPO / "usr/src/pkg/manifests/driver-network-ice.p5m"

# The core names each part; the prefix selects the family label the driver
# logs.  E835 parts share the E830 MAC type and programming model.
FAMILIES = (
    ("ICE_DEV_ID_E810", "E810"),
    ("ICE_DEV_ID_E822", "E822"),
    ("ICE_DEV_ID_E823", "E823"),
    ("ICE_DEV_ID_E825C", "E825-C"),
    ("ICE_DEV_ID_E830", "E830"),
    ("ICE_DEV_ID_E835", "E830"),
)


def core_ids():
    text = (CORE / "ice_devids.h").read_text()
    return {name: int(value, 16) for name, value in
            re.findall(r"#define\s+(ICE_DEV_ID_\w+)\s+0x([0-9A-Fa-f]+)",
                       text)}, \
        {name: int(value, 16) for name, value in
         re.findall(r"#define\s+(ICE_SUBDEV_ID_\w+)\s+0x([0-9A-Fa-f]+)",
                    text)}


def mapped_ids(common):
    body = re.search(r"int ice_set_mac_type\(struct ice_hw \*hw\)\n\{"
                     r"([\s\S]*?)\n\}", common).group(1)
    mapping, pending = {}, []
    for line in body.splitlines():
        case = re.search(r"case (ICE_DEV_ID_\w+):", line)
        if case:
            pending.append(case.group(1))
        mac = re.search(r"hw->mac_type = (ICE_MAC_\w+);", line)
        if mac:
            mapping.update((name, mac.group(1)) for name in pending)
            pending = []
    return mapping


def aliases():
    return {int(v, 16) for v in re.findall(r"alias=pciex8086,([0-9a-f]+)",
                                           MANIFEST.read_text())}


def family(name):
    for prefix, label in FAMILIES:
        if name.startswith(prefix):
            return label
    raise AssertionError(f"no family for {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_hw.c")
    parser.add_argument("--intr-source", type=Path,
                        default=DRIVER / "ice_intr.c")
    args = parser.parse_args()
    ids, subsystems = core_ids()
    common = (CORE / "ice_common.c").read_text()
    mapping = mapped_ids(common)
    supported = {ids[name] for name, mac in mapping.items()
                 if mac != "ICE_MAC_UNKNOWN"}
    bound = aliases()

    assert bound == supported, (
        f"aliases differ from the core mapping: missing "
        f"{sorted(map(hex, supported - bound))}, extra "
        f"{sorted(map(hex, bound - supported))}")
    # Subsystem IDs identify boards, not controllers.
    assert not bound & set(subsystems.values()) - supported
    assert ids["ICE_DEV_ID_E822_SI_DFLT"] not in bound

    fragments = []
    for name in ("ice_set_mac_type", "ice_is_generic_mac", "ice_is_e823",
                 "ice_is_e825c", "ice_is_e830"):
        fragments.append(extract(common,
            rf"^(?:int|bool) {name}\(struct ice_hw \*hw\)\n\{{[\s\S]*?^\}}",
            CORE / "ice_common.c"))
    source = args.source.read_text()
    for name in ("ice_family_name", "ice_reset_empr_slow"):
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    for name in ("ice_phy_fw_wait",):
        fragments.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    fragments.insert(0, "\n".join(re.findall(
        r"^#define\tICE_PHY_FW_\w+\t+\d+$", source, re.MULTILINE)))
    fragments.insert(0, extract(common,
        r"^#define ice_get_link_status_data_ver[\s\S]*?^\}",
        CORE / "ice_common.c"))
    ddp = (CORE / "ice_ddp_common.c").read_text()
    for name in ("ice_get_pkg_segment_id", "ice_get_pkg_sign_type"):
        fragments.append(extract(ddp,
            rf"^static u32 {name}\([\s\S]*?^\}}", CORE / "ice_ddp_common.c"))
    header = (DRIVER / "ice.h").read_text()
    fragments.insert(0, extract(header,
        r"^#define\tICE_E830_GL_MDET_TX_TCLAN[\s\S]*?PF_MDET_TX_TCLAN\)$",
        DRIVER / "ice.h"))
    intr = args.intr_source.read_text()
    fragments.append(extract(intr,
        r"^static void\nice_sbq_drain\([\s\S]*?^}", args.intr_source))
    autogen = (CORE / "ice_hw_autogen.h").read_text()
    regs = "\n".join(re.findall(
        r"^#define (?:GLGEN_RSTAT(?:_RESET_TYPE_[SM])?|GL_MNG_FWSM|"
        r"GL_MNG_FWSM_FW_LOADING_M|GL_MDET_TX_TCLAN|PF_MDET_TX_TCLAN)\s.*$",
        autogen, re.MULTILINE))
    cases = [f"{ids[name]:x}:{family(name)}" for name in mapping]
    cases.append(f"{ids['ICE_DEV_ID_E822_SI_DFLT']:x}:none")
    run_c(TESTDIR / "mac_family.c",
          {"ice_family_body.h": "\n".join(fragments),
           "ice_family_regs.h": regs + "\n",
           "ice_devids.h": (CORE / "ice_devids.h").read_text()},
          cflags=("-Wno-unused-function",), cases=(tuple(cases),))
    check_package()
    print(f"PASS: {len(bound)} aliases match the core MAC type mapping")


# Configuration segment and signature each family loads (mac_family.c checks
# that the core selects these).
PACKAGE_NEEDS = {
    "E810, E822, E823": (0x10, 1),
    "E825-C": (0x10, 5),
    "E830": (0x17, 3),
}


def check_package():
    """The shipped DDP package must carry a signed segment for each family."""
    data = (DRIVER / "firmware/ice.pkg").read_bytes()
    count = struct.unpack_from("<I", data, 4)[0]
    configs, signed = set(), set()
    for i in range(count):
        offset = struct.unpack_from("<I", data, 8 + 4 * i)[0]
        seg_type = struct.unpack_from("<I", data, offset)[0]
        if seg_type in (0x10, 0x17):
            configs.add(seg_type)
        elif seg_type == 0x1001:
            # ice_sign_seg: 44-byte generic header, seg_id, sign_type
            signed.add(struct.unpack_from("<II", data, offset + 44))
    for family, (seg, sign) in PACKAGE_NEEDS.items():
        assert seg in configs, f"{family}: no configuration segment {seg:#x}"
        assert (seg, sign) in signed, f"{family}: no signature type {sign}"


if __name__ == "__main__":
    main()

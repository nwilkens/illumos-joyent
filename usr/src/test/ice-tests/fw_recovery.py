#!/usr/bin/env python3
"""Execute firmware recovery-mode detection and check where it runs."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_hw.c")
    parser.add_argument("--lifecycle-source", type=Path,
                        default=DRIVER / "ice.c")
    args = parser.parse_args()
    source = args.source.read_text()
    core = DRIVER / "core"
    autogen = (core / "ice_hw_autogen.h").read_text()
    regs = re.findall(r"^#define (?:GL_MNG_FWSM|GL_MNG_FWSM_FW_MODES_M_BY_MAC"
                      r"|E800_GL_MNG_FWSM_FW_MODES_M|"
                      r"E830_GL_MNG_FWSM_FW_MODES_M|E800_GL_MNG_FWSM_FW_MODES_M"
                      r")[\s(].*$", autogen, re.MULTILINE)
    common = (core / "ice_common.c").read_text()
    body = [extract(common,
        r"^enum ice_fw_modes ice_get_fw_mode\(struct ice_hw \*hw\)\n\{[\s\S]*?^\}",
        core / "ice_common.c"),
        extract(source, r"^#define\tICE_FWSM_MODE_RECOVERY\t.*$", args.source)]
    for name in ("ice_fw_recovery_mode", "ice_fw_recovery_report"):
        body.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    run_c(TESTDIR / "fw_recovery.c",
          {"fw_recovery_regs.h": "\n".join(regs) + "\n",
           "fw_recovery_body.h": "\n".join(body)})

    # Attach checks before the first admin queue command, after the MAC type
    # that selects the field width is known; the rebuild checks right after
    # the reset completes and fails closed.
    init = source[source.index("\nice_hw_init(ice_t *ice)"):]
    init = init[:init.index("\n}\n")]
    assert init.index("ice_set_mac_type(hw)") < \
        init.index("ice_fw_recovery_mode(ice, &fwsm)") < \
        init.index("ice_init_hw(hw)")
    lifecycle = args.lifecycle_source.read_text()
    rebuild = lifecycle[lifecycle.index("\nice_rebuild(ice_t *ice"):]
    rebuild = rebuild[:rebuild.index("\nreset_failed:")]
    check = rebuild.index("ice_fw_recovery_mode(ice, &fwsm)")
    assert rebuild.index("ice_check_reset(hw)") < check < \
        rebuild.index("ice_init_all_ctrlq(hw)")
    assert "goto reset_failed;" in rebuild[check:check + 200]


if __name__ == "__main__":
    main()

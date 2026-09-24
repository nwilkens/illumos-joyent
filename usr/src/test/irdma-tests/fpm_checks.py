#!/usr/bin/env python3
"""Run the FPM query and commit checks against hostile firmware values."""

import re

from irdma_test import IRDMA, TESTDIR, function, run_c


def main():
    osdep = IRDMA / "irdma_osdep.c"
    text = osdep.read_text(encoding="utf-8")
    defines = "\n".join(re.findall(r"^#define\tIRDMA_FPM_\w+\t.*$", text,
                                   re.MULTILINE))
    parts = [defines] + [function(osdep, name) for name in (
        "irdma_osdep_fpm_query_check", "irdma_osdep_fpm_commit_check")]
    run_c(TESTDIR / "fpm_checks.c", {"fpm_bodies.h": "\n".join(parts)},
          cflags=("-Wno-unused-parameter", "-Wno-format"))

    # The core calls both checks where it parses the firmware buffers.
    ctrl = (IRDMA / "core/ctrl.c").read_text(encoding="utf-8")
    assert re.search(r"return irdma_osdep_fpm_query_check\(dev, hmc_info, "
                     r"hmc_fpm_misc\);\n}", ctrl)
    assert re.search(r"irdma_sc_parse_fpm_commit_buf\([\s\S]{0,200}?"
                     r"ret_code = irdma_osdep_fpm_commit_check\(dev, "
                     r"hmc_info\);", ctrl)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Execute the firmware diagnostic ioctls with hostile callers and firmware."""

import argparse
from pathlib import Path
import re

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_ioctl.c")
    args = parser.parse_args()
    source = args.source.read_text()
    body = re.findall(r"^#define\tICE_FWDUMP_MASK_\w+\t.*$", source,
                      re.MULTILINE)
    body.append(extract(source,
        r"^typedef struct ice_diag_fwlog \{[\s\S]*?^} ice_diag_fwlog_t;",
        args.source))
    names = re.findall(r"^(ice_diag_\w+)\(", source, re.MULTILINE)
    assert "ice_diag_ioctl" in names and "ice_diag_priv" in names
    for name in names:
        body.append(extract(source,
            rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", args.source))
    run_c(TESTDIR / "diag_ioctl.c",
          {"diag_ioctl_body.h": "\n".join(body),
           "ice_ioctl.h": (DRIVER / "ice_ioctl.h").read_text()},
          cflags=("-Wno-unused-function",))

    # MAC hands unknown ioctls to the diagnostics; no card-wide reset exists.
    gld = (DRIVER / "ice_gld.c").read_text()
    assert "if (ice_diag_ioctl(ice, q, mp))" in gld
    header = (DRIVER / "ice_ioctl.h").read_text()
    assert "RESET" not in header and "CORER" not in header
    intr = (DRIVER / "ice_intr.c").read_text()
    assert "ice_diag_fwlog_event(ice, evt.msg_buf, evt.msg_len);" in intr


if __name__ == "__main__":
    main()

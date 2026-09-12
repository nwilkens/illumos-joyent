#!/usr/bin/env python3
"""Execute the real ice.pkg validator against constructed section tables."""

import argparse
from pathlib import Path

from c_test import DRIVER, TESTDIR, extract, run_c


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_ddp.c")
    args = parser.parse_args()
    source = args.source.read_text()
    fragments = []
    for name in ("ice_ddp_seg_hdr", "ice_ddp_cfg_seg_bufs", "ice_ddp_sect_min",
                 "ice_ddp_buf_ok", "ice_ddp_cfg_seg_ok", "ice_ddp_sign_seg_ok",
                 "ice_ddp_pkg_valid"):
        fragments.append(extract(source,
            rf"^static [\w *]+\n{name}\([\s\S]*?^}}", args.source))
    run_c(TESTDIR / "ddp_sections.c",
          {"ddp_sections_body.h": "\n".join(fragments)})

    # The core's own metadata consumer checks the size it reads.
    core = (DRIVER / "core/ice_ddp_common.c").read_text()
    info = core[core.index("ice_init_pkg_info(struct ice_hw *hw"):]
    info = info[:info.index("\n}\n")]
    assert "sizeof(*meta)" in info
    assert info.index("sizeof(*meta)") < info.index("hw->pkg_ver = meta->ver;")


if __name__ == "__main__":
    main()

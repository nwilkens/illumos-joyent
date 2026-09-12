#!/usr/bin/env python3
"""Execute the real MAC L3 parser against IPv6 extension-header chains."""

import argparse
from pathlib import Path

from c_test import REPO, TESTDIR, extract, run_c

MAC = REPO / "usr/src/uts/common/io/mac/mac_provider.c"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=MAC)
    args = parser.parse_args()
    source = args.source.read_text()
    fragments = [
        extract(source, r"^typedef struct mac_mblk_cursor \{[\s\S]*?"
                r"^} mac_mblk_cursor_t;", args.source),
        extract(source, r"^static void mac_mmc_advance\(.*", args.source),
        extract(source, r"^static void mac_mmc_reset\(.*", args.source),
    ]
    for name in ("mac_parse_is_ipv6eh", "mac_mmc_init", "mac_mmc_reset",
                 "mac_mmc_mp_left", "mac_mmc_mp_ptr", "mac_mmc_offset",
                 "mac_mmc_advance", "mac_mmc_seek", "mac_mmc_get_uint8",
                 "mac_mmc_get_uint16", "mac_mmc_parse_l3"):
        fragments.append(extract(source,
            rf"^(?:static )?(?:inline )?[\w *]+\n{name}\([\s\S]*?^}}",
            args.source))
    run_c(TESTDIR / "mac_ipv6_eh.c",
          {"mac_ipv6_eh_body.h": "\n".join(fragments)},
          cflags=("-Wno-unused-function",))

    # The caller defines every output before the parse.
    info = source[source.index("mac_partial_offload_info(mblk_t *mp"):]
    info = info[:info.index("\n}\n")]
    assert "uint8_t ipproto = 0;" in info
    assert "uint16_t l3_sz = 0;" in info
    assert "mac_ether_offload_flags_t frag_flags = 0;" in info
    assert "return (-1);" not in source


if __name__ == "__main__":
    main()

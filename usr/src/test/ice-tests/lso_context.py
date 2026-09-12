#!/usr/bin/env python3

"""Compile and exercise the actual ICE transmit offload validation."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


TESTDIR = Path(__file__).resolve().parent
REPO = TESTDIR.parents[3]
DRIVER = REPO / "usr/src/uts/common/io/ice"


def extract(path: Path, pattern: str) -> str:
    source = path.read_text(encoding="utf-8")
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise ValueError(f"cannot extract {pattern!r} from {path}")
    line = source.count("\n", 0, match.start()) + 1
    return f"#line {line} {json.dumps(str(path))}\n{match.group()}\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DRIVER / "ice_tx.c")
    args = parser.parse_args()
    header = DRIVER / "ice.h"
    desc = DRIVER / "core/ice_lan_tx_rx.h"
    mac = REPO / "usr/src/uts/common/sys/mac_provider.h"
    fragments = [
        extract(header, r"^typedef struct ice_tx_ctx_t \{[\s\S]*?^} ice_tx_ctx_t;"),
        extract(args.source,
                r"^typedef enum ice_tx_build \{[\s\S]*?^} ice_tx_build_t;"),
        extract(mac, r"^typedef enum mac_ether_offload_flags \{[\s\S]*?"
                r"^} mac_ether_offload_info_t;"),
        extract(desc, r"^enum ice_tx_desc_cmd_bits \{[\s\S]*?"
                r"^#define ICE_TXD_QW1_TX_BUF_SZ_S[^\n]*"),
    ]
    definitions = (
        (header, ("ICE_TX_LSO_MIN_MSS", "ICE_TX_LSO_MAX_HDRLEN", "ICE_LSO_MAXLEN")),
        (desc, ("ICE_TXD_CTX_MAX_MSS",)),
        (DRIVER / "core/ice_defs.h", ("ICE_BYTES_PER_WORD", "ICE_BYTES_PER_DWORD")),
        (REPO / "usr/src/uts/common/sys/pattr.h",
         ("HCK_IPV4_HDRCKSUM", "HCK_PARTIALCKSUM", "HW_LSO")),
    )
    for path, names in definitions:
        for name in names:
            fragments.append(extract(path, rf"^#define\s+{name}\s+[^\n]*"))

    body = extract(args.source,
                   r"^static ice_tx_build_t\nice_tx_context\([\s\S]*?^}")
    # Permit testing the reviewed baseline, which also passed the MTU owner.
    legacy = "ice_tx_context(ice_t *ice," in body
    call = "ice_tx_context(&ice, mp, ctx)" if legacy else "ice_tx_context(mp, ctx)"
    with tempfile.TemporaryDirectory(prefix="ice-lso-context-") as tmp:
        work = Path(tmp)
        (work / "ice_tx_types.h").write_text("\n".join(fragments), encoding="utf-8")
        (work / "ice_tx_context.h").write_text(body, encoding="utf-8")
        binary = work / "lso_context"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + [
            "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            f"-DCONTEXT(mp,ctx)={call}", "-I", str(work),
            str(TESTDIR / "lso_context.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

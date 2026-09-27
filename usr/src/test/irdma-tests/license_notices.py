#!/usr/bin/env python3
"""Check that each OpenIB-licensed file's copyright notices are packaged.

The OpenIB license requires binary redistributions to reproduce the
copyright notice, so every third-party notice in such a file must appear
word for word in the THIRDPARTYLICENSE that the package ships for it.

Each OpenIB file of rdmak must also have its provenance in README.illumos:
the Linux paths it comes from with their blob IDs.  With a Linux tree at
$LINUX_TREE (or ~/workspace/linux), each recorded blob must be the one at
the pinned commit.
"""

import os
from pathlib import Path
import re
import subprocess

from irdma_test import IRDMA

RDMA = IRDMA.parent / "rdma"
OWN = "Edgecast Cloud LLC"

GROUPS = (
    (RDMA, RDMA / "THIRDPARTYLICENSE"),
    (IRDMA, IRDMA / "core" / "THIRDPARTYLICENSE"),
    (IRDMA / "core", IRDMA / "core" / "THIRDPARTYLICENSE"),
)


def notices(path):
    head = "\n".join(path.read_text(encoding="utf-8").splitlines()[:40])
    found = re.findall(r"Copyright \(c\) [^\n]*?(?:\.  All rights reserved\."
                       r"|\. All rights reserved\.|Corporation)", head)
    return [n for n in found if OWN not in n]


LINUX_COMMIT = "552c50713f273b494ac6c77052032a49bc9255e2"
PAIR = re.compile(r"((?:drivers|include)/[\w./-]+),?\s+([0-9a-f]{40})\b")


def entry(text, name):
    """The README.illumos entry whose file list names the file."""
    for m in re.finditer(r"^    (\S[^\n]*)\n(.*?)(?=^    \S|^\S|\Z)", text,
                         re.M | re.S):
        if re.search(rf"\b{re.escape(name)}\b", m.group(1)):
            return m.group(0)
    return None


def provenance():
    readme = (RDMA / "README.illumos").read_text(encoding="utf-8")
    pairs = 0
    for path in sorted(RDMA.glob("*.[ch]")):
        if "OpenIB" not in path.read_text(encoding="utf-8")[:4000]:
            continue
        e = entry(readme, path.name)
        assert e is not None, f"{path.name}: no README.illumos entry"
        found = PAIR.findall(e)
        assert found, f"{path.name}: its entry names no Linux path and blob"
        pairs += len(found)
    # A file the README does not list must be caught.
    stripped = re.sub(r"rdk_ibcm_msg\.[ch]", "x", readme)
    assert entry(stripped, "rdk_ibcm_msg.c") is None, "vacuous entry check"
    tree = Path(os.environ.get("LINUX_TREE",
                               Path.home() / "workspace" / "linux"))
    verified = 0
    if (tree / ".git").exists():
        for lpath, blob in set(PAIR.findall(readme)):
            got = subprocess.run(["git", "-C", str(tree), "rev-parse",
                                  f"{LINUX_COMMIT}:{lpath}"],
                                 capture_output=True, text=True).stdout.strip()
            assert got == blob, f"{lpath}: README.illumos has {blob}, " \
                f"the tree has {got or 'nothing'}"
            verified += 1
    return pairs, verified


def main():
    checked = 0
    for directory, license_file in GROUPS:
        text = license_file.read_text(encoding="utf-8")
        for path in sorted(directory.glob("*.[ch]")):
            if "OpenIB" not in path.read_text(encoding="utf-8")[:4000]:
                continue
            found = notices(path)
            assert found, f"{path}: OpenIB file with no copyright notice"
            for notice in found:
                assert notice in text, \
                    f"{path}: '{notice}' missing from {license_file}"
                checked += 1
    assert checked > 0
    pairs, verified = provenance()
    print(f"PASS license_notices ({checked} notices, {pairs} provenance "
          f"entries, {verified} blobs checked against Linux)")


if __name__ == "__main__":
    main()

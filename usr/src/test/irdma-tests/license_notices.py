#!/usr/bin/env python3
"""Check that each OpenIB-licensed file's copyright notices are packaged.

The OpenIB license requires binary redistributions to reproduce the
copyright notice, so every third-party notice in such a file must appear
word for word in the THIRDPARTYLICENSE that the package ships for it.
"""

import re

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
    print(f"PASS license_notices ({checked} notices)")


if __name__ == "__main__":
    main()

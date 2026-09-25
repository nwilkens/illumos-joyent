#!/usr/bin/env python3
"""Check the provenance record of the imported irdma shared code.

Every file in core/ carries the dual license line and a blob ID in
README.illumos.  A file with no illumos: marker must still hash to that blob;
the functions that hold markers must be the ones README.illumos lists.
"""

import re
import subprocess

from irdma_test import IRDMA

CORE = IRDMA / "core"
NOT_IMPORTED = {"README.illumos", "THIRDPARTYLICENSE",
                "THIRDPARTYLICENSE.descrip"}


def blob(path):
    return subprocess.run(["git", "hash-object", str(path)], check=True,
                          capture_output=True, text=True).stdout.strip()


def marked_functions(path):
    marks, function = set(), None
    for line in path.read_text(encoding="utf-8").splitlines():
        head = re.match(r"^(?:[a-z_][\w ]*\s\*?)?(irdma_\w+)\(", line)
        if head:
            function = head.group(1)
        if "illumos:" in line:
            assert function is not None, (path.name, line)
            marks.add(function)
    return marks


def main():
    readme = (CORE / "README.illumos").read_text(encoding="utf-8")
    listed = dict(re.findall(r"^    (\S+\.[ch])\s+([0-9a-f]{40})$", readme,
                             re.MULTILINE))
    files = {p.name for p in CORE.iterdir() if p.name not in NOT_IMPORTED}
    assert files == set(listed), sorted(files ^ set(listed))
    assert "552c50713f273b494ac6c77052032a49bc9255e2" in readme

    changes = readme.split("Local changes", 1)[1].split("The checks", 1)[0]
    edited = 0
    for name in sorted(files):
        path = CORE / name
        text = path.read_text(encoding="utf-8")
        first = text.splitlines()[0]
        assert re.search(r"SPDX-License-Identifier: GPL-2\.0 (OR|or) "
                         r"Linux-OpenIB", first), (name, first)
        # Only the illumos shim of etherdevice.h may come from linux/.
        for inc in re.findall(r'#include <(linux/[^>]+)>', text):
            assert inc == "linux/etherdevice.h", (name, inc)
        marks = marked_functions(path)
        if not marks:
            assert blob(path) == listed[name], (name, "modified unmarked")
            continue
        edited += 1
        section = changes.split(f"    {name}\n", 1)[1]
        section = re.split(r"\n    \S+\.[ch]\n", section, maxsplit=1)[0]
        documented = set(re.findall(r"\b(irdma_\w+)\(\)", section))
        assert marks <= documented, (name, marks - documented)
    assert (IRDMA / "linux/etherdevice.h").exists()
    lic = (CORE / "THIRDPARTYLICENSE").read_text(encoding="utf-8")
    assert "Linux-OpenIB" in lic and "Intel Corporation" in lic
    print(f"PASS: {len(files)} imported files match their blob IDs or list "
          f"their local changes ({edited} edited)")


if __name__ == "__main__":
    main()

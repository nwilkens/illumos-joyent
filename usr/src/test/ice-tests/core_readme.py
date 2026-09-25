#!/usr/bin/env python3
"""Check that core/README.illumos lists every local change to the core."""

from pathlib import Path
import re


REPO = Path(__file__).resolve().parents[4]
CORE = REPO / "usr/src/uts/common/io/ice/core"


def marked_functions():
    """Map each core file to the functions that hold an illumos: marker."""
    marks = {}
    for path in sorted(CORE.glob("*.[ch]")):
        function = None
        for line in path.read_text(encoding="utf-8").splitlines():
            head = re.match(r"^(?:[a-z_][\w ]*\s\*?)?(ice_\w+)\(", line)
            if head:
                function = head.group(1)
            if "illumos:" in line:
                assert function is not None, (path.name, line)
                marks.setdefault(path.name, set()).add(function)
    return marks


def listed_functions(readme):
    """Map each file named under "Local changes" to the functions listed."""
    section = readme.split("Local changes", 1)[1].split("usr/src/test", 1)[0]
    listed, current = {}, None
    for line in section.splitlines():
        name = re.match(r"^    (ice_\w+\.[ch])$", line)
        if name:
            current = name.group(1)
            listed[current] = set()
        elif current:
            listed[current].update(re.findall(r"\b(ice_\w+)\(\)", line))
    return listed


def main():
    readme = (CORE / "README.illumos").read_text(encoding="utf-8")
    assert "unmodified" in readme and "Re-import policy" in readme
    marks = marked_functions()
    listed = listed_functions(readme)
    assert set(marks) == set(listed), (sorted(marks), sorted(listed))
    for name, functions in marks.items():
        assert functions <= listed[name], (name, functions - listed[name])
    # Uncompiled imports are named so a refresh keeps them out of the build.
    for name in ("ice_dcb.c", "ice_vf_mbx.c"):
        assert name in readme
    print(f"PASS: README.illumos lists the local changes in {len(marks)} "
          f"core files")


if __name__ == "__main__":
    main()

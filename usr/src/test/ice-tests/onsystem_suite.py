#!/usr/bin/env python3
"""Check the on-system test-runner suite: runfiles, scripts and packaging."""

import configparser
from pathlib import Path
import re
import shutil
import subprocess

from c_test import REPO, TESTDIR


MANIFEST = REPO / "usr/src/pkg/manifests/system-test-icetest.p5m"


def installed_tests():
    """Map each installed test name to its source file."""
    make = (TESTDIR / "tests/Makefile").read_text()
    progs = re.search(r"^PROGS = (.*)$", make, re.MULTILINE).group(1).split()
    scripts = re.search(r"^SCRIPTS = (.*)$", make,
                        re.MULTILINE).group(1).split()
    found = {f"{p}.64": TESTDIR / "tests" / f"{p}.c" for p in progs}
    found.update({s: TESTDIR / "tests" / f"{s}.ksh" for s in scripts})
    found["datapath_accept"] = TESTDIR / "datapath_accept.sh"
    return found


def main():
    tests = installed_tests()
    for name, source in tests.items():
        assert source.is_file(), (name, source)

    manifest = MANIFEST.read_text()
    for name in tests:
        assert f"file path=opt/ice-tests/tests/{name} mode=0555" in manifest
    assert "file path=opt/ice-tests/bin/icetest mode=0555" in manifest

    runs = sorted((TESTDIR / "runfiles").glob("*.run"))
    assert {r.name for r in runs} == {"default.run", "datapath.run"}
    listed = set()
    for run in runs:
        assert f"file path=opt/ice-tests/runfiles/{run.name} mode=0444" \
            in manifest
        cfg = configparser.ConfigParser()
        cfg.read(run)
        assert cfg["DEFAULT"]["user"] == "root"
        for section in cfg.sections():
            assert section == "/opt/ice-tests/tests", section
            for test in eval(cfg[section]["tests"]):
                assert test in tests, (run.name, test)
                listed.add(test)
    # Every installed test runs from some runfile except the helper script.
    assert listed == set(tests) - {"datapath_accept"}, listed

    # Tests without their device or peer report SKIP (exit 4).
    for name, source in tests.items():
        if name == "datapath_accept":
            continue
        text = source.read_text()
        assert "SKIP" in text and ("exit 4" in text or "return (4)" in text), \
            name

    top = (REPO / "usr/src/test/Makefile").read_text()
    assert re.search(r"^\tice-tests \\$", top, re.MULTILINE)

    ksh = shutil.which("ksh")
    for script in [TESTDIR / "cmd/icetest.ksh",
                   *(p for p in tests.values() if p.suffix == ".ksh")]:
        if ksh is not None:
            subprocess.run([ksh, "-n", str(script)], check=True)
    bash = shutil.which("bash")
    if bash is not None:
        subprocess.run([bash, "-n", str(TESTDIR / "datapath_accept.sh")],
                       check=True)
    print(f"PASS: {len(tests)} on-system tests are wired and packaged")


if __name__ == "__main__":
    main()

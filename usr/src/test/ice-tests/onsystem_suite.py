#!/usr/bin/env python3
"""Check the on-system test-runner suite: runfiles, scripts and packaging."""

import configparser
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

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


KSTAT_STUB = """#!/bin/sh
case "$2" in
*:fm:*) grep "^$2	" "$(dirname "$0")/fm" ;;
*) printf '%s\\t0\\n' "$2" ;;
esac
"""


def check_attach_fma(ksh):
    """attach fails when an FMA counter kstat is missing or nonzero."""
    def run(fm):
        with tempfile.TemporaryDirectory(prefix="ice-attach-") as tmp:
            bindir = Path(tmp)
            (bindir / "modinfo").write_text("#!/bin/sh\necho ' 1 ice (x)'\n")
            (bindir / "kstat").write_text(KSTAT_STUB)
            (bindir / "fm").write_text(
                "".join(f"ice:0:fm:{k}\t{v}\n" for k, v in fm.items()))
            for name in ("modinfo", "kstat"):
                (bindir / name).chmod(0o755)
            env = dict(os.environ, PATH=f"{bindir}:/usr/bin:/bin",
                       ICE_TEST_LINK="net0", ICE_TEST_DEVICE="ice0")
            return subprocess.run([ksh, str(TESTDIR / "tests/attach.ksh")],
                                  env=env, capture_output=True, text=True,
                                  timeout=30)

    clean = {"acc_err": 0, "dma_err": 0, "erpt_dropped": 0}
    result = run(clean)
    assert result.returncode == 0, result.stdout + result.stderr
    for name in clean:
        missing = {k: v for k, v in clean.items() if k != name}
        result = run(missing)
        assert result.returncode == 1 and "missing" in result.stderr, name
        result = run(dict(clean, **{name: 2}))
        assert result.returncode == 1 and f"{name} is 2" in result.stderr


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
    if ksh is not None:
        check_attach_fma(ksh)
    bash = shutil.which("bash")
    if bash is not None:
        subprocess.run([bash, "-n", str(TESTDIR / "datapath_accept.sh")],
                       check=True)
    print(f"PASS: {len(tests)} on-system tests are wired and packaged")


if __name__ == "__main__":
    main()

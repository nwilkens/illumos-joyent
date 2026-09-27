"""Build whole rdmak sources on the host against rdk_kenv.h and run them."""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from irdma_test import REPO, TESTDIR

RDMA = REPO / "usr/src/uts/common/io/rdma"
SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]


class HostFailure(RuntimeError):
    """A compile or run failed; the message has the output."""


def source(name, replace=()):
    """An rdmak file without its #include lines, with (old, new) edits."""
    path = RDMA / name
    text = path.read_text(encoding="utf-8")
    for old, new in replace:
        assert old in text, f"{name}: {old!r}"
        text = text.replace(old, new)
    text = re.sub(r"^#include [<\"].*$", "", text, flags=re.MULTILINE)
    return f"#line 1 {json.dumps(str(path))}\n{text}\n"


def unit(names, replace=None):
    """rdk.h, rdk_impl.h and the named .c files as one translation unit."""
    replace = replace or {}
    parts = ['#include "rdk_kenv.h"']
    for name in ("rdk.h", "rdk_impl.h", *names):
        parts.append(source(name, replace.get(name, ())))
    return "\n".join(parts)


def _run(command, phase, timeout):
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise HostFailure(f"{phase}: timed out after {timeout}s") from error
    if result.returncode != 0:
        raise HostFailure(f"{phase}: exited {result.returncode}: "
                          f"{shlex.join(command)}\n"
                          f"{result.stdout}{result.stderr}")
    return result.stdout


def run(test, names, replace=None, args=(), timeout=120):
    """Compile test (a .c in TESTDIR that includes "rdk_unit.h") with the
    sanitizers when the compiler has them, run it and return its output."""
    with tempfile.TemporaryDirectory(prefix="rdk-host-") as tmp:
        work = Path(tmp)
        (work / "rdk_unit.h").write_text(unit(names, replace),
                                         encoding="utf-8")
        binary = work / "t"
        cc = shlex.split(os.environ.get("CC", "cc"))
        base = cc + ["-std=gnu11", "-g", "-O1", "-pthread", "-Wall",
                     "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-Wno-unused-function", "-Wno-missing-field-initializers",
                     "-Wno-sign-compare", "-I", str(work), "-I", str(TESTDIR),
                     str(TESTDIR / test), "-o", str(binary)]
        try:
            _run(base[:len(cc)] + SANITIZE + base[len(cc):], "compile", 120)
        except HostFailure:
            _run(base, "compile", 120)
        return _run([str(binary), *args], "run", timeout)

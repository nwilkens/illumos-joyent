"""Shared support for the rdma CM checks: paths and a C test runner."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

TESTDIR = Path(__file__).resolve().parent
REPO = TESTDIR.parents[3]
RDMA = REPO / "usr/src/uts/common/io/rdma"
IWC = REPO / "usr/src/uts/common/io/iwcxgbe"
T4NEX = REPO / "usr/src/uts/common/io/cxgbe/t4nex"
CXGBE_TESTS = REPO / "usr/src/test/cxgbe-tests"

# illumos has boolean_t in <sys/types.h>; other hosts need it here.
COMPAT = """#include <sys/types.h>
#include <stdint.h>
#if !defined(__sun) && !defined(__illumos__)
typedef enum { B_FALSE = 0, B_TRUE = 1 } boolean_t;
#endif
"""

SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]


class CTestFailure(RuntimeError):
    pass


def _run(command, phase, timeout):
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise CTestFailure(f"{phase}: timed out: {shlex.join(command)}") \
            from error
    if result.returncode != 0:
        raise CTestFailure(f"{phase}: exited {result.returncode}: "
                           f"{shlex.join(command)}\n"
                           f"{result.stdout}{result.stderr}")
    return result.stdout


def run_c(sources, files, includes=(), args=(), timeout=120):
    """Compile the sources with the generated files beside them, with the
    sanitizers when the compiler has them, and run the program."""
    with tempfile.TemporaryDirectory(prefix="rdma-cm-") as tmp:
        work = Path(tmp)
        files = dict(files, **{"compat.h": COMPAT})
        for name, text in files.items():
            (work / name).write_text(text, encoding="utf-8")
        binary = work / "t"
        cc = shlex.split(os.environ.get("CC", "cc"))
        base = cc + ["-std=gnu99", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                     "-include", str(work / "compat.h"), "-I", str(work)]
        base += [f"-I{d}" for d in includes]
        base += [str(work / s) if isinstance(s, str) else str(s)
                 for s in sources] + ["-o", str(binary)]
        try:
            _run(base[:len(cc)] + SANITIZE + base[len(cc):], "compile", 120)
            mode = "with sanitizers"
        except CTestFailure:
            _run(base, "compile", 120)
            mode = "without sanitizers"
        out = _run([str(binary), *args], "run", timeout)
        return out, mode

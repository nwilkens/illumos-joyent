"""Small shared support for compiling and running extracted driver C bodies."""

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


class CTestFailure(RuntimeError):
    """Identify whether a selected source failed to compile or to run."""

    def __init__(self, phase, detail):
        self.phase = phase
        super().__init__(f"{phase}: {detail}")


def extract(source, pattern, path):
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise ValueError(f"cannot extract {pattern!r} from {path}")
    line = source.count("\n", 0, match.start()) + 1
    return f"#line {line} {json.dumps(str(path))}\n{match.group()}\n"


def extract_file(path, pattern):
    return extract(path.read_text(encoding="utf-8"), pattern, path)


def _execute(command, phase, timeout):
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise CTestFailure(phase, f"timed out after {timeout}s: "
                           f"{shlex.join(command)}") from error
    except OSError as error:
        raise CTestFailure(phase, str(error)) from error
    if result.returncode != 0:
        raise CTestFailure(phase, f"exited {result.returncode}: "
                           f"{shlex.join(command)}\n"
                           f"{result.stdout}{result.stderr}")
    print(result.stdout, end="")
    print(result.stderr, end="")


def run_c(source, headers, *, cflags=(), cases=((),), timeout=15,
          compile_timeout=30):
    """Compile once with generated headers, then execute each argument tuple."""
    with tempfile.TemporaryDirectory(prefix=f"ice-{source.stem}-") as tmp:
        work = Path(tmp)
        for name, text in headers.items():
            (work / name).write_text(text, encoding="utf-8")
        binary = work / source.stem
        compiler = shlex.split(os.environ.get("CC", "cc"))
        command = compiler + ["-std=c99", "-Wall", "-Wextra", "-Werror",
                              "-pedantic", *cflags, "-I", str(work),
                              str(source), "-o", str(binary)]
        _execute(command, "compile", compile_timeout)
        for arguments in cases:
            _execute([str(binary), *arguments], "run", timeout)

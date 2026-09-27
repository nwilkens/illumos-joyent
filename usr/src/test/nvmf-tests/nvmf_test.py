"""Compile kernel functions from the nvmf sources into host test programs."""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

TESTDIR = Path(__file__).resolve().parent
SRC = TESTDIR.parents[1]
UTS = SRC / "uts/common"
NVMF = UTS / "io/nvmf"
NVMFT = UTS / "io/comstar/port/nvmft"
NVMF_H = UTS / "sys/nvme/nvmf.h"
NVME_H = UTS / "sys/nvme.h"
NVME_REG_H = UTS / "io/nvme/nvme_reg.h"

SANITIZE = ("-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", "-g")


def _mark(text, path, offset):
    line = text.count("\n", 0, offset) + 1
    return f"#line {line} {json.dumps(str(path))}\n"


def function(path, name):
    """One function definition, from its return type line to its brace."""
    text = path.read_text(encoding="utf-8")
    m = re.search(rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}\n", text,
                  re.MULTILINE)
    assert m is not None, f"{name} not in {path}"
    return _mark(text, path, m.start()) + m.group(0)


def typedef(path, name):
    """A 'typedef struct|union|enum { ... } name;' block."""
    text = path.read_text(encoding="utf-8")
    end = re.search(rf"^}} {name};\n", text, re.MULTILINE)
    assert end is not None, f"{name} not in {path}"
    start = text.rfind("\ntypedef ", 0, end.start()) + 1
    return _mark(text, path, start) + text[start:end.end()]


def struct(path, name):
    """A 'struct name { ... };' block."""
    text = path.read_text(encoding="utf-8")
    m = re.search(rf"^struct {name} {{\n[\s\S]*?^}};\n", text, re.MULTILINE)
    assert m is not None, f"struct {name} not in {path}"
    return _mark(text, path, m.start()) + m.group(0)


def define(path, name):
    """One #define, with its continuation lines."""
    text = path.read_text(encoding="utf-8")
    m = re.search(rf"^#define\s+{name}\b(?:[^\n]*\\\n)*[^\n]*\n", text,
                  re.MULTILINE)
    assert m is not None, f"{name} not in {path}"
    return m.group(0)


def run_c(source, headers, *, cflags=(), cases=((),), timeout=120):
    """Build source with the generated headers under ASan/UBSan and run it."""
    with tempfile.TemporaryDirectory(prefix=f"nvmf-{source.stem}-") as tmp:
        work = Path(tmp)
        for name, text in headers.items():
            (work / name).write_text(text, encoding="utf-8")
        binary = work / source.stem
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmd = cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-parameter",
                    *SANITIZE, *cflags, "-I", str(work), str(source),
                    "-o", str(binary), "-lpthread"]
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=120)
        if result.returncode != 0:
            raise SystemExit(f"compile failed: {shlex.join(cmd)}\n"
                             f"{result.stdout}{result.stderr}")
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0",
                   UBSAN_OPTIONS="print_stacktrace=1")
        for args in cases:
            result = subprocess.run([str(binary), *args], capture_output=True,
                                    text=True, timeout=timeout, env=env)
            print(result.stdout, end="")
            if result.returncode != 0:
                raise SystemExit(f"{source.stem} {' '.join(args)} exited "
                                 f"{result.returncode}\n{result.stderr}")

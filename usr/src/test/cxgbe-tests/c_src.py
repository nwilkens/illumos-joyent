"""Shared support: C source parsing and compiling extracted driver code."""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


TESTDIR = Path(__file__).resolve().parent
REPO = TESTDIR.parents[3]
T4NEX = REPO / "usr/src/uts/common/io/cxgbe/t4nex"
COMMON = REPO / "usr/src/uts/common/io/cxgbe/common"

# The offload core, as opposed to the NIC code it sits beside.
OFLD_FILES = ("t4_ofld.c", "t4_ofld_sge.c", "t4_ofld_cpl.c",
              "t4_ofld_orphan.c", "t4_ofld_ops.c", "t4_ofld_dma.c",
              "t4_rdma_peer.c", "t4_ofld_test.c", "t4_tid.c", "t4_l2t.c",
              "t4_clip.c")

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do",
            "case", "else", "goto", "defined", "offsetof"}


def strip(text):
    """Blank comments, strings and preprocessor lines, keeping offsets."""
    out = []
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c == "#":
            j = i
            while j < n:
                if text[j] == "\n" and text[j - 1] != "\\":
                    break
                j += 1
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        if text.startswith("/*", i):
            j = text.index("*/", i + 2) + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * (j - i - 1) + c)
            i = j + 1
            line_start = False
            continue
        out.append(c)
        line_start = c == "\n" or (line_start and c in " \t")
        i += 1
    return "".join(out)


def functions(text):
    """Map each function defined at file scope to its body text."""
    code = strip(text)
    found = {}
    depth = 0
    start = head = 0
    name = None
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                start = i
                names = [x for x in re.findall(r"(\w+)\s*\(", code[head:i])
                         if x not in KEYWORDS]
                is_func = bool(names) and code[head:i].rstrip().endswith(")")
                name = names[0] if is_func else None
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                if name is not None:
                    found[name] = code[start:i + 1]
                head = i + 1
        elif c == ";" and depth == 0:
            head = i + 1
    return found


def calls(body):
    return {name for name in re.findall(r"\b(\w+)\s*\(", body)
            if name not in KEYWORDS}


def extract(path, pattern):
    """The source matching pattern, with a #line directive for diagnostics."""
    source = path.read_text(encoding="utf-8")
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError(f"cannot extract {pattern!r} from {path}")
    line = source.count("\n", 0, match.start()) + 1
    return f"#line {line} {json.dumps(str(path))}\n{match.group()}\n"


def function_source(path, name):
    """The full definition of a file-scope function, return type included."""
    source = path.read_text(encoding="utf-8")
    match = re.search(rf"^{name}\(", source, re.MULTILINE)
    if match is None:
        raise ValueError(f"cannot find {name} in {path}")
    start = source.rfind("\n", 0, match.start() - 1) + 1
    code = strip(source)
    depth = 0
    for i in range(code.index("{", match.start()), len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                line = source.count("\n", 0, start) + 1
                return (f"#line {line} {json.dumps(str(path))}\n"
                        f"{source[start:i + 1]}\n")
    raise ValueError(f"unterminated {name} in {path}")


class CTestFailure(RuntimeError):
    pass


def _execute(command, phase, timeout):
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
    print(result.stdout, end="")


def run_c(source, files, *, cases=((),), timeout=15):
    """Compile source with the generated files beside it, then run each case."""
    with tempfile.TemporaryDirectory(prefix=f"cxgbe-{source.stem}-") as tmp:
        work = Path(tmp)
        for name, text in files.items():
            dest = work / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(text, encoding="utf-8")
        binary = work / source.stem
        compiler = shlex.split(os.environ.get("CC", "cc"))
        _execute(compiler + ["-std=gnu99", "-Wall", "-Wextra", "-Werror",
                             "-Wno-unused-function", "-I", str(work),
                             str(source), "-o", str(binary)], "compile", 60)
        for arguments in cases:
            _execute([str(binary), *arguments], "run", timeout)

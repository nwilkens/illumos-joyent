"""Extract mlxcx driver code and run it against host stubs."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


TESTDIR = Path(__file__).resolve().parent
REPO = TESTDIR.parents[3]
DRIVER = REPO / "usr/src/uts/common/io/mlxcx"


class CTestFailure(RuntimeError):
    """Identify whether a selected source failed to compile or to run."""

    def __init__(self, phase, detail):
        self.phase = phase
        super().__init__(f"{phase}: {detail}")


def source_parser(doc):
    parser = argparse.ArgumentParser(description=doc)
    parser.add_argument("--source-dir", type=Path, default=DRIVER,
                        help="mlxcx source directory to test")
    parser.add_argument("--scenario", action="append",
                        help="run only this scenario (repeatable)")
    return parser


def _located(text, start, end, path):
    line = text.count("\n", 0, start) + 1
    return f"#line {line} {json.dumps(str(path))}\n{text[start:end]}\n"


def function(path, name):
    """Return one top-level function definition in illumos style."""
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"^[A-Za-z_][\w \t\*]*\n{re.escape(name)}\("
                      r"[\s\S]*?\n}\n", text, re.MULTILINE)
    if match is None:
        raise CTestFailure("extract", f"no function {name} in {path}")
    return _located(text, match.start(), match.end(), path)


def optional_function(path, name):
    try:
        return function(path, name)
    except CTestFailure:
        return ""


def region(path, first, last):
    """Return every line from function first through function last."""
    text = path.read_text(encoding="utf-8")
    start = re.search(rf"^[A-Za-z_][\w \t\*]*\n{re.escape(first)}\(",
                      text, re.MULTILINE)
    end = re.search(rf"^{re.escape(last)}\([\s\S]*?\n}}\n", text,
                    re.MULTILINE)
    if start is None or end is None or end.end() <= start.start():
        raise CTestFailure("extract", f"no region {first}..{last} in {path}")
    return _located(text, start.start(), end.end(), path)


def typedef(path, name):
    """Return the typedef that ends with '} name;', or '' if absent."""
    text = path.read_text(encoding="utf-8")
    end = re.search(rf"^}} {re.escape(name)};\n", text, re.MULTILINE)
    if end is None:
        return ""
    start = text.rfind("\ntypedef ", 0, end.start()) + 1
    return _located(text, start, end.end(), path)


def struct(path, name):
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"^struct {re.escape(name)} {{[\s\S]*?^}};\n", text,
                      re.MULTILINE)
    if match is None:
        raise CTestFailure("extract", f"no struct {name} in {path}")
    return _located(text, match.start(), match.end(), path)


def define(path, name):
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"^#define\t{re.escape(name)}\b[^\n]*\n", text,
                      re.MULTILINE)
    if match is None:
        raise CTestFailure("extract", f"no #define {name} in {path}")
    return _located(text, match.start(), match.end(), path)


FORWARDS = """
struct mlxcx;
typedef struct mlxcx mlxcx_t;
typedef struct mlxcx_cmd mlxcx_cmd_t;
typedef struct mlxcx_port mlxcx_port_t;
"""


def mlxcx_types(srcdir):
    """The small part of mlxcx.h that the command and page code needs."""
    header = srcdir / "mlxcx.h"
    parts = [FORWARDS]
    for name in ("MLXCX_REG_NUMBER", "MLXCX_CMD_DMA_PAGE_SIZE",
                 "MLXCX_CMD_REVISION", "MLXCX_HW_PAGE_SIZE",
                 "MLXCX_FUNC_ID_MAX"):
        parts.append(define(header, name))
    for name in ("mlxcx_pages_request_t", "mlxcx_async_param_t",
                 "mlxcx_dma_buffer_flags_t", "mlxcx_dma_buffer_t",
                 "mlxcx_dev_page_t", "mlxcx_cmd_queue_status_t",
                 "mlxcx_cmd_abandon_t", "mlxcx_cmd_queue_t",
                 "mlxcx_cmd_mbox_t", "mlxcx_bf_t", "mlxcx_uar_t",
                 "mlxcx_cmd_state_t", "mlxcx_hca_cap_t"):
        parts.append(typedef(header, name))
    parts.append(struct(header, "mlxcx_cmd"))
    return "\n".join(parts)


def _execute(command, phase, timeout, quiet=False):
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
    if not quiet:
        print(result.stdout, end="")
        print(result.stderr, end="")


def run_c(source, headers, srcdir, *, cases=((),), timeout=30):
    """Compile once with generated headers, then run each argument tuple."""
    with tempfile.TemporaryDirectory(prefix=f"mlxcx-{source.stem}-") as tmp:
        work = Path(tmp)
        for name, text in headers.items():
            (work / name).write_text(text, encoding="utf-8")
        binary = work / source.stem
        compiler = shlex.split(os.environ.get("CC", "cc"))
        command = compiler + ["-std=gnu11", "-g", "-Wall", "-Werror",
                              "-Wno-unused-function",
                              "-Wno-unused-variable",
                              "-Wno-unused-but-set-variable",
                              "-Wno-unknown-warning-option",
                              "-Wno-gnu-variable-sized-type-not-at-end",
                              "-Wno-address-of-packed-member",
                              "-fsanitize=address,undefined",
                              "-fno-sanitize-recover=all",
                              "-I", str(work), "-I", str(TESTDIR),
                              "-I", str(TESTDIR / "include"),
                              "-I", str(srcdir),
                              str(source), "-o", str(binary)]
        _execute(command, "compile", 120)
        for arguments in cases:
            _execute([str(binary), *arguments], "run", timeout)


def scenarios(args, names):
    chosen = args.scenario or list(names)
    for name in chosen:
        if name not in names:
            raise SystemExit(f"unknown scenario {name!r}")
    return tuple((name,) for name in chosen)

"""Shared support for the irdma checks; the C harness comes from ice-tests."""

from pathlib import Path
import re
import sys

TESTDIR = Path(__file__).resolve().parent
REPO = TESTDIR.parents[3]
IRDMA = REPO / "usr/src/uts/common/io/irdma"
ICE = REPO / "usr/src/uts/common/io/ice"
sys.path.insert(0, str(REPO / "usr/src/test/ice-tests"))

from c_test import extract, run_c  # noqa: E402,F401


def function(path, name):
    """Extract one function definition, return type line included."""
    return extract(path.read_text(encoding="utf-8"),
                   rf"^(?:static )?[\w *]+\n{name}\([\s\S]*?^}}", path)


def body(text, name):
    """The text of one function, from its name to its closing brace."""
    start = re.search(rf"^{name}\(", text, re.MULTILINE)
    assert start is not None, name
    end = text.index("\n}\n", start.start())
    return text[start.start():end]

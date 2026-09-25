#!/usr/bin/env python3
"""Exercise the portable runners with real compiler and child processes."""

from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import shlex
import sys
import tempfile
import unittest
from unittest.mock import patch

from c_test import CTestFailure, extract, run_c
from run_tests import TESTS, run_suite


class RunnerChecks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ice-runner-check-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)

    def source(self, text):
        path = self.work / "probe.c"
        path.write_text(text)
        return path

    def test_headers_flags_arguments_and_cases(self):
        source = self.source('''#include "value.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (VALUE != 17 || FLAG != 23 || FROM_CC != 31 || argc != 2 ||
        (strcmp(argv[1], "first") != 0 && strcmp(argv[1], "second") != 0))
        return (1);
    (void) puts(argv[1]);
    return (0);
}
''')
        compiler = os.environ.get("CC", "cc") + " -DFROM_CC=31"
        output = io.StringIO()
        with patch.dict(os.environ, {"CC": compiler}), redirect_stdout(output):
            run_c(source, {"value.h": "#define VALUE 17\n"},
                  cflags=("-DFLAG=23",), cases=(("first",), ("second",)))
        self.assertEqual(output.getvalue(), "first\nsecond\n")

    def test_compile_failure_is_not_runtime_failure(self):
        with self.assertRaises(CTestFailure) as caught:
            run_c(self.source("int main(void) { invalid C; }"), {})
        self.assertEqual(caught.exception.phase, "compile")
        self.assertIn("exited", str(caught.exception))

    def test_runtime_failure_keeps_exit_status(self):
        with self.assertRaises(CTestFailure) as caught:
            run_c(self.source("int main(void) { return (7); }"), {})
        self.assertEqual(caught.exception.phase, "run")
        self.assertIn("exited 7", str(caught.exception))

    def test_runtime_timeout_is_reported(self):
        source = self.source("int main(void) { for (;;) {} }")
        with self.assertRaises(CTestFailure) as caught:
            run_c(source, {}, timeout=0.1)
        self.assertEqual(caught.exception.phase, "run")
        self.assertIn("timed out", str(caught.exception))

    def test_compile_timeout_is_reported(self):
        compiler = shlex.join([sys.executable, "-c", "while True: pass"])
        with patch.dict(os.environ, {"CC": compiler}):
            with self.assertRaises(CTestFailure) as caught:
                run_c(self.source("int main(void) { return (0); }"), {},
                      compile_timeout=0.1)
        self.assertEqual(caught.exception.phase, "compile")
        self.assertIn("timed out", str(caught.exception))

    def test_source_locations_are_preserved(self):
        text = "/* prelude */\n\nstatic int\nvalue(void)\n{\n\treturn (1);\n}\n"
        body = extract(text, r"^static int[\s\S]*?^}", Path("original.c"))
        self.assertTrue(body.startswith('#line 3 "original.c"\n'))
        self.assertIn("return (1);", body)
        with self.assertRaises(ValueError):
            extract(text, r"^missing", Path("original.c"))

    def test_suite_continues_and_reports_failures(self):
        marker = self.work / "finished"
        (self.work / "fail.py").write_text("raise SystemExit(9)\n")
        (self.work / "pass.py").write_text(
            f"from pathlib import Path\nPath({str(marker)!r}).touch()\n")
        output = io.StringIO()
        with redirect_stdout(output):
            status = run_suite(self.work, ("fail.py", "pass.py"))
        self.assertEqual(status, 1)
        self.assertTrue(marker.exists())
        self.assertIn("FAIL fail.py (exit 9)", output.getvalue())
        self.assertIn("PASS pass.py", output.getvalue())
        self.assertIn("1/2 passed", output.getvalue())

    def test_suite_timeout_does_not_skip_later_tests(self):
        (self.work / "hang.py").write_text("while True: pass\n")
        (self.work / "pass.py").write_text("pass\n")
        output = io.StringIO()
        with redirect_stdout(output):
            status = run_suite(self.work, ("hang.py", "pass.py"), timeout=2)
        self.assertEqual(status, 1)
        self.assertIn("TIMEOUT hang.py", output.getvalue())
        self.assertIn("PASS pass.py", output.getvalue())

    def test_suite_success_and_support_exclusion(self):
        (self.work / "pass.py").write_text("pass\n")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(run_suite(self.work, ("pass.py",)), 0)
        self.assertNotIn("c_test.py", TESTS)
        self.assertNotIn("rx_test.py", TESTS)
        self.assertNotIn("run_tests.py", TESTS)
        self.assertEqual(len(TESTS), len(set(TESTS)))


if __name__ == "__main__":
    unittest.main()

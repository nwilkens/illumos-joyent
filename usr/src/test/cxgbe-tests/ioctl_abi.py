#!/usr/bin/env python3
"""Check that the offload test ioctl has one layout for ILP32 and LP64.

Every field of t4_ofld_test_t before the connection array is a 32-bit or
smaller type, the array starts on an 8-byte boundary through explicit
padding, and each element keeps its 64-bit counters 8-byte aligned, so both
data models see the same offsets.  The layout is compiled and checked here.
"""

import sys

from c_src import CTestFailure, T4NEX, TESTDIR, run_c


def main():
    text = (T4NEX / "t4nex.h").read_text(encoding="utf-8")
    first = text.index("#define\tT4_OFLD_TEST_NCONN")
    end = text.index("} t4_ofld_test_t;") + len("} t4_ofld_test_t;")
    body = text[text.index("typedef struct t4_ofld_test {"):
                text.index("t4_ofld_test_conn_t tot_conn")]
    for word in ("uint64_t", "long", "size_t", "void *", "caddr_t"):
        if word in body:
            print(f"t4_ofld_test_t has a {word} before the connection array")
            return 1
    try:
        run_c(TESTDIR / "ioctl_abi.c",
              {"ofld_test.h": "#include <stdint.h>\n" + text[first:end] +
               "\n"})
    except CTestFailure as error:
        print(error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

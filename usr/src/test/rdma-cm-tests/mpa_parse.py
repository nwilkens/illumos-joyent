#!/usr/bin/env python3
"""Run the iwcxgbe MPA start frame parser against malformed and random peer
input: bad keys, revisions and lengths, enhanced frames without their
parameters, bytes past the frame, oversize input, and a seeded fuzz run.
Every input is fed whole, a byte at a time and in random pieces.  Parsers
with a check taken out must fail."""

import sys

from cm_test import CTestFailure, IWC, TESTDIR, run_c

# Each mutation drops one check the peer could otherwise get past.
MUTATIONS = (
    ("\t\tif (plen < IWC_MPA_V2_LEN)\n\t\t\treturn (EPROTO);\n", ""),
    ("\tif (plen > IWC_MPA_MAX_PDATA)\n\t\treturn (EPROTO);\n", ""),
    ("\tif (rx->mr_len > IWC_MPA_HDR_LEN + plen)\n\t\treturn (EPROTO);\n",
     ""),
    ("\tif (b[17] < 1 || b[17] > 2)\n", "\tif (b[17] > 2)\n"),
)


def run(parser_source, iters):
    files = {"iwc_mpa.c": parser_source}
    return run_c([TESTDIR / "mpa_parse.c", "iwc_mpa.c"], files,
                 includes=[IWC], args=[str(iters)])


def main():
    real = (IWC / "iwc_mpa.c").read_text(encoding="utf-8")
    try:
        out, mode = run(real, 200000)
    except CTestFailure as error:
        print(error)
        return 1
    print(out, end="")
    for old, new in MUTATIONS:
        if real.count(old) != 1:
            print(f"mutation anchor not found: {old!r}")
            return 1
        try:
            run(real.replace(old, new), 20000)
        except CTestFailure:
            continue
        print(f"a parser without {old.strip()!r} passed")
        return 1
    print(f"PASS: MPA parser ({mode}), {len(MUTATIONS)} mutations caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())

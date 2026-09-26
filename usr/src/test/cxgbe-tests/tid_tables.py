#!/usr/bin/env python3
"""Run the real TID tables (t4_tid.c) against stub kernel headers.

The atid free list must hand a freed ID out last, IPv6 servers must take an
aligned stid pair whose odd half is not an ID, and a chip-assigned TID must be
range checked, claimable without allocation only when free or on its way to
release, held only by its owner from its own queue, and walkable.  Unanswered
SYNs are capped.
"""

from pathlib import Path
import re
import sys

from c_src import CTestFailure, T4NEX, TESTDIR, run_c


def tid_header():
    """t4_ofld.h for the test: the TID types from the real header."""
    real = (T4NEX / "t4_ofld.h").read_text(encoding="utf-8")
    parts = []
    for pattern in (r"#define\tT4_OFLD_M_TID.*?\n",
                    r"#define\tT4_OFLD_MAX_EMBRYOS.*?\n",
                    r"typedef enum t4_tid_kind \{.*?\} t4_tid_kind_t;",
                    r"typedef enum t4_tid_state \{.*?\} t4_tid_state_t;",
                    r"#define\tTEF_V6.*?#define\tTEF_RELEASING[^\n]*\n",
                    r"#define\tT4_TID_NIL.*?\n",
                    r"typedef struct t4_tid_ent \{.*?\} t4_tid_ent_t;",
                    r"typedef struct t4_tid_tab \{.*?\} t4_tid_tab_t;",
                    r"typedef struct t4_tids \{.*?\} t4_tids_t;"):
        match = re.search(pattern, real, re.DOTALL)
        if match is None:
            raise ValueError(f"t4_ofld.h lacks {pattern!r}")
        parts.append(match.group())
    return ("#include \"kstub.h\"\n"
            "struct adapter { int intr_pri; };\n" + "\n".join(parts) + "\n"
            "typedef struct t4_ofld {\n"
            "\tstruct adapter *of_sc;\n"
            "\tuint32_t of_natids, of_nstids, of_stid_base;\n"
            "\tuint32_t of_ntids, of_tid_base;\n"
            "\tt4_tids_t of_tids;\n"
            "} t4_ofld_t;\n")


def main():
    source = (T4NEX / "t4_tid.c").read_text(encoding="utf-8")
    under_test = f"#line 1 \"{T4NEX / 't4_tid.c'}\"\n" + source
    files = {
        "t4_ofld.h": tid_header(),
        "kstub.h": (TESTDIR / "kstub.h").read_text(encoding="utf-8"),
        "t4_tid_under_test.c": under_test,
        "sys/ddi.h": "",
        "sys/sunddi.h": "",
        "sys/sysmacros.h": "",
    }
    try:
        run_c(TESTDIR / "tid_tables.c", files)
    except CTestFailure as error:
        print(error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

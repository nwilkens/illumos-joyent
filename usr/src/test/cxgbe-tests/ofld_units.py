#!/usr/bin/env python3
"""Run the firmware range checks and the completion waiters on the host.

t4_ofld_range() and t4_ofld_overlap() decide which firmware-reported regions
offload will use: an empty, wrapping, out-of-limit, misaligned or 4 GB range
is refused.  The work request completion waiters take cookies back from the
firmware: a reply must name a live slot with its generation and the t4nex
bit, a duplicate or forged reply changes nothing, and a reply for an
abandoned request frees the slot rather than completing a newer one.
"""

import re
import sys

from c_src import CTestFailure, T4NEX, TESTDIR, function_source, run_c


def header_part(pattern):
    real = (T4NEX / "t4_ofld.h").read_text(encoding="utf-8")
    match = re.search(pattern, real, re.DOTALL)
    if match is None:
        raise ValueError(f"t4_ofld.h lacks {pattern!r}")
    return match.group()


def units_header():
    rdma = (T4NEX / "t4_rdma.h").read_text(encoding="utf-8")
    rng = re.search(r"typedef struct t4_rdma_range \{.*?\} t4_rdma_range_t;",
                    rdma, re.DOTALL).group()
    return "\n".join([
        '#include "kstub.h"',
        "#define BE_64(x) __builtin_bswap64(x)",
        "#define FW6_TYPE_WR_RPL 1",
        "#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))",
        "static inline int\ncv_timedwait(kcondvar_t *c, kmutex_t *m, "
        "clock_t t)\n{\n\t(void) c; (void) m; (void) t;\n\treturn (-1);\n}",
        "static inline clock_t ddi_get_lbolt(void) { return (0); }",
        "static inline clock_t drv_usectohz(clock_t u) { return (u); }",
        "struct cpl_fw6_msg {\n\tuint8_t opcode;\n\tuint8_t type;\n"
        "\tuint16_t rsvd0;\n\tuint32_t rsvd1;\n\tuint64_t data[4];\n};",
        rng,
        header_part(r"#define\tT4_OFLD_NWAITERS.*?\n"),
        header_part(r"typedef enum t4_waiter_state \{.*?\} "
                    r"t4_waiter_state_t;"),
        header_part(r"typedef struct t4_ofld_waiter \{.*?\} "
                    r"t4_ofld_waiter_t;"),
        header_part(r"#define\tT4_OFLD_COOKIE_PARENT.*?\n"),
        "typedef struct {\n\tuint64_t os_wr_badcookie;\n} t4_ofld_stats_t;",
        "typedef struct t4_ofld {\n\tkmutex_t of_wlock;\n"
        "\tkcondvar_t of_wcv;\n\tuint32_t of_wgen;\n"
        "\tt4_ofld_waiter_t of_waiter[T4_OFLD_NWAITERS];\n"
        "\tt4_ofld_stats_t of_stats;\n} t4_ofld_t;",
        "#define T4_OFLD_STAT(of, f) ((of)->of_stats.f++)",
        function_source(T4NEX / "t4_ofld.c", "t4_ofld_range"),
        function_source(T4NEX / "t4_ofld.c", "t4_ofld_overlap"),
        function_source(T4NEX / "t4_ofld.c", "t4_ofld_optional"),
        function_source(T4NEX / "t4_ofld_cpl.c", "t4_ofld_waiter_get"),
        function_source(T4NEX / "t4_ofld_cpl.c", "t4_ofld_waiter_find"),
        function_source(T4NEX / "t4_ofld_cpl.c", "t4_ofld_waiter_put"),
        function_source(T4NEX / "t4_ofld_cpl.c", "t4_ofld_wr_rpl"),
        ""])


def main():
    files = {"units.h": units_header(),
             "kstub.h": (TESTDIR / "kstub.h").read_text(encoding="utf-8")}
    try:
        run_c(TESTDIR / "ofld_units.c", files)
    except CTestFailure as error:
        print(error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

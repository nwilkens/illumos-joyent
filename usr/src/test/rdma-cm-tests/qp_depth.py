#!/usr/bin/env python3
"""Check that iwcxgbe advertises only QP depths whose SQ memory, with and
without the DSGL page list area, fits one t4nex DMA buffer, and that the
next depth up would not."""

import re
import sys

from cm_test import CTestFailure, IWC, T4NEX, TESTDIR, run_c

sys.path.insert(0, str(TESTDIR.parent / "cxgbe-tests"))
from c_src import function_source  # noqa: E402

HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned int uint_t;
#define PAGESIZE 4096
#define P2ROUNDUP(x, a) (((x) + ((a) - 1)) & ~((size_t)(a) - 1))
#define T4_EQ_ENTRY_SIZE 64
%(defs)s
typedef struct { uint32_t tri_eq_spg_len;
    struct { int trv_memwrite_dsgl; } tri_vres; } info_t;
typedef struct { info_t iwc_info; } iwc_t;
%(bytes)s
%(maxwr)s
int
main(void)
{
	int bad = 0;
	for (int dsgl = 0; dsgl < 2; dsgl++) {
		for (uint32_t spg = 1; spg <= 2; spg++) {
			iwc_t iwc = { { spg, { dsgl } } };
			size_t pbl;
			uint32_t n = iwc_max_qp_wr(&iwc);
			size_t b = iwc_sq_bytes(&iwc, n + 1, &pbl);
			size_t b2 = iwc_sq_bytes(&iwc, n + 2, &pbl);
			printf("dsgl %%d spg %%u: %%u WRs, %%zu bytes\n", dsgl,
			    spg, n, b);
			if (b > T4_RDMA_DMA_MAX_LEN)
				bad++;
			if (n < IWC_MAX_QP_WR && b2 <= T4_RDMA_DMA_MAX_LEN)
				bad++;
			if (dsgl && pbl + (size_t)(n + 1) * T4_MAX_FR_DSGL > b)
				bad++;
		}
	}
	return (bad != 0);
}
"""


def define(path, name):
    text = path.read_text(encoding="utf-8")
    m = re.search(rf"^#define\s+{name}\s+(.*)$", text, re.M)
    return f"#define {name} {m.group(1)}"


def main():
    defs = "\n".join([
        define(T4NEX / "t4_rdma.h", "T4_RDMA_DMA_MAX_LEN"),
        define(IWC / "iwc.h", "IWC_MAX_QP_WR"),
        define(IWC / "iwc_t4.h", "T4_MAX_FR_DSGL"),
        define(IWC / "iwc_t4.h", "T4_SQ_NUM_SLOTS"),
        define(IWC / "iwc_t4.h", "T4_SQ_NUM_BYTES"),
    ])
    files = {"t.c": HARNESS % {
        "defs": defs,
        "bytes": function_source(IWC / "iwc_qp.c", "iwc_sq_bytes"),
        "maxwr": function_source(IWC / "iwc_qp.c", "iwc_max_qp_wr")}}
    try:
        out, mode = run_c(["t.c"], files)
    except CTestFailure as error:
        print(error)
        return 1
    print(out, end="")
    print(f"PASS: advertised QP depths fit one DMA buffer ({mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

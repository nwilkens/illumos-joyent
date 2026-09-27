#!/usr/bin/env python3
"""Check the TERMINATE codes iwcxgbe sends for each asynchronous error.

iwc_term_codes() runs on the host against the layer and code Linux
build_term_codes() (cxgb4/qp.c) gives for every CQE status, for a send, a
send with invalidate, a write and a read response.
"""

import re
import sys

from cm_test import CTestFailure, IWC, TESTDIR, run_c

sys.path.insert(0, str(TESTDIR.parent / "cxgbe-tests"))
from c_src import function_source  # noqa: E402

# status: (layer, code) for send, send_inv, write (tagged), read resp.
S, INV, W, RR = 0, 1, 2, 3
LINUX = {
    0x01: [(0x01, 0x00), (0x02, 0x09), (0x01, 0x00), (0x01, 0x00)],
    0x02: [(0x01, 0x03), (0x01, 0x09), (0x01, 0x03), (0x01, 0x03)],
    0x03: [(0x01, 0x03)] * 4,
    0x04: [(0x01, 0x02)] * 4,
    0x05: [(0x01, 0x04)] * 4,
    0x06: [(0x01, 0x01), (0x01, 0x01), (0x11, 0x01), (0x11, 0x01)],
    0x07: [(0x02, 0x09)] * 4,
    0x08: [(0x02, 0x09)] * 4,
    0x09: [(0x00, 0x00)] * 4,
    0x0a: [(0x00, 0x00)] * 4,
    0x0b: [(0x11, 0x01)] * 4,
    0x10: [(0x23, 0x02)] * 4,
    0x11: [(0x23, 0x03)] * 4,
    0x12: [(0x12, 0x05)] * 4,
    0x13: [(0x12, 0x02)] * 4,
    0x14: [(0x12, 0x06), (0x12, 0x06), (0x11, 0x04), (0x11, 0x04)],
    0x15: [(0x02, 0x05)] * 4,
    0x16: [(0x02, 0x06)] * 4,
    0x17: [(0x12, 0x01)] * 4,
    0x18: [(0x12, 0x03)] * 4,
    0x19: [(0x10, 0x00)] * 4,
    0x1a: [(0x12, 0x04)] * 4,
    0x1b: [(0x12, 0x03)] * 4,
    0x1c: [(0x12, 0x03)] * 4,
    0x1d: [(0x12, 0x03)] * 4,
    0x1e: [(0x00, 0x00)] * 4,
    0x1f: [(0x00, 0x00)] * 4,
}

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef unsigned int uint_t;
typedef struct { uint_t st, op, sq; } t4_cqe_t;
#define CQE_STATUS(c) ((c)->st)
#define CQE_OPCODE(c) ((c)->op)
#define CQE_SQ(c) ((c)->sq)
#define FW_RI_RDMA_WRITE 0x0
#define FW_RI_READ_RESP 0x2
#define FW_RI_SEND 0x3
#define FW_RI_SEND_WITH_INV 0x4
#define FW_RI_SEND_WITH_SE_INV 0x6
%(defines)s
%(defs)s
%(func)s
static const uint_t ops[4][2] = {
    { FW_RI_SEND, 0 }, { FW_RI_SEND_WITH_INV, 0 },
    { FW_RI_RDMA_WRITE, 0 }, { FW_RI_READ_RESP, 0 } };
static const struct { uint_t st; uint8_t l[4], c[4]; } want[] = {
%(table)s
};
int
main(void)
{
	int bad = 0;
	for (size_t i = 0; i < sizeof (want) / sizeof (want[0]); i++) {
		for (int k = 0; k < 4; k++) {
			t4_cqe_t c = { want[i].st, ops[k][0], ops[k][1] };
			uint8_t l, e;
			iwc_term_codes(&c, &l, &e);
			if (l != want[i].l[k] || e != want[i].c[k]) {
				printf("status 0x%%x kind %%d: 0x%%x/0x%%x, Linux "
				    "0x%%x/0x%%x\n", want[i].st, k, l, e,
				    want[i].l[k], want[i].c[k]);
				bad++;
			}
		}
	}
	printf("%%zu statuses\n", sizeof (want) / sizeof (want[0]));
	return (bad != 0);
}
"""


def main():
    qp = IWC / "iwc_qp.c"
    t4h = (IWC / "iwc_t4.h").read_text(encoding="utf-8")
    defines = "\n".join(re.findall(r"^#define\tT4_ERR_\w+\s+0x[0-9a-f]+$",
                                   t4h, re.M))
    src = qp.read_text(encoding="utf-8")
    defs = "\n".join(re.findall(r"^#define\tTERM_\w+\t+0x[0-9a-f]+.*$",
                                src, re.M))
    func = function_source(qp, "iwc_term_codes")
    table = ",\n".join(
        "\t{ 0x%x, { %s }, { %s } }" % (
            st, ", ".join(str(l) for l, _ in v),
            ", ".join(str(c) for _, c in v))
        for st, v in sorted(LINUX.items()))
    files = {"t.c": HARNESS % {"defines": defines, "defs": defs,
                               "func": func, "table": table}}
    try:
        out, mode = run_c(["t.c"], files)
    except CTestFailure as error:
        print(error)
        return 1
    print(out, end="")
    print(f"PASS: TERMINATE codes match Linux ({mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

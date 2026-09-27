#!/usr/bin/env python3
"""Check the RoCE path MTU rule of rdk.h at the boundaries of each IB MTU:
a path MTU plus the 96 bytes of RoCE headers must fit the link MTU.  Also
check that the CM and irdma derive path MTUs from a link MTU only through
rdk_roce_mtu().  A rule with the header room cut must fail."""

import re
import sys

from cm_test import CTestFailure, RDMA, REPO, run_c

IRDMA = REPO / "usr/src/uts/common/io/irdma"

# (link MTU, path MTU); 0 is no path MTU.
CASES = ((0, 0), (351, 0), (352, 256), (607, 256), (608, 512),
         (1119, 512), (1120, 1024), (1500, 1024), (2143, 1024),
         (2144, 2048), (4096, 2048), (4100, 2048), (4191, 2048),
         (4192, 4096), (9000, 4096), (65535, 4096))


def extract(rdk_h):
    parts = [re.search(r"enum rdk_mtu \{.*?\};", rdk_h, re.S).group(0),
             re.search(r"#define\tRDK_ROCE_HDR_ROOM[^\n]*", rdk_h).group(0)]
    for name in ("rdk_mtu_enum_to_int", "rdk_mtu_int_to_enum",
                 "rdk_roce_mtu"):
        parts.append(re.search(rf"static inline [^\n]*\n{name}\(.*?\n\}}\n",
                               rdk_h, re.S).group(0))
    return "\n".join(parts) + "\n"


def run(rdk_h):
    table = ",\n".join(f"\t{{ {a}, {b} }}" for a, b in CASES)
    main = f"""#include <stdio.h>
#include "mtu.h"
static const struct {{ int link, path; }} cases[] = {{
{table}
}};
int
main(void)
{{
	unsigned int i;
	int bad = 0;

	for (i = 0; i < sizeof (cases) / sizeof (cases[0]); i++) {{
		enum rdk_mtu m = rdk_roce_mtu(cases[i].link);
		int got = m == 0 ? 0 : rdk_mtu_enum_to_int(m);

		if (got != cases[i].path) {{
			printf("link %d: path %d, want %d\\n", cases[i].link,
			    got, cases[i].path);
			bad = 1;
		}}
		if (got != 0 && got + RDK_ROCE_HDR_ROOM > cases[i].link) {{
			printf("link %d: path %d overruns it\\n", cases[i].link,
			    got);
			bad = 1;
		}}
	}}
	return (bad);
}}
"""
    return run_c(["main.c"], {"mtu.h": extract(rdk_h), "main.c": main})


def callers():
    """A link MTU turns into a path MTU only through rdk_roce_mtu()."""
    bad = []
    for path in [RDMA / "rdk_cm_roce_conn.c"] + sorted(IRDMA.glob("*.c")):
        text = path.read_text(encoding="utf-8")
        for m in re.finditer(r"rdk_mtu_int_to_enum\(([^;]*)\)", text):
            if re.search(r"irdma_mtu|vsi\.mtu|ip_mtu|cp_mtu", m.group(1)):
                bad.append(f"{path.name}: {m.group(0)}")
        if path.name == "irdma_qp.c" and \
                "rdk_mtu_enum_to_int(attr->path_mtu) > (int)" in text:
            bad.append(f"{path.name}: path MTU checked against the link")
    return bad


def main():
    rdk_h = (RDMA / "rdk.h").read_text(encoding="utf-8")
    try:
        run(rdk_h)
    except CTestFailure as error:
        print(error)
        return 1
    bad = callers()
    if bad:
        print("\n".join(bad))
        return 1
    try:
        run(rdk_h.replace("(40 + 8 + 12 + 4 + 28 + 4)", "(40 + 8 + 12)"))
    except CTestFailure:
        print(f"PASS: RoCE path MTU at {len(CASES)} link MTUs, callers use "
              f"it; a short header room is caught")
        return 0
    print("a rule with 60 bytes of header room passed")
    return 1


if __name__ == "__main__":
    sys.exit(main())

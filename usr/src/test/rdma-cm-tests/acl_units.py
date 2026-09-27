#!/usr/bin/env python3
"""Run the rdk_cm peer allow-list on the host: rdk_cm_acl_create() refuses
empty, oversize and non-unicast lists, and rdk_cm_acl_allows() admits
exactly the listed peers."""

import re
import sys

from cm_test import CTestFailure, CXGBE_TESTS, RDMA, TESTDIR, run_c

sys.path.insert(0, str(CXGBE_TESTS))
from c_src import function_source  # noqa: E402


STUBS = """#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define	CHECK(x)	do {						\\
	if (!(x)) {							\\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\\n", __FILE__,	\\
		    __LINE__, #x);					\\
		exit(1);						\\
	}								\\
} while (0)
#define	KM_SLEEP	0
typedef uint32_t ipaddr_t;
#ifndef IN_LOOPBACKNET
#define	IN_LOOPBACKNET	127
#endif
#define	CLASSD(a)	((ntohl(a) & 0xf0000000U) == 0xe0000000U)
static inline void *
kmem_zalloc(size_t n, int f)
{
	(void) f;
	return (calloc(1, n));
}
static inline void
kmem_free(void *p, size_t n)
{
	(void) n;
	free(p);
}
"""


def main():
    rdk = (RDMA / "rdk.h").read_text(encoding="utf-8")
    impl = (RDMA / "rdk_cm_impl.h").read_text(encoding="utf-8")
    parts = [
        STUBS,
        re.search(r"#define\tRDK_CM_ACL_MAX\t+\d+\n", rdk).group(),
        re.search(r"typedef struct rdk_cm_acl rdk_cm_acl_t;\n", rdk).group(),
        re.search(r"struct rdk_cm_acl \{.*?\};\n", impl, re.DOTALL).group(),
        function_source(RDMA / "rdk_cm_addr.c", "rdk_cm_unicast"),
        function_source(RDMA / "rdk_cm_listen.c", "rdk_cm_acl_create"),
        function_source(RDMA / "rdk_cm_listen.c", "rdk_cm_acl_allows"),
    ]
    try:
        out, mode = run_c([TESTDIR / "acl_units.c"],
                          {"units.h": "\n".join(parts)})
    except CTestFailure as error:
        print(error)
        return 1
    print(out, end="")
    print(f"PASS: allow-list ({mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Drive libnvmf's TCP transport with forged PDUs over a socketpair."""

from pathlib import Path
import shutil
import tempfile

from nvmf_test import NVME_REG_H, SRC, TESTDIR, UTS, run_c

LIB = SRC / "lib/libnvmf/common"
SOURCES = ("nvmf_tcp.c", "nvmf_tcp_pdu.c", "nvmf_tcp_data.c",
           "nvmf_transport.c", "nvmf_controller.c", "nvmf_host.c",
           "nvmf_subr.c", "nvmf_crc32c.c")

# The tree's own copies of these are used, on any build host.
TREE_HEADERS = ("sys/nvme.h", "sys/crc32.h", "sys/nvme/nvmf.h",
                "sys/nvme/nvmf_tcp.h", "sys/nvme/nvmf_ioctl.h")

# Stand-ins for the illumos headers the library needs.  On illumos each one
# defers to the real header.
SHIM = """#ifndef _PDU_TEST_SHIM_H
#define	_PDU_TEST_SHIM_H
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
typedef unsigned int uint_t;
typedef unsigned long ulong_t;
typedef unsigned char uchar_t;
typedef unsigned short ushort_t;
typedef enum { B_FALSE, B_TRUE } boolean_t;
typedef struct { uint8_t u[16]; } pdu_test_uuid_t;
#define	LE_16(x)	((uint16_t)(x))
#define	LE_32(x)	((uint32_t)(x))
#define	LE_64(x)	((uint64_t)(x))
#define	CTASSERT(x)	_Static_assert(x, #x)
#define	VERIFY(x)	assert(x)
#define	VERIFY3U(a, op, b)	assert((a) op (b))
#define	CE_WARN	2
#ifndef MSG_NOSIGNAL
#define	MSG_NOSIGNAL	0
#endif
#ifndef PAGESIZE
#define	PAGESIZE	4096
#endif
#if !defined(__unused) && !defined(__linux__)
#define	__unused	__attribute__((unused))
#endif
#define	atomic_inc_uint(p)	\\
	((void)__atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST))
#define	atomic_dec_uint_nv(p)	__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#endif
"""

SHIMMED = ("sys/debug.h", "sys/stddef.h", "sys/types32.h", "sys/byteorder.h",
           "sys/cmn_err.h", "sys/sysmacros.h", "sys/uuid.h", "atomic.h")

# The handoff to the kernel is not part of this test.
NVPAIR = """#if defined(__sun)
#include_next <libnvpair.h>
#else
#ifndef _PDU_TEST_LIBNVPAIR_H
#define	_PDU_TEST_LIBNVPAIR_H
#include "pdu_test_shim.h"
#include <errno.h>
typedef struct nvlist nvlist_t;
#define	NV_UNIQUE_NAME	1
#define	NV_ENCODE_NATIVE	0
static inline int
nvlist_alloc(nvlist_t **n, uint_t f, int k)
{
	(void) f, (void) k;
	*n = NULL;
	return (ENOTSUP);
}
static inline int
nvlist_pack(nvlist_t *n, char **b, size_t *l, int e, int f)
{
	(void) n, (void) b, (void) l, (void) e, (void) f;
	return (ENOTSUP);
}
#define	nvlist_free(n)	((void)(n))
#define	nvlist_add_boolean_value(n, k, v)	((void)(n), (void)(v), 0)
#define	nvlist_add_uint64(n, k, v)	((void)(n), (void)(v), 0)
#define	nvlist_add_nvlist(n, k, v)	((void)(n), (void)(v), 0)
#define	nvlist_add_byte_array(n, k, v, c)	((void)(n), (void)(v), 0)
#endif
#endif
"""


def shims(work):
    (work / "sys/nvme").mkdir(parents=True)
    for name in TREE_HEADERS:
        shutil.copy(UTS / name, work / name)
    shutil.copy(NVME_REG_H, work / "nvme_reg.h")
    (work / "pdu_test_shim.h").write_text(SHIM, encoding="utf-8")
    (work / "libnvpair.h").write_text(NVPAIR, encoding="utf-8")
    for name in SHIMMED:
        extra = ""
        if name == "sys/uuid.h":
            extra = "#define\tuuid_t\tpdu_test_uuid_t\n"
        (work / name).write_text(
            f"#if defined(__sun)\n#include_next <{name}>\n#else\n"
            f"#include \"pdu_test_shim.h\"\n{extra}#endif\n",
            encoding="utf-8")


def main():
    with tempfile.TemporaryDirectory(prefix="nvmf-pdu-shim-") as tmp:
        work = Path(tmp)
        shims(work)
        cflags = ["-I", str(work), "-I", str(LIB),
                  "-Dmalloc=test_malloc", "-Dcalloc=test_calloc",
                  "-Dfree=test_free", "-Wno-sign-compare",
                  "-Wno-missing-field-initializers"]
        cflags += [str(LIB / name) for name in SOURCES]
        run_c(TESTDIR / "pdu_validate.c", {}, cflags=cflags)


if __name__ == "__main__":
    main()

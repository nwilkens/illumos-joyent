#!/usr/bin/env python3
"""Run the imported CQE poll, with its illumos changes, against forged CQEs."""

import re

from irdma_test import IRDMA, TESTDIR, run_c

STUB = """#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16;
typedef int16_t s16; typedef uint32_t u32; typedef int32_t s32;
typedef unsigned long long u64; typedef long long s64;
typedef uint16_t __le16; typedef uint32_t __le32; typedef u64 __le64;
typedef uint16_t __be16; typedef uint32_t __be32; typedef u64 dma_addr_t;
#define __iomem
#define __packed __attribute__((__packed__))
#define fallthrough __attribute__((__fallthrough__))
#define likely(x) (x)
#define unlikely(x) (x)
#define ARRAY_SIZE(a) (sizeof (a) / sizeof ((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define cpu_to_le64(x) (x)
#define le64_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define dma_wmb() __sync_synchronize()
#define dma_rmb() __sync_synchronize()
#define wmb() __sync_synchronize()
#define mb() __sync_synchronize()
#define udelay(x) ((void)0)
#define ETH_ALEN 6
#define ibdev_dbg(...) ((void)0)
#define print_hex_dump_debug(...) ((void)0)
struct list_head { struct list_head *next, *prev; };
typedef struct { int x; } spinlock_t;
struct ib_sge { u64 addr; u32 length; u32 lkey; };
struct ib_device;
struct irdma_sc_dev;
struct irdma_hw;
extern struct ib_device *to_ibdev(struct irdma_sc_dev *);
static inline void writel(u32 v, volatile void *a) { (void)v; (void)a; }
struct irdma_dma_mem { void *va; dma_addr_t pa; u32 size; };
struct irdma_virt_mem { void *va; u32 size; };
"""


def main():
    osdep = (IRDMA / "osdep.h").read_text(encoding="utf-8")
    parts = [STUB]
    for name in ("BIT", "BIT_ULL", "GENMASK", "GENMASK_ULL", "FIELD_PREP",
                 "FIELD_GET"):
        m = re.search(rf"^#define\t{name}\((?:[^\n]*\\\n)*[^\n]*$", osdep,
                      re.MULTILINE)
        assert m, name
        parts.append(m.group(0))
    m = re.search(r"^struct irdma_cq_uk;\n[\s\S]*?\(\(\(r\)\.head[^\n]*$",
                  osdep, re.MULTILINE)
    assert m, "IRDMA_OSDEP_RING_HOLDS"
    parts.append(m.group(0))
    core = IRDMA / "core"
    headers = {"osdep.h": "\n".join(parts) + "\n"}
    for name in ("defs.h", "user.h", "irdma.h", "uk.c"):
        headers[name] = (core / name).read_text(encoding="utf-8")
    run_c(TESTDIR / "cqe_checks.c", headers,
          cflags=("-std=gnu99", "-w", "-Wno-error"))


if __name__ == "__main__":
    main()

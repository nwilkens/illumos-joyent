"""Build the nvmf_rdma queue and data path, with the rdmak files it calls,
on the host against the rdmak kernel environment (irdma-tests/rdk_kenv.h)
and run a test program over it.  The CM, the pool and the module glue are
the test's to provide."""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

from nvmf_test import (NVMF, NVMF_H, NVME_H, NVME_REG_H, SRC, TESTDIR,
                       UTS, define, function, typedef)

IRDMA_TESTS = SRC / "test/irdma-tests"
sys.path.insert(0, str(IRDMA_TESTS))
from rdk_host import source as rdk_source  # noqa: E402

SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]

RDMAK = ("rdk_quiesce.c", "rdk_cq.c", "rdk_verbs.c", "rdk_rw.c")
NR_FILES = ("nvmf_rdma_subr.c", "nvmf_rdma.c", "nvmf_rdma_xfer.c")

KENV_EXTRA = r"""
#define	_KERNEL	1
typedef struct nvlist nvlist_t;
typedef struct msgb {
	struct msgb	*b_cont;
	unsigned char	*b_rptr;
	unsigned char	*b_wptr;
} mblk_t;
static inline void freemsg(mblk_t *mp) { (void) mp; }
/* A tick is a millisecond here, as in rdk_kenv.h. */
static inline void delay(long t) { (void) usleep((useconds_t)t * 1000); }
#define	MBLKL(mp)	((size_t)((mp)->b_wptr - (mp)->b_rptr))
typedef struct vmem vmem_t;
typedef uintptr_t timeout_id_t;
#define	LE_16(x)	((uint16_t)(x))
#define	LE_32(x)	((uint32_t)(x))
#define	LE_64(x)	((uint64_t)(x))
#define	cmn_err(l, ...)	((void)(l))

#define	LN(l, o)	((list_node_t *)(void *)((char *)(o) + (l)->list_offset))
#define	LO(l, n)	((n) == &(l)->list_head ? NULL : \
	(void *)((char *)(n) - (l)->list_offset))
static inline void
list_create(list_t *l, size_t sz, size_t off)
{
	l->list_size = sz;
	l->list_offset = off;
	l->list_head.list_next = l->list_head.list_prev = &l->list_head;
}
static inline void
list_destroy(list_t *l)
{
	VERIFY(l->list_head.list_next == &l->list_head);
}
static inline int
list_is_empty(list_t *l)
{
	return (l->list_head.list_next == &l->list_head);
}
static inline void *
list_head(list_t *l)
{
	return (LO(l, l->list_head.list_next));
}
static inline void *
list_next(list_t *l, void *o)
{
	return (LO(l, LN(l, o)->list_next));
}
static inline void
list_insert_head(list_t *l, void *o)
{
	list_node_t *n = LN(l, o);

	VERIFY(n->list_next == NULL);
	n->list_next = l->list_head.list_next;
	n->list_prev = &l->list_head;
	n->list_next->list_prev = n;
	l->list_head.list_next = n;
}
static inline void
list_insert_tail(list_t *l, void *o)
{
	list_node_t *n = LN(l, o);

	VERIFY(n->list_next == NULL);
	n->list_prev = l->list_head.list_prev;
	n->list_next = &l->list_head;
	n->list_prev->list_next = n;
	l->list_head.list_prev = n;
}
static inline void
list_remove(list_t *l, void *o)
{
	list_node_t *n = LN(l, o);

	VERIFY(n->list_next != NULL);
	n->list_prev->list_next = n->list_next;
	n->list_next->list_prev = n->list_prev;
	n->list_next = n->list_prev = NULL;
}
static inline void *
list_remove_head(list_t *l)
{
	void *o = list_head(l);

	if (o != NULL)
		list_remove(l, o);
	return (o);
}

/* rdk_verbs.c calls into rdk_cm_roce_conn.c, which is not built here. */
struct rdk_qp;
void rdk_cm_roce_qp_gone(struct rdk_qp *);
"""


def _text(path, replace=()):
    text = path.read_text(encoding="utf-8")
    for old, new in replace:
        assert old in text, f"{path.name}: {old!r}"
        text = text.replace(old, new)
    text = re.sub(r"^#include [<\"].*$", "", text, flags=re.MULTILINE)
    return f"#line 1 {json.dumps(str(path))}\n{text}\n"


def unit(replace=None):
    """One translation unit: the environment, rdmak, the nvmf types and
    core helpers, and the transport's queue and data files."""
    replace = replace or {}
    parts = ['#include "rdk_kenv.h"', KENV_EXTRA]
    for name in ("rdk.h", "rdk_impl.h", *RDMAK):
        parts.append(rdk_source(name, replace.get(name, ())))
    for name in ("NVME_CQE_SC_GEN_SUCCESS", "NVME_CQE_SC_GEN_INV_FLD",
                 "NVME_CQE_SC_GEN_DATA_XFR_ERR",
                 "NVME_CQE_SC_GEN_INTERNAL_ERR",
                 "NVME_CQE_SC_GEN_INV_DSGL_LEN",
                 "NVME_CQE_SC_GEN_INV_SGL_DESC",
                 "NVME_CQE_SC_GEN_INV_SGL_OFF", "NVME_CQE_SCT_GENERIC"):
        parts.append(define(NVME_H, name))
    parts.append(typedef(NVME_H, "nvme_cqe_sf_t"))
    for name in ("nvme_sgl_t", "nvme_sqe_t", "nvme_cqe_t"):
        parts.append(typedef(NVME_REG_H, name))
    parts.append(typedef(NVMF_H, "nvmf_trtype_t"))
    parts.append(define(NVMF_H, "NVMF_SGL_SUBTYPE_INVALIDATE_KEY"))
    parts.append(_text(UTS / "sys/nvme/nvmf_transport.h"))
    parts.append(_text(NVMF / "nvmf_transport_internal.h"))
    core = NVMF / "nvmf_transport.c"
    parts.append(define(core, "NVMF_XFER_HOST_TO_CTRLR"))
    for name in ("nvmf_memdesc_copy", "nvmf_memdesc_copyin",
                 "nvmf_memdesc_copyout", "nvmf_sqe_xfer_dir",
                 "nvmf_sgl_decode", "nvmf_capsule_defer_response"):
        parts.append(function(core, name))
    parts.append(_text(UTS / "sys/nvme/nvmf_rdma.h"))
    parts.append(_text(NVMF / "nvmf_rdma_impl.h",
                       replace.get("nvmf_rdma_impl.h", ())))
    for name in NR_FILES:
        parts.append(_text(NVMF / name, replace.get(name, ())))
    return "\n".join(parts)


class HostFailure(RuntimeError):
    """A compile or run failed; the message has the output."""


def _run(command, phase, timeout, env=None):
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=timeout, env=env)
    except subprocess.TimeoutExpired as error:
        raise HostFailure(f"{phase}: timed out after {timeout}s") from error
    if result.returncode != 0:
        raise HostFailure(f"{phase}: exited {result.returncode}: "
                          f"{shlex.join(command)}\n"
                          f"{result.stdout}{result.stderr}")
    return result.stdout


def run(test, replace=None, args=(), timeout=240):
    """Compile test (a .c here that includes "nr_unit.h") under the
    sanitizers when the compiler has them, run it and return its output."""
    with tempfile.TemporaryDirectory(prefix="nr-host-") as tmp:
        work = Path(tmp)
        (work / "nr_unit.h").write_text(unit(replace), encoding="utf-8")
        binary = work / "t"
        cc = shlex.split(os.environ.get("CC", "cc"))
        base = cc + ["-std=gnu11", "-g", "-O1", "-pthread", "-Wall",
                     "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-Wno-unused-function",
                     "-Wno-missing-field-initializers", "-Wno-sign-compare",
                     "-I", str(work), "-I", str(IRDMA_TESTS),
                     str(TESTDIR / test), "-o", str(binary)]
        try:
            _run(base[:len(cc)] + SANITIZE + base[len(cc):], "compile", 180)
        except HostFailure as error:
            if "sanitize" not in str(error):
                raise
            _run(base, "compile", 180)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0",
                   UBSAN_OPTIONS="print_stacktrace=1")
        return _run([str(binary), *args], "run", timeout, env)


def mutant_fails(test, replace, args=()):
    """True if the test fails once replace is applied.  A mutant that does
    not build proves nothing, so it counts as surviving."""
    try:
        run(test, replace, args)
    except HostFailure as error:
        if "--verbose" in sys.argv:
            print(error)
        return not str(error).startswith("compile:")
    return False

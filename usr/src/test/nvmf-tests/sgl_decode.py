#!/usr/bin/env python3
"""Fuzz the keyed SGL decoder and the memdesc copy helpers."""

from common_h import KSHIM
from nvmf_test import (NVMF, NVMF_H, NVME_H, NVME_REG_H, TESTDIR, define,
                       function, run_c, typedef)


def main():
    core = NVMF / "nvmf_transport.c"
    internal = NVMF / "nvmf_transport_internal.h"
    parts = [KSHIM, """
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	panic(...)	abort()
/* Reach the release-build clamp; the test checks the result itself. */
#undef	ASSERT3U
#define	ASSERT3U(a, op, b)	((void)0)
typedef struct msgb {
	struct msgb *b_cont;
	unsigned char *b_rptr, *b_wptr;
} mblk_t;
#define	MBLKL(mp)	((size_t)((mp)->b_wptr - (mp)->b_rptr))
typedef struct { uint64_t dmac_laddress; size_t dmac_size; } ddi_dma_cookie_t;
"""]
    for name in ("NVME_CQE_SC_GEN_SUCCESS", "NVME_CQE_SC_GEN_INV_FLD",
                 "NVME_CQE_SC_GEN_INV_DSGL_LEN", "NVME_CQE_SC_GEN_INV_SGL_DESC",
                 "NVME_CQE_SC_GEN_INV_SGL_OFF"):
        parts.append(define(NVME_H, name))
    parts.append(define(NVMF_H, "NVMF_SGL_SUBTYPE_INVALIDATE_KEY"))
    for name in ("NVME_PSDT_SGL", "NVMF_SQE_PSDT", "NVMF_FABRICS_OPC",
                 "NVMF_SGL_DATA_BLOCK", "NVMF_SGL_KEYED_DATA_BLOCK",
                 "NVMF_SGL_SUBTYPE_ADDRESS", "NVMF_SGL_SUBTYPE_OFFSET"):
        parts.append(define(internal, name))
    parts.append(define(core, "NVMF_XFER_HOST_TO_CTRLR"))
    for name in ("nvme_sgl_t", "nvme_sqe_t"):
        parts.append(typedef(NVME_REG_H, name))
    for name in ("nvmf_memdesc_type_t", "nvmf_seg_t", "nvmf_memdesc_t",
                 "nvmf_sgl_t"):
        parts.append(typedef(internal, name))
    for name in ("nvmf_memdesc_copy", "nvmf_memdesc_copyin",
                 "nvmf_memdesc_copyout", "nvmf_sqe_xfer_dir",
                 "nvmf_sgl_decode"):
        parts.append(function(core, name))
    run_c(TESTDIR / "sgl_decode.c", {"sgl.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

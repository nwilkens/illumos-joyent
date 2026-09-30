#!/usr/bin/env python3
"""Check that a controller data send completes, and responds, exactly once."""

from common_h import KSHIM
from nvmf_test import (NVMF, NVMF_H, NVME_H, NVME_REG_H, NVMFT, TESTDIR,
                       define, function, run_c, struct, typedef)


def main():
    core = NVMF / "nvmf_transport.c"
    internal = NVMF / "nvmf_transport_internal.h"
    public = NVMF.parents[1] / "sys/nvme/nvmf_transport.h"
    stmf = NVMFT / "nvmft_stmf.c"
    parts = [KSHIM, """
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	panic(...)	abort()
typedef struct msgb {
	struct msgb *b_cont;
	unsigned char *b_rptr, *b_wptr;
} mblk_t;
#define	MBLKL(mp)	((size_t)((mp)->b_wptr - (mp)->b_rptr))
#define	BPRI_MED	0
mblk_t *allocb(size_t, unsigned);
size_t msgdsize(mblk_t *);
typedef struct { uint64_t dmac_laddress; size_t dmac_size; } ddi_dma_cookie_t;
typedef struct nvlist nvlist_t;
struct nvmf_transport;
struct nvmf_capsule;
struct nvmf_io_request;
typedef void nvmf_qpair_error_t(void *, int);
typedef void nvmf_capsule_receive_t(void *, struct nvmf_capsule *);
typedef void nvmf_io_complete_t(void *, size_t, int);
struct nvmf_memdesc;
""", define(public, "NVMF_SUCCESS_SENT"), define(public, "NVMF_MORE")]
    for name in ("NVME_CQE_SC_GEN_SUCCESS", "NVME_CQE_SC_GEN_INV_FLD",
                 "NVME_CQE_SC_GEN_INV_DSGL_LEN", "NVME_CQE_SC_GEN_INTERNAL_ERR",
                 "NVME_CQE_SC_GEN_DATA_XFR_ERR", "NVME_CQE_SCT_GENERIC"):
        parts.append(define(NVME_H, name))
    parts.append(typedef(NVMF_H, "nvmf_trtype_t"))
    parts.append(typedef(NVME_H, "nvme_cqe_sf_t"))
    for name in ("nvme_sgl_t", "nvme_sqe_t", "nvme_cqe_t"):
        parts.append(typedef(NVME_REG_H, name))
    parts.append(define(public, "NVMF_SUCCESS_SENT"))
    text = public.read_text(encoding="utf-8")
    start = text.index("typedef void nvmf_send_complete_t")
    parts.append(text[start:text.index(";", start) + 1] + "\n")
    for name in ("nvmf_memdesc_type_t", "nvmf_seg_t", "nvmf_memdesc_t",
                 "nvmf_databuf_t"):
        parts.append(typedef(internal, name))
    parts.append(define(internal, "NVMF_CAPSULE_CONSUMER_WORDS"))
    for name in ("nvmf_send_request", "nvmf_transport_ops", "nvmf_qpair",
                 "nvmf_io_request", "nvmf_capsule"):
        parts.append(struct(internal, name))
    for name in ("nvmf_memdesc_copy", "nvmf_memdesc_copyout",
                 "nvmf_allocate_response", "nvmf_free_capsule",
                 "nvmf_transmit_capsule", "nvmf_send_controller_data",
                 "nvmf_send_data_sync", "nvmf_send_controller_data_io"):
        parts.append(function(core, name))

    # The nvmft half: the per-dbuf transfer state and its completion.
    parts.append("""
typedef uint64_t stmf_status_t;
#define	STMF_SUCCESS	((uint64_t)0)
#define	STMF_FAILURE	((uint64_t)0x1000000000000000)
#define	DB_DIRECTION_TO_RPORT	0x0001
#define	DB_SEND_STATUS_GOOD	0x0004
#define	STMF_IOF_LPORT_DONE	0x0002
typedef struct scsi_task { void *task_port_private; } scsi_task_t;
typedef struct stmf_local_port stmf_local_port_t;
#define	STMF_ABORTED	(STMF_FAILURE | 5)
#define	STMF_BUSY	((uint64_t)0x2000000000000000)
#define	STMF_ABORT_SUCCESS	((uint64_t)0x3000000000000000)
#define	STMF_LPORT_ABORT_TASK	0x40
#define	STMF_REQUEUE_TASK_ABORT_LPORT	2
#define	STATUS_CHECK	0x02
#define	ASSERT0(x)	assert((x) == 0)
void stmf_abort(int, scsi_task_t *, stmf_status_t, void *);
void nvmf_abort_capsule_data(struct nvmf_capsule *, int);
typedef struct stmf_data_buf {
	uint16_t db_flags;
	stmf_status_t db_xfer_status;
	uint32_t db_data_size;
} stmf_data_buf_t;
struct nvmft_qpair;
struct nvmft_internal_io;
void stmf_data_xfer_done(scsi_task_t *, stmf_data_buf_t *, uint32_t);
void nvmft_command_completed(struct nvmft_qpair *, struct nvmf_capsule *);
uint32_t nvmft_qpair_caps(struct nvmft_qpair *);
#define	atomic_or_uint_nv(p, v)	__atomic_or_fetch((p), (v), __ATOMIC_SEQ_CST)
""")
    for line in public.read_text(encoding="utf-8").splitlines():
        if line.startswith("#define\tNVMF_QP_CAP_"):
            parts.append(line + "\n")
    for name in ("NVMFT_XFER_SUBMITTED", "NVMFT_XFER_COMPLETED"):
        parts.append(define(stmf, name))
    for name in ("nvmft_task_priv_t", "nvmft_internal_io_t", "nvmft_xfer_t"):
        parts.append(typedef(stmf, name))
    for name in ("nvmft_xfer_begin", "nvmft_xfer_end", "nvmft_xfer_refused",
                 "nvmft_xfer_arrive", "nvmft_xfer_finish",
                 "nvmft_datamove_out_cb", "nvmft_datamove_in_cb",
                 "nvmft_lport_abort"):
        parts.append(function(stmf, name))
    run_c(TESTDIR / "send_once.c", {"send.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

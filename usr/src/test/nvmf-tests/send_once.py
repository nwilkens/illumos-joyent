#!/usr/bin/env python3
"""Check that a controller data send completes, and responds, exactly once."""

from common_h import KSHIM
from nvmf_test import (NVMF, NVMF_H, NVME_H, NVME_REG_H, NVMFT, TESTDIR,
                       define, function, run_c, struct, typedef)


def main():
    core = NVMF / "nvmf_transport.c"
    internal = NVMF / "nvmf_transport_internal.h"
    public = NVMF.parents[1] / "sys/nvme/nvmf_transport.h"
    stmf = NVMFT / "nvmft_io.c"
    stmf_h = NVMFT / "nvmft_io.h"
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
    parts.append(struct(core, "nvmf_transport"))
    parts.append("#define\tatomic_dec_uint(p)\t"
                 "((void)atomic_dec_uint_nv(p))\n")
    for name in ("nvmf_memdesc_copy", "nvmf_memdesc_copyout",
                 "nvmf_transport_rele",
                 "nvmf_allocate_response", "nvmf_free_capsule",
                 "nvmf_transmit_capsule", "nvmf_send_controller_data",
                 "nvmf_send_data_sync", "nvmf_send_controller_data_io",
                 "nvmf_cancel_controller_data"):
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
#define	ASSERT0(x)	assert((x) == 0)
#define	MUTEX_HELD(m)	1
#define	mutex_tryenter(m)	(pthread_mutex_trylock(m) == 0)
typedef struct taskq taskq_t;
typedef struct taskq_ent { int tqent_unused; } taskq_ent_t;
void taskq_dispatch_ent(taskq_t *, void (*)(void *), void *, uint_t,
    taskq_ent_t *);
typedef struct nvmft_controller {
	kmutex_t ctrlr_lock;
	kcondvar_t ctrlr_pending_cv;
	list_t ctrlr_io_work;
	boolean_t ctrlr_io_running;
	taskq_ent_t ctrlr_io_task;
} nvmft_controller_t;
static struct { taskq_t *ns_io_taskq; } nvmft_softc, *nvmft_global =
    &nvmft_softc;
typedef struct stmf_data_buf {
	uint16_t db_flags;
	stmf_status_t db_xfer_status;
	uint32_t db_data_size;
} stmf_data_buf_t;
struct nvmft_qpair;
void stmf_data_xfer_done(scsi_task_t *, stmf_data_buf_t *, uint32_t);
void nvmft_command_completed(struct nvmft_qpair *, struct nvmf_capsule *);
uint32_t nvmft_qpair_caps(struct nvmft_qpair *);
#define	atomic_or_uint_nv(p, v)	__atomic_or_fetch((p), (v), __ATOMIC_SEQ_CST)
""")
    for line in public.read_text(encoding="utf-8").splitlines():
        if line.startswith("#define\tNVMF_QP_CAP_"):
            parts.append(line + "\n")
    for name in ("NVMFT_XFER_SUBMITTED", "NVMFT_XFER_COMPLETED"):
        parts.append(define(stmf_h, name))
    parts.append(define(NVME_H, "NVME_CQE_SC_GEN_ABORT_REQUEST"))
    for name in ("nvmft_task_priv_t", "nvmft_internal_io_t", "nvmft_xfer_t"):
        parts.append(typedef(stmf_h, name))
    # What the transfer code calls in the rest of nvmft_io.c.
    parts.append("""
static void nvmft_task_finish(nvmft_task_priv_t *, boolean_t);
static void nvmft_task_unlist_locked(nvmft_controller_t *,
    nvmft_task_priv_t *);
nvmft_controller_t *nvmft_task_ctrlr(nvmft_task_priv_t *);
int nvmft_send_generic_error(struct nvmft_qpair *, struct nvmf_capsule *,
    uint8_t);
void nvmft_internal_aborted(nvmft_internal_io_t *);
static void nvmft_io_task(void *);
static void nvmft_answer_command(nvmft_controller_t *, nvmft_task_priv_t *);
static void nvmft_post_command(nvmft_task_priv_t *);
""")
    for name in ("nvmft_xfer_begin", "nvmft_xfer_end", "nvmft_xfer_done",
                 "nvmft_xfer_refuse", "nvmft_xfer_arrive",
                 "nvmft_xfer_finish", "nvmft_datamove_out_cb",
                 "nvmft_datamove_in_cb", "nvmft_io_work",
                 "nvmft_cancel_command", "nvmft_io_task",
                 "nvmft_lport_abort"):
        parts.append(function(stmf, name))
    run_c(TESTDIR / "send_once.c", {"send.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

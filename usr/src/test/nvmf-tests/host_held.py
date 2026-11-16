#!/usr/bin/env python3
"""Drive nvmf_host block commands through loss, reconnect and teardown."""

from common_h import KSHIM
from nvmf_test import NVMF, NVME_H, NVME_REG_H, TESTDIR, UTS, define, \
    function, run_c, typedef

HOST = UTS / "io/nvmf_host"


def main():
    internal = NVMF / "nvmf_transport_internal.h"
    var = HOST / "nvmf_var.h"
    blkdev = HOST / "nvmf_blkdev.c"
    parts = [KSHIM, """
#define	MUTEX_HELD(m)	1
typedef struct { uint64_t dmac_laddress; size_t dmac_size; } ddi_dma_cookie_t;
typedef struct msgb mblk_t;
"""]
    for name in ("NVME_CQE_SCT_PATH", "NVME_CQE_SC_PATH_HOST_ABRT"):
        parts.append(define(NVME_H, name))
    parts.append(define(UTS / "sys/blkdev.h", "BD_XFER_POLL"))
    parts.append(typedef(NVME_H, "nvme_cqe_sf_t"))
    for name in ("nvme_sgl_t", "nvme_sqe_t", "nvme_cqe_t"):
        parts.append(typedef(NVME_REG_H, name))
    for name in ("nvmf_memdesc_type_t", "nvmf_seg_t", "nvmf_memdesc_t"):
        parts.append(typedef(internal, name))
    for name in ("nvmf_sc_state_t", "nvmf_io_action_t"):
        parts.append(typedef(var, name))
    parts.append("""
struct nvmf_host_qpair;
struct nvmf_capsule;
struct nvmf_path;
typedef void nvmf_request_complete_t(void *, const nvme_cqe_t *);
typedef void nvmf_io_complete_t(void *, size_t, int);
typedef struct bd_xfer {
	unsigned	x_flags;
	int		t_done;
	int		t_error;
} bd_xfer_t;
typedef struct nvmf_softc {
	kmutex_t		lock;
	nvmf_sc_state_t		state;
	boolean_t		io_enabled;
	list_t			held;
	struct nvmf_host_qpair	**io;
	uint_t			num_io_queues;
	uint_t			qp_users;
} nvmf_softc_t;
typedef struct nvmf_request {
	struct nvmf_capsule	*nc;
} nvmf_request_t;
static struct { uint_t cpu_seqid; } test_cpu, *CPU = &test_cpu;
extern boolean_t nvmf_fail_disconnect;
void bd_xfer_done(bd_xfer_t *, int);
nvmf_softc_t *nvmf_mpath_path_sc(struct nvmf_path *);
boolean_t nvmf_mpath_path_gone(struct nvmf_path *);
void nvmf_mpath_path_set_gone(struct nvmf_path *);
void nvmf_mpath_select_rele(struct nvmf_path *);
nvmf_request_t *nvmf_allocate_request(struct nvmf_host_qpair *, void *,
    nvmf_request_complete_t *, void *, int);
void nvmf_submit_request(nvmf_request_t *);
int nvmf_capsule_append_data(struct nvmf_capsule *, nvmf_memdesc_t *, size_t,
    boolean_t, nvmf_io_complete_t *, void *);
void nvmf_qp_rele(nvmf_softc_t *);
""")
    for name in ("nvmf_cqe_aborted", "nvmf_cqe_failed"):
        parts.append(function(var, name))
    parts.append(function(HOST / "nvmf_conn.c", "nvmf_io_policy"))
    parts.append(typedef(blkdev, "nvmf_bd_cmd_t"))
    for name in ("nvmf_bd_free", "nvmf_bd_done", "nvmf_bd_requeue",
                 "nvmf_bd_finish", "nvmf_bd_cqe_complete",
                 "nvmf_bd_io_complete", "nvmf_bd_dispatch", "nvmf_bd_resume",
                 "nvmf_bd_fail", "nvmf_bd_fail_held", "nvmf_bd_path_gone"):
        parts.append(function(blkdev, name))
    run_c(TESTDIR / "host_held.c", {"held.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

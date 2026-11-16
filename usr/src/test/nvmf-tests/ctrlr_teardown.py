#!/usr/bin/env python3
"""Run nvmft's controller shutdown and terminate against the orders in which
errors, timers and the teardown taskq can interleave."""

from common_h import KSHIM
from nvmf_test import NVME_REG_H, NVMFT, TESTDIR, define, function, run_c, \
    typedef


def main():
    ctl = NVMFT / "nvmft_controller.c"
    parts = [KSHIM, """
#define	MUTEX_HELD(m)	1
#define	drv_usectohz(us)	((long)(us))
typedef struct timeout *timeout_id_t;
timeout_id_t timeout(void (*)(void *), void *, long);
long untimeout(timeout_id_t);
typedef struct taskq_ent {
	void (*func)(void *);
	void *arg;
	struct taskq_ent *next;
	int queued;
} taskq_ent_t;
typedef struct taskq taskq_t;
void taskq_dispatch_ent(taskq_t *, void (*)(void *), void *, unsigned,
    taskq_ent_t *);
typedef struct { taskq_t *ns_taskq; } nvmft_softc_t;
extern nvmft_softc_t *nvmft_global;
struct nvmft_qpair;
typedef struct nvmft_io_qpair {
	struct nvmft_qpair *nio_qp;
	boolean_t nio_shutdown;
} nvmft_io_qpair_t;
typedef struct nvmft_port {
	kmutex_t np_lock;
	list_t np_controllers;
	boolean_t np_offlining;
	void *np_ids;
} nvmft_port_t;
typedef struct nvmft_controller {
	struct nvmft_qpair *ctrlr_admin;
	nvmft_io_qpair_t *ctrlr_io_qpairs;
	uint_t ctrlr_num_io_queues;
	boolean_t ctrlr_shutdown;
	boolean_t ctrlr_shutdown_pending;
	boolean_t ctrlr_admin_closed;
	boolean_t ctrlr_admin_stopped;
	uint_t ctrlr_accepting;
	kcondvar_t ctrlr_accept_cv;
	uint16_t ctrlr_cntlid;
	uint32_t ctrlr_cc;
	uint32_t ctrlr_csts;
	nvmft_port_t *ctrlr_np;
	kmutex_t ctrlr_lock;
	list_node_t ctrlr_link;
	timeout_id_t ctrlr_ka_timer;
	long ctrlr_ka_ticks;
	taskq_ent_t ctrlr_shutdown_task;
	taskq_ent_t ctrlr_terminate_task;
	timeout_id_t ctrlr_terminate_timer;
	boolean_t ctrlr_terminate_queued;
} nvmft_controller_t;
void nvmft_qpair_shutdown(struct nvmft_qpair *);
void nvmft_qpair_destroy(struct nvmft_qpair *);
void nvmft_deferred_drain(nvmft_controller_t *);
void nvmft_io_drain(nvmft_controller_t *);
void nvmft_internal_drain(nvmft_controller_t *);
void nvmft_terminate_commands(nvmft_controller_t *);
void nvmft_admin_stop(nvmft_controller_t *);
void nvmft_session_deregister(nvmft_controller_t *);
void nvmft_port_offline_complete(nvmft_port_t *);
void nvmft_port_rele(nvmft_port_t *);
void id_free(void *, int);
int nvmft_printf(nvmft_controller_t *, const char *, ...);
static void nvmft_controller_free(nvmft_controller_t *);
static void nvmft_keep_alive_timer(void *);
static void nvmft_controller_shutdown(void *);
static void nvmft_controller_terminate(void *);
static void nvmft_controller_terminate_timeout(void *);
static void nvmft_controller_fail(nvmft_controller_t *, boolean_t);
"""]
    for name in ("NVME_CSTS_SHN_OCCURING", "NVME_CSTS_SHN_COMPLETE"):
        parts.append(define(NVME_REG_H, name))
    for name in ("nvme_reg_cc_t", "nvme_reg_csts_t"):
        parts.append(typedef(NVME_REG_H, name))
    parts.append(define(NVMFT / "nvmft_var.h", "NVMFT_CC_EN"))
    for name in ("nvmft_controller_shutdown",
                 "nvmft_controller_terminate_timeout",
                 "nvmft_controller_terminate", "nvmft_controller_fail",
                 "nvmft_controller_error", "nvmft_controller_offline",
                 "nvmft_controller_start_shutdown"):
        parts.append(function(ctl, name))
    run_c(TESTDIR / "ctrlr_teardown.c", {"teardown.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

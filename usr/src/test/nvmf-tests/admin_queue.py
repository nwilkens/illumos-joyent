#!/usr/bin/env python3
"""Run nvmft's admin command queue for transports that must not block."""

from common_h import KSHIM
from nvmf_test import NVMFT, TESTDIR, function, run_c, typedef


def main():
    ctl = NVMFT / "nvmft_controller.c"
    parts = [KSHIM, """
#define	NVMF_CAPSULE_CONSUMER_WORDS	12
struct nvmf_capsule {
	int nc_id;
	uint64_t nc_consumer[NVMF_CAPSULE_CONSUMER_WORDS];
};
#define	NVMF_CAPSULE_CONSUMER(nc)	((void *)(nc)->nc_consumer)
typedef struct taskq_ent { pthread_t t; } taskq_ent_t;
typedef struct taskq taskq_t;
typedef void task_func_t(void *);
void taskq_dispatch_ent(taskq_t *, task_func_t, void *, unsigned,
    taskq_ent_t *);
typedef struct nvmft_controller {
	kmutex_t ctrlr_lock;
	list_t ctrlr_admin_cmds;
	boolean_t ctrlr_admin_running;
	boolean_t ctrlr_admin_stopped;
	kcondvar_t ctrlr_admin_cv;
	taskq_ent_t ctrlr_admin_task;
} nvmft_controller_t;
typedef struct { taskq_t *ns_admin_taskq; } nvmft_softc_t;
extern nvmft_softc_t *nvmft_global;
void nvmft_handle_admin_command(nvmft_controller_t *, struct nvmf_capsule *);
void nvmf_free_capsule(struct nvmf_capsule *);
"""]
    parts.append(typedef(ctl, "nvmft_admin_cmd_t"))
    for name in ("nvmft_admin_task", "nvmft_queue_admin_command",
                 "nvmft_admin_stop"):
        parts.append(function(ctl, name))
    run_c(TESTDIR / "admin_queue.c", {"admin.h": "\n".join(parts)})


if __name__ == "__main__":
    main()

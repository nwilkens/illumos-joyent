/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

#ifndef _IRDMA_IMPL_H
#define	_IRDMA_IMPL_H

#include <sys/list.h>
#include <sys/kstat.h>
#include <sys/ddifm.h>
#include <sys/fm/protocol.h>
#include <sys/fm/io/ddi.h>

#include "osdep.h"
#include "hmc.h"
#include "defs.h"
#include "type.h"
#include "protos.h"
#include "pble.h"
#include "ws.h"
#include "icrdma_hw.h"
#include "ice_rdma.h"
#include "irdma_ioctl.h"
#include "rdk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define	IRDMA_MODULE_NAME	"irdma"

/*
 * Control-plane bring-up, in order.  irdma_progress records each completed
 * step and teardown undoes them in reverse.
 */
typedef enum irdma_step {
	IRDMA_STEP_OPEN = 0,	/* the ice peer is open */
	IRDMA_STEP_DEV,		/* core device and host memory */
	IRDMA_STEP_INTR,	/* handlers on the RDMA vectors */
	IRDMA_STEP_CQP,
	IRDMA_STEP_FPM,		/* feature query, FPM query and commit */
	IRDMA_STEP_HMC,		/* HMC objects and their SDs */
	IRDMA_STEP_CCQ,
	IRDMA_STEP_CEQ0,
	IRDMA_STEP_AEQ,
	IRDMA_STEP_CEQS,	/* the completion CEQs */
	IRDMA_STEP_PBLE,
	IRDMA_STEP_WS,		/* work scheduler tree and the TC0 qset */
	IRDMA_STEP_PEFLTR,
	IRDMA_STEP_MAX
} irdma_step_t;

#define	IRDMA_MAX_VECTORS	32
/* The longest a vector holds its interrupt; the device takes 8160. */
#define	IRDMA_MAX_CQ_HOLD_US	1000

/* irdma_flags */
#define	IRDMA_F_TAINTED		0x01	/* device may still reach freed DMA */
#define	IRDMA_F_CQP_DEAD	0x02	/* no more CQP commands */
#define	IRDMA_F_STOPPING	0x04	/* detach has begun */
#define	IRDMA_F_CQP_LIVE	0x08	/* CQP created and not destroyed */
#define	IRDMA_F_DEFER		0x10	/* hold freed DMA; see irdma_osdep.c */

/* A CQP request: the command, its waiter and the completion. */
typedef enum irdma_req_state {
	IRDMA_REQ_FREE = 0,
	IRDMA_REQ_BUSY,		/* submitted or queued */
	IRDMA_REQ_DONE,
	IRDMA_REQ_ABANDONED	/* the waiter gave up; the device may not */
} irdma_req_state_t;

typedef struct irdma_cqp_req {
	irdma_req_state_t	icr_state;
	uint16_t		icr_gen;
	kcondvar_t		icr_cv;
	struct irdma_ccq_cqe_info icr_cqe;
	struct cqp_cmds_info	icr_cmd;
} irdma_cqp_req_t;

#define	IRDMA_CQP_NREQS		64

/* The osdep view of the RDMA function; see irdma_osdep.c. */
struct device {
	struct irdma		*od_irdma;
	kmutex_t		od_lock;
	list_t			od_bufs;
	list_t			od_deferred;
	uint_t			od_nbufs;
};

struct ib_device {
	dev_info_t		*ib_dip;
};

struct irdma_qp;
struct irdma_cq;
struct irdma_arp_entry;

/*
 * A reserved RDMA vector and its thread; see irdma_intr.c.  iv_lock is at
 * the priority of the vectors and covers the fields up to iv_did.
 */
typedef struct irdma_vec {
	struct irdma		*iv_irdma;
	uint_t			iv_idx;		/* index into irdma_intr */
	boolean_t		iv_ctl;		/* vector 0: AEQ and CEQ 0 */
	kmutex_t		iv_lock;
	kcondvar_t		iv_cv;
	boolean_t		iv_owed;	/* the vector fired */
	boolean_t		iv_resched;	/* a CQ wants another call */
	boolean_t		iv_off;
	boolean_t		iv_busy;
	boolean_t		iv_exit;
	uint_t			iv_rechecks;
	uint64_t		iv_passes;
	uint64_t		iv_intrs;
	uint64_t		iv_busy_ns;
	uint16_t		iv_itr_us;	/* see irdma_ceq_set_itr() */
	hrtime_t		iv_last;	/* end of the last pass */
	uint64_t		iv_rescues;	/* see irdma_vec_idle() */
	uint64_t		iv_rescues_on;
	struct irdma_ceq	*iv_ceq;
	kt_did_t		iv_did;
} irdma_vec_t;

/*
 * A completion CEQ.  ic_lock covers the ring, the holds of its CQs and
 * ic_resched.
 */
typedef struct irdma_ceq {
	irdma_vec_t		*ic_vec;
	uint32_t		ic_id;
	struct irdma_sc_ceq	ic_sc;
	struct irdma_dma_mem	ic_mem;
	struct irdma_sc_cq	**ic_reg;
	uint32_t		ic_nreg;
	boolean_t		ic_live;	/* created on the device */
	kmutex_t		ic_lock;
	list_t			ic_resched;
	uint64_t		ic_events;
} irdma_ceq_t;

/* Doorbell counts, one per CPU, padded to a cache line. */
typedef struct irdma_dbstat {
	uint64_t	ids_sq_doorbells;
	uint64_t	ids_cq_arms;
	uint64_t	ids_pad[6];
} irdma_dbstat_t;

typedef struct irdma_kstats {
	kstat_named_t	ik_progress;
	kstat_named_t	ik_flags;
	kstat_named_t	ik_cqp_submitted;
	kstat_named_t	ik_cqp_completed;
	kstat_named_t	ik_cqp_timeouts;
	kstat_named_t	ik_cqp_errors;
	kstat_named_t	ik_ceq_intrs;
	kstat_named_t	ik_aeq_intrs;
	kstat_named_t	ik_aeqes;
	kstat_named_t	ik_bad_entries;
	kstat_named_t	ik_events;
	kstat_named_t	ik_crit_errors;
	kstat_named_t	ik_dma_bufs;
	kstat_named_t	ik_hmc_sds;
	kstat_named_t	ik_qp_cnt;
	kstat_named_t	ik_cq_cnt;
	kstat_named_t	ik_mr_cnt;
	kstat_named_t	ik_pble_cnt;
	kstat_named_t	ik_link;
	kstat_named_t	ik_mtu;
	kstat_named_t	ik_qps;
	kstat_named_t	ik_cqs;
	kstat_named_t	ik_mrs;
	kstat_named_t	ik_pds;
	kstat_named_t	ik_ahs;
	kstat_named_t	ik_bad_cqes;
	kstat_named_t	ik_qp_errors;
	kstat_named_t	ik_flushes;
	kstat_named_t	ik_sq_doorbells;
	kstat_named_t	ik_cq_arms;
	kstat_named_t	ik_comp_vectors;
	kstat_named_t	ik_ceq_busy_ns;
	kstat_named_t	ik_ceq_rescues;
	kstat_named_t	ik_ceq_rescues_on;
	kstat_named_t	ik_ceqn_intrs[IRDMA_MAX_VECTORS];
} irdma_kstats_t;

typedef struct irdma {
	dev_info_t		*irdma_dip;
	int			irdma_instance;
	uint32_t		irdma_progress;	/* irdma_cfg_lock */
	volatile uint32_t	irdma_flags;

	/*
	 * irdma_cfg_lock serializes attach, detach and the ioctls.  It is the
	 * outermost driver lock and is never held across a wait for the
	 * interrupt taskq.
	 */
	kmutex_t		irdma_cfg_lock;

	ice_rdma_peer_t		*irdma_peer;
	const ice_rdma_ops_t	*irdma_ops;
	ice_rdma_info_t		irdma_info;
	ice_rdma_intr_t		irdma_intr;
	uint32_t		irdma_intr_mask;	/* handlers added */
	irdma_vec_t		irdma_vecs[IRDMA_MAX_VECTORS];
	uint_t			irdma_nvecs;
	irdma_ceq_t		*irdma_ceqs;	/* CEQ 1 and on */
	uint32_t		irdma_nceqs;
	uint32_t		irdma_ceqs_alloc;

	struct device		irdma_osdev;
	struct ib_device	irdma_ibdev;
	struct irdma_hw		irdma_hw;
	struct irdma_sc_dev	irdma_sc;
	struct irdma_dma_mem	irdma_obj_mem;
	struct irdma_dma_mem	irdma_obj_next;
	void			*irdma_hmc_mem;
	size_t			irdma_hmc_mem_size;
	struct irdma_hmc_pble_rsrc *irdma_pble;
	boolean_t		irdma_pble_live;

	/* CQP */
	struct irdma_sc_cqp	irdma_cqp;
	struct irdma_dma_mem	irdma_cqp_sq;
	u64			*irdma_cqp_scratch;
	struct irdma_ooo_cqp_op	*irdma_cqp_ooo;
	kmutex_t		irdma_req_lock;
	irdma_cqp_req_t		irdma_reqs[IRDMA_CQP_NREQS];
	uint_t			irdma_req_waiters;
	kcondvar_t		irdma_req_cv;	/* a request became free */

	/* CCQ, CEQ 0 and the AEQ, on vector 0 */
	struct irdma_sc_cq	irdma_ccq;
	struct irdma_dma_mem	irdma_ccq_mem;
	struct irdma_dma_mem	irdma_ccq_shadow;
	kmutex_t		irdma_ccq_lock;
	struct irdma_sc_ceq	irdma_ceq0;
	struct irdma_dma_mem	irdma_ceq0_mem;
	struct irdma_sc_cq	**irdma_ceq0_reg;
	uint32_t		irdma_ceq0_nreg;
	struct irdma_sc_aeq	irdma_aeq;
	struct irdma_dma_mem	irdma_aeq_mem;

	/* VSI and work scheduler */
	struct irdma_sc_vsi	irdma_vsi;
	struct irdma_l2params	irdma_l2;
	struct irdma_vsi_pestat	*irdma_pestat;
	kmutex_t		irdma_ws_lock;
	unsigned long		*irdma_ws_ids;
	uint16_t		irdma_ws_max;

	/* Tunables, read at attach. */
	uint32_t		irdma_qp_limit;
	uint32_t		irdma_cqp_timeout_ms;
	uint32_t		irdma_comp_limit;	/* completion vectors */

	/* Test hooks; see irdma_ioctl.h. */
	uint32_t		irdma_fail_step;	/* 0: none */
	boolean_t		irdma_hold_cqes;
	ddi_taskq_t		*irdma_test_taskq;
	irdma_ioc_status_t	irdma_test_result;

	kstat_t			*irdma_kstat;
	irdma_kstats_t		irdma_kstats;
	irdma_dbstat_t		*irdma_dbstats;	/* max_ncpus */
	uint64_t		irdma_cqp_submitted;
	uint64_t		irdma_cqp_completed;
	uint64_t		irdma_cqp_timeouts;
	uint64_t		irdma_cqp_errors;
	uint64_t		irdma_aeqes;
	uint64_t		irdma_bad_entries;
	uint64_t		irdma_events;
	uint64_t		irdma_crit_errors;
	link_state_t		irdma_link;
	uint32_t		irdma_mtu;
	uint64_t		irdma_speed;

	/*
	 * Verbs (irdma_verbs.c).  irdma_rdk_lock guards irdma_rdk_live so
	 * that events are not dispatched to an unregistered device.  The
	 * tables and bitmaps are sized from the HMC once the control plane
	 * is up.  Lock order: a QP's iqp_mod_lock, then irdma_arp_cmd_lock,
	 * then a CQ's icq_lock, then the QP's iqp_lock, then
	 * irdma_cqtable_lock, irdma_qptable_lock and a CEQ's ic_lock, then
	 * irdma_rsrc_lock and irdma_arp_lock.  Only iqp_mod_lock and
	 * irdma_arp_cmd_lock are held across a CQP command.
	 */
	struct rdk_device	irdma_rdk;
	krwlock_t		irdma_rdk_lock;
	boolean_t		irdma_rdk_live;
	boolean_t		irdma_verbs_live;
	kmutex_t		irdma_rsrc_lock;
	void			*irdma_rsrc_mem;
	size_t			irdma_rsrc_size;
	ulong_t			*irdma_qp_map;
	ulong_t			*irdma_cq_map;
	ulong_t			*irdma_mr_map;
	ulong_t			*irdma_pd_map;
	ulong_t			*irdma_ah_map;
	ulong_t			*irdma_arp_map;
	uint32_t		irdma_max_qp;
	uint32_t		irdma_max_cq;
	uint32_t		irdma_max_mr;
	uint32_t		irdma_max_pd;
	uint32_t		irdma_max_ah;
	uint32_t		irdma_arp_size;
	uint32_t		irdma_next_qp;
	uint32_t		irdma_next_cq;
	uint32_t		irdma_next_pd;
	uint32_t		irdma_next_ah;
	uint32_t		irdma_next_arp;
	uint32_t		irdma_mr_stagmask;
	boolean_t		irdma_gsi_used;
	kmutex_t		irdma_qptable_lock;
	struct irdma_qp		**irdma_qp_table;
	kmutex_t		irdma_cqtable_lock;
	struct irdma_cq		**irdma_cq_table;
	kmutex_t		irdma_arp_lock;
	kmutex_t		irdma_arp_cmd_lock;	/* ARP adds, deletes */
	struct irdma_arp_entry	*irdma_arp_table;
	kmutex_t		irdma_ceq_lock;	/* CEQ 0 */
	ddi_taskq_t		*irdma_wq;	/* QP errors and flushes */
	uint32_t		irdma_nqps;
	uint32_t		irdma_ncqs;
	uint32_t		irdma_nmrs;
	uint32_t		irdma_npds;
	uint32_t		irdma_nahs;
	uint64_t		irdma_bad_cqes;
	uint64_t		irdma_qp_errors;
	uint64_t		irdma_flushes;
} irdma_t;

#define	IRDMA_FROM_DEV(d)	container_of((d), irdma_t, irdma_sc)

/*
 * irdma.c
 */
extern void irdma_error(irdma_t *, const char *, ...) __KPRINTFLIKE(2);
extern void irdma_fm_report(irdma_t *, const char *, int);
extern void irdma_fatal(irdma_t *, const char *);

/*
 * irdma_ctl.c: bring-up and teardown, the CQP request layer and the event
 * queues.  The step functions need irdma_cfg_lock.
 */
extern int irdma_ctl_start(irdma_t *);
extern irdma_cqp_req_t *irdma_req_alloc(irdma_t *);
extern u64 irdma_req_scratch(irdma_t *, irdma_cqp_req_t *);
extern int irdma_cqp_exec(irdma_t *, irdma_cqp_req_t *,
    struct irdma_ccq_cqe_info *);
extern int irdma_ctl_stop(irdma_t *);
extern void irdma_ccq_poll(irdma_t *);
extern int irdma_cqp_probe(irdma_t *);
extern void irdma_ctl_hold_release(irdma_t *);
extern void irdma_cqp_fail_all(irdma_t *);

/*
 * irdma_intr.c: the vectors, their threads and the completion CEQs.
 */
extern uint32_t irdma_hw_vec(irdma_t *, uint_t);
extern void irdma_vec_enable(irdma_t *, uint_t);
extern void irdma_vec_disable(irdma_t *, uint_t);
extern uint_t irdma_intr(caddr_t, caddr_t);
extern void irdma_vecs_init(irdma_t *);
extern void irdma_vecs_fini(irdma_t *);
extern void irdma_vec_barrier(irdma_vec_t *);
extern void irdma_intr_barrier(irdma_t *);
extern void irdma_intr_off(irdma_t *);
extern void irdma_intr_quiesce(irdma_t *);
extern int irdma_step_intr(irdma_t *);
extern void irdma_unstep_intr(irdma_t *);
extern int irdma_step_ceqs(irdma_t *);
extern void irdma_unstep_ceqs(irdma_t *);
extern void irdma_ceq_kick(irdma_ceq_t *);
extern void irdma_ceq_set_itr(irdma_ceq_t *);

/*
 * irdma_osdep.c
 */
extern void irdma_osdep_regs_init(void);
extern void irdma_osdep_regs_fini(void);
extern void irdma_osdep_init(irdma_t *);
extern void irdma_osdep_fini(irdma_t *);
extern boolean_t irdma_osdep_regs_add(caddr_t, size_t, ddi_acc_handle_t);
extern void irdma_osdep_regs_dbs(caddr_t, caddr_t, caddr_t, irdma_dbstat_t *);
extern void irdma_osdep_regs_remove(caddr_t);
extern boolean_t irdma_quiesced(irdma_t *);
extern void irdma_taint(irdma_t *);
extern void irdma_osdep_defer(irdma_t *);
extern void irdma_osdep_release(irdma_t *, boolean_t);
extern void irdma_osdep_free_consumer(irdma_t *, void *, uint64_t, size_t);
extern boolean_t irdma_hw_ok(irdma_t *);
extern boolean_t irdma_healthy(irdma_t *);

#ifdef __cplusplus
}
#endif

#endif /* _IRDMA_IMPL_H */

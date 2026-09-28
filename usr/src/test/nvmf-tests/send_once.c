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

/*
 * Run nvmf_send_controller_data_io() over a transport that has only the mblk
 * op, as TCP does, for each result that op can give: the callback runs once,
 * the response goes out once, and the data is freed.  Then race nvmft's
 * submitter against the transport completion and check that STMF sees the
 * dbuf back exactly once, with the right flags.
 */
#include "send.h"

#include <unistd.h>

static int fail_allocb;
static unsigned allocbs, frees;

mblk_t *
allocb(size_t n, unsigned pri)
{
	mblk_t *mp;

	(void) pri;
	if (fail_allocb)
		return (NULL);
	mp = calloc(1, sizeof (*mp));
	mp->b_rptr = mp->b_wptr = malloc(n + 1);
	allocbs++;
	return (mp);
}

size_t
msgdsize(mblk_t *mp)
{
	return (MBLKL(mp));
}

static void
freemsg(mblk_t *mp)
{
	free(mp->b_rptr);
	free(mp);
	frees++;
}

static uint_t op_status;
static uint8_t sent[4096];
static size_t sent_len;
static nvme_cqe_t responses[8];
static int nresponses, completions;
static uint_t completed_status;

static uint_t
fake_send(struct nvmf_capsule *nc, uint32_t off, mblk_t *mp, size_t len)
{
	(void) nc;
	(void) off;
	memcpy(sent, mp->b_rptr, len);
	sent_len = len;
	freemsg(mp);
	return (op_status);
}

static struct nvmf_capsule *
fake_alloc_capsule(struct nvmf_qpair *qp, int how)
{
	(void) qp;
	(void) how;
	return (calloc(1, sizeof (struct nvmf_capsule)));
}

static void
fake_free_capsule(struct nvmf_capsule *nc)
{
	free(nc);
}

static int
fake_transmit(struct nvmf_capsule *nc)
{
	assert(nresponses < 8);
	responses[nresponses++] = nc->nc_cqe;
	return (0);
}

static struct nvmf_transport_ops ops = {
	.allocate_capsule = fake_alloc_capsule,
	.free_capsule = fake_free_capsule,
	.transmit_capsule = fake_transmit,
	.send_controller_data = fake_send,
};

static struct nvmf_qpair qp = { .nq_ops = &ops, .nq_controller = B_TRUE };

static void
on_complete(void *arg, uint_t status)
{
	assert(arg == &completions);
	completions++;
	completed_status = status;
}

static void
run(uint_t status, int final, int expect_rc, uint_t expect_status,
    int expect_resp, uint8_t expect_sc, int line)
{
	struct nvmf_capsule nc = { .nc_qpair = &qp };
	uint8_t data[512];
	nvmf_memdesc_t md = { .nmd_type = NVMF_MEMDESC_VADDR,
	    .nmd_len = sizeof (data), .nmd_u.nmd_vaddr = data };
	nvme_cqe_t cqe;
	unsigned a = allocbs, f = frees;
	size_t i;
	int rc;

	for (i = 0; i < sizeof (data); i++)
		data[i] = (uint8_t)(i * 7);
	memset(&cqe, 0, sizeof (cqe));
	cqe.cqe_cid = 0x1234;
	cqe.cqe_sqhd = 17;
	op_status = status;
	nresponses = completions = 0;
	rc = nvmf_send_controller_data_io(&nc, 0, &md,
	    expect_rc == EINVAL ? sizeof (data) + 1 : sizeof (data),
	    final ? &cqe : NULL, on_complete, &completions);
	if (rc != expect_rc || completions != (rc == 0) ||
	    (rc == 0 && completed_status != expect_status) ||
	    nresponses != expect_resp || allocbs - a != frees - f) {
		fprintf(stderr, "line %d: rc %d completions %d status %#x "
		    "responses %d\n", line, rc, completions, completed_status,
		    nresponses);
		abort();
	}
	if (rc == 0)
		assert(sent_len == sizeof (data) &&
		    memcmp(sent, data, sizeof (data)) == 0);
	if (nresponses == 1) {
		assert(responses[0].cqe_cid == 0x1234);
		assert(responses[0].cqe_sqhd == 17);
		assert(responses[0].cqe_sf.sf_sc == expect_sc);
	}
}

#define	RUN(st, fin, rc, est, resp, sc) \
	run(st, fin, rc, est, resp, sc, __LINE__)

static void
adapter(void)
{
	/* No response from the transport without a final CQE. */
	RUN(NVME_CQE_SC_GEN_SUCCESS, 0, 0, NVME_CQE_SC_GEN_SUCCESS, 0, 0);
	RUN(NVMF_MORE, 0, 0, NVMF_MORE, 0, 0);
	RUN(NVMF_SUCCESS_SENT, 0, 0, NVMF_SUCCESS_SENT, 0, 0);
	RUN(NVME_CQE_SC_GEN_INV_FLD, 0, 0, NVME_CQE_SC_GEN_INV_FLD, 0, 0);

	/* With one, exactly one response, whatever happened to the data. */
	RUN(NVME_CQE_SC_GEN_SUCCESS, 1, 0, NVMF_SUCCESS_SENT, 1, 0);
	RUN(NVMF_SUCCESS_SENT, 1, 0, NVMF_SUCCESS_SENT, 0, 0);
	RUN(NVMF_MORE, 1, 0, NVME_CQE_SC_GEN_INV_DSGL_LEN, 1,
	    NVME_CQE_SC_GEN_INV_DSGL_LEN);
	RUN(NVME_CQE_SC_GEN_INV_FLD, 1, 0, NVME_CQE_SC_GEN_INV_FLD, 1,
	    NVME_CQE_SC_GEN_INV_FLD);
	RUN(NVME_CQE_SC_GEN_INTERNAL_ERR, 1, 0, NVME_CQE_SC_GEN_INTERNAL_ERR,
	    1, NVME_CQE_SC_GEN_INTERNAL_ERR);

	/* Refused sends leave the response and the callback to the caller. */
	fail_allocb = 1;
	RUN(NVME_CQE_SC_GEN_SUCCESS, 1, ENOMEM, 0, 0, 0);
	fail_allocb = 0;
	RUN(NVME_CQE_SC_GEN_SUCCESS, 1, EINVAL, 0, 0, 0);
}

static int xfer_dones, cid_clears, requeues;

void
stmf_abort(int cmd, scsi_task_t *task, stmf_status_t s, void *arg)
{
	(void) task;
	(void) s;
	(void) arg;
	assert(cmd == STMF_REQUEUE_TASK_ABORT_LPORT);
	__atomic_add_fetch(&requeues, 1, __ATOMIC_SEQ_CST);
}

void
nvmf_abort_capsule_data(struct nvmf_capsule *nc, int error)
{
	(void) nc;
	assert(error == ECANCELED);
}
static uint32_t last_iof;
static stmf_status_t last_status;

void
stmf_data_xfer_done(scsi_task_t *task, stmf_data_buf_t *dbuf, uint32_t iof)
{
	(void) task;
	__atomic_add_fetch(&xfer_dones, 1, __ATOMIC_SEQ_CST);
	last_iof = iof;
	last_status = dbuf->db_xfer_status;
}

uint32_t
nvmft_qpair_caps(struct nvmft_qpair *q)
{
	(void) q;
	return (0);
}

void
nvmft_command_completed(struct nvmft_qpair *q, struct nvmf_capsule *nc)
{
	(void) q;
	(void) nc;
	cid_clears++;
}

/* One transfer where the completion runs before, or races, the submitter. */
static void *
complete_thread(void *arg)
{
	nvmft_xfer_t *nx = arg;

	if (nx->nx_to_rport)
		nvmft_datamove_out_cb(nx, nx->nx_status);
	else
		nvmft_datamove_in_cb(nx, nx->nx_dbuf->db_data_size,
		    (int)nx->nx_status);
	return (NULL);
}

static void
finish_case(int to_rport, int final, uint16_t flags, uint_t status,
    stmf_status_t want_status, uint32_t want_iof, int want_sent,
    int want_clear, int line)
{
	nvmft_task_priv_t priv;
	scsi_task_t task = { .task_port_private = &priv };
	stmf_data_buf_t dbuf = { .db_flags = flags, .db_data_size = 512 };
	nvmft_xfer_t nx;
	pthread_t t;
	int order;

	for (order = 0; order < 3; order++) {
		memset(&priv, 0, sizeof (priv));
		mutex_init(&priv.ntp_lock, NULL, MUTEX_DRIVER, NULL);
		assert(nvmft_xfer_begin(&priv));
		memset(&nx, 0, sizeof (nx));
		nx.nx_task = &task;
		nx.nx_dbuf = &dbuf;
		nx.nx_to_rport = to_rport;
		nx.nx_final = final;
		nx.nx_status = status;
		xfer_dones = cid_clears = 0;
		if (order == 0) {
			/* The transport completes inside the submit call. */
			complete_thread(&nx);
			assert(xfer_dones == 0);
			if (nvmft_xfer_arrive(&nx, NVMFT_XFER_SUBMITTED))
				nvmft_xfer_finish(&nx);
		} else if (order == 1) {
			if (nvmft_xfer_arrive(&nx, NVMFT_XFER_SUBMITTED))
				nvmft_xfer_finish(&nx);
			assert(xfer_dones == 0);
			complete_thread(&nx);
		} else {
			assert(pthread_create(&t, NULL, complete_thread,
			    &nx) == 0);
			if (nvmft_xfer_arrive(&nx, NVMFT_XFER_SUBMITTED))
				nvmft_xfer_finish(&nx);
			assert(pthread_join(t, NULL) == 0);
		}
		if (xfer_dones != 1 || last_iof != want_iof ||
		    last_status != want_status ||
		    priv.ntp_success_sent != want_sent ||
		    cid_clears != want_clear) {
			fprintf(stderr, "line %d order %d: dones %d iof %u "
			    "sent %d clears %d\n", line, order, xfer_dones,
			    last_iof, priv.ntp_success_sent, cid_clears);
			abort();
		}
		assert(priv.ntp_xfers == 0);
		mutex_destroy(&priv.ntp_lock);
	}
}

/* A transport that reports fewer bytes than the dbuf fails the dbuf. */
static void
short_write(void)
{
	nvmft_task_priv_t priv;
	scsi_task_t task = { .task_port_private = &priv };
	stmf_data_buf_t dbuf = { .db_data_size = 512 };
	nvmft_xfer_t nx;

	memset(&priv, 0, sizeof (priv));
	mutex_init(&priv.ntp_lock, NULL, MUTEX_DRIVER, NULL);
	assert(nvmft_xfer_begin(&priv));
	memset(&nx, 0, sizeof (nx));
	nx.nx_task = &task;
	nx.nx_dbuf = &dbuf;
	xfer_dones = 0;
	nvmft_datamove_in_cb(&nx, 511, 0);
	assert(nvmft_xfer_arrive(&nx, NVMFT_XFER_SUBMITTED));
	nvmft_xfer_finish(&nx);
	assert(xfer_dones == 1 && last_status == STMF_FAILURE);
}

#define	FINISH(r, f, fl, st, ws, wi, wsent, wc) \
	finish_case(r, f, fl, st, ws, wi, wsent, wc, __LINE__)
#define	C2H	DB_DIRECTION_TO_RPORT
#define	GOOD	DB_SEND_STATUS_GOOD
#define	DONE	STMF_IOF_LPORT_DONE

static void
nvmft_side(void)
{
	FINISH(0, 0, 0, 0, STMF_SUCCESS, 0, 0, 0);
	FINISH(0, 0, 0, EIO, STMF_FAILURE, 0, 0, 0);
	short_write();
	FINISH(1, 0, C2H, NVME_CQE_SC_GEN_SUCCESS, STMF_SUCCESS, 0, 0, 0);
	FINISH(1, 0, C2H, NVMF_MORE, STMF_SUCCESS, 0, 0, 0);
	/* TCP folded success into the last data without an LU status. */
	FINISH(1, 0, C2H, NVMF_SUCCESS_SENT, STMF_SUCCESS, 0, 1, 1);
	FINISH(1, 0, C2H, NVME_CQE_SC_GEN_INV_FLD, STMF_FAILURE, 0, 0, 0);
	/* The LU put status in the dbuf: the transport sent the response. */
	FINISH(1, 1, C2H | GOOD, NVMF_SUCCESS_SENT, STMF_SUCCESS, DONE, 1, 0);
	FINISH(1, 1, C2H | GOOD, NVME_CQE_SC_GEN_INTERNAL_ERR, STMF_FAILURE,
	    DONE, 1, 0);
}

/* Many racing completions: each transfer still finishes once. */
static void
stress(void)
{
	nvmft_task_priv_t priv;
	scsi_task_t task = { .task_port_private = &priv };
	stmf_data_buf_t dbuf = { .db_flags = C2H, .db_data_size = 512 };
	int i;

	memset(&priv, 0, sizeof (priv));
	mutex_init(&priv.ntp_lock, NULL, MUTEX_DRIVER, NULL);
	for (i = 0; i < 20000; i++) {
		nvmft_xfer_t nx;
		pthread_t t;

		assert(nvmft_xfer_begin(&priv));
		memset(&nx, 0, sizeof (nx));
		nx.nx_task = &task;
		nx.nx_dbuf = &dbuf;
		nx.nx_to_rport = B_TRUE;
		nx.nx_status = NVME_CQE_SC_GEN_SUCCESS;
		xfer_dones = 0;
		assert(pthread_create(&t, NULL, complete_thread, &nx) == 0);
		if (i & 1)
			usleep(1);
		if (nvmft_xfer_arrive(&nx, NVMFT_XFER_SUBMITTED))
			nvmft_xfer_finish(&nx);
		assert(pthread_join(t, NULL) == 0);
		assert(xfer_dones == 1);
	}
}

/*
 * An abort while the transport holds transfers: STMF hears BUSY, the dbufs
 * never come back to STMF, each completion retries the abort, and the abort
 * succeeds once the last transfer is gone.  No new transfer starts.
 */
static void
abort_drain(void)
{
	nvmft_task_priv_t priv;
	scsi_task_t task = { .task_port_private = &priv };
	stmf_data_buf_t dbuf[2] = { { .db_flags = C2H, .db_data_size = 512 },
	    { .db_data_size = 512 } };
	nvmft_xfer_t nx[2];
	int i;

	memset(&priv, 0, sizeof (priv));
	mutex_init(&priv.ntp_lock, NULL, MUTEX_DRIVER, NULL);
	memset(nx, 0, sizeof (nx));
	for (i = 0; i < 2; i++) {
		assert(nvmft_xfer_begin(&priv));
		nx[i].nx_task = &task;
		nx[i].nx_dbuf = &dbuf[i];
		nx[i].nx_to_rport = (i == 0);
		nx[i].nx_status = NVME_CQE_SC_GEN_SUCCESS;
		assert(!nvmft_xfer_arrive(&nx[i], NVMFT_XFER_SUBMITTED));
	}
	xfer_dones = requeues = 0;
	assert(nvmft_lport_abort(NULL, STMF_LPORT_ABORT_TASK, &task, 0) ==
	    STMF_BUSY);
	assert(!nvmft_xfer_begin(&priv));

	nvmft_datamove_out_cb(&nx[0], NVME_CQE_SC_GEN_SUCCESS);
	assert(xfer_dones == 0 && requeues == 1);
	assert(nvmft_lport_abort(NULL, STMF_LPORT_ABORT_TASK, &task, 0) ==
	    STMF_BUSY);
	nvmft_datamove_in_cb(&nx[1], 512, 0);
	assert(xfer_dones == 0 && requeues == 2);
	assert(nvmft_lport_abort(NULL, STMF_LPORT_ABORT_TASK, &task, 0) ==
	    STMF_ABORT_SUCCESS);
	assert(priv.ntp_xfers == 0);
	mutex_destroy(&priv.ntp_lock);

	/*
	 * TCP folded success into the last data while an abort was pending:
	 * the host has its completion, so the CID must be free for reuse.
	 */
	memset(&priv, 0, sizeof (priv));
	mutex_init(&priv.ntp_lock, NULL, MUTEX_DRIVER, NULL);
	memset(nx, 0, sizeof (nx));
	assert(nvmft_xfer_begin(&priv));
	nx[0].nx_task = &task;
	nx[0].nx_dbuf = &dbuf[0];
	nx[0].nx_to_rport = B_TRUE;
	assert(!nvmft_xfer_arrive(&nx[0], NVMFT_XFER_SUBMITTED));
	xfer_dones = requeues = cid_clears = 0;
	assert(nvmft_lport_abort(NULL, STMF_LPORT_ABORT_TASK, &task, 0) ==
	    STMF_BUSY);
	nvmft_datamove_out_cb(&nx[0], NVMF_SUCCESS_SENT);
	assert(cid_clears == 1 && priv.ntp_success_sent);
	assert(xfer_dones == 0 && requeues == 1);
	mutex_destroy(&priv.ntp_lock);
}

int
main(void)
{
	adapter();
	nvmft_side();
	abort_drain();
	stress();
	printf("send exactly-once passed\n");
	return (0);
}

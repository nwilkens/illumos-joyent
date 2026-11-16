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
 * Drive the nvmf_host block command path with a stub queue pair and a stub
 * blkdev.  Each transfer must be completed exactly once, with no softc lock
 * held, whether it is sent, held across a lost association, sent again after
 * a reconnect, or failed by a teardown or a namespace removal.
 */
#include "held.h"

#define	XFER_LEN	4096

boolean_t nvmf_fail_disconnect;

struct nvmf_path {
	nvmf_softc_t	*sc;
	boolean_t	gone;
	int		refs;
};

struct nvmf_capsule {
	nvmf_request_complete_t	*cqe_cb;
	nvmf_io_complete_t	*io_cb;
	void			*arg;
	size_t			len;
};

static nvmf_softc_t sc;
static struct nvmf_host_qpair *test_io[1];
static boolean_t qp_dead;
static struct nvmf_capsule *inflight[16];
static uint_t ninflight;
static nvmf_bd_cmd_t *reenter;
static int reenter_error;

nvmf_softc_t *
nvmf_mpath_path_sc(struct nvmf_path *path)
{
	return (path->sc);
}

boolean_t
nvmf_mpath_path_gone(struct nvmf_path *path)
{
	return (path->gone);
}

void
nvmf_mpath_path_set_gone(struct nvmf_path *path)
{
	path->gone = B_TRUE;
}

void
nvmf_mpath_select_rele(struct nvmf_path *path)
{
	assert(path->refs > 0);
	path->refs--;
}

void
nvmf_qp_rele(nvmf_softc_t *scp)
{
	mutex_enter(&scp->lock);
	assert(scp->qp_users > 0);
	scp->qp_users--;
	mutex_exit(&scp->lock);
}

nvmf_request_t *
nvmf_allocate_request(struct nvmf_host_qpair *qp, void *sqe,
    nvmf_request_complete_t *cb, void *cb_arg, int how)
{
	nvmf_request_t *req;

	(void) sqe;
	(void) how;
	assert(qp == test_io[0]);
	assert(sc.qp_users > 0);
	if (qp_dead)
		return (NULL);
	req = calloc(1, sizeof (*req));
	req->nc = calloc(1, sizeof (*req->nc));
	req->nc->cqe_cb = cb;
	req->nc->arg = cb_arg;
	return (req);
}

int
nvmf_capsule_append_data(struct nvmf_capsule *nc, nvmf_memdesc_t *mem,
    size_t len, boolean_t send, nvmf_io_complete_t *cb, void *arg)
{
	(void) send;
	assert(mem->nmd_len == len && arg == nc->arg);
	nc->io_cb = cb;
	nc->len = len;
	return (0);
}

void
nvmf_submit_request(nvmf_request_t *req)
{
	assert(ninflight < ARRAY_SIZE(inflight));
	inflight[ninflight++] = req->nc;
	free(req);
}

/* Deliver both completions of the oldest command in flight. */
static void
complete(boolean_t aborted, size_t xfered)
{
	struct nvmf_capsule *nc = inflight[0];
	nvme_cqe_t cqe;

	assert(ninflight > 0);
	memmove(&inflight[0], &inflight[1],
	    --ninflight * sizeof (inflight[0]));

	memset(&cqe, 0, sizeof (cqe));
	if (aborted) {
		cqe.cqe_sf.sf_sct = NVME_CQE_SCT_PATH;
		cqe.cqe_sf.sf_sc = NVME_CQE_SC_PATH_HOST_ABRT;
	}
	nc->io_cb(nc->arg, xfered, aborted ? ECONNABORTED : 0);
	nc->cqe_cb(nc->arg, &cqe);
	free(nc);
}

void
bd_xfer_done(bd_xfer_t *xfer, int error)
{
	nvmf_bd_cmd_t *bc = reenter;

	assert(pthread_mutex_trylock(&sc.lock) == 0);
	mutex_exit(&sc.lock);

	assert(xfer->t_done == 0);
	xfer->t_done = 1;
	xfer->t_error = error;

	/* blkdev starts the next transfer from inside the completion. */
	if (bc != NULL) {
		reenter = NULL;
		reenter_error = nvmf_bd_dispatch(bc);
		if (reenter_error != 0)
			nvmf_bd_free(bc);
	}
}

static nvmf_bd_cmd_t *
new_cmd(struct nvmf_path *path, bd_xfer_t *xfer)
{
	static char data[XFER_LEN];
	nvmf_bd_cmd_t *bc = calloc(1, sizeof (*bc));

	memset(xfer, 0, sizeof (*xfer));
	path->refs++;
	bc->bc_xfer = xfer;
	bc->bc_path = path;
	bc->bc_data = data;
	bc->bc_data_len = XFER_LEN;
	return (bc);
}

static void
set_state(nvmf_sc_state_t state, boolean_t enabled)
{
	mutex_enter(&sc.lock);
	sc.state = state;
	sc.io_enabled = enabled;
	mutex_exit(&sc.lock);
}

static void
idle(struct nvmf_path *path)
{
	assert(path->refs == 0);
	assert(ninflight == 0);
	assert(sc.qp_users == 0);
	assert(list_is_empty(&sc.held));
}

int
main(void)
{
	struct nvmf_path p1 = { .sc = &sc }, p2 = { .sc = &sc };
	bd_xfer_t x1, x2, x3;
	nvmf_bd_cmd_t *bc;

	mutex_init(&sc.lock, NULL, MUTEX_DRIVER, NULL);
	list_create(&sc.held, sizeof (nvmf_bd_cmd_t),
	    offsetof(nvmf_bd_cmd_t, bc_link));
	sc.io = test_io;
	sc.num_io_queues = 1;
	test_io[0] = (struct nvmf_host_qpair *)&test_io;

	/* Sent and completed. */
	set_state(NVMF_SC_CONNECTED, B_TRUE);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	assert(ninflight == 1 && x1.t_done == 0);
	complete(B_FALSE, XFER_LEN);
	assert(x1.t_done == 1 && x1.t_error == 0);
	idle(&p1);

	/* A short transfer with a good status is an error. */
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	complete(B_FALSE, XFER_LEN - 1);
	assert(x1.t_done == 1 && x1.t_error == EIO);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	complete(B_FALSE, 0);
	assert(x1.t_done == 1 && x1.t_error == EIO);
	idle(&p1);

	/* Aborted by a lost association, held, sent again, completed. */
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	set_state(NVMF_SC_RESETTING, B_FALSE);
	complete(B_TRUE, 0);
	assert(x1.t_done == 0 && ninflight == 0 && p1.refs == 1);
	set_state(NVMF_SC_LOST, B_FALSE);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x2)) == 0);
	assert(x2.t_done == 0 && ninflight == 0 && p1.refs == 2);
	set_state(NVMF_SC_RECONNECTING, B_FALSE);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x3)) == 0);
	assert(ninflight == 0);
	set_state(NVMF_SC_CONNECTED, B_TRUE);
	nvmf_bd_resume(&sc);
	assert(ninflight == 3 && list_is_empty(&sc.held));
	complete(B_FALSE, XFER_LEN);
	complete(B_FALSE, XFER_LEN);
	complete(B_FALSE, XFER_LEN);
	assert(x1.t_done == 1 && x1.t_error == 0);
	assert(x2.t_done == 1 && x2.t_error == 0);
	assert(x3.t_done == 1 && x3.t_error == 0);
	idle(&p1);

	/* Held, then failed when the loss timeout expires. */
	set_state(NVMF_SC_LOST, B_FALSE);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x2)) == 0);
	set_state(NVMF_SC_FAILED, B_FALSE);
	reenter = new_cmd(&p1, &x3);
	nvmf_bd_fail_held(&sc, EIO);
	assert(x1.t_done == 1 && x1.t_error == EIO);
	assert(x2.t_done == 1 && x2.t_error == EIO);
	assert(reenter == NULL && reenter_error == ENXIO && x3.t_done == 0);
	idle(&p1);

	/* A second loss while the held commands are being sent again. */
	set_state(NVMF_SC_LOST, B_FALSE);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	set_state(NVMF_SC_RESETTING, B_FALSE);
	nvmf_bd_resume(&sc);
	assert(ninflight == 0 && x1.t_done == 0 && p1.refs == 1);
	set_state(NVMF_SC_DISCONNECTING, B_FALSE);
	nvmf_bd_fail_held(&sc, ENXIO);
	assert(x1.t_done == 1 && x1.t_error == ENXIO);
	idle(&p1);

	/* Aborted by a user disconnect: failed, not held. */
	set_state(NVMF_SC_CONNECTED, B_TRUE);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	set_state(NVMF_SC_DISCONNECTING, B_FALSE);
	complete(B_TRUE, 0);
	assert(x1.t_done == 1 && x1.t_error == EIO);
	idle(&p1);

	/* Fail fast: nothing is held. */
	nvmf_fail_disconnect = B_TRUE;
	set_state(NVMF_SC_LOST, B_FALSE);
	bc = new_cmd(&p1, &x1);
	assert(nvmf_bd_dispatch(bc) == ENXIO);
	nvmf_bd_free(bc);
	nvmf_fail_disconnect = B_FALSE;
	idle(&p1);

	/* A queue pair that was shut down under a submitter. */
	set_state(NVMF_SC_ATTACHING, B_TRUE);
	qp_dead = B_TRUE;
	bc = new_cmd(&p1, &x1);
	assert(nvmf_bd_dispatch(bc) == ENXIO);
	nvmf_bd_free(bc);
	qp_dead = B_FALSE;
	idle(&p1);

	/* Removing a namespace fails only its own held commands. */
	set_state(NVMF_SC_LOST, B_FALSE);
	assert(nvmf_bd_dispatch(new_cmd(&p1, &x1)) == 0);
	assert(nvmf_bd_dispatch(new_cmd(&p2, &x2)) == 0);
	nvmf_bd_path_gone(&sc, &p1);
	assert(x1.t_done == 1 && x1.t_error == ENXIO && x2.t_done == 0);
	assert(p1.gone && p1.refs == 0 && p2.refs == 1);
	bc = new_cmd(&p1, &x1);
	assert(nvmf_bd_dispatch(bc) == ENXIO);
	nvmf_bd_free(bc);

	/* A namespace found changed on reconnect fails when resumed. */
	assert(nvmf_bd_dispatch(new_cmd(&p2, &x3)) == 0);
	p2.gone = B_TRUE;
	set_state(NVMF_SC_CONNECTED, B_TRUE);
	nvmf_bd_resume(&sc);
	assert(ninflight == 0);
	assert(x2.t_done == 1 && x2.t_error == ENXIO);
	assert(x3.t_done == 1 && x3.t_error == ENXIO);
	idle(&p1);
	idle(&p2);

	list_destroy(&sc.held);
	printf("host_held: ok\n");
	return (0);
}

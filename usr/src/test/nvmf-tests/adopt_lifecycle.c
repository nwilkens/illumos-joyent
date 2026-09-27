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
 * Run nvmf_adopt_qpair() and nvmf_free_qpair() from nvmf_transport.c against
 * the transport registry: a qpair that the transport made in the kernel must
 * hold the transport exactly as a handed-off qpair does, and unregister must
 * not return while one is alive.
 */
#include "adopt.h"

#include <unistd.h>

static unsigned frees, allocs;
static int errors, receives;

int
nvlist_lookup_boolean_value(nvlist_t *nvl, const char *name, boolean_t *v)
{
	(void) nvl;
	(void) name;
	*v = B_TRUE;
	return (0);
}

static struct nvmf_qpair *
fake_allocate(boolean_t controller, const nvlist_t *nvl)
{
	(void) controller;
	(void) nvl;
	__atomic_add_fetch(&allocs, 1, __ATOMIC_SEQ_CST);
	return (calloc(1, sizeof (struct nvmf_qpair)));
}

static void
fake_free(struct nvmf_qpair *qp)
{
	__atomic_add_fetch(&frees, 1, __ATOMIC_SEQ_CST);
	free(qp);
}

static size_t pool_len;
static unsigned buf_frees;

static int
fake_alloc_buf(struct nvmf_qpair *qp, size_t len, size_t min_len,
    nvmf_databuf_t *db)
{
	(void) qp;
	(void) min_len;
	db->ndb_len = pool_len != 0 ? pool_len : len;
	db->ndb_addr = malloc(db->ndb_len);
	return (0);
}

static int
fake_map_buf(struct nvmf_qpair *qp, const nvmf_seg_t *segs, uint_t nsegs,
    nvmf_databuf_t *db)
{
	(void) qp;
	(void) segs;
	db->ndb_ncookies = nsegs;
	return (nsegs == 0 ? EINVAL : 0);
}

static void
fake_free_buf(nvmf_databuf_t *db)
{
	free(db->ndb_addr);
	buf_frees++;
}

static struct nvmf_transport_ops ops = {
	.allocate_qpair = fake_allocate,
	.free_qpair = fake_free,
	.alloc_data_buf = fake_alloc_buf,
	.map_data_buf = fake_map_buf,
	.free_data_buf = fake_free_buf,
	.trtype = NVMF_TRTYPE_RDMA,
};

/* A second provider of the same type that was never registered. */
static struct nvmf_transport_ops stranger = {
	.free_qpair = fake_free,
	.trtype = NVMF_TRTYPE_RDMA,
};

static struct nvmf_transport_ops bogus = {
	.free_qpair = fake_free,
	.trtype = (nvmf_trtype_t)200,
};

static void
on_error(void *arg, int error)
{
	assert(arg == &errors);
	errors += error;
}

static void
on_receive(void *arg, struct nvmf_capsule *nc)
{
	assert(arg == &receives);
	assert(nc == NULL);
	receives++;
}

static struct nvmf_qpair *
adopt(struct nvmf_transport_ops *o, boolean_t admin, int expect)
{
	struct nvmf_qpair *qp = calloc(1, sizeof (*qp));
	int error;

	error = nvmf_adopt_qpair(o, qp, B_TRUE, admin, on_error, &errors,
	    on_receive, &receives);
	assert(error == expect);
	if (error != 0) {
		/* The transport still owns it; the core touched nothing. */
		assert(qp->nq_transport == NULL && qp->nq_ops == NULL);
		free(qp);
		return (NULL);
	}
	assert(qp->nq_ops == o && qp->nq_controller && qp->nq_admin == admin);
	return (qp);
}

static unsigned
active(void)
{
	struct nvmf_transport *nt = list_head(&nvmf_transports[ops.trtype]);

	assert(nt != NULL && nt->nt_ops == &ops);
	return (nt->nt_active_qpairs);
}

static void
lifecycle(void)
{
	struct nvmf_qpair *a, *b, *h;

	(void) adopt(&ops, B_TRUE, ENXIO);
	(void) adopt(&bogus, B_TRUE, ENXIO);
	assert(nvmf_transport_register(&ops) == 0);
	(void) adopt(&stranger, B_TRUE, ENXIO);

	a = adopt(&ops, B_TRUE, 0);
	b = adopt(&ops, B_FALSE, 0);
	h = nvmf_allocate_qpair(NVMF_TRTYPE_RDMA, B_TRUE, NULL, on_error,
	    &errors, on_receive, &receives);
	assert(h != NULL && active() == 3);

	nvmf_qpair_error(a, 5);
	nvmf_capsule_received(b, NULL);
	assert(errors == 5 && receives == 1);

	assert(nvmf_transport_unregister(&ops) == EBUSY);
	nvmf_free_qpair(a);
	nvmf_free_qpair(h);
	assert(active() == 1 && frees == 2);
	assert(nvmf_transport_unregister(&ops) == EBUSY);
	nvmf_free_qpair(b);
	assert(frees == 3);
	assert(nvmf_transport_unregister(&ops) == 0);
	(void) adopt(&ops, B_TRUE, ENXIO);
	assert(frees == 3);
}

/*
 * Data buffers pin the transport like qpairs do and may outlive the qpair
 * they came from.  A transport that returns too small a buffer loses it.
 */
static void
data_buffers(void)
{
	nvmf_seg_t seg = { 512, NULL };
	nvmf_databuf_t pool, lu, bad;
	struct nvmf_qpair *qp;

	assert(nvmf_transport_register(&ops) == 0);
	qp = adopt(&ops, B_FALSE, 0);
	assert(nvmf_alloc_data_buf(qp, 4096, 512, &pool) == 0);
	assert(pool.ndb_len == 4096 && pool.ndb_transport != NULL);
	assert(nvmf_map_data_buf(qp, &seg, 1, &lu) == 0);
	assert(nvmf_map_data_buf(qp, &seg, 0, &bad) == EINVAL);
	assert(nvmf_alloc_data_buf(qp, 512, 4096, &bad) == EINVAL);
	pool_len = 100;
	assert(nvmf_alloc_data_buf(qp, 4096, 512, &bad) == EINVAL);
	assert(buf_frees == 1);
	pool_len = 8192;
	assert(nvmf_alloc_data_buf(qp, 4096, 512, &bad) == EINVAL);
	assert(buf_frees == 2);
	pool_len = 0;
	assert(active() == 3);

	nvmf_free_qpair(qp);
	assert(nvmf_transport_unregister(&ops) == EBUSY);
	nvmf_free_data_buf(&pool);
	assert(nvmf_transport_unregister(&ops) == EBUSY);
	nvmf_free_data_buf(&lu);
	assert(buf_frees == 4);
	assert(nvmf_transport_unregister(&ops) == 0);

	/* A transport without a pool says so. */
	stranger.allocate_qpair = fake_allocate;
	assert(nvmf_transport_register(&stranger) == 0);
	qp = adopt(&stranger, B_FALSE, 0);
	assert(nvmf_alloc_data_buf(qp, 4096, 512, &bad) == ENOTSUP);
	assert(nvmf_map_data_buf(qp, &seg, 1, &bad) == ENOTSUP);
	nvmf_free_qpair(qp);
	assert(nvmf_transport_unregister(&stranger) == 0);
}

static volatile int stop;

static void *
churn(void *arg)
{
	(void) arg;
	while (!stop) {
		struct nvmf_qpair *qp = calloc(1, sizeof (*qp));

		if (nvmf_adopt_qpair(&ops, qp, B_TRUE, B_FALSE, on_error,
		    &errors, on_receive, &receives) != 0) {
			free(qp);
			continue;
		}
		/* Hold it across a moment of unregister activity. */
		usleep(10);
		nvmf_free_qpair(qp);
		usleep(40);
	}
	return (NULL);
}

/*
 * Unregister races with threads that adopt and free.  Once it returns 0 the
 * transport is gone; a qpair still pointing at it would be a use after free
 * that ASan reports.
 */
static void
unload_race(void)
{
	pthread_t t[4];
	int i, round, tries, busy = 0;

	for (round = 0; round < 200; round++) {
		assert(nvmf_transport_register(&ops) == 0);
		stop = 0;
		for (i = 0; i < 4; i++)
			assert(pthread_create(&t[i], NULL, churn, NULL) == 0);
		usleep(200);
		/* Let the churn run for a while, then let it drain. */
		for (tries = 0; nvmf_transport_unregister(&ops) != 0; tries++) {
			if (tries == 20)
				stop = 1;
			usleep(5);
		}
		busy += (tries < 20);
		stop = 1;
		for (i = 0; i < 4; i++)
			assert(pthread_join(t[i], NULL) == 0);
		assert(nvmf_transport_unregister(&ops) == 0);
	}
	printf("unload race: %d of 200 unregisters won against live churn\n",
	    busy);
}

int
main(void)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(nvmf_transports); i++) {
		list_create(&nvmf_transports[i],
		    sizeof (struct nvmf_transport),
		    offsetof(struct nvmf_transport, nt_link));
	}
	rw_init(&nvmf_transports_lock, NULL, RW_DRIVER, NULL);
	mutex_init(&nvmf_transports_cv_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&nvmf_transports_cv, NULL, CV_DRIVER, NULL);

	lifecycle();
	data_buffers();
	unload_race();
	printf("adopt lifecycle passed (%u qpairs freed)\n", frees);
	return (0);
}

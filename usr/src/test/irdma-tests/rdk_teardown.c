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
 * Teardown from callbacks, with rdk_quiesce.c, rdk_cq.c and rdk_verbs.c
 * running whole against a fake provider on real threads.  A done() that
 * frees its own CQ, or a CQ whose poller is busy in another thread, must
 * not wait; a teardown runs once, outside callbacks, and may free itself;
 * the verbs that would wait for a callback refuse to run in one.  A
 * deadlock trips the alarm.
 */

/* rdk_verbs.c calls into rdk_cm_roce_conn.c, which is not built here. */
struct rdk_qp;
void rdk_cm_roce_qp_gone(struct rdk_qp *);

#include "rdk_unit.h"

void
rdk_cm_roce_qp_gone(struct rdk_qp *qp)
{
	(void) qp;
}

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		exit(1);						\
	}								\
} while (0)

/* rdk_device.c, which is not built here. */
int
rdk_obj_hold(struct rdk_device *dev)
{
	(void) dev;
	return (0);
}

void
rdk_obj_rele(struct rdk_device *dev)
{
	(void) dev;
}

boolean_t
rdk_port_valid(struct rdk_device *dev, uint32_t port)
{
	(void) dev;
	return (port == 1);
}

int
rdk_resolve_ah_attr(struct rdk_device *dev, struct rdk_ah_attr *ah)
{
	(void) dev; (void) ah;
	return (EINVAL);
}

void
rdk_put_gid_attr(const struct rdk_gid_attr *attr)
{
	CHECK(attr == NULL);
}

void
rdk_device_taint(struct rdk_device *dev)
{
	atomic_or_32(&dev->rd_tainted, 1);
}

/* The fake provider. */
#define	FQ	64

struct fake_cq {
	struct rdk_cq	fc_cq;
	pthread_mutex_t	fc_lock;
	struct rdk_wc	fc_q[FQ];
	int		fc_n;
};

static int cq_destroys, qp_destroys, mr_deregs;
static kthread_t *cq_destroyer;

static int
fake_create_cq(struct rdk_cq *cq, const struct rdk_cq_init_attr *attr)
{
	(void) attr;
	(void) pthread_mutex_init(&((struct fake_cq *)cq)->fc_lock, NULL);
	return (0);
}

static void
fake_destroy_cq(struct rdk_cq *cq)
{
	(void) pthread_mutex_destroy(&((struct fake_cq *)cq)->fc_lock);
	cq_destroyer = curthread;
	__atomic_add_fetch(&cq_destroys, 1, __ATOMIC_SEQ_CST);
}

static int
fake_poll_cq(struct rdk_cq *cq, int n, struct rdk_wc *wc)
{
	struct fake_cq *fc = (struct fake_cq *)cq;
	int i;

	(void) pthread_mutex_lock(&fc->fc_lock);
	n = MIN(n, fc->fc_n);
	for (i = 0; i < n; i++)
		wc[i] = fc->fc_q[i];
	(void) memmove(fc->fc_q, fc->fc_q + n,
	    (fc->fc_n - n) * sizeof (fc->fc_q[0]));
	fc->fc_n -= n;
	(void) pthread_mutex_unlock(&fc->fc_lock);
	return (n);
}

static int
fake_req_notify(struct rdk_cq *cq, enum rdk_cq_notify_flags flags)
{
	struct fake_cq *fc = (struct fake_cq *)cq;
	int more;

	(void) pthread_mutex_lock(&fc->fc_lock);
	more = fc->fc_n > 0;
	(void) pthread_mutex_unlock(&fc->fc_lock);
	return ((flags & RDK_CQ_REPORT_MISSED_EVENTS) != 0 && more ? 1 : 0);
}

static int
fake_ok(void)
{
	return (0);
}

static int
fake_alloc_pd(struct rdk_pd *pd)
{
	(void) pd;
	return (fake_ok());
}

static void
fake_dealloc_pd(struct rdk_pd *pd)
{
	(void) pd;
}

static int
fake_create_qp(struct rdk_qp *qp, struct rdk_qp_init_attr *init)
{
	static uint32_t qpn = 10;

	(void) init;
	qp->qp_num = qpn++;
	return (0);
}

static void
fake_destroy_qp(struct rdk_qp *qp)
{
	(void) qp;
	__atomic_add_fetch(&qp_destroys, 1, __ATOMIC_SEQ_CST);
}

static int
fake_modify_qp(struct rdk_qp *qp, struct rdk_qp_attr *a, int mask)
{
	(void) qp; (void) a; (void) mask;
	return (0);
}

/* The drains query first; a fake QP is always connected. */
static int
fake_query_qp(struct rdk_qp *qp, struct rdk_qp_attr *a, int mask,
    struct rdk_qp_init_attr *init)
{
	(void) qp; (void) mask; (void) init;
	a->qp_state = RDK_QPS_RTS;
	return (0);
}

static int
fake_alloc_mr(struct rdk_pd *pd, enum rdk_mr_type t, uint32_t n,
    struct rdk_mr **mrp)
{
	(void) pd; (void) t; (void) n;
	*mrp = kmem_zalloc(sizeof (struct rdk_mr), KM_SLEEP);
	return (0);
}

static int
fake_dereg_mr(struct rdk_mr *mr)
{
	kmem_free(mr, sizeof (*mr));
	__atomic_add_fetch(&mr_deregs, 1, __ATOMIC_SEQ_CST);
	return (0);
}

static void push(struct rdk_cq *, struct rdk_cqe *);

/* Every work request completes at once as flushed, as on a QP in error. */
static void
flush(struct rdk_cq *cq, struct rdk_cqe *cqe)
{
	push(cq, cqe);
	rdk_comp_upcall(cq);
}

static int
fake_post_send(struct rdk_qp *qp, const struct rdk_send_wr *wr,
    const struct rdk_send_wr **bad)
{
	(void) bad;
	for (; wr != NULL; wr = wr->next)
		flush(qp->send_cq, wr->wr_cqe);
	return (0);
}

static int
fake_post_recv(struct rdk_qp *qp, const struct rdk_recv_wr *wr,
    const struct rdk_recv_wr **bad)
{
	(void) bad;
	for (; wr != NULL; wr = wr->next)
		flush(qp->recv_cq, wr->wr_cqe);
	return (0);
}

static const struct rdk_device_ops fake_ops = {
	.version = RDK_ABI_VERSION,
	.alloc_pd = fake_alloc_pd,
	.dealloc_pd = fake_dealloc_pd,
	.create_cq = fake_create_cq,
	.destroy_cq = fake_destroy_cq,
	.poll_cq = fake_poll_cq,
	.req_notify_cq = fake_req_notify,
	.create_qp = fake_create_qp,
	.modify_qp = fake_modify_qp,
	.query_qp = fake_query_qp,
	.destroy_qp = fake_destroy_qp,
	.post_send = fake_post_send,
	.post_recv = fake_post_recv,
	.alloc_mr = fake_alloc_mr,
	.dereg_mr = fake_dereg_mr,
	.size_pd = sizeof (struct rdk_pd),
	.size_cq = sizeof (struct fake_cq),
	.size_qp = sizeof (struct rdk_qp),
	.size_ah = sizeof (struct rdk_ah)
};

static struct rdk_device dev;

static void
push(struct rdk_cq *cq, struct rdk_cqe *cqe)
{
	struct fake_cq *fc = (struct fake_cq *)cq;

	(void) pthread_mutex_lock(&fc->fc_lock);
	CHECK(fc->fc_n < FQ);
	memset(&fc->fc_q[fc->fc_n], 0, sizeof (fc->fc_q[0]));
	fc->fc_q[fc->fc_n].wr_cqe = cqe;
	fc->fc_q[fc->fc_n].status = RDK_WC_WR_FLUSH_ERR;
	fc->fc_n++;
	(void) pthread_mutex_unlock(&fc->fc_lock);
}

/* A completion vector: calls the CQ's handler as the provider would. */
static void *
vector(void *arg)
{
	rdk_comp_upcall(arg);
	return (NULL);
}

static void
wait_for(int *ctr, int want)
{
	hrtime_t end = gethrtime() + SEC2NSEC(10);

	while (__atomic_load_n(ctr, __ATOMIC_SEQ_CST) < want) {
		CHECK(gethrtime() < end);
		(void) usleep(1000);
	}
}

/*
 * 1: a done() frees its own CQ from the vector thread; the vector returns
 * and the CQ is destroyed on the teardown taskq.
 */
static struct rdk_cq *t1_cq;
static int t1_calls, t1_in_cb;

static void
t1_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	(void) wc;
	CHECK(cq == t1_cq);
	if (rdk_in_callback())
		t1_in_cb++;
	if (++t1_calls == 2)
		rdk_free_cq(cq);
}

static void
test_free_own_cq(void)
{
	struct rdk_cqe cqe = { .done = t1_done };
	pthread_t t;
	int i, base = cq_destroys;

	CHECK(rdk_alloc_cq(&dev, NULL, 16, 0, RDK_POLL_TASKQ, &t1_cq) == 0);
	for (i = 0; i < 3; i++)
		push(t1_cq, &cqe);
	CHECK(pthread_create(&t, NULL, vector, t1_cq) == 0);
	CHECK(pthread_join(t, NULL) == 0);
	wait_for(&cq_destroys, base + 1);
	CHECK(cq_destroyer != NULL);
	CHECK(t1_calls == 3 && t1_in_cb == 3);
	CHECK(!rdk_in_callback());
}

/* 2: the same from a direct poll, which must stop at the batch end. */
static int t2_calls;

static void
t2_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	(void) wc;
	if (++t2_calls == 1)
		rdk_free_cq(cq);
}

static void
test_free_direct_cq(void)
{
	struct rdk_cqe cqe = { .done = t2_done };
	struct rdk_cq *cq;
	int i, base = cq_destroys;

	CHECK(rdk_alloc_cq(&dev, NULL, 64, 0, RDK_POLL_DIRECT, &cq) == 0);
	for (i = 0; i < 40; i++)
		push(cq, &cqe);
	/* One batch of 16, and no poll after the done() that freed it. */
	CHECK(rdk_process_cq_direct(cq, -1) == 16);
	CHECK(t2_calls == 16);
	wait_for(&cq_destroys, base + 1);
	CHECK(cq_destroyer != curthread);
}

/*
 * 3: a done() of one CQ frees another whose poller is busy in a done() of
 * its own that waits for the first; waiting there would deadlock.
 */
static struct rdk_cq *t3_a, *t3_b;
static volatile int t3_b_in, t3_freed;

static void
t3_b_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	(void) cq; (void) wc;
	t3_b_in = 1;
	while (!t3_freed)
		(void) usleep(1000);
}

static void
t3_a_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	(void) cq; (void) wc;
	while (!t3_b_in)
		(void) usleep(1000);
	rdk_free_cq(t3_b);
	t3_freed = 1;
}

static void
test_free_busy_cq(void)
{
	struct rdk_cqe a = { .done = t3_a_done }, b = { .done = t3_b_done };
	pthread_t ta, tb;
	int base = cq_destroys;

	CHECK(rdk_alloc_cq(&dev, NULL, 16, 0, RDK_POLL_TASKQ, &t3_a) == 0);
	CHECK(rdk_alloc_cq(&dev, NULL, 16, 0, RDK_POLL_TASKQ, &t3_b) == 0);
	push(t3_a, &a);
	push(t3_b, &b);
	CHECK(pthread_create(&tb, NULL, vector, t3_b) == 0);
	CHECK(pthread_create(&ta, NULL, vector, t3_a) == 0);
	CHECK(pthread_join(ta, NULL) == 0);
	CHECK(pthread_join(tb, NULL) == 0);
	wait_for(&cq_destroys, base + 1);
	rdk_free_cq(t3_a);
	CHECK(cq_destroys == base + 2);
}

/*
 * 4: a done() starts a teardown twice; it runs once, outside callbacks,
 * destroys the QP, MR and CQ, and frees itself.
 */
struct t4_queue {
	struct rdk_cqe		q_cqe;
	struct rdk_pd		*q_pd;
	struct rdk_cq		*q_cq;
	struct rdk_qp		*q_qp;
	struct rdk_mr		*q_mr;
	rdk_teardown_t		*q_td;
	int			q_runs;
	int			q_in_cb;
	int			q_started;
	int			q_done;
};

static void
t4_teardown(void *arg)
{
	struct t4_queue *q = arg;

	q->q_runs++;
	q->q_in_cb = rdk_in_callback();
	CHECK(rdk_teardown_dying(q->q_td));
	rdk_drain_qp(q->q_qp);
	rdk_destroy_qp(q->q_qp);
	CHECK(rdk_dereg_mr(q->q_mr) == 0);
	rdk_free_cq(q->q_cq);
	rdk_dealloc_pd(q->q_pd);
	rdk_teardown_free(q->q_td);
	__atomic_store_n(&q->q_done, 1, __ATOMIC_SEQ_CST);
}

static void
t4_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	struct t4_queue *q = cq->cq_context;

	(void) wc;
	CHECK(!rdk_teardown_dying(q->q_td) || q->q_started > 0);
	if (rdk_teardown_start(q->q_td))
		q->q_started++;
	CHECK(!rdk_teardown_start(q->q_td));
	CHECK(rdk_teardown_dying(q->q_td));
	/* Freed by the teardown's own call once it returns. */
	(void) usleep(20000);
}

static void
test_teardown(void)
{
	static struct t4_queue q;
	struct rdk_qp_init_attr init;
	pthread_t t;
	int cq0 = cq_destroys, qp0 = qp_destroys, mr0 = mr_deregs;

	q.q_cqe.done = t4_done;
	CHECK(rdk_alloc_pd(&dev, 0, &q.q_pd) == 0);
	CHECK(rdk_alloc_cq(&dev, &q, 16, 0, RDK_POLL_TASKQ, &q.q_cq) == 0);
	memset(&init, 0, sizeof (init));
	init.send_cq = init.recv_cq = q.q_cq;
	init.qp_type = RDK_QPT_UD;
	init.sq_sig_type = RDK_SIGNAL_REQ_WR;
	CHECK(rdk_create_qp(q.q_pd, &init, &q.q_qp) == 0);
	CHECK(rdk_alloc_mr(q.q_pd, RDK_MR_TYPE_MEM_REG, 4, &q.q_mr) == 0);
	q.q_td = rdk_teardown_alloc(t4_teardown, &q);
	push(q.q_cq, &q.q_cqe);
	CHECK(pthread_create(&t, NULL, vector, q.q_cq) == 0);
	CHECK(pthread_join(t, NULL) == 0);
	wait_for(&q.q_done, 1);
	CHECK(q.q_runs == 1 && q.q_started == 1 && q.q_in_cb == 0);
	CHECK(cq_destroys == cq0 + 1 && qp_destroys == qp0 + 1 &&
	    mr_deregs == mr0 + 1);
}

/*
 * 5: from a callback the verbs that wait refuse: destroys and drains
 * panic before they touch anything, and dereg returns EDEADLK.
 */
static struct rdk_qp *t5_qp;
static struct rdk_cq *t5_raw;
static struct rdk_mr *t5_mr;
static rdk_teardown_t *t5_td;
static int t5_panics, t5_edeadlk;

static void
t5_nothing(void *arg)
{
	int *ran = arg;

	(*ran)++;
	(void) usleep(50000);
}

#define	EXPECT_PANIC(call, what)	do {				\
	jmp_buf jb;							\
	kenv_panic_jmp = &jb;						\
	if (setjmp(jb) == 0) {						\
		call;							\
		CHECK(!"no panic from " what);				\
	}								\
	kenv_panic_jmp = NULL;						\
	CHECK(strstr(kenv_panic_msg, what) != NULL);			\
	t5_panics++;							\
} while (0)

static void
t5_done(struct rdk_cq *cq, struct rdk_wc *wc)
{
	int deregs = mr_deregs, qps = qp_destroys;

	(void) cq; (void) wc;
	EXPECT_PANIC(rdk_destroy_qp(t5_qp), "rdk_destroy_qp");
	EXPECT_PANIC(rdk_drain_qp(t5_qp), "rdk_drain_sq");
	EXPECT_PANIC(rdk_drain_rq(t5_qp), "rdk_drain_rq");
	EXPECT_PANIC(rdk_destroy_cq(t5_raw), "rdk_destroy_cq");
	CHECK(rdk_teardown_start(t5_td));
	EXPECT_PANIC(rdk_teardown_wait(t5_td), "rdk_teardown_wait");
	if (rdk_dereg_mr(t5_mr) == EDEADLK)
		t5_edeadlk++;
	CHECK(mr_deregs == deregs && qp_destroys == qps);
	/* A pending teardown freed from a callback goes when it returns. */
	rdk_teardown_free(t5_td);
}

static void
test_forbidden(void)
{
	struct rdk_cqe cqe = { .done = t5_done };
	struct rdk_cq_init_attr ca = { .cqe = 4 };
	struct rdk_qp_init_attr init;
	struct rdk_pd *pd;
	struct rdk_cq *cq;
	int ran = 0;

	CHECK(rdk_alloc_pd(&dev, 0, &pd) == 0);
	CHECK(rdk_alloc_cq(&dev, NULL, 16, 0, RDK_POLL_DIRECT, &cq) == 0);
	memset(&init, 0, sizeof (init));
	init.send_cq = init.recv_cq = cq;
	init.qp_type = RDK_QPT_RC;
	init.sq_sig_type = RDK_SIGNAL_REQ_WR;
	CHECK(rdk_create_qp(pd, &init, &t5_qp) == 0);
	CHECK(rdk_alloc_mr(pd, RDK_MR_TYPE_MEM_REG, 4, &t5_mr) == 0);
	CHECK(rdk_create_cq(&dev, NULL, NULL, NULL, &ca, &t5_raw) == 0);
	t5_td = rdk_teardown_alloc(t5_nothing, &ran);
	push(cq, &cqe);
	CHECK(rdk_process_cq_direct(cq, -1) == 1);
	CHECK(t5_panics == 5 && t5_edeadlk == 1);
	CHECK(!rdk_in_callback());
	/* Outside callbacks the same verbs run. */
	CHECK(rdk_dereg_mr(t5_mr) == 0);
	rdk_destroy_qp(t5_qp);
	rdk_destroy_cq(t5_raw);
	rdk_free_cq(cq);
	rdk_dealloc_pd(pd);
	(void) usleep(100000);
	CHECK(ran == 1);
}

/* 6: an event upcall marks the handler as a callback. */
static int t6_seen;

static void
t6_handler(struct rdk_event *ev, void *arg)
{
	CHECK(ev->event == RDK_EVENT_QP_FATAL && arg == &t6_seen);
	if (rdk_in_callback())
		t6_seen++;
}

static void
test_upcall(void)
{
	struct rdk_event ev;

	memset(&ev, 0, sizeof (ev));
	ev.event = RDK_EVENT_QP_FATAL;
	rdk_event_upcall(t6_handler, &ev, &t6_seen);
	rdk_event_upcall(NULL, &ev, &t6_seen);
	CHECK(t6_seen == 1 && !rdk_in_callback());
}

/* 8: a raw CQ's handler runs marked, and cannot destroy its CQ. */
static int t8_marked, t8_panics;

static void
t8_handler(struct rdk_cq *cq, void *arg)
{
	jmp_buf jb;

	(void) arg;
	if (rdk_in_callback())
		t8_marked++;
	kenv_panic_jmp = &jb;
	if (setjmp(jb) == 0)
		rdk_destroy_cq(cq);
	else
		t8_panics++;
	kenv_panic_jmp = NULL;
}

static void
test_raw_handler(void)
{
	struct rdk_cq_init_attr ca = { .cqe = 4 };
	struct rdk_cq *cq;
	int base = cq_destroys;

	CHECK(rdk_create_cq(&dev, t8_handler, NULL, NULL, &ca, &cq) == 0);
	rdk_comp_upcall(cq);
	CHECK(t8_marked == 1 && t8_panics == 1 && cq_destroys == base);
	CHECK(!rdk_in_callback());
	rdk_destroy_cq(cq);
	CHECK(cq_destroys == base + 1);
}

/* 7: a teardown waited for from thread context, then freed. */
static void
t7_fn(void *arg)
{
	(void) usleep(20000);
	__atomic_store_n((int *)arg, 1, __ATOMIC_SEQ_CST);
}

static void
test_wait(void)
{
	int done = 0;
	rdk_teardown_t *td = rdk_teardown_alloc(t7_fn, &done);

	CHECK(!rdk_teardown_dying(td));
	rdk_teardown_wait(td);
	CHECK(rdk_teardown_start(td));
	rdk_teardown_wait(td);
	CHECK(done == 1);
	CHECK(!rdk_teardown_start(td));
	rdk_teardown_free(td);
}

int
main(int argc, char **argv)
{
	(void) alarm(argc > 1 ? (unsigned)atoi(argv[1]) : 60);
	(void) strcpy(dev.rd_name, "fake0");
	dev.rd_ops = &fake_ops;
	dev.rd_num_comp_vectors = 1;
	dev.rd_attr.max_cqe = 1024;
	dev.rd_attr.kernel_cap_flags = RDK_KCAP_LOCAL_DMA_LKEY;
	dev.rd_attr.max_fast_reg_page_list_len = 64;
	CHECK(rdk_cq_init() == 0);

	test_free_own_cq();
	test_free_direct_cq();
	test_free_busy_cq();
	test_teardown();
	test_forbidden();
	test_upcall();
	test_wait();
	test_raw_handler();

	rdk_cq_fini();
	(void) printf("PASS: teardown from callbacks\n");
	return (0);
}

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
 * Drive nvmf_tcp.c as a whole: a host qpair and a controller qpair joined by
 * a socketpair move commands and data, and a raw peer sends what a hostile or
 * dying peer would.  The stand-ins below give the transport messages, worker
 * threads, a socket and an open file; ASan watches every buffer.
 */
#include "tcp_xfer.h"

#include <fcntl.h>
#include <poll.h>

int mod_miscops;

/* Messages. */

mblk_t *
allocb_wait(size_t size, unsigned pri, unsigned flags, int *error)
{
	dblk_t *db = malloc(sizeof (*db) + size);
	mblk_t *mp = calloc(1, sizeof (*mp));

	(void) pri;
	assert(flags == STR_NOSIG && db != NULL && mp != NULL);
	*error = 0;
	db->db_ref = 1;
	db->db_size = size;
	mp->b_datap = db;
	mp->b_rptr = mp->b_wptr = (unsigned char *)(db + 1);
	return (mp);
}

mblk_t *
dupb(mblk_t *mp)
{
	mblk_t *nmp = calloc(1, sizeof (*nmp));

	*nmp = *mp;
	nmp->b_next = nmp->b_cont = NULL;
	__atomic_add_fetch(&mp->b_datap->db_ref, 1, __ATOMIC_SEQ_CST);
	return (nmp);
}

void
freeb(mblk_t *mp)
{
	if (__atomic_sub_fetch(&mp->b_datap->db_ref, 1, __ATOMIC_SEQ_CST) == 0)
		free(mp->b_datap);
	free(mp);
}

void
freemsg(mblk_t *mp)
{
	while (mp != NULL) {
		mblk_t *next = mp->b_cont;

		assert(mp->b_next == NULL);
		freeb(mp);
		mp = next;
	}
}

size_t
msgdsize(mblk_t *mp)
{
	size_t n = 0;

	for (; mp != NULL; mp = mp->b_cont)
		n += MBLKL(mp);
	return (n);
}

/* Worker threads. */

struct task {
	void	(*t_func)(void *);
	void	*t_arg;
};

static void *
task_main(void *arg)
{
	struct task *t = arg;

	t->t_func(t->t_arg);
	free(t);
	return (NULL);
}

taskq_t *
taskq_create(const char *name, int n, int pri, int min, int max, unsigned f)
{
	(void) name, (void) n, (void) pri, (void) min, (void) max, (void) f;
	return (calloc(1, sizeof (taskq_t)));
}

int
taskq_dispatch(taskq_t *tq, void (*func)(void *), void *arg, unsigned flags)
{
	struct task *t = malloc(sizeof (*t));

	(void) flags;
	assert(!tq->tq_running);
	t->t_func = func;
	t->t_arg = arg;
	tq->tq_running = B_TRUE;
	assert(pthread_create(&tq->tq_thread, NULL, task_main, t) == 0);
	return (1);
}

void
taskq_wait(taskq_t *tq)
{
	if (tq->tq_running) {
		assert(pthread_join(tq->tq_thread, NULL) == 0);
		tq->tq_running = B_FALSE;
	}
}

void
taskq_destroy(taskq_t *tq)
{
	taskq_wait(tq);
	free(tq);
}

/* Open files and sockets.  The fd table has one process: this one. */

#define	NFILES	64
static file_t *files[NFILES];
static unsigned closes;

static int
fd_install(int fd)
{
	file_t *fp = calloc(1, sizeof (*fp));

	assert(fd >= 0 && fd < NFILES && files[fd] == NULL);
	mutex_init(&fp->f_tlock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&fp->f_so.so_lock, NULL, MUTEX_DRIVER, NULL);
	fp->f_count = 1;
	fp->f_vnode = &fp->f_so.so_vnode;
	fp->f_so.so_family = AF_INET;
	fp->f_so.so_type = SOCK_STREAM;
	fp->f_so.so_protocol = IPPROTO_TCP;
	fp->f_so.so_state = SS_ISCONNECTED;
	fp->f_so.so_fd = fd;
	files[fd] = fp;
	return (fd);
}

struct sonode *
getsonode(int fd, int *errorp, file_t **fpp)
{
	if (fd < 0 || fd >= NFILES || files[fd] == NULL) {
		*errorp = EBADF;
		return (NULL);
	}
	*fpp = files[fd];
	return (&files[fd]->f_so);
}

void
releasef(int fd)
{
	assert(files[fd] != NULL);
}

int
closef(file_t *fp)
{
	unsigned count;

	mutex_enter(&fp->f_tlock);
	count = fp->f_count--;
	mutex_exit(&fp->f_tlock);
	assert(count >= 1);
	if (count > 1)
		return (0);
	/* The last close: nothing of the kernel's is left on the socket. */
	assert((fp->f_so.so_mode & SM_KERNEL) == 0);
	(void) close(fp->f_so.so_fd);
	__atomic_add_fetch(&closes, 1, __ATOMIC_SEQ_CST);
	free(fp);
	return (0);
}

/* What the daemon's close(2) does. */
static void
daemon_close(int fd)
{
	file_t *fp = files[fd];

	files[fd] = NULL;
	(void) closef(fp);
}

static int recv_timeouts;

int
ksocket_recv(ksocket_t ks, void *buf, size_t len, int flags, size_t *recvd,
    cred_t *cr)
{
	ssize_t n;

	(void) cr;
	assert((ks->so_mode & SM_KERNEL) != 0);
	/* A receive timeout that the daemon left behind. */
	if ((flags & MSG_WAITALL) != 0 && recv_timeouts != 0 &&
	    __atomic_fetch_sub(&recv_timeouts, 1, __ATOMIC_SEQ_CST) > 0) {
		*recvd = 0;
		return (EAGAIN);
	}
	do {
		n = recv(ks->so_fd, buf, len, flags);
	} while (n == -1 && errno == EINTR);
	*recvd = n > 0 ? (size_t)n : 0;
	return (n == -1 ? errno : 0);
}

int
ksocket_sendmblk(ksocket_t ks, struct msghdr *msg, int flags, mblk_t **mpp,
    cred_t *cr)
{
	mblk_t *mp;
	int error = 0;

	(void) msg, (void) flags, (void) cr;
	assert((ks->so_mode & SM_KERNEL) != 0);
	for (mp = *mpp; mp != NULL && error == 0; mp = mp->b_cont) {
		size_t done = 0;

		while (done < MBLKL(mp)) {
			ssize_t n = send(ks->so_fd, mp->b_rptr + done,
			    MBLKL(mp) - done, 0);

			if (n == -1 && errno == EINTR)
				continue;
			if (n == -1) {
				error = errno;
				break;
			}
			done += n;
		}
	}
	freemsg(*mpp);
	*mpp = NULL;
	return (error);
}

int
ksocket_shutdown(ksocket_t ks, int how, cred_t *cr)
{
	(void) cr;
	assert((ks->so_mode & SM_KERNEL) != 0);
	return (shutdown(ks->so_fd, how) == -1 ? errno : 0);
}

static unsigned timeouts_cleared;

int
ksocket_setsockopt(ksocket_t ks, int level, int name, const void *val,
    int len, cred_t *cr)
{
	const struct timeval *tv = val;

	(void) cr;
	assert((ks->so_mode & SM_KERNEL) != 0 && level == SOL_SOCKET);
	assert(name == SO_RCVTIMEO || name == SO_SNDTIMEO);
	assert(len == sizeof (*tv) && tv->tv_sec == 0 && tv->tv_usec == 0);
	timeouts_cleared++;
	return (0);
}

/* A handoff nvlist. */

struct nvlist {
	uint64_t	fd, rxpda, txpda, maxr2t, maxh2cdata, max_icd, qsize;
	boolean_t	hdgst, ddgst, sqfc;
	const char	*missing;
};

int
nvlist_lookup_uint64(nvlist_t *nvl, const char *name, uint64_t *v)
{
	static const char *names[] = { "fd", "rxpda", "txpda", "maxr2t",
	    "maxh2cdata", "max_icd", "qsize" };
	const uint64_t *vals = &nvl->fd;
	unsigned i;

	if (nvl->missing != NULL && strcmp(name, nvl->missing) == 0)
		return (ENOENT);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (strcmp(name, names[i]) == 0) {
			*v = vals[i];
			return (0);
		}
	}
	return (ENOENT);
}

int
nvlist_lookup_boolean_value(nvlist_t *nvl, const char *name, boolean_t *v)
{
	if (nvl->missing != NULL && strcmp(name, nvl->missing) == 0)
		return (ENOENT);
	if (strcmp(name, "header_digests") == 0)
		*v = nvl->hdgst;
	else if (strcmp(name, "data_digests") == 0)
		*v = nvl->ddgst;
	else if (strcmp(name, "sq_flow_control") == 0)
		*v = nvl->sqfc;
	else
		return (ENOENT);
	return (0);
}

/* The module framework and the transport core's registry. */

static struct nvmf_transport transport;

int
mod_install(struct modlinkage *ml)
{
	(void) ml;
	return (0);
}

int
mod_remove(struct modlinkage *ml)
{
	(void) ml;
	return (0);
}

int
mod_info(struct modlinkage *ml, struct modinfo *mi)
{
	(void) ml, (void) mi;
	return (0);
}

int
nvmf_transport_register(struct nvmf_transport_ops *ops)
{
	transport.nt_ops = ops;
	return (0);
}

int
nvmf_transport_unregister(struct nvmf_transport_ops *ops)
{
	(void) ops;
	return (transport.nt_active_qpairs != 0 ? EBUSY : 0);
}

/* The consumer of one qpair. */

typedef struct consumer {
	struct nvmf_qpair	*c_qp;
	kmutex_t		c_lock;
	kcondvar_t		c_cv;
	struct nvmf_capsule	*c_rx[16];
	unsigned		c_nrx;
	int			c_errors;
	int			c_error;
	int			c_fd;
} consumer_t;

static void
on_error(void *arg, int error)
{
	consumer_t *c = arg;

	mutex_enter(&c->c_lock);
	c->c_errors++;
	c->c_error = error;
	cv_broadcast(&c->c_cv);
	mutex_exit(&c->c_lock);
}

static void
on_receive(void *arg, struct nvmf_capsule *nc)
{
	consumer_t *c = arg;

	mutex_enter(&c->c_lock);
	assert(c->c_nrx < ARRAY_SIZE(c->c_rx));
	c->c_rx[c->c_nrx++] = nc;
	cv_broadcast(&c->c_cv);
	mutex_exit(&c->c_lock);
}

static struct nvmf_capsule *
next_capsule(consumer_t *c)
{
	struct nvmf_capsule *nc;

	mutex_enter(&c->c_lock);
	while (c->c_nrx == 0 && c->c_errors == 0)
		cv_wait(&c->c_cv, &c->c_lock);
	assert(c->c_nrx != 0);
	nc = c->c_rx[0];
	c->c_nrx--;
	memmove(&c->c_rx[0], &c->c_rx[1], c->c_nrx * sizeof (c->c_rx[0]));
	mutex_exit(&c->c_lock);
	return (nc);
}

static int
wait_error(consumer_t *c)
{
	int error;

	mutex_enter(&c->c_lock);
	while (c->c_errors == 0)
		cv_wait(&c->c_cv, &c->c_lock);
	error = c->c_error;
	mutex_exit(&c->c_lock);
	return (error);
}

static struct nvlist defaults = {
	.rxpda = 4, .txpda = 4, .maxr2t = 0, .maxh2cdata = 8192,
	.max_icd = 4096, .qsize = 8, .hdgst = B_TRUE, .ddgst = B_TRUE,
	.sqfc = B_FALSE,
};

/* What nvmf_allocate_qpair() does with the TCP transport's ops. */
static consumer_t *
attach(int fd, boolean_t controller, const struct nvlist *params)
{
	consumer_t *c = calloc(1, sizeof (*c));
	struct nvlist nvl = *params;
	struct nvmf_qpair *qp;

	nvl.fd = fd;
	qp = tcp_ops.allocate_qpair(controller, &nvl);
	if (qp == NULL) {
		free(c);
		return (NULL);
	}
	atomic_inc_uint(&transport.nt_active_qpairs);
	mutex_init(&c->c_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&c->c_cv, NULL, CV_DRIVER, NULL);
	c->c_fd = fd;
	c->c_qp = qp;
	qp->nq_transport = &transport;
	qp->nq_ops = &tcp_ops;
	qp->nq_controller = controller;
	qp->nq_error = on_error;
	qp->nq_error_arg = c;
	qp->nq_receive = on_receive;
	qp->nq_receive_arg = c;
	tcp_ops.start_qpair(qp);
	return (c);
}

static void
detach(consumer_t *c)
{
	tcp_ops.free_qpair(c->c_qp);
	nvmf_transport_rele(&transport);
	assert(c->c_nrx == 0);
	free(c);
}

static void
pair(int sv[2])
{
	int one = 1;

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
#ifdef SO_NOSIGPIPE
	(void) setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
	(void) setsockopt(sv[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
#else
	(void) one;
#endif
}

/* One transfer's completion. */
typedef struct xfer {
	kmutex_t	x_lock;
	kcondvar_t	x_cv;
	int		x_calls;
	size_t		x_xfered;
	int		x_error;
} xfer_t;

static void
xfer_init(xfer_t *x)
{
	memset(x, 0, sizeof (*x));
	mutex_init(&x->x_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&x->x_cv, NULL, CV_DRIVER, NULL);
}

static void
xfer_done(void *arg, size_t xfered, int error)
{
	xfer_t *x = arg;

	mutex_enter(&x->x_lock);
	x->x_calls++;
	x->x_xfered = xfered;
	x->x_error = error;
	cv_broadcast(&x->x_cv);
	mutex_exit(&x->x_lock);
}

static void
xfer_wait(xfer_t *x)
{
	mutex_enter(&x->x_lock);
	while (x->x_calls == 0)
		cv_wait(&x->x_cv, &x->x_lock);
	mutex_exit(&x->x_lock);
}

static void
fill(uint8_t *p, size_t len, unsigned seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(seed + i * 7 + (i >> 8));
}

static nvmf_memdesc_t
vaddr(void *p, size_t len)
{
	nvmf_memdesc_t md = { .nmd_type = NVMF_MEMDESC_VADDR, .nmd_len = len };

	md.nmd_u.nmd_vaddr = p;
	return (md);
}

/* A host write of len bytes: in the capsule, or fetched with R2Ts. */
static void
do_write(consumer_t *h, consumer_t *c, uint16_t cid, size_t len)
{
	uint8_t *src = malloc(len), *dst = malloc(len);
	nvmf_memdesc_t smd = vaddr(src, len), dmd = vaddr(dst, len);
	struct nvmf_capsule *cmd, *nc, *rsp;
	nvme_sqe_t sqe = { .sqe_opc = 0x01, .sqe_cid = cid };
	nvme_cqe_t cqe = { .cqe_cid = cid };
	xfer_t hx, cx;

	fill(src, len, cid);
	xfer_init(&hx);
	xfer_init(&cx);

	cmd = nvmf_allocate_command(h->c_qp, &sqe, KM_SLEEP);
	assert(nvmf_capsule_append_data(cmd, &smd, len, B_TRUE, xfer_done,
	    &hx) == 0);
	assert(tcp_ops.transmit_capsule(cmd) == 0);

	nc = next_capsule(c);
	assert(tcp_ops.validate_command_capsule(nc) == 0);
	assert(tcp_ops.capsule_data_len(nc) == len);
	assert(nvmf_receive_controller_data(nc, 0, &dmd, len, xfer_done,
	    &cx) == 0);
	xfer_wait(&cx);
	assert(cx.x_calls == 1 && cx.x_error == 0 && cx.x_xfered == len);
	assert(memcmp(src, dst, len) == 0);

	rsp = nvmf_allocate_response(c->c_qp, &cqe, KM_SLEEP);
	assert(tcp_ops.transmit_capsule(rsp) == 0);
	nvmf_free_capsule(rsp);
	nvmf_free_capsule(nc);

	nc = next_capsule(h);
	assert(nc->nc_cqe.cqe_cid == cid && nc->nc_sqhd_valid);
	nvmf_free_capsule(nc);
	xfer_wait(&hx);
	assert(hx.x_calls == 1 && hx.x_error == 0 && hx.x_xfered == len);
	nvmf_free_capsule(cmd);
	free(src);
	free(dst);
}

/* A host read: C2H data in max_tx_data pieces, the last with SUCCESS. */
static void
do_read(consumer_t *h, consumer_t *c, uint16_t cid, size_t len)
{
	uint8_t *src = malloc(len), *dst = malloc(len);
	nvmf_memdesc_t dmd = vaddr(dst, len);
	struct nvmf_capsule *cmd, *nc;
	nvme_sqe_t sqe = { .sqe_opc = 0x02, .sqe_cid = cid };
	mblk_t *mp;
	xfer_t hx;
	int error;

	fill(src, len, cid);
	xfer_init(&hx);
	cmd = nvmf_allocate_command(h->c_qp, &sqe, KM_SLEEP);
	assert(nvmf_capsule_append_data(cmd, &dmd, len, B_FALSE, xfer_done,
	    &hx) == 0);
	assert(tcp_ops.transmit_capsule(cmd) == 0);

	nc = next_capsule(c);
	assert(tcp_ops.validate_command_capsule(nc) == 0);
	mp = allocb_wait(len, BPRI_MED, STR_NOSIG, &error);
	memcpy(mp->b_wptr, src, len);
	mp->b_wptr += len;
	assert(tcp_ops.send_controller_data(nc, 0, mp, len) ==
	    NVMF_SUCCESS_SENT);
	nvmf_free_capsule(nc);

	nc = next_capsule(h);
	assert(nc->nc_cqe.cqe_cid == cid && !nc->nc_sqhd_valid);
	nvmf_free_capsule(nc);
	xfer_wait(&hx);
	assert(hx.x_calls == 1 && hx.x_error == 0 && hx.x_xfered == len);
	assert(memcmp(src, dst, len) == 0);
	nvmf_free_capsule(cmd);
	free(src);
	free(dst);
}

static void
host_and_controller(void)
{
	consumer_t *h, *c;
	int sv[2];

	pair(sv);
	h = attach(fd_install(sv[0]), B_FALSE, &defaults);
	c = attach(fd_install(sv[1]), B_TRUE, &defaults);
	assert(h != NULL && c != NULL);
	/* The daemon closes its descriptors once the handoff returns. */
	daemon_close(sv[0]);
	daemon_close(sv[1]);
	assert(tcp_ops.max_ioccsz(c->c_qp) == 64 + nvmf_tcp_max_icd);
	assert(tcp_ops.max_xfer_size(h->c_qp) == nvmf_tcp_max_io_size);

	recv_timeouts = 4;
	do_write(h, c, 1, 512);			/* in-capsule */
	do_write(h, c, 2, 4096);		/* in-capsule, at the limit */
	do_write(h, c, 3, 4097);		/* one R2T, one H2C PDU */
	do_write(h, c, 4, 100000);		/* one R2T, many H2C PDUs */
	do_read(h, c, 5, 1);
	do_read(h, c, 6, 300000);		/* two C2H PDUs */
	do_read(h, c, 7, 1024 * 1024);

	assert(h->c_errors == 0 && c->c_errors == 0);
	assert(closes == 0 && recv_timeouts <= 0);
	detach(h);
	/* The peer sees the close as an orderly end. */
	assert(wait_error(c) == 0);
	detach(c);
	assert(closes == 2 && transport.nt_active_qpairs == 0);
}

/* A raw peer: build and read PDUs by hand, without digests. */

static const struct nvlist plain = {
	.rxpda = 4, .txpda = 4, .maxr2t = 0, .maxh2cdata = 8192,
	.max_icd = 4096, .qsize = 8, .hdgst = B_FALSE, .ddgst = B_FALSE,
	.sqfc = B_TRUE,
};

static void
put(int fd, const void *p, size_t len)
{
	assert(send(fd, p, len, 0) == (ssize_t)len);
}

/* Read one PDU; returns its type, or -1 at the end of the stream. */
static int
get(int fd, uint8_t *pdu, size_t size)
{
	nvmf_tcp_common_pdu_hdr_t *ch = (void *)pdu;
	ssize_t n;

	n = recv(fd, pdu, sizeof (*ch), MSG_WAITALL);
	if (n <= 0)
		return (-1);
	assert(n == sizeof (*ch) && ch->ntcph_plen <= size);
	n = recv(fd, pdu + sizeof (*ch), ch->ntcph_plen - sizeof (*ch),
	    MSG_WAITALL);
	assert(n == (ssize_t)(ch->ntcph_plen - sizeof (*ch)));
	return (ch->ntcph_pdu_type);
}

/* Bits 7:6 of the second byte of the commands that put_command() sends. */
static uint8_t put_psdt = NVME_PSDT_SGL;

static void
put_command(int fd, uint16_t cid, uint32_t sgl_len, uint32_t icd)
{
	uint8_t buf[sizeof (nvmf_tcp_cmd_t) + 32768] = { 0 };
	nvmf_tcp_cmd_t *cmd = (void *)buf;
	nvme_sqe_t sqe = { .sqe_opc = 0x01, .sqe_cid = cid };
	uint8_t type = icd != 0 ? NVME_SGL_TYPE_ICD :
	    NVME_SGL_TYPE_COMMAND_BUFFER;

	assert(icd <= 32768);
	sqe.sqe_dptr.d_sgl.sgl_len = sgl_len;
	sqe.sqe_dptr.d_sgl.sgl_type = type >> 4;
	sqe.sqe_dptr.d_sgl.sgl_zero = type & 0xf;
	NVMF_SQE_SET_PSDT(&sqe, put_psdt);
	cmd->ntc_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_CMD;
	cmd->ntc_common.ntcph_hlen = sizeof (*cmd);
	cmd->ntc_common.ntcph_pdo = icd != 0 ? sizeof (*cmd) : 0;
	cmd->ntc_common.ntcph_plen = sizeof (*cmd) + icd;
	memcpy(cmd->ntc_ccsqe, &sqe, sizeof (sqe));
	put(fd, buf, sizeof (*cmd) + icd);
}

static void
put_h2c(int fd, uint16_t cid, uint16_t ttag, uint32_t off, uint32_t len,
    boolean_t last, unsigned seed)
{
	uint8_t buf[sizeof (nvmf_tcp_h2c_data_hdr_t) + 8192] = { 0 };
	nvmf_tcp_h2c_data_hdr_t *h2c = (void *)buf;

	assert(len <= 8192);
	h2c->nth2c_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_H2C_DATA;
	h2c->nth2c_common.ntcph_flags = last ?
	    NVMF_TCP_H2C_DATA_FLAGS_LAST_PDU : 0;
	h2c->nth2c_common.ntcph_hlen = sizeof (*h2c);
	h2c->nth2c_common.ntcph_pdo = sizeof (*h2c);
	h2c->nth2c_common.ntcph_plen = sizeof (*h2c) + len;
	h2c->nth2c_cccid = cid;
	h2c->nth2c_ttag = ttag;
	h2c->nth2c_datao = off;
	h2c->nth2c_datal = len;
	fill(buf + sizeof (*h2c), len, seed);
	put(fd, buf, sizeof (*h2c) + len);
}

/* The peer expects a termination request with this status, then the end. */
static void
expect_term(int fd, uint16_t fes)
{
	uint8_t pdu[NVMF_TCP_TERM_REQ_PDU_MAX_SIZE];
	nvmf_tcp_term_req_hdr_t *term = (void *)pdu;
	int type = get(fd, pdu, sizeof (pdu));

	assert(type == NVMF_TCP_PDU_TYPE_C2H_TERM_REQ ||
	    type == NVMF_TCP_PDU_TYPE_H2C_TERM_REQ);
	assert(term->nttr_fes == fes);
}

static consumer_t *
raw(int *peer, boolean_t controller, const struct nvlist *params)
{
	consumer_t *c;
	int sv[2];

	pair(sv);
	c = attach(fd_install(sv[0]), controller, params);
	assert(c != NULL);
	daemon_close(sv[0]);
	*peer = sv[1];
	return (c);
}

/*
 * Cancel a receive whose R2T went out and that got half its data.  The
 * callback runs once, on this thread, and the buffer can be freed at once;
 * the rest of the data arrives later and goes nowhere.  The connection lives.
 */
static void
cancel_open_transfer(void)
{
	uint8_t pdu[256], *dst = malloc(8192), ref[4096];
	nvmf_memdesc_t dmd = vaddr(dst, 8192);
	nvmf_tcp_r2t_hdr_t *r2t = (void *)pdu;
	nvme_cqe_t cqe = { .cqe_cid = 9 };
	struct nvmf_capsule *nc, *rsp;
	consumer_t *c;
	uint16_t ttag;
	xfer_t x;
	int peer;

	c = raw(&peer, B_TRUE, &plain);
	xfer_init(&x);
	put_command(peer, 9, 8192, 0);
	nc = next_capsule(c);
	assert(tcp_ops.validate_command_capsule(nc) == 0);
	/* Nothing to cancel yet. */
	nvmf_cancel_controller_data(nc);
	assert(nvmf_receive_controller_data(nc, 0, &dmd, 8192, xfer_done,
	    &x) == 0);
	assert(get(peer, pdu, sizeof (pdu)) == NVMF_TCP_PDU_TYPE_R2T);
	assert(r2t->ntr2t_cccid == 9 && r2t->ntr2t_r2tl == 8192);
	ttag = r2t->ntr2t_ttag;

	put_h2c(peer, 9, ttag, 0, 4096, B_FALSE, 1);
	fill(ref, 4096, 1);
	while (memcmp(dst, ref, 4096) != 0)
		usleep(100);

	nvmf_cancel_controller_data(nc);
	assert(x.x_calls == 1 && x.x_error == ECANCELED && x.x_xfered == 4096);
	free(dst);
	nvmf_cancel_controller_data(nc);
	assert(x.x_calls == 1);

	rsp = nvmf_allocate_response(c->c_qp, &cqe, KM_SLEEP);
	assert(tcp_ops.transmit_capsule(rsp) == 0);
	nvmf_free_capsule(rsp);
	nvmf_free_capsule(nc);
	assert(get(peer, pdu, sizeof (pdu)) == NVMF_TCP_PDU_TYPE_CAPSULE_RESP);

	put_h2c(peer, 9, ttag, 4096, 4096, B_TRUE, 2);
	/* The tag is free again once the rest arrived: a new command works. */
	put_command(peer, 10, 100, 100);
	nc = next_capsule(c);
	assert(tcp_ops.validate_command_capsule(nc) == 0);
	nvmf_free_capsule(nc);
	assert(x.x_calls == 1 && c->c_errors == 0);

	detach(c);
	assert(get(peer, pdu, sizeof (pdu)) == -1);
	(void) close(peer);
}

/*
 * With one transfer tag, a second receive of the same command waits its turn.
 * Cancel takes both; freeing the qpair with them cancelled calls nothing.
 */
static void
cancel_queued_transfer(void)
{
	uint8_t pdu[256], *a = malloc(4096), *b = malloc(4096);
	nvmf_memdesc_t amd = vaddr(a, 4096), bmd = vaddr(b, 4096);
	struct nvlist one = plain;
	struct nvmf_capsule *nc;
	consumer_t *c;
	xfer_t xa, xb;
	int peer;

	one.qsize = 2;
	c = raw(&peer, B_TRUE, &one);
	xfer_init(&xa);
	xfer_init(&xb);
	put_command(peer, 3, 8192, 0);
	nc = next_capsule(c);
	assert(nvmf_receive_controller_data(nc, 0, &amd, 4096, xfer_done,
	    &xa) == 0);
	assert(nvmf_receive_controller_data(nc, 4096, &bmd, 4096, xfer_done,
	    &xb) == 0);
	assert(get(peer, pdu, sizeof (pdu)) == NVMF_TCP_PDU_TYPE_R2T);
	nvmf_cancel_controller_data(nc);
	assert(xa.x_calls == 1 && xa.x_error == ECANCELED);
	assert(xb.x_calls == 1 && xb.x_error == ECANCELED);
	free(a);
	free(b);

	/* The capsule outlives the qpair; cancel is still valid on it. */
	detach(c);
	nvmf_cancel_controller_data(nc);
	assert(nvmf_transport_unregister(&tcp_ops) == EBUSY);
	nvmf_free_capsule(nc);
	assert(xa.x_calls == 1 && xb.x_calls == 1);
	assert(nvmf_transport_unregister(&tcp_ops) == 0);
	(void) close(peer);
}

/* Freeing the qpair completes a transfer that nobody cancelled. */
static void
free_completes_transfer(void)
{
	uint8_t pdu[256], *dst = malloc(8192);
	nvmf_memdesc_t dmd = vaddr(dst, 8192);
	struct nvmf_capsule *nc;
	consumer_t *c;
	xfer_t x;
	int peer;

	c = raw(&peer, B_TRUE, &plain);
	xfer_init(&x);
	put_command(peer, 4, 8192, 0);
	nc = next_capsule(c);
	assert(nvmf_receive_controller_data(nc, 0, &dmd, 8192, xfer_done,
	    &x) == 0);
	assert(get(peer, pdu, sizeof (pdu)) == NVMF_TCP_PDU_TYPE_R2T);
	detach(c);
	assert(x.x_calls == 1 && x.x_error == ECONNABORTED);
	/* A receive on a dead qpair fails through its callback. */
	assert(nvmf_receive_controller_data(nc, 0, &dmd, 8192, xfer_done,
	    &x) == 0);
	assert(x.x_calls == 2 && x.x_error == ECONNABORTED);
	nvmf_free_capsule(nc);
	free(dst);
	(void) close(peer);
}

struct racer {
	int		r_peer;
	uint16_t	r_ttag;
};

static void *
race_sender(void *arg)
{
	struct racer *r = arg;

	put_h2c(r->r_peer, 1, r->r_ttag, 0, 4096, B_FALSE, 1);
	put_h2c(r->r_peer, 1, r->r_ttag, 4096, 4096, B_TRUE, 2);
	return (NULL);
}

/*
 * Cancel while the data is arriving.  Whichever wins, the callback runs once
 * and the buffer is not written after cancel returns: it is freed here, so a
 * late copy is a use after free.
 */
static void
cancel_race(void)
{
	uint8_t pdu[256];
	nvme_cqe_t cqe = { .cqe_cid = 1 };
	int round, peer, cancelled = 0;
	consumer_t *c;

	c = raw(&peer, B_TRUE, &plain);
	for (round = 0; round < 300; round++) {
		uint8_t *dst = malloc(8192);
		nvmf_memdesc_t dmd = vaddr(dst, 8192);
		nvmf_tcp_r2t_hdr_t *r2t = (void *)pdu;
		struct nvmf_capsule *nc, *rsp;
		struct racer r = { .r_peer = peer };
		pthread_t t;
		xfer_t x;

		xfer_init(&x);
		put_command(peer, 1, 8192, 0);
		nc = next_capsule(c);
		assert(nvmf_receive_controller_data(nc, 0, &dmd, 8192,
		    xfer_done, &x) == 0);
		assert(get(peer, pdu, sizeof (pdu)) == NVMF_TCP_PDU_TYPE_R2T);
		r.r_ttag = r2t->ntr2t_ttag;
		assert(pthread_create(&t, NULL, race_sender, &r) == 0);
		if (round % 3 != 0)
			usleep(round % 50);
		nvmf_cancel_controller_data(nc);
		assert(x.x_calls == 1);
		cancelled += (x.x_error == ECANCELED);
		assert(x.x_error == ECANCELED || x.x_xfered == 8192);
		free(dst);
		assert(pthread_join(t, NULL) == 0);

		rsp = nvmf_allocate_response(c->c_qp, &cqe, KM_SLEEP);
		assert(tcp_ops.transmit_capsule(rsp) == 0);
		nvmf_free_capsule(rsp);
		nvmf_free_capsule(nc);
		assert(get(peer, pdu, sizeof (pdu)) ==
		    NVMF_TCP_PDU_TYPE_CAPSULE_RESP);
		assert(x.x_calls == 1);
	}
	assert(c->c_errors == 0);
	detach(c);
	(void) close(peer);
	printf("cancel race: cancel won %d of 300\n", cancelled);
}

/*
 * A target answers an 8K write with an R2T for the first half and then one
 * whose offset plus length wraps 32 bits.  The host must refuse it and send
 * nothing from beyond the buffer.
 */
static void
r2t_wrap(void)
{
	uint8_t pdu[16384], *src = malloc(8192);
	nvmf_memdesc_t smd = vaddr(src, 8192);
	nvme_sqe_t sqe = { .sqe_opc = 0x01, .sqe_cid = 7 };
	nvmf_tcp_r2t_hdr_t r2t = { 0 };
	struct nvmf_capsule *cmd;
	consumer_t *h;
	xfer_t x;
	int peer;

	h = raw(&peer, B_FALSE, &plain);
	xfer_init(&x);
	cmd = nvmf_allocate_command(h->c_qp, &sqe, KM_SLEEP);
	assert(nvmf_capsule_append_data(cmd, &smd, 8192, B_TRUE, xfer_done,
	    &x) == 0);
	assert(tcp_ops.transmit_capsule(cmd) == 0);
	assert(get(peer, pdu, sizeof (pdu)) == NVMF_TCP_PDU_TYPE_CAPSULE_CMD);

	r2t.ntr2t_common.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_R2T;
	r2t.ntr2t_common.ntcph_hlen = sizeof (r2t);
	r2t.ntr2t_common.ntcph_plen = sizeof (r2t);
	r2t.ntr2t_cccid = 7;
	r2t.ntr2t_r2to = 0;
	r2t.ntr2t_r2tl = 4096;
	put(peer, &r2t, sizeof (r2t));
	assert(get(peer, pdu, sizeof (pdu)) == NVMF_TCP_PDU_TYPE_H2C_DATA);

	r2t.ntr2t_r2to = 4096;
	r2t.ntr2t_r2tl = 0xfffff001;
	put(peer, &r2t, sizeof (r2t));
	expect_term(peer, NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE);
	(void) close(peer);
	assert(wait_error(h) == EBADMSG);
	detach(h);
	assert(x.x_calls == 1 && x.x_error == ECONNABORTED);
	nvmf_free_capsule(cmd);
	free(src);
}

/* Limits on what a peer may make the transport buffer. */
static void
receive_limits(void)
{
	nvmf_tcp_common_pdu_hdr_t ch = { 0 };
	struct nvmf_capsule *nc;
	hrtime_t start;
	consumer_t *c;
	int peer;

	/* In-capsule data up to the limit is taken; one byte more is not. */
	c = raw(&peer, B_TRUE, &plain);
	put_command(peer, 1, nvmf_tcp_max_icd, nvmf_tcp_max_icd);
	nc = next_capsule(c);
	assert(tcp_ops.validate_command_capsule(nc) == 0);
	nvmf_free_capsule(nc);
	put_command(peer, 2, nvmf_tcp_max_icd + 4, nvmf_tcp_max_icd + 4);
	expect_term(peer, NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED);
	(void) close(peer);
	assert(wait_error(c) == EBADMSG);
	detach(c);

	/* A length that no PDU may have is refused before it is read. */
	c = raw(&peer, B_TRUE, &plain);
	ch.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_H2C_DATA;
	ch.ntcph_hlen = sizeof (nvmf_tcp_h2c_data_hdr_t);
	ch.ntcph_pdo = sizeof (nvmf_tcp_h2c_data_hdr_t);
	ch.ntcph_plen = 0x7fffffff;
	put(peer, &ch, sizeof (ch));
	expect_term(peer, NVMF_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED);
	(void) close(peer);
	assert(wait_error(c) == EBADMSG);
	detach(c);

	/* A header shorter than itself. */
	c = raw(&peer, B_TRUE, &plain);
	ch.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_CMD;
	ch.ntcph_hlen = sizeof (nvmf_tcp_cmd_t);
	ch.ntcph_pdo = 0;
	ch.ntcph_plen = 4;
	put(peer, &ch, sizeof (ch));
	expect_term(peer, NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD);
	(void) close(peer);
	assert(wait_error(c) == EBADMSG);
	detach(c);

	/*
	 * After a termination request the error waits for the peer to close,
	 * for a limited time, and what the peer still sends is discarded.
	 */
	nvmf_tcp_term_wait_secs = 1;
	c = raw(&peer, B_TRUE, &plain);
	ch.ntcph_plen = 4;
	put(peer, &ch, sizeof (ch));
	expect_term(peer, NVMF_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD);
	put_command(peer, 1, 0, 0);
	start = gethrtime();
	assert(wait_error(c) == EBADMSG && c->c_nrx == 0);
	assert(gethrtime() - start > NANOSEC / 2);
	detach(c);
	(void) close(peer);
	nvmf_tcp_term_wait_secs = 30;

	/* The end of the stream inside a PDU is a reset, not a close. */
	ch.ntcph_pdu_type = NVMF_TCP_PDU_TYPE_CAPSULE_CMD;
	ch.ntcph_hlen = sizeof (nvmf_tcp_cmd_t);
	ch.ntcph_pdo = 0;
	c = raw(&peer, B_TRUE, &plain);
	ch.ntcph_plen = sizeof (nvmf_tcp_cmd_t);
	put(peer, &ch, sizeof (ch));
	(void) close(peer);
	assert(wait_error(c) == ECONNRESET);
	detach(c);
}

/* A received command keeps the PSDT of the wire, so a wrong one is seen. */
static void
received_psdt(void)
{
	struct nvmf_capsule *nc;
	consumer_t *c;
	uint8_t psdt;
	int peer;

	c = raw(&peer, B_TRUE, &plain);
	for (psdt = 0; psdt < 4; psdt++) {
		put_psdt = psdt;
		put_command(peer, psdt, 512, 512);
		nc = next_capsule(c);
		assert(NVMF_SQE_PSDT(&nc->nc_sqe) == psdt);
		assert(nvmf_validate_command_capsule(nc) ==
		    (psdt == NVME_PSDT_SGL ? NVME_CQE_SC_GEN_SUCCESS :
		    NVME_CQE_SC_GEN_INV_FLD));
		nvmf_free_capsule(nc);
	}
	put_psdt = NVME_PSDT_SGL;
	(void) close(peer);
	(void) wait_error(c);
	detach(c);
}

/* Nonsense in the handoff is refused and leaves the descriptor alone. */
static void
handoff_checks(void)
{
	static const char *keys[] = { "fd", "rxpda", "txpda", "maxr2t",
	    "maxh2cdata", "qsize", "header_digests", "data_digests",
	    "sq_flow_control" };
	struct nvlist nvl;
	unsigned i, before = closes;
	consumer_t *c;
	int sv[2];

	pair(sv);
	(void) fd_install(sv[0]);
#define	REFUSE(field, value)	\
	nvl = plain; nvl.field = (value); \
	assert(attach(sv[0], B_TRUE, &nvl) == NULL); \
	assert(attach(sv[0], B_FALSE, &nvl) == NULL)
	REFUSE(rxpda, 0);
	REFUSE(rxpda, 256);
	REFUSE(rxpda, 6);
	REFUSE(txpda, 0);
	REFUSE(txpda, 132);
	REFUSE(maxh2cdata, 0);
	REFUSE(maxh2cdata, 4095);
	REFUSE(maxh2cdata, 0x100000000ULL);
	REFUSE(maxr2t, 0xffffffffULL);
	REFUSE(qsize, 0);
	REFUSE(qsize, 65537);
#undef REFUSE
	for (i = 0; i < ARRAY_SIZE(keys); i++) {
		nvl = plain;
		nvl.missing = keys[i];
		assert(attach(sv[0], B_TRUE, &nvl) == NULL);
		assert(attach(sv[0], B_FALSE, &nvl) == NULL);
	}

	/* max_icd is the controller's limit, which only a host is told. */
	nvl = plain;
	nvl.max_icd = 0x100000000ULL;
	assert(attach(sv[0], B_FALSE, &nvl) == NULL);
	nvl.missing = "max_icd";
	assert(attach(sv[0], B_FALSE, &nvl) == NULL);
	c = attach(sv[0], B_TRUE, &nvl);
	assert(c != NULL);
	detach(c);
	assert(attach(NFILES - 1, B_TRUE, &plain) == NULL);

	/* Not TCP, not connected, a STREAMS socket, or taken already. */
	files[sv[0]]->f_so.so_protocol = 132;
	assert(attach(sv[0], B_TRUE, &plain) == NULL);
	files[sv[0]]->f_so.so_protocol = 0;
	files[sv[0]]->f_so.so_type = SOCK_DGRAM;
	assert(attach(sv[0], B_TRUE, &plain) == NULL);
	files[sv[0]]->f_so.so_type = SOCK_STREAM;
	files[sv[0]]->f_so.so_family = AF_UNIX;
	assert(attach(sv[0], B_TRUE, &plain) == NULL);
	files[sv[0]]->f_so.so_family = AF_INET6;
	files[sv[0]]->f_so.so_state = 0;
	assert(attach(sv[0], B_TRUE, &plain) == NULL);
	files[sv[0]]->f_so.so_state = SS_ISCONNECTED | SS_NONBLOCK;
	assert(attach(sv[0], B_TRUE, &plain) == NULL);
	files[sv[0]]->f_so.so_state = SS_ISCONNECTED;
	files[sv[0]]->f_so.so_vnode.v_stream = &nvl;
	assert(attach(sv[0], B_TRUE, &plain) == NULL);
	files[sv[0]]->f_so.so_vnode.v_stream = NULL;
	assert(files[sv[0]]->f_count == 1 && files[sv[0]]->f_so.so_mode == 0);

	timeouts_cleared = 0;
	c = attach(sv[0], B_TRUE, &plain);
	assert(c != NULL && files[sv[0]]->f_count == 2);
	assert(timeouts_cleared == 2);
	assert(attach(sv[0], B_TRUE, &plain) == NULL);

	/*
	 * The qpair goes away before the daemon closes: nothing waits, the
	 * peer sees the end of the stream, and the daemon's close is the last.
	 */
	detach(c);
	assert(closes == before && files[sv[0]]->f_count == 1);
	assert(files[sv[0]]->f_so.so_mode == 0);
	assert(recv(sv[1], &nvl, 1, 0) == 0);
	daemon_close(sv[0]);
	assert(closes == before + 1);
	(void) close(sv[1]);
}

/*
 * A response sent just before the qpair is freed still reaches the peer.  A
 * peer that does not read holds the free for the drain time and no longer.
 */
static void
drain_at_free(void)
{
	uint8_t pdu[256];
	nvme_cqe_t cqe = { .cqe_cid = 5 };
	struct nvmf_capsule *rsp;
	hrtime_t start;
	consumer_t *c;
	int peer, round, i;

	for (round = 0; round < 50; round++) {
		c = raw(&peer, B_TRUE, &plain);
		rsp = nvmf_allocate_response(c->c_qp, &cqe, KM_SLEEP);
		assert(tcp_ops.transmit_capsule(rsp) == 0);
		nvmf_free_capsule(rsp);
		detach(c);
		assert(get(peer, pdu, sizeof (pdu)) ==
		    NVMF_TCP_PDU_TYPE_CAPSULE_RESP);
		assert(get(peer, pdu, sizeof (pdu)) == -1);
		(void) close(peer);
	}

	nvmf_tcp_drain_secs = 1;
	c = raw(&peer, B_TRUE, &plain);
	for (i = 0; i < 100000; i++) {
		rsp = nvmf_allocate_response(c->c_qp, &cqe, KM_SLEEP);
		assert(tcp_ops.transmit_capsule(rsp) == 0);
		nvmf_free_capsule(rsp);
	}
	start = gethrtime();
	detach(c);
	assert(gethrtime() - start < 3 * NANOSEC);
	(void) close(peer);
	assert(transport.nt_active_qpairs == 0);
}

int
main(void)
{
	(void) signal(SIGPIPE, SIG_IGN);
	assert(_init() == 0);
	host_and_controller();
	handoff_checks();
	cancel_open_transfer();
	cancel_queued_transfer();
	free_completes_transfer();
	cancel_race();
	r2t_wrap();
	receive_limits();
	received_psdt();
	drain_at_free();
	assert(_fini() == 0);
	printf("tcp transfer passed\n");
	return (0);
}

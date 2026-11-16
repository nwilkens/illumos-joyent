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
 * Host stand-ins for what nvmf_tcp.c uses of STREAMS, sockfs, ksocket, taskq
 * and the file table.  A socket is one end of a socketpair.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

/* The kernel's clock_t is signed. */
#define	clock_t	long

#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	MAX(a, b)	((a) > (b) ? (a) : (b))
#define	VERIFY3U(a, op, b)	assert((a) op (b))
#define	MUTEX_HELD(m)	1
#define	panic(...)	abort()
#define	atomic_dec_uint(p)	((void)atomic_dec_uint_nv(p))
typedef long long hrtime_t;
#define	NANOSEC	1000000000LL
#define	MICROSEC	1000000

static inline hrtime_t
gethrtime(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * NANOSEC + ts.tv_nsec);
}

/* One tick is a millisecond. */
#define	hz	1000
#define	SEC_TO_TICK(s)	((clock_t)(s) * hz)
#define	drv_usectohz(us)	((clock_t)((us) / 1000))
#define	TR_CLOCK_TICK	0
#define	ddi_get_lbolt()	((clock_t)(gethrtime() / 1000000))

static inline clock_t
cv_timedwait(kcondvar_t *cv, kmutex_t *mp, clock_t when)
{
	struct timespec ts;
	clock_t now = ddi_get_lbolt(), delta = when > now ? when - now : 0;

	(void) clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += delta / 1000;
	ts.tv_nsec += (delta % 1000) * 1000000L;
	if (ts.tv_nsec >= NANOSEC) {
		ts.tv_sec++;
		ts.tv_nsec -= NANOSEC;
	}
	return (pthread_cond_timedwait(cv, mp, &ts) == 0 ? 1 : -1);
}
#define	cv_reltimedwait(cv, mp, delta, res)	\
	cv_timedwait((cv), (mp), ddi_get_lbolt() + (delta))

/* Messages: a dblk is shared by dupb() and counted. */
typedef struct datab {
	unsigned	db_ref;
	size_t		db_size;
} dblk_t;

typedef struct msgb {
	struct msgb	*b_next;
	struct msgb	*b_cont;
	unsigned char	*b_rptr;
	unsigned char	*b_wptr;
	dblk_t		*b_datap;
} mblk_t;

#define	MBLKL(mp)	((size_t)((mp)->b_wptr - (mp)->b_rptr))
#define	BPRI_MED	0
#define	STR_NOSIG	1

mblk_t	*allocb_wait(size_t, unsigned, unsigned, int *);
mblk_t	*dupb(mblk_t *);
void	freeb(mblk_t *);
void	freemsg(mblk_t *);
size_t	msgdsize(mblk_t *);

/* Each task runs on a thread of its own; a taskq here takes one task. */
typedef struct taskq {
	pthread_t	tq_thread;
	boolean_t	tq_running;
} taskq_t;
#define	minclsyspri	0
#define	TASKQ_PREPOPULATE	0
#define	TQ_SLEEP	0
taskq_t	*taskq_create(const char *, int, int, int, int, unsigned);
int	taskq_dispatch(taskq_t *, void (*)(void *), void *, unsigned);
void	taskq_wait(taskq_t *);
void	taskq_destroy(taskq_t *);

typedef struct vnode {
	void	*v_stream;
} vnode_t;

struct sonode {
	kmutex_t	so_lock;
	short		so_family;
	short		so_type;
	short		so_protocol;
	unsigned	so_state;
	unsigned	so_mode;
	vnode_t		so_vnode;
	int		so_fd;
};
#define	SOTOV(so)	(&(so)->so_vnode)
#define	SS_ISCONNECTED	0x2
#define	SS_NDELAY	0x40
#define	SS_NONBLOCK	0x80
#define	SM_KERNEL	0x200

typedef struct file {
	kmutex_t	f_tlock;
	unsigned	f_count;
	vnode_t		*f_vnode;
	struct sonode	f_so;
} file_t;

struct sonode *getsonode(int, int *, file_t **);
void	releasef(int);
int	closef(file_t *);

typedef struct sonode *ksocket_t;
typedef struct cred cred_t;
#define	CRED()	((cred_t *)NULL)
int	ksocket_recv(ksocket_t, void *, size_t, int, size_t *, cred_t *);
int	ksocket_sendmblk(ksocket_t, struct msghdr *, int, mblk_t **, cred_t *);
int	ksocket_shutdown(ksocket_t, int, cred_t *);
int	ksocket_setsockopt(ksocket_t, int, int, const void *, int, cred_t *);

typedef struct nvlist nvlist_t;
int	nvlist_lookup_uint64(nvlist_t *, const char *, uint64_t *);
int	nvlist_lookup_boolean_value(nvlist_t *, const char *, boolean_t *);

typedef struct { uint64_t dmac_laddress; size_t dmac_size; } ddi_dma_cookie_t;

struct modlmisc {
	void		*misc_modops;
	const char	*misc_linkinfo;
};
struct modlinkage {
	int	ml_rev;
	void	*ml_linkage[2];
};
struct modinfo;
#define	MODREV_1	1
extern int mod_miscops;
int	mod_install(struct modlinkage *);
int	mod_remove(struct modlinkage *);
int	mod_info(struct modlinkage *, struct modinfo *);

struct nvmf_transport_ops;

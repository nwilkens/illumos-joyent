"""Host stand-ins for the illumos kernel types the extracted code uses."""

BASE = """#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef int boolean_t;
#define	B_FALSE	0
#define	B_TRUE	1
typedef unsigned int uint_t;
typedef unsigned long ulong_t;
typedef unsigned char uchar_t;
#define	LE_16(x)	((uint16_t)(x))
#define	LE_32(x)	((uint32_t)(x))
#define	LE_64(x)	((uint64_t)(x))
#define	_NOTE(x)
#define	CTASSERT(x)	_Static_assert(x, #x)
"""

# Locks, lists, atomics and kmem on top of pthreads and libc.
KSHIM = BASE + """#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))
#define	ASSERT(x)	assert(x)
#define	ASSERT3U(a, op, b)	assert((a) op (b))
#define	ASSERT3P(a, op, b)	assert((a) op (b))
#define	VERIFY(x)	assert(x)
#define	VERIFY0(x)	assert((x) == 0)
#define	KM_SLEEP	0
#define	KM_NOSLEEP	1
#define	CE_WARN	1
#define	CE_NOTE	2
#define	cmn_err(l, ...)	((void)(l), fprintf(stderr, __VA_ARGS__))
static inline void *kmem_zalloc(size_t n, int f) { (void)f;
    return (calloc(1, n)); }
static inline void *kmem_alloc(size_t n, int f) { (void)f;
    return (malloc(n)); }
static inline void kmem_free(void *p, size_t n) { (void)n; free(p); }
#define	bcopy(s, d, n)	memcpy((d), (s), (n))
#define	bzero(p, n)	memset((p), 0, (n))

typedef pthread_rwlock_t krwlock_t;
typedef enum { RW_READER, RW_WRITER } krw_t;
#define	RW_DRIVER	0
#define	rw_init(l, n, t, a)	pthread_rwlock_init((l), NULL)
#define	rw_destroy(l)	pthread_rwlock_destroy(l)
#define	rw_enter(l, t)	((t) == RW_WRITER ? pthread_rwlock_wrlock(l) : \\
	pthread_rwlock_rdlock(l))
#define	rw_exit(l)	pthread_rwlock_unlock(l)
typedef pthread_mutex_t kmutex_t;
#define	MUTEX_DRIVER	0
#define	mutex_init(m, n, t, a)	pthread_mutex_init((m), NULL)
#define	mutex_destroy(m)	pthread_mutex_destroy(m)
#define	mutex_enter(m)	pthread_mutex_lock(m)
#define	mutex_exit(m)	pthread_mutex_unlock(m)
typedef pthread_cond_t kcondvar_t;
#define	CV_DRIVER	0
#define	cv_init(c, n, t, a)	pthread_cond_init((c), NULL)
#define	cv_destroy(c)	pthread_cond_destroy(c)
#define	cv_wait(c, m)	pthread_cond_wait((c), (m))
#define	cv_broadcast(c)	pthread_cond_broadcast(c)
#define	cv_signal(c)	pthread_cond_signal(c)
#define	atomic_inc_uint(p)	((void)__atomic_add_fetch((p), 1, \\
	__ATOMIC_SEQ_CST))
#define	atomic_dec_uint_nv(p)	__atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define	membar_producer()	__atomic_thread_fence(__ATOMIC_RELEASE)
#define	membar_consumer()	__atomic_thread_fence(__ATOMIC_ACQUIRE)

typedef struct list_node { struct list_node *next, *prev; } list_node_t;
typedef struct list {
	size_t off;
	list_node_t head;
} list_t;
#define	LNODE(l, o)	((list_node_t *)((char *)(o) + (l)->off))
#define	LOBJ(l, n)	((n) == &(l)->head ? NULL : (void *)((char *)(n) - \\
	(l)->off))
static inline void list_create(list_t *l, size_t sz, size_t off) {
	(void)sz; l->off = off; l->head.next = l->head.prev = &l->head; }
static inline void list_destroy(list_t *l) { assert(l->head.next ==
	&l->head); }
static inline int list_is_empty(list_t *l) { return (l->head.next ==
	&l->head); }
static inline void *list_head(list_t *l) { return (LOBJ(l, l->head.next)); }
static inline void *list_next(list_t *l, void *o) {
	return (LOBJ(l, LNODE(l, o)->next)); }
static inline void list_insert_after(list_t *l, void *o, void *n) {
	list_node_t *a = o == NULL ? &l->head : LNODE(l, o), *b = LNODE(l, n);
	b->next = a->next; b->prev = a; a->next->prev = b; a->next = b; }
static inline void list_insert_head(list_t *l, void *n) {
	list_insert_after(l, NULL, n); }
static inline void list_insert_tail(list_t *l, void *n) {
	list_node_t *b = LNODE(l, n); b->prev = l->head.prev;
	b->next = &l->head; l->head.prev->next = b; l->head.prev = b; }
static inline void list_remove(list_t *l, void *o) {
	list_node_t *a = LNODE(l, o); a->prev->next = a->next;
	a->next->prev = a->prev; a->next = a->prev = NULL; }
static inline void *list_remove_head(list_t *l) { void *o = list_head(l);
	if (o != NULL) list_remove(l, o); return (o); }
static inline void list_move_tail(list_t *dst, list_t *src) {
	void *o; while ((o = list_remove_head(src)) != NULL)
	list_insert_tail(dst, o); }
"""

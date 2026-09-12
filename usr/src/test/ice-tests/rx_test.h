/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/* Controlled allocation, STREAMS, and DDI boundary for production RX bodies. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef int boolean_t;
typedef unsigned int uint_t;
typedef char *caddr_t;

#define	B_TRUE	1
#define	B_FALSE	0
#define	DDI_SUCCESS	0
#define	DDI_FM_OK	0
#define	DDI_SERVICE_DEGRADED	1
#define	DDI_DMA_SYNC_FORKERNEL	1
#define	DDI_DMA_SYNC_FORDEV	2
#define	KM_SLEEP	0
#define	ETHERADDRL	6
#define	ETHERTYPE_VLAN	0x8100
#define	VLAN_TAGSZ	4
#define	BIT(n)	(1U << (n))
#define	ICE_RX_FLEX_DESC_STATUS0_DD_S	0
#define	ICE_RX_FLEX_DESC_STATUS0_EOF_S	1
#define	ICE_RX_FLEX_DESC_STATUS0_L2TAG1P_S	5
#define	ICE_RX_FLEX_DESC_STATUS0_RXE_S	10
#define	ICE_RX_FLX_DESC_PKT_LEN_M	0x3fff
#define	ICE_RX_FLEX_DESC_PTYPE_M	0x3ff
#define	CPU_TO_LE64(n)	(n)
#define	LE16_TO_CPU(n)	checked_le16(&(n))
#define	ASSERT(n)	assert(n)
#define	ASSERT0(n)	assert((n) == 0)
/* The op parameter is a comparison operator, not a function name. */
/* CSTYLED */
#define	ASSERT3U(a, op, b)	assert((a) op (b))
/* CSTYLED */
#define	ASSERT3S(a, op, b)	assert((a) op (b))
/* CSTYLED */
#define	ASSERT3P(a, op, b)	assert((a) op (b))
#define	MUTEX_HELD(m)	(*(m) != 0)
#undef bcopy
#define	bcopy(s, d, n)	((void) memmove((d), (s), (n)))
#define	ovbcopy(s, d, n)	((void) memmove((d), (s), (n)))
#define	ICE_DMA_PA(d)	((d)->idb_cookie.dmac_laddress)
#define	QRX_TAIL(n)	(n)

struct ether_header {
	uint8_t bytes[14];
};
struct ether_vlan_header {
	uint8_t bytes[18];
};
typedef struct {
	void (*free_func)(caddr_t);
	caddr_t free_arg;
} frtn_t;
typedef struct {
	unsigned char *db_base;
	frtn_t *frtn;
} dblk_t;
typedef struct mblk {
	unsigned char *b_rptr, *b_wptr;
	struct mblk *b_cont, *b_next;
	dblk_t *b_datap;
} mblk_t;
#define	MBLKL(m)	((size_t)((m)->b_wptr - (m)->b_rptr))

typedef struct dma_handle {
	unsigned char *base;
	size_t length;
	unsigned syncs, checks;
	size_t last_offset, last_length;
	int fail_sync, fail_check;
	unsigned fail_sync_after, fail_check_after;
	int fail_direction;
} *ddi_dma_handle_t;
typedef int ddi_acc_handle_t;
typedef int ddi_dma_attr_t;
typedef int ddi_device_acc_attr_t;
typedef struct {
	caddr_t idb_va;
	size_t idb_len;
	ddi_dma_handle_t idb_dma_handle;
	unsigned idb_ncookies;
	struct {
		uint64_t dmac_laddress;
	} idb_cookie;
} ice_dma_buffer_t;
typedef struct {
	union {
		uint64_t ui64;
	} value;
} kstat_named_t;
typedef struct {
	kstat_named_t icrxs_copy_nomem, icrxs_copy_bytes, icrxs_copy_segs;
	kstat_named_t icrxs_no_rcb, icrxs_bind_bytes, icrxs_bind_segs;
	kstat_named_t icrxs_desc_error, icrxs_bytes, icrxs_packets;
	kstat_named_t icrxs_intr_limit;
} ice_rxq_stat_t;
struct ice_hw {
	int unused;
};
typedef struct {
	struct ice_hw ice_hw;
	struct {
		uint16_t vi_max_frame;
	} ice_pf_vsi;
	struct {
		int ios_reg_handle;
	} ice_osdep;
	uint32_t ice_state, ice_rx_limit_per_intr;
	int ice_dip, ice_mac_hdl;
} ice_t;
typedef enum {
	IRXB_FREE,
	IRXB_ONRING,
	IRXB_ONLOAN
} ice_rcb_state_t;
struct ice_rx_ring;
typedef struct {
	mblk_t *ircb_mp;
	struct ice_rx_ring *ircb_ring;
	ice_dma_buffer_t ircb_dma;
	frtn_t ircb_free_rtn;
	ice_rcb_state_t ircb_state;
} ice_rx_ctrl_block_t;
union ice_32b_rx_flex_desc {
	struct {
		uint64_t pkt_addr, hdr_addr;
	} read;
	struct {
		uint16_t ptype_flex_flags0, pkt_len, l2tag1, pad;
		uint16_t status_error0;
	} wb;
	uint8_t bytes[32];
};
typedef struct ice_rx_ring {
	ice_t *irxr_ice;
	uint32_t irxr_index, irxr_dbuf;
	int irxr_lock, irxr_cv, irxr_intr_cv, irxr_macrxring;
	boolean_t irxr_shutdown, irxr_started, irxr_intr_poll, irxr_intr_busy;
	uint64_t irxr_rxgen;
	ice_dma_buffer_t irxr_desc_dma;
	union ice_32b_rx_flex_desc *irxr_descs;
	ice_rx_ctrl_block_t **irxr_rcbs, *irxr_rcb_area, **irxr_free_rcbs;
	uint16_t irxr_size, irxr_head, irxr_tail;
	uint_t irxr_nrcb, irxr_nfree, irxr_nreserve, irxr_nloaned;
	ice_rxq_stat_t irxr_stats;
} ice_rx_ring_t;

static unsigned live_mblks, live_dma, impacts, barriers, doorbells, delivered;
static int alloc_fail, desballoc_fail, acc_fail;
static unsigned wb_reads, bad_wb_reads;
static ice_rx_ring_t *active_ring;
static mblk_t *checksum_head;
static void ice_rx_recycle(caddr_t);
static void ice_rx_free_rcbs(ice_rx_ring_t *);

static uint16_t
checked_le16(const uint16_t *p)
{
	ddi_dma_handle_t d = active_ring->irxr_desc_dma.idb_dma_handle;

	wb_reads++;
	if ((d->fail_sync && d->syncs >= d->fail_sync_after &&
	    (!d->fail_direction ||
	    d->fail_direction == DDI_DMA_SYNC_FORKERNEL)) ||
	    (d->fail_check && d->checks >= d->fail_check_after))
		bad_wb_reads++;
	return (*p);
}

static void
mutex_enter(int *lock)
{
	assert(!*lock);
	*lock = 1;
}

static void
mutex_exit(int *lock)
{
	assert(*lock);
	*lock = 0;
}

static void
cv_signal(int *cv)
{
	(void) cv;
}

static void
cv_broadcast(int *cv)
{
	(void) cv;
}

static void
membar_consumer(void)
{
	barriers++;
}

static void
atomic_or_32(uint32_t *p, uint32_t bits)
{
	*p |= bits;
}

static void
ddi_fm_service_impact(int dip, int impact)
{
	(void) dip;
	assert(impact == DDI_SERVICE_DEGRADED);
	impacts++;
}

static void
ice_error(ice_t *ice, const char *fmt, unsigned index)
{
	(void) ice;
	(void) fmt;
	(void) index;
}

static void *
kmem_zalloc(size_t n, int flag)
{
	void *p = calloc(1, n);

	(void) flag;
	assert(p);
	return (p);
}

static void
kmem_free(void *p, size_t n)
{
	(void) n;
	free(p);
}

static void
ice_pkt_dma_attr(ice_t *ice, ddi_dma_attr_t *attr)
{
	(void) ice;
	*attr = 0;
}

static void
ice_dma_acc_attr(ice_t *ice, ddi_device_acc_attr_t *attr)
{
	(void) ice;
	*attr = 0;
}

static boolean_t
ice_dma_alloc(ice_t *ice, ice_dma_buffer_t *d, ddi_dma_attr_t *attr,
    ddi_device_acc_attr_t *acc, boolean_t zero, size_t length, boolean_t sleep)
{
	(void) ice;
	(void) attr;
	(void) acc;
	(void) zero;
	(void) sleep;
	d->idb_va = calloc(1, length);
	d->idb_len = length;
	d->idb_dma_handle = calloc(1, sizeof (*d->idb_dma_handle));
	assert(d->idb_va && d->idb_dma_handle);
	d->idb_dma_handle->base = (unsigned char *)d->idb_va;
	d->idb_dma_handle->length = length;
	d->idb_cookie.dmac_laddress = (uintptr_t)d->idb_va;
	d->idb_ncookies = 1;
	live_dma++;
	return (B_TRUE);
}

static void
ice_dma_free(ice_dma_buffer_t *d)
{
	if (d->idb_va == NULL)
		return;
	free(d->idb_va);
	free(d->idb_dma_handle);
	memset(d, 0, sizeof (*d));
	live_dma--;
}

static mblk_t *
desballoc(unsigned char *base, size_t length, int pri, frtn_t *frtn)
{
	mblk_t *m;

	(void) length;
	(void) pri;
	if (frtn != NULL && desballoc_fail)
		return (NULL);
	m = calloc(1, sizeof (*m));
	assert(m);
	m->b_datap = calloc(1, sizeof (*m->b_datap));
	assert(m->b_datap);
	m->b_datap->db_base = base;
	m->b_datap->frtn = frtn;
	m->b_rptr = m->b_wptr = base;
	live_mblks++;
	return (m);
}

static mblk_t *
allocb(size_t n, int pri)
{
	unsigned char *p;

	if (alloc_fail)
		return (NULL);
	p = calloc(1, n);
	assert(p);
	return (desballoc(p, n, pri, NULL));
}

static void
freeb(mblk_t *m)
{
	assert(m->b_cont == NULL && m->b_next == NULL);
	if (m->b_datap->frtn != NULL) {
		frtn_t *f = m->b_datap->frtn;

		f->free_func(f->free_arg);
	} else {
		free(m->b_datap->db_base);
	}
	free(m->b_datap);
	free(m);
	live_mblks--;
}

static void
freemsg(mblk_t *m)
{
	while (m != NULL) {
		mblk_t *next = m->b_cont;

		m->b_cont = NULL;
		freeb(m);
		m = next;
	}
}

static void
freemsgchain(mblk_t *m)
{
	while (m != NULL) {
		mblk_t *next = m->b_next;

		m->b_next = NULL;
		freemsg(m);
		m = next;
	}
}

static int
ddi_dma_sync(ddi_dma_handle_t d, off_t offset, size_t len, int dir)
{
	assert(offset >= 0 && (size_t)offset <= d->length);
	assert(len <= d->length - (size_t)offset);
	d->syncs++;
	d->last_offset = (size_t)offset;
	d->last_length = len;
	return (d->fail_sync && d->syncs >= d->fail_sync_after &&
	    (!d->fail_direction || d->fail_direction == dir) ?
	    -1 : DDI_SUCCESS);
}

static int
ice_check_dma_handle(ddi_dma_handle_t d)
{
	d->checks++;
	return (d->fail_check && d->checks >= d->fail_check_after ?
	    -1 : DDI_FM_OK);
}

static int
ice_check_acc_handle(ice_t *ice, ddi_acc_handle_t handle)
{
	(void) ice;
	(void) handle;
	return (acc_fail);
}

static void
wr32(struct ice_hw *hw, unsigned reg, unsigned value)
{
	(void) hw;
	(void) reg;
	(void) value;
	doorbells++;
}

static void
ice_rx_hcksum(ice_rx_ring_t *irr, mblk_t *m, uint16_t s, uint16_t p)
{
	(void) irr;
	(void) s;
	(void) p;
	checksum_head = m;
}

static void
mac_rx_ring(int handle, int ring, mblk_t *m, uint64_t generation)
{
	(void) handle;
	(void) ring;
	(void) generation;
	assert(!active_ring->irxr_lock);
	delivered++;
	freemsgchain(m);
}

#include "rx_functions.h"

static void
setup(ice_rx_ring_t *r, ice_t *ice)
{
	ddi_dma_attr_t attr = 0;
	ddi_device_acc_attr_t acc = 0;
	unsigned i;

	memset(r, 0, sizeof (*r));
	memset(ice, 0, sizeof (*ice));
	impacts = barriers = doorbells = delivered = 0;
	wb_reads = bad_wb_reads = 0;
	alloc_fail = desballoc_fail = acc_fail = 0;
	checksum_head = NULL;
	active_ring = r;
	ice->ice_pf_vsi.vi_max_frame = 9728;
	ice->ice_rx_limit_per_intr = 256;
	r->irxr_ice = ice;
	r->irxr_size = 16;
	r->irxr_dbuf = ICE_RX_BUF_SIZE;
	r->irxr_rcbs = calloc(r->irxr_size, sizeof (*r->irxr_rcbs));
	assert(ice_dma_alloc(ice, &r->irxr_desc_dma, &attr, &acc, B_TRUE,
	    r->irxr_size * sizeof (*r->irxr_descs), B_TRUE));
	r->irxr_descs = (void *)r->irxr_desc_dma.idb_va;
	mutex_enter(&r->irxr_lock);
	assert(ice_rx_alloc_rcbs(r));
	for (i = 0; i < r->irxr_size; i++)
		ice_rx_reset_desc(r, i, ice_rcb_alloc(r, B_FALSE));
	mutex_exit(&r->irxr_lock);
}

static void
teardown(ice_rx_ring_t *r)
{
	assert(r->irxr_nloaned == 0);
	mutex_enter(&r->irxr_lock);
	ice_rx_free_rcbs(r);
	mutex_exit(&r->irxr_lock);
	ice_dma_free(&r->irxr_desc_dma);
	free(r->irxr_rcbs);
	assert(live_mblks == 0 && live_dma == 0);
}

static void
post(ice_rx_ring_t *r, unsigned index, const unsigned char *data,
    size_t length, boolean_t eof, boolean_t vlan)
{
	ice_rx_ctrl_block_t *rcb = r->irxr_rcbs[index];
	union ice_32b_rx_flex_desc *d = &r->irxr_descs[index];
	unsigned char *dest = (void *)(uintptr_t)d->read.pkt_addr;

	assert(dest >= (unsigned char *)rcb->ircb_dma.idb_va);
	assert(dest + length <= (unsigned char *)rcb->ircb_dma.idb_va +
	    rcb->ircb_dma.idb_len);
	memcpy(dest, data, length);
	d->wb.pkt_len = (uint16_t)length;
	d->wb.ptype_flex_flags0 = 1;
	d->wb.l2tag1 = 0xabcd;
	d->wb.status_error0 = BIT(ICE_RX_FLEX_DESC_STATUS0_DD_S) |
	    (eof ? BIT(ICE_RX_FLEX_DESC_STATUS0_EOF_S) : 0) |
	    (vlan ? BIT(ICE_RX_FLEX_DESC_STATUS0_L2TAG1P_S) : 0);
}

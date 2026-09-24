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
 * Run the actual MSI-X sizing against a model of the APIX limit: a device
 * without an interrupt resource management callback gets at most
 * ddi_msix_alloc_limit (8) vectors, one with a callback gets what it asks.
 * Then drive the actual callback through reclaims and offers, and check that
 * every ring's vector is allocated and that the vector's handler services
 * exactly the rings mapped to it.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int uint_t;
typedef int boolean_t;
typedef char *caddr_t;
typedef int kmutex_t;
typedef int kcondvar_t;
typedef struct dev_info dev_info_t;
typedef void *ddi_intr_handle_t;
typedef void *ddi_cb_handle_t;
typedef int ddi_cb_action_t;
typedef int (*ddi_cb_func_t)(dev_info_t *, ddi_cb_action_t, void *, void *,
    void *);

#define	B_TRUE			1
#define	B_FALSE			0
#define	DDI_SUCCESS		0
#define	DDI_FAILURE		-1
#define	DDI_EINVAL		-4
#define	DDI_ENOTSUP		-7
#define	DDI_INTR_CLAIMED	1
#define	DDI_INTR_TYPE_MSIX	4
#define	DDI_INTR_FLAG_BLOCK	0x100
#define	DDI_INTR_ALLOC_NORMAL	0
#define	DDI_CB_FLAG_INTR	1
#define	DDI_CB_INTR_ADD		0
#define	DDI_CB_INTR_REMOVE	1
#define	DDI_CB_OTHER		2
#define	DDI_DEV_T_ANY		0
#define	DDI_SERVICE_LOST	2
#define	MUTEX_DRIVER		0
#define	CV_DRIVER		0
#define	CE_NOTE			1
#define	KM_SLEEP		0
#define	LINK_STATE_DOWN		0
#define	ICE_INTR_MSIX_MIN	2
#define	ICE_DEF_QUEUES		16
#define	ICE_MAX_QUEUES		127
#define	ICE_ITR_IDX_0		0
#define	ICE_GLINT_DYN_CTL_REARM	0x1
#define	GLINT_DYN_CTL_SWINT_TRIG_M	0x4
#define	GLINT_DYN_CTL_SW_ITR_INDX_ENA_M	0x8
#define	GLINT_DYN_CTL_SW_ITR_INDX_S	4
#define	GLINT_DYN_CTL_SW_ITR_INDX_M	0x30
#define	GLINT_DYN_CTL(v)	(v)
#define	APIX_LIMIT		8
#define	NRINGS			16
#define	PRI			6
#define	DDI_INTR_PRI(p)		((void *)(uintptr_t)(p))
#define	MIN(a, b)		((a) < (b) ? (a) : (b))
#define	MAX(a, b)		((a) > (b) ? (a) : (b))
/* The op parameter is a comparison operator, not a function name. */
/* CSTYLED */
#define	ASSERT3U(a, op, b)	assert((a) op (b))
/* CSTYLED */
#define	ASSERT3S(a, op, b)	assert((a) op (b))
#define	ASSERT(x)		assert(x)
#define	MUTEX_HELD(m)		(*(m) != 0)
#define	_NOTE(x)

#include "ice_intr_irm_types.h"

struct dev_info {
	int nintrs;
};

struct ice_hw_common_caps {
	uint32_t num_rxq, num_txq, num_msix_vectors, rss_table_entry_width;
};

struct ice_hw {
	struct {
		struct ice_hw_common_caps common_cap;
	} func_caps;
};

typedef struct {
	uint32_t irxr_vec;
	kmutex_t irxr_lock;
	int irxr_serviced, irxr_limit;
	void *irxr_macrxring;
} ice_rx_ring_t;

typedef struct {
	uint32_t itxr_vec;
	int itxr_serviced;
	void *itxr_mactxring;
} ice_tx_ring_t;

typedef struct ice {
	dev_info_t *ice_dip;
	struct ice_hw ice_hw;
	int ice_intr_type, ice_intr_cap, ice_intr_count;
	uint_t ice_intr_pri;
	size_t ice_intr_size;
	ddi_intr_handle_t *ice_intr_handles;
	ddi_cb_handle_t ice_intr_cb;
	boolean_t ice_irm_busy, ice_attaching, ice_detaching;
	uint16_t ice_nqueues;
	uint32_t ice_state;
	ice_attach_state_t ice_attach_progress;
	uint_t ice_num_rxr, ice_num_txr;
	ice_rx_ring_t *ice_rxr;
	ice_tx_ring_t *ice_txr;
	kmutex_t ice_lock, ice_lse_lock, ice_rebuild_lock;
	kcondvar_t ice_lse_cv;
} ice_t;

static int ncpus = 40, max_ncpus = 40, boot_max_ncpus = -1;

/* IRM and the DDI: which MSI-X entries are allocated, and handler state. */
static struct {
	boolean_t fail_register, fail_alloc, bad_pri;
	int fail_add, fail_remove, fail_disable, fail_free;
	int registered, unregisters, allocated, errors, notes;
	int grant, nreq;
	boolean_t live[64], handler[64], enabled[64];
	ddi_cb_func_t cb;
	void *cb_arg;
} m;

/* The driver lifecycle as the adjustment sees it. */
static struct {
	int pauses, starts, resumes, wakes, maps, reports, impacts, macsets;
	boolean_t pause_fails, start_fails;
	boolean_t mac_cleared;
	void *mac_rx[NRINGS], *mac_tx[NRINGS];
} l;

static uint32_t dyn_ctl[64];
static int tokens[64];

static void
ice_error(ice_t *ice, const char *fmt, ...)
{
	(void) ice;
	(void) fmt;
	m.errors++;
}

static void
dev_err(dev_info_t *dip, int level, const char *fmt, ...)
{
	(void) dip;
	(void) fmt;
	assert(level == CE_NOTE);
	m.notes++;
}

static int
ddi_prop_get_int(int dev, dev_info_t *dip, int flags, const char *name,
    int def)
{
	(void) dev;
	(void) dip;
	(void) flags;
	assert(strcmp(name, "num_queues") == 0);
	return (def);
}

static int
ddi_cb_register(dev_info_t *dip, int flags, ddi_cb_func_t cb, void *arg1,
    void *arg2, ddi_cb_handle_t *hdlp)
{
	(void) dip;
	(void) arg2;
	assert(flags == DDI_CB_FLAG_INTR && !m.registered);
	/* Registration must precede the first vector count. */
	assert(m.allocated == 0);
	if (m.fail_register)
		return (DDI_FAILURE);
	m.registered = 1;
	m.cb = cb;
	m.cb_arg = arg1;
	*hdlp = &m;
	return (DDI_SUCCESS);
}

static int
ddi_cb_unregister(ddi_cb_handle_t hdl)
{
	assert(hdl == &m && m.registered);
	/* IRM requires the vectors to be freed first. */
	assert(m.allocated == 0);
	m.registered = 0;
	m.unregisters++;
	return (DDI_SUCCESS);
}

static int
ddi_intr_get_supported_types(dev_info_t *dip, int *types)
{
	(void) dip;
	*types = DDI_INTR_TYPE_MSIX;
	return (DDI_SUCCESS);
}

static int
ddi_intr_get_nintrs(dev_info_t *dip, int type, int *n)
{
	assert(type == DDI_INTR_TYPE_MSIX);
	*n = dip->nintrs;
	return (DDI_SUCCESS);
}

/* i_ddi_intr_get_limit(): the APIX default binds only drivers without IRM. */
static int
ddi_intr_get_navail(dev_info_t *dip, int type, int *n)
{
	assert(type == DDI_INTR_TYPE_MSIX);
	*n = m.registered ? dip->nintrs : MIN(dip->nintrs, APIX_LIMIT);
	return (DDI_SUCCESS);
}

/* MSI-X entries are handed out contiguously from inum. */
static int
ddi_intr_alloc(dev_info_t *dip, ddi_intr_handle_t *h, int type, int inum,
    int count, int *actual, int behavior)
{
	int navail, i;

	assert(type == DDI_INTR_TYPE_MSIX && inum == m.allocated);
	assert(behavior == DDI_INTR_ALLOC_NORMAL && count > 0);
	if (m.fail_alloc)
		return (DDI_FAILURE);
	(void) ddi_intr_get_navail(dip, type, &navail);
	if (m.grant != 0)
		navail = MIN(navail, m.grant);
	*actual = MIN(count, navail - m.allocated);
	assert(*actual > 0);
	if (inum == 0)
		m.nreq = count;
	for (i = 0; i < *actual; i++) {
		assert(!m.live[inum + i]);
		m.live[inum + i] = B_TRUE;
		h[i] = &tokens[inum + i];
	}
	m.allocated += *actual;
	return (DDI_SUCCESS);
}

static int
entry(ddi_intr_handle_t h)
{
	int i = (int)((int *)h - tokens);

	assert(i >= 0 && i < 64 && m.live[i]);
	return (i);
}

/* A handler must be gone before its vector is freed. */
static int
ddi_intr_free(ddi_intr_handle_t h)
{
	int i = entry(h);

	if (i == m.fail_free)
		return (DDI_FAILURE);
	assert(!m.handler[i] && !m.enabled[i]);
	m.live[i] = B_FALSE;
	m.allocated--;
	return (DDI_SUCCESS);
}

static int
ddi_intr_get_pri(ddi_intr_handle_t h, uint_t *pri)
{
	*pri = (m.bad_pri && entry(h) >= NRINGS / 2) ? PRI + 1 : PRI;
	return (DDI_SUCCESS);
}

static int
ddi_intr_get_cap(ddi_intr_handle_t h, int *cap)
{
	(void) h;
	*cap = 0;
	return (DDI_SUCCESS);
}

static void *
kmem_zalloc(size_t n, int flags)
{
	void *p = calloc(1, n);

	(void) flags;
	assert(p != NULL);
	return (p);
}

static void
kmem_free(void *p, size_t n)
{
	(void) n;
	free(p);
}

static void
mutex_init(kmutex_t *mp, void *name, int type, void *pri)
{
	(void) name;
	(void) type;
	(void) pri;
	*mp = 0;
}

static void
mutex_destroy(kmutex_t *mp)
{
	(void) mp;
}

static void
mutex_enter(kmutex_t *mp)
{
	assert(!*mp);
	*mp = 1;
}

static void
mutex_exit(kmutex_t *mp)
{
	assert(*mp);
	*mp = 0;
}

static void
cv_init(kcondvar_t *cv, void *name, int type, void *arg)
{
	(void) name;
	(void) type;
	(void) arg;
	*cv = 0;
}

static ice_t dev;

/*
 * The DDI handler and enable operations, each of which can be made to fail
 * for one vector.
 */
static uint_t
ice_intr_msix(char *arg1, char *arg2)
{
	(void) arg1;
	(void) arg2;
	return (DDI_INTR_CLAIMED);
}

static int
ddi_intr_add_handler(ddi_intr_handle_t h, uint_t (*func)(char *, char *),
    void *arg1, void *arg2)
{
	int i = entry(h);

	assert(func == ice_intr_msix && arg1 == &dev);
	assert((int)(uintptr_t)arg2 == i && !m.handler[i] && !m.enabled[i]);
	if (i == m.fail_add)
		return (DDI_FAILURE);
	m.handler[i] = B_TRUE;
	return (DDI_SUCCESS);
}

/* As in the DDI, an enabled vector keeps its handler. */
static int
ddi_intr_remove_handler(ddi_intr_handle_t h)
{
	int i = entry(h);

	if (i == m.fail_remove || m.enabled[i] || !m.handler[i])
		return (DDI_FAILURE);
	m.handler[i] = B_FALSE;
	return (DDI_SUCCESS);
}

static int
ddi_intr_enable(ddi_intr_handle_t h)
{
	int i = entry(h);

	assert(m.handler[i]);
	m.enabled[i] = B_TRUE;
	return (DDI_SUCCESS);
}

static int
ddi_intr_disable(ddi_intr_handle_t h)
{
	int i = entry(h);

	if (i == m.fail_disable)
		return (DDI_FAILURE);
	m.enabled[i] = B_FALSE;
	return (DDI_SUCCESS);
}

static int
ddi_intr_block_enable(ddi_intr_handle_t *h, int n)
{
	(void) h;
	(void) n;
	assert(!"block enable");
	return (DDI_FAILURE);
}

static int
ddi_intr_block_disable(ddi_intr_handle_t *h, int n)
{
	(void) h;
	(void) n;
	assert(!"block disable");
	return (DDI_FAILURE);
}

static boolean_t
ice_datapath_pause(ice_t *ice)
{
	assert(MUTEX_HELD(&ice->ice_rebuild_lock));
	/* The queues are quiet before any handler goes away. */
	assert(m.enabled[0] && m.handler[ice->ice_intr_count - 1]);
	l.pauses++;
	if (l.pause_fails) {
		ice->ice_state |= ICE_STATE_ERROR | ICE_STATE_PFR_REQ;
		return (B_FALSE);
	}
	return (B_TRUE);
}

static int
ice_start_datapath(ice_t *ice)
{
	assert(m.enabled[ice->ice_intr_count - 1]);
	l.starts++;
	return (l.start_fails ? 5 : 0);
}

static boolean_t
ice_rx_rings_resume(ice_t *ice)
{
	(void) ice;
	l.resumes++;
	return (B_TRUE);
}

static void
ice_tx_wake(ice_t *ice)
{
	(void) ice;
	l.wakes++;
}

static void
ice_queues_intr_map(ice_t *ice)
{
	(void) ice;
	l.maps++;
}

static void
ice_link_report(ice_t *ice, int state)
{
	(void) ice;
	assert(state == LINK_STATE_DOWN);
	l.reports++;
}

static void
ddi_fm_service_impact(dev_info_t *dip, int impact)
{
	(void) dip;
	assert(impact == DDI_SERVICE_LOST);
	l.impacts++;
}

static void
atomic_or_32(uint32_t *p, uint32_t bits)
{
	*p |= bits;
}

static void ice_intr_rings_map(ice_t *);

/* MAC takes its perimeter here; the driver must hold none of its locks. */
static void
ice_mac_intr_set(ice_t *ice, boolean_t set)
{
	uint_t i;

	assert(!MUTEX_HELD(&ice->ice_rebuild_lock));
	assert(ice->ice_irm_busy);
	for (i = 0; i < ice->ice_num_rxr; i++) {
		assert(!MUTEX_HELD(&ice->ice_rxr[i].irxr_lock));
		l.mac_rx[i] = set ?
		    ice->ice_intr_handles[ice->ice_rxr[i].irxr_vec] : NULL;
	}
	for (i = 0; i < ice->ice_num_txr; i++) {
		l.mac_tx[i] = set ?
		    ice->ice_intr_handles[ice->ice_txr[i].itxr_vec] : NULL;
	}
	l.mac_cleared = !set;
	l.macsets++;
}

static boolean_t
ice_rx_ring_intr(ice_rx_ring_t *irr)
{
	irr->irxr_serviced++;
	return (irr->irxr_limit);
}

static void
ice_tx_ring_intr(ice_tx_ring_t *itr)
{
	itr->itxr_serviced++;
}

static void
wr32(struct ice_hw *hw, uint32_t reg, uint32_t value)
{
	(void) hw;
	dyn_ctl[reg] = value;
}

static void
ice_flush(struct ice_hw *hw)
{
	(void) hw;
}

static int ice_intr_adjust(ice_t *, ddi_cb_action_t, int);

#include "ice_intr_irm.h"

static dev_info_t dip;
static ice_rx_ring_t rxr[NRINGS];
static ice_tx_ring_t txr[NRINGS];

static void
reset(int nintrs)
{
	memset(&m, 0, sizeof (m));
	m.fail_add = m.fail_remove = m.fail_disable = m.fail_free = -1;
	memset(&l, 0, sizeof (l));
	memset(&dev, 0, sizeof (dev));
	dip.nintrs = nintrs;
	dev.ice_dip = &dip;
	dev.ice_hw.func_caps.common_cap.num_rxq = 256;
	dev.ice_hw.func_caps.common_cap.num_txq = 256;
	dev.ice_hw.func_caps.common_cap.num_msix_vectors = 2048;
	dev.ice_hw.func_caps.common_cap.rss_table_entry_width = 8;
}

/* An attached instance with NRINGS queue pairs and its handlers enabled. */
static void
attached(int nintrs)
{
	uint_t i;

	reset(nintrs);
	assert(ice_alloc_intrs(&dev));
	dev.ice_num_rxr = dev.ice_num_txr = dev.ice_nqueues;
	dev.ice_rxr = rxr;
	dev.ice_txr = txr;
	memset(rxr, 0, sizeof (rxr));
	memset(txr, 0, sizeof (txr));
	for (i = 0; i < dev.ice_num_rxr; i++) {
		rxr[i].irxr_vec = ice_ring_vector(&dev, i);
		txr[i].itxr_vec = ice_ring_vector(&dev, i);
	}
	dev.ice_attach_progress = ICE_ATTACH_ADD_INTR | ICE_ATTACH_ENABLE_INTR;
	mutex_enter(&dev.ice_rebuild_lock);
	assert(ice_add_intr_handlers(&dev) && ice_intr_enable(&dev));
	mutex_exit(&dev.ice_rebuild_lock);
}

static void finish(void);

static int
callback(int action, int count)
{
	return (m.cb(&dip, action, (void *)(uintptr_t)count, m.cb_arg, NULL));
}

/*
 * Every ring names an allocated vector other than 0, each handler services
 * exactly the rings mapped to it, and MAC holds each ring's handle.
 */
static void
check_map(void)
{
	uint_t i;
	int v;

	assert(m.allocated == dev.ice_intr_count && !l.mac_cleared);
	assert(!dev.ice_irm_busy && !dev.ice_rebuild_lock);
	for (v = 0; v < 64; v++) {
		boolean_t in = v < dev.ice_intr_count;

		assert(m.live[v] == in && m.handler[v] == in &&
		    m.enabled[v] == in);
	}
	for (i = 0; i < dev.ice_num_rxr; i++) {
		assert(rxr[i].irxr_vec >= 1 &&
		    (int)rxr[i].irxr_vec < dev.ice_intr_count);
		assert(txr[i].itxr_vec == rxr[i].irxr_vec);
		assert(l.mac_rx[i] == dev.ice_intr_handles[rxr[i].irxr_vec]);
		assert(l.mac_tx[i] == dev.ice_intr_handles[txr[i].itxr_vec]);
	}
	for (v = 1; v < dev.ice_intr_count; v++) {
		for (i = 0; i < dev.ice_num_rxr; i++)
			rxr[i].irxr_serviced = txr[i].itxr_serviced = 0;
		assert(ice_intr_queue(&dev, (uint_t)v) == DDI_INTR_CLAIMED);
		for (i = 0; i < dev.ice_num_rxr; i++) {
			int want = (int)rxr[i].irxr_vec == v;

			assert(rxr[i].irxr_serviced == want);
			assert(txr[i].itxr_serviced == want);
		}
		assert(dyn_ctl[v] == ICE_GLINT_DYN_CTL_REARM);
	}
}

static void
irm(void)
{
	int before;
	uint_t i;

	/* A reclaim folds the 16 rings onto the 7 queue vectors left. */
	attached(1024);
	assert(dev.ice_intr_count == 17);
	l.mac_rx[0] = l.mac_tx[0] = NULL;
	for (i = 0; i < NRINGS; i++) {
		l.mac_rx[i] = dev.ice_intr_handles[rxr[i].irxr_vec];
		l.mac_tx[i] = dev.ice_intr_handles[txr[i].itxr_vec];
	}
	check_map();
	assert(callback(DDI_CB_INTR_REMOVE, 9) == DDI_SUCCESS);
	assert(dev.ice_intr_count == 8 && l.macsets == 2);
	assert(rxr[7].irxr_vec == 1 && rxr[15].irxr_vec == 2);
	check_map();
	/* Stopped, so the queues are only rerouted. */
	assert(l.pauses == 0 && l.starts == 0 && l.maps == 1);

	/* A limit hit on any ring the vector serves asks for a refire. */
	rxr[8].irxr_limit = B_TRUE;
	(void) ice_intr_queue(&dev, rxr[8].irxr_vec);
	assert(dyn_ctl[rxr[8].irxr_vec] & GLINT_DYN_CTL_SWINT_TRIG_M);
	rxr[8].irxr_limit = B_FALSE;

	/* An offer spreads them out again, up to one vector per ring. */
	dev.ice_state = ICE_STATE_STARTED;
	assert(callback(DDI_CB_INTR_ADD, INT32_MAX) == DDI_SUCCESS);
	assert(dev.ice_intr_count == 17);
	for (i = 0; i < NRINGS; i++)
		assert(rxr[i].irxr_vec == 1 + i);
	check_map();
	assert(l.pauses == 1 && l.starts == 1 && l.resumes == 1);
	assert(l.wakes == 1 && l.maps == 1);
	assert(callback(DDI_CB_INTR_ADD, 4) == DDI_SUCCESS);
	assert(dev.ice_intr_count == 17 && l.pauses == 1);

	/* Vector 0 and one queue vector always stay. */
	assert(callback(DDI_CB_INTR_REMOVE, 16) == DDI_FAILURE);
	assert(dev.ice_intr_count == 17 && l.pauses == 1);
	assert(callback(DDI_CB_INTR_REMOVE, 15) == DDI_SUCCESS);
	assert(dev.ice_intr_count == 2);
	for (i = 0; i < NRINGS; i++)
		assert(rxr[i].irxr_vec == 1);
	check_map();
	assert(callback(DDI_CB_INTR_REMOVE, 0) == DDI_SUCCESS);
	assert(callback(DDI_CB_INTR_REMOVE, -1) == DDI_EINVAL);
	assert(callback(DDI_CB_OTHER, 1) == DDI_ENOTSUP);

	/* A failed offer leaves the vectors and the map as they were. */
	m.fail_alloc = B_TRUE;
	assert(callback(DDI_CB_INTR_ADD, 3) == DDI_FAILURE);
	assert(dev.ice_intr_count == 2);
	check_map();
	m.fail_alloc = B_FALSE;
	/* So does one at another priority than the locks were made at. */
	m.bad_pri = B_TRUE;
	assert(callback(DDI_CB_INTR_ADD, 15) == DDI_FAILURE);
	assert(dev.ice_intr_count == 2);
	check_map();
	m.bad_pri = B_FALSE;

	/* A datapath that is down or owes a reset is only rerouted. */
	before = l.starts;
	dev.ice_state = ICE_STATE_STARTED | ICE_STATE_RESET_PENDING;
	assert(callback(DDI_CB_INTR_ADD, 3) == DDI_SUCCESS);
	assert(dev.ice_intr_count == 5 && l.starts == before);
	check_map();
	dev.ice_state = ICE_STATE_STARTED;
	l.pause_fails = B_TRUE;
	assert(callback(DDI_CB_INTR_REMOVE, 1) == DDI_SUCCESS);
	assert(dev.ice_intr_count == 4 && l.starts == before);
	assert((dev.ice_state & ICE_STATE_PFR_REQ) != 0);
	check_map();
	l.pause_fails = B_FALSE;
	dev.ice_state = ICE_STATE_STARTED;
	l.start_fails = B_TRUE;
	assert(callback(DDI_CB_INTR_ADD, 1) == DDI_SUCCESS);
	assert((dev.ice_state & ICE_STATE_ERROR) != 0 && l.reports == 1);
	check_map();
	l.start_fails = B_FALSE;

	/* Attach, detach and a change in progress refuse and touch nothing. */
	dev.ice_state = 0;
	before = l.macsets;
	dev.ice_attaching = B_TRUE;
	assert(callback(DDI_CB_INTR_REMOVE, 1) == DDI_FAILURE);
	dev.ice_attaching = B_FALSE;
	dev.ice_detaching = B_TRUE;
	assert(callback(DDI_CB_INTR_ADD, 1) == DDI_FAILURE);
	dev.ice_detaching = B_FALSE;
	dev.ice_irm_busy = B_TRUE;
	assert(callback(DDI_CB_INTR_ADD, 1) == DDI_FAILURE);
	dev.ice_irm_busy = B_FALSE;
	assert(dev.ice_intr_count == 5 && l.macsets == before);
	check_map();

	/* A terminal reset takes back vectors but refuses more. */
	dev.ice_state = ICE_STATE_RESET_FAILED | ICE_STATE_ERROR;
	assert(callback(DDI_CB_INTR_ADD, 4) == DDI_FAILURE);
	assert(callback(DDI_CB_INTR_REMOVE, 2) == DDI_SUCCESS);
	assert(dev.ice_intr_count == 3);
	check_map();

	/* Handlers that cannot be restored leave the instance down for good. */
	dev.ice_state = 0;
	m.fail_add = 1;
	assert(callback(DDI_CB_INTR_ADD, 1) == DDI_FAILURE);
	assert((dev.ice_state & ICE_STATE_RESET_FAILED) != 0 && l.impacts == 1);
	assert((dev.ice_attach_progress & ICE_ATTACH_ADD_INTR) == 0);
	assert(!dev.ice_irm_busy && !dev.ice_rebuild_lock);
	for (i = 0; i < 64; i++)
		assert(!m.handler[i] && !m.enabled[i]);
	finish();
}

/* Tear an instance down whatever state a failure left its vectors in. */
static void
finish(void)
{
	int v;

	m.fail_add = m.fail_remove = m.fail_disable = m.fail_free = -1;
	for (v = 0; v < 64; v++)
		m.handler[v] = m.enabled[v] = B_FALSE;
	ice_free_intrs(&dev);
	assert(m.allocated == 0 && m.unregisters == 1);
}

static void
handles_intact(int n)
{
	int v;

	assert(dev.ice_intr_count == n && m.allocated == n);
	for (v = 0; v < n; v++)
		assert(dev.ice_intr_handles[v] == &tokens[v]);
	assert(!dev.ice_irm_busy && !dev.ice_rebuild_lock);
}

/*
 * A DDI step that fails never leaves a vector that can still interrupt, or
 * one IRM kept, without its handle below the count.
 */
static void
irm_failures(void)
{
	/* A vector that stays enabled keeps its handler and handle. */
	attached(1024);
	m.fail_disable = 16;
	assert(callback(DDI_CB_INTR_REMOVE, 9) == DDI_FAILURE);
	handles_intact(17);
	assert(m.enabled[16] && m.handler[16]);
	assert((dev.ice_state & ICE_STATE_RESET_FAILED) != 0);
	finish();

	/* So does one whose handler will not come off. */
	attached(1024);
	m.fail_remove = 12;
	assert(callback(DDI_CB_INTR_REMOVE, 9) == DDI_FAILURE);
	handles_intact(17);
	assert(m.handler[12] && !m.enabled[12]);
	assert((dev.ice_state & ICE_STATE_RESET_FAILED) != 0);
	finish();

	/* A vector IRM cannot take back stays below the count, in use. */
	attached(1024);
	m.fail_free = 12;
	assert(callback(DDI_CB_INTR_REMOVE, 9) == DDI_FAILURE);
	assert(dev.ice_intr_count == 13 && dev.ice_intr_handles[13] == NULL);
	assert((dev.ice_state & ICE_STATE_RESET_FAILED) == 0);
	m.fail_free = -1;
	check_map();
	finish();

	/* An offer at another priority that cannot be given back. */
	attached(1024);
	assert(callback(DDI_CB_INTR_REMOVE, 9) == DDI_SUCCESS);
	m.bad_pri = B_TRUE;
	m.fail_free = 10;
	assert(callback(DDI_CB_INTR_ADD, 9) == DDI_FAILURE);
	handles_intact(11);
	assert(!m.handler[8] && !m.handler[10]);
	assert((dev.ice_state & ICE_STATE_RESET_FAILED) != 0);
	m.bad_pri = B_FALSE;
	finish();
}

int
main(void)
{
	/* With the callback, the default 16 queue pairs get their vectors. */
	reset(1024);
	assert(ice_alloc_intrs(&dev));
	assert(m.registered && dev.ice_intr_count == 17);
	assert(dev.ice_nqueues == 16 && m.allocated == 17 && m.notes == 1);
	ice_free_intrs(&dev);
	assert(m.allocated == 0 && m.unregisters == 1 && !m.registered);
	assert(dev.ice_intr_cb == NULL && dev.ice_intr_handles == NULL);

	/* A partial grant keeps the full request, so IRM can offer more. */
	reset(1024);
	m.grant = 9;
	assert(ice_alloc_intrs(&dev));
	assert(dev.ice_intr_count == 9 && dev.ice_nqueues == 8 && m.nreq == 17);
	ice_free_intrs(&dev);

	/* Without it the platform default of 8 leaves 7 queue pairs. */
	reset(1024);
	m.fail_register = B_TRUE;
	assert(ice_alloc_intrs(&dev));
	assert(dev.ice_intr_count == APIX_LIMIT && dev.ice_nqueues == 7);
	/* One note for the missing callback, one for the vector report. */
	assert(m.notes == 2 && m.errors == 0);
	ice_free_intrs(&dev);
	assert(m.allocated == 0 && m.unregisters == 0);

	/* A device with few vectors still gets them all. */
	reset(5);
	assert(ice_alloc_intrs(&dev) && dev.ice_nqueues == 4);
	ice_free_intrs(&dev);

	/* Every failure after registration unregisters. */
	reset(1);
	assert(!ice_alloc_intrs(&dev));
	assert(m.unregisters == 1 && !m.registered && m.errors == 1);
	reset(1024);
	dev.ice_hw.func_caps.common_cap.num_msix_vectors = 1;
	assert(!ice_alloc_intrs(&dev));
	assert(m.unregisters == 1 && !m.registered);
	reset(1024);
	m.fail_alloc = B_TRUE;
	assert(!ice_alloc_intrs(&dev));
	assert(m.unregisters == 1 && !m.registered);
	assert(dev.ice_intr_handles == NULL && m.allocated == 0);
	/* Teardown after a failed attach step is harmless. */
	ice_free_intrs(&dev);
	assert(m.unregisters == 1);

	irm();
	irm_failures();

	(void) puts("PASS: MSI-X sizing registers for interrupt resource "
	    "management, and reclaims and offers keep every ring on a live "
	    "vector");
	return (0);
}

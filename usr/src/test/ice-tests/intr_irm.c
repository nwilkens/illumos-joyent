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
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int uint_t;
typedef int boolean_t;
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
#define	DDI_ENOTSUP		-7
#define	DDI_INTR_TYPE_MSIX	4
#define	DDI_INTR_ALLOC_NORMAL	0
#define	DDI_CB_FLAG_INTR	1
#define	DDI_CB_INTR_ADD		0
#define	DDI_CB_INTR_REMOVE	1
#define	DDI_DEV_T_ANY		0
#define	MUTEX_DRIVER		0
#define	CV_DRIVER		0
#define	CE_NOTE			1
#define	KM_SLEEP		0
#define	ICE_INTR_MSIX_MIN	2
#define	ICE_DEF_QUEUES		16
#define	ICE_MAX_QUEUES		127
#define	APIX_LIMIT		8
#define	DDI_INTR_PRI(p)		((void *)(uintptr_t)(p))
#define	MIN(a, b)		((a) < (b) ? (a) : (b))
#define	MAX(a, b)		((a) > (b) ? (a) : (b))
#define	ASSERT3U(a, op, b)	assert((a) op(b))
#define	_NOTE(x)

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

typedef struct ice {
	dev_info_t *ice_dip;
	struct ice_hw ice_hw;
	int ice_intr_type, ice_intr_cap, ice_intr_count;
	uint_t ice_intr_pri;
	size_t ice_intr_size;
	ddi_intr_handle_t *ice_intr_handles;
	ddi_cb_handle_t ice_intr_cb;
	uint16_t ice_nqueues;
	kmutex_t ice_lock, ice_lse_lock;
	kcondvar_t ice_lse_cv;
} ice_t;

static int ncpus = 40, max_ncpus = 40, boot_max_ncpus = -1;

static struct {
	boolean_t fail_register, fail_alloc;
	int registered, unregisters, allocated, errors, notes;
	ddi_cb_func_t cb;
	void *cb_arg;
} m;

static int handle_token;

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

static int
ddi_intr_alloc(dev_info_t *dip, ddi_intr_handle_t *h, int type, int inum,
    int count, int *actual, int behavior)
{
	int navail, i;

	assert(type == DDI_INTR_TYPE_MSIX && inum == 0);
	assert(behavior == DDI_INTR_ALLOC_NORMAL);
	if (m.fail_alloc)
		return (DDI_FAILURE);
	(void) ddi_intr_get_navail(dip, type, &navail);
	*actual = MIN(count, navail);
	for (i = 0; i < *actual; i++)
		h[i] = &handle_token;
	m.allocated += *actual;
	return (DDI_SUCCESS);
}

static int
ddi_intr_free(ddi_intr_handle_t h)
{
	assert(h == &handle_token && m.allocated > 0);
	m.allocated--;
	return (DDI_SUCCESS);
}

static int
ddi_intr_get_pri(ddi_intr_handle_t h, uint_t *pri)
{
	(void) h;
	*pri = 6;
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
mutex_init(kmutex_t *m, void *name, int type, void *pri)
{
	(void) name;
	(void) type;
	(void) pri;
	*m = 0;
}

static void
mutex_destroy(kmutex_t *m)
{
	(void) m;
}

static void
cv_init(kcondvar_t *cv, void *name, int type, void *arg)
{
	(void) name;
	(void) type;
	(void) arg;
	*cv = 0;
}

#include "ice_intr_irm.h"

static dev_info_t dip;
static ice_t dev;

static void
reset(int nintrs)
{
	memset(&m, 0, sizeof (m));
	memset(&dev, 0, sizeof (dev));
	dip.nintrs = nintrs;
	dev.ice_dip = &dip;
	dev.ice_hw.func_caps.common_cap.num_rxq = 256;
	dev.ice_hw.func_caps.common_cap.num_txq = 256;
	dev.ice_hw.func_caps.common_cap.num_msix_vectors = 2048;
	dev.ice_hw.func_caps.common_cap.rss_table_entry_width = 8;
}

int
main(void)
{
	/* With the callback, the default 16 queue pairs get their vectors. */
	reset(1024);
	assert(ice_alloc_intrs(&dev));
	assert(m.registered && dev.ice_intr_count == 17);
	assert(dev.ice_nqueues == 16 && m.allocated == 17 && m.notes == 1);
	/* Offers and reclaims are declined; the rings keep their vectors. */
	assert(m.cb(&dip, DDI_CB_INTR_ADD, (void *)4, m.cb_arg, NULL) ==
	    DDI_ENOTSUP);
	assert(m.cb(&dip, DDI_CB_INTR_REMOVE, (void *)4, m.cb_arg, NULL) ==
	    DDI_ENOTSUP);
	ice_free_intrs(&dev);
	assert(m.allocated == 0 && m.unregisters == 1 && !m.registered);
	assert(dev.ice_intr_cb == NULL && dev.ice_intr_handles == NULL);

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

	(void) puts("PASS: MSI-X sizing registers for interrupt resource "
	    "management and unwinds it");
	return (0);
}

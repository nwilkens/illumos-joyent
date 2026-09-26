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
 * Copyright 2026 MNX Cloud, Inc.
 */

/*
 * A cut-down mlxcx_t that holds every field the extracted code uses, in
 * both the fixed and the original driver, plus a DMA model that keeps freed
 * buffers so that late device writes to them are caught.
 */

#ifndef _MLXCX_MIN_H
#define	_MLXCX_MIN_H

/* Just enough of an event queue and a port for the async interrupt path. */
typedef struct mlxcx_event_queue {
	kmutex_t		mleq_mtx;
	kcondvar_t		mleq_cv;
	uint_t			mleq_state;
	uint_t			mleq_intr_index;
	mlxcx_dma_buffer_t	mleq_dma;
	void			*mleq_ent;
} mlxcx_event_queue_t;

/* Just enough of the work and completion queues for their teardown. */
typedef struct mlxcx_completion_queue mlxcx_completion_queue_t;

typedef struct mlxcx_work_queue {
	kmutex_t		mlwq_mtx;
	list_node_t		mlwq_entry;
	uint_t			mlwq_type;
	uint_t			mlwq_num;
	mlxcx_completion_queue_t *mlwq_cq;
	void			*mlwq_bufs;
	void			*mlwq_foreign_bufs;
	uint_t			mlwq_state;
	mlxcx_dma_buffer_t	mlwq_dma;
	mlxcx_dma_buffer_t	mlwq_doorbell_dma;
	void			*mlwq_send_ent;
	void			*mlwq_doorbell;
} mlxcx_work_queue_t;

struct mlxcx_completion_queue {
	kmutex_t		mlcq_mtx;
	mlxcx_work_queue_t	*mlcq_wq;
	uint_t			mlcq_state;
	mlxcx_dma_buffer_t	mlcq_dma;
	mlxcx_dma_buffer_t	mlcq_doorbell_dma;
	void			*mlcq_ent;
	void			*mlcq_doorbell;
};

struct mlxcx_port {
	kmutex_t		mlp_mtx;
	mlxcx_async_param_t	mlx_port_event;
};

struct mlxcx {
	dev_info_t		*mlx_dip;
	int			mlx_inst;
	int			mlx_fm_caps;
	uint16_t		mlx_fw_maj;
	uint16_t		mlx_fw_min;
	uint16_t		mlx_fw_rev;
	uint16_t		mlx_cmd_rev;
	ddi_acc_handle_t	mlx_regs_handle;
	caddr_t			mlx_regs_base;
	off_t			mlx_regs_size;
	mlxcx_cmd_queue_t	mlx_cmd;
	uint_t			mlx_intr_pri;
	uint_t			mlx_async_intr_pri;
	mlxcx_uar_t		mlx_uar;
	kmutex_t		mlx_pagemtx;
	uint_t			mlx_npages;
	uint_t			mlx_npages_max;
	uint64_t		mlx_pages_unknown;
	uint64_t		mlx_pages_refused;
	uint64_t		mlx_pages_bad_req;
	avl_tree_t		mlx_pages;
	mlxcx_async_param_t	mlx_npages_req[MLXCX_FUNC_ID_MAX + 1];
	taskq_t			*mlx_async_tq;
	taskq_t			*mlx_pages_tq;
	uint_t			mlx_nports;
	mlxcx_port_t		*mlx_ports;
	kmutex_t		mlx_quarantine_mtx;
	list_t			mlx_quarantine;
	list_t			mlx_wqs;
};

#ifdef DEBUG
#define	MLXCX_DMA_SYNC(dma, flag)	VERIFY0(ddi_dma_sync( \
					    (dma).mxdb_dma_handle, 0, 0, \
					    (flag)))
#else
#define	MLXCX_DMA_SYNC(dma, flag)	(void) ddi_dma_sync( \
					    (dma).mxdb_dma_handle, 0, 0, \
					    (flag))
#endif

static void
mlxcx_warn(mlxcx_t *mlxp, const char *fmt, ...)
{
	va_list ap;

	(void) mlxp;
	stub_warnings++;
	if (!stub_verbose)
		return;
	va_start(ap, fmt);
	(void) fprintf(stderr, "warn: ");
	(void) vfprintf(stderr, fmt, ap);
	(void) fprintf(stderr, "\n");
	va_end(ap);
}

#define	mlxcx_note	mlxcx_warn

static void
mlxcx_panic(mlxcx_t *mlxp, const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	(void) mlxp;
	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);
	stub_fail("kernel panic: mlxcx_panic: %s", buf);
}

static void
mlxcx_fm_ereport(mlxcx_t *mlxp, const char *detail)
{
	(void) mlxp; (void) detail;
	stub_ereports++;
}

/* DMA buffers with fake bus addresses. */
#define	STUB_DMA_MAX		16384
#define	STUB_DMA_BASE		0x100000000ULL
#define	STUB_DMA_STRIDE		0x10000ULL

typedef struct {
	void			*sd_va;
	size_t			sd_len;
	int			sd_live;
	ddi_dma_cookie_t	sd_cookie;
} stub_dma_t;

static stub_dma_t stub_dma[STUB_DMA_MAX];
static uint_t stub_ndma;
static int64_t stub_dma_live;

static void
mlxcx_dma_acc_attr(mlxcx_t *mlxp, ddi_device_acc_attr_t *acc)
{
	(void) mlxp;
	memset(acc, 0, sizeof (*acc));
}

static void
mlxcx_dma_page_attr(mlxcx_t *mlxp, ddi_dma_attr_t *attr)
{
	(void) mlxp;
	memset(attr, 0, sizeof (*attr));
}

static boolean_t stub_dma_fail;

static boolean_t
mlxcx_dma_alloc(mlxcx_t *mlxp, mlxcx_dma_buffer_t *mxdb, ddi_dma_attr_t *attr,
    ddi_device_acc_attr_t *acc, boolean_t zero, size_t size, boolean_t wait)
{
	stub_dma_t *sd;

	(void) mlxp; (void) attr; (void) acc; (void) wait;
	if (stub_dma_fail)
		return (B_FALSE);
	if (stub_ndma == STUB_DMA_MAX)
		stub_fail("DMA model exhausted");
	sd = &stub_dma[stub_ndma];
	sd->sd_va = aligned_alloc(4096, (size + 4095) & ~(size_t)4095);
	memset(sd->sd_va, zero ? 0 : 0x5a, size);
	sd->sd_len = size;
	sd->sd_live = 1;
	sd->sd_cookie.dmac_laddress = STUB_DMA_BASE +
	    STUB_DMA_STRIDE * stub_ndma;
	sd->sd_cookie.dmac_size = size;
	stub_ndma++;
	stub_dma_live++;
	memset(mxdb, 0, sizeof (*mxdb));
	mxdb->mxdb_va = sd->sd_va;
	mxdb->mxdb_len = size;
	mxdb->mxdb_dma_handle = sd;
	mxdb->mxdb_ncookies = 1;
	mxdb->mxdb_flags = MLXCX_DMABUF_HDL_ALLOC | MLXCX_DMABUF_MEM_ALLOC |
	    MLXCX_DMABUF_BOUND;
	return (B_TRUE);
}

static void
mlxcx_dma_free(mlxcx_dma_buffer_t *mxdb)
{
	stub_dma_t *sd = mxdb->mxdb_dma_handle;

	if (sd == NULL) {
		memset(mxdb, 0, sizeof (*mxdb));
		return;
	}
	if (!sd->sd_live)
		stub_fail("DMA buffer freed twice");
	sd->sd_live = 0;
	stub_dma_live--;
	memset(sd->sd_va, 0xdd, sd->sd_len);
	memset(mxdb, 0, sizeof (*mxdb));
}

static const ddi_dma_cookie_t *
mlxcx_dma_cookie_one(mlxcx_dma_buffer_t *mxdb)
{
	stub_dma_t *sd = mxdb->mxdb_dma_handle;

	return (&sd->sd_cookie);
}

/* Resolve a device bus address; a write to a freed buffer is a UAF. */
static void *
stub_dma_va(uint64_t pa, size_t len, boolean_t write)
{
	uint64_t i;
	stub_dma_t *sd;

	if (pa < STUB_DMA_BASE)
		stub_fail("device DMA to unmapped address 0x%" PRIx64, pa);
	i = (pa - STUB_DMA_BASE) / STUB_DMA_STRIDE;
	if (i >= stub_ndma)
		stub_fail("device DMA to unmapped address 0x%" PRIx64, pa);
	sd = &stub_dma[i];
	if (pa - sd->sd_cookie.dmac_laddress + len > sd->sd_len)
		stub_fail("device DMA past the end of a buffer");
	if (write && !sd->sd_live)
		stub_fail("device DMA write to freed memory at 0x%" PRIx64
		    " (use after free)", pa);
	return ((char *)sd->sd_va + (pa - sd->sd_cookie.dmac_laddress));
}

#endif /* _MLXCX_MIN_H */

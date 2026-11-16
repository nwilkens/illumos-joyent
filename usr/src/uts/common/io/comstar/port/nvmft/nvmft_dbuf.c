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
 * STMF requires every local port to supply a dbuf store; the LU calls
 * stmf_alloc_dbuf() (which dispatches to ds_alloc_data_buf) to obtain a buffer,
 * fills it (WRITE) or has us fill it (READ), and drives the transfer through
 * lport_xfer_data().  A port serves every transport, so the store asks the
 * task's transport for a buffer from its pool, and falls back to kmem (as for
 * TCP) when the transport has none.  A transport that maps LU memory gets the
 * LU's buffers through ds_setup_dbuf.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/kmem.h>
#include <sys/list.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include <sys/time.h>
#include <sys/stmf.h>
#include <sys/lpif.h>
#include <sys/stmf_ioctl.h>
#include <sys/portif.h>

#include "nvmft_io.h"

static stmf_data_buf_t *nvmft_dbuf_alloc(scsi_task_t *task, uint32_t size,
    uint32_t *pminsize, uint32_t flags);
static void nvmft_dbuf_free(stmf_dbuf_store_t *ds, stmf_data_buf_t *dbuf);

static struct nvmf_qpair *
nvmft_dbuf_qpair(scsi_task_t *task, uint32_t cap, struct nvmft_qpair **qpp)
{
	nvmft_task_priv_t *priv = task->task_port_private;

	if (priv == NULL || priv->ntp_iio != NULL ||
	    (nvmft_qpair_caps(priv->ntp_qp) & cap) == 0)
		return (NULL);
	*qpp = priv->ntp_qp;
	return (nvmft_qpair_data_hold(priv->ntp_qp));
}

/* ARGSUSED */
static stmf_data_buf_t *
nvmft_dbuf_alloc(scsi_task_t *task, uint32_t size, uint32_t *pminsize,
    uint32_t flags)
{
	stmf_data_buf_t *dbuf;
	nvmft_dbuf_priv_t *ndp;
	struct nvmft_qpair *qp;
	struct nvmf_qpair *nq;
	void *buf = NULL;
	int error;

	_NOTE(ARGUNUSED(flags));

	if (size == 0)
		return (NULL);

	dbuf = stmf_alloc(STMF_STRUCT_DATA_BUF, sizeof (nvmft_dbuf_priv_t), 0);
	if (dbuf == NULL)
		return (NULL);
	ndp = dbuf->db_port_private;

	nq = nvmft_dbuf_qpair(task, NVMF_QP_CAP_DATA_BUF, &qp);
	if (nq != NULL) {
		error = nvmf_alloc_data_buf(nq, size, MIN(*pminsize, size),
		    &ndp->ndp_db);
		nvmft_qpair_data_rele(qp);
		if (error != 0) {
			stmf_free(dbuf);
			return (NULL);
		}
		ndp->ndp_transport = B_TRUE;
		buf = ndp->ndp_db.ndb_addr;
		size = (uint32_t)ndp->ndp_db.ndb_len;
	} else {
		buf = kmem_alloc(size, KM_NOSLEEP);
		if (buf == NULL) {
			stmf_free(dbuf);
			return (NULL);
		}
		ndp->ndp_buf = buf;
		ndp->ndp_size = size;
	}
	ndp->ndp_magic = NVMFT_DBUF_MAGIC;

	dbuf->db_flags = DB_DONT_CACHE;
	dbuf->db_buf_size = size;
	dbuf->db_data_size = size;
	dbuf->db_sglist_length = 1;
	dbuf->db_sglist[0].seg_addr = buf;
	dbuf->db_sglist[0].seg_length = size;
	return (dbuf);
}

/* ARGSUSED */
static void
nvmft_dbuf_free(stmf_dbuf_store_t *ds, stmf_data_buf_t *dbuf)
{
	nvmft_dbuf_priv_t *ndp = dbuf->db_port_private;

	_NOTE(ARGUNUSED(ds));

	if (ndp->ndp_transport)
		nvmf_free_data_buf(&ndp->ndp_db);
	else
		kmem_free(ndp->ndp_buf, ndp->ndp_size);
	stmf_free(dbuf);
}

/* Let the transport map an LU buffer.  sbd leaves db_port_private to us. */
/* ARGSUSED */
static stmf_status_t
nvmft_dbuf_setup(scsi_task_t *task, stmf_data_buf_t *dbuf, uint32_t flags)
{
	nvmft_dbuf_priv_t *ndp;
	struct nvmft_qpair *qp;
	struct nvmf_qpair *nq;
	int error;

	_NOTE(ARGUNUSED(flags));

	nq = nvmft_dbuf_qpair(task, NVMF_QP_CAP_LU_DBUF, &qp);
	if (nq == NULL)
		return (STMF_FAILURE);
	ndp = kmem_zalloc(sizeof (*ndp), KM_NOSLEEP);
	if (ndp == NULL) {
		nvmft_qpair_data_rele(qp);
		return (STMF_FAILURE);
	}
	error = nvmf_map_data_buf(nq, (const nvmf_seg_t *)dbuf->db_sglist,
	    dbuf->db_sglist_length, &ndp->ndp_db);
	nvmft_qpair_data_rele(qp);
	if (error != 0) {
		kmem_free(ndp, sizeof (*ndp));
		return (STMF_FAILURE);
	}
	ndp->ndp_magic = NVMFT_DBUF_MAGIC;
	ndp->ndp_transport = B_TRUE;
	dbuf->db_port_private = ndp;
	return (STMF_SUCCESS);
}

/* ARGSUSED */
static void
nvmft_dbuf_teardown(stmf_dbuf_store_t *ds, stmf_data_buf_t *dbuf)
{
	nvmft_dbuf_priv_t *ndp = dbuf->db_port_private;

	_NOTE(ARGUNUSED(ds));

	nvmf_free_data_buf(&ndp->ndp_db);
	kmem_free(ndp, sizeof (*ndp));
	dbuf->db_port_private = NULL;
}

stmf_dbuf_store_t *
nvmft_dbuf_store_create(void)
{
	stmf_dbuf_store_t *ds;

	ds = stmf_alloc(STMF_STRUCT_DBUF_STORE, 0, 0);
	if (ds == NULL)
		return (NULL);
	ds->ds_alloc_data_buf = nvmft_dbuf_alloc;
	ds->ds_free_data_buf = nvmft_dbuf_free;
	ds->ds_setup_dbuf = nvmft_dbuf_setup;
	ds->ds_teardown_dbuf = nvmft_dbuf_teardown;
	return (ds);
}

void
nvmft_dbuf_store_destroy(stmf_dbuf_store_t *ds)
{
	stmf_free(ds);
}

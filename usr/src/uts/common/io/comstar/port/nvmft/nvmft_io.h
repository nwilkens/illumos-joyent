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

#ifndef	_NVMFT_IO_H
#define	_NVMFT_IO_H

/*
 * State shared by the files that bind nvmft to STMF: the per-task and per-dbuf
 * private data, and the local port entry points.
 */

#include "nvmft_var.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The private data of an STMF task.  A host command keeps it in its capsule;
 * an internal task keeps it in its nvmft_internal_io_t.
 */
typedef struct nvmft_task_priv {
	struct nvmf_capsule	*ntp_nc;
	struct nvmft_qpair	*ntp_qp;
	scsi_task_t		*ntp_task;
	list_node_t		ntp_link;	/* deferred or io_work */
	list_node_t		ntp_task_link;	/* ctrlr_tasks */
	/*
	 * Transfers the transport holds for this task.  STMF frees the dbufs
	 * of an aborted task, so an abort waits for these to drain.
	 */
	kmutex_t		ntp_lock;
	uint_t			ntp_xfers;
	/* Command payload length, charged against ctrlr_pending_bytes. */
	uint32_t		ntp_data_len;
	boolean_t		ntp_aborting;
	/* The host has its response, or the transport sends it. */
	boolean_t		ntp_responded;
	/* The status of the response if the task ends without one. */
	uint8_t			ntp_sc;
	/* A cancel of the task's receives is queued or running; ctrlr_lock. */
	boolean_t		ntp_cancel;
	struct nvmft_internal_io *ntp_iio;
} nvmft_task_priv_t;

/*
 * A READ CAPACITY(16) that nvmft sends to an LU for Identify Namespace.  The
 * waiter and the task each hold a reference, so either may finish first.
 */
typedef struct nvmft_internal_io {
	nvmft_task_priv_t	iio_priv;
	nvmft_controller_t	*iio_ctrlr;
	kmutex_t		iio_lock;
	kcondvar_t		iio_cv;
	uint_t			iio_refs;
	boolean_t		iio_done;
	uint8_t			iio_scsi_status;
	uint8_t			iio_sense_key;
	uint32_t		iio_xfered;
	uint8_t			iio_buf[32];
} nvmft_internal_io_t;

/*
 * State of the transfer in flight on one dbuf.  The thread that submits it and
 * the transport completion race; whichever arrives second finishes it, so the
 * submitter has dropped its qpair reference before STMF can free the task.
 */
#define	NVMFT_XFER_SUBMITTED	0x1
#define	NVMFT_XFER_COMPLETED	0x2

typedef struct nvmft_xfer {
	scsi_task_t	*nx_task;
	stmf_data_buf_t	*nx_dbuf;
	volatile uint_t	nx_state;
	uint_t		nx_status;
	boolean_t	nx_to_rport;
	boolean_t	nx_final;	/* the transport sends the response */
	nvme_cqe_t	nx_cqe;
} nvmft_xfer_t;

/* Per-dbuf private state of the dbuf store. */
#define	NVMFT_DBUF_MAGIC	0x6e766474	/* "nvdt" */

typedef struct nvmft_dbuf_priv {
	uint32_t	ndp_magic;
	uint32_t	ndp_size;
	void		*ndp_buf;	/* kmem buffer */
	boolean_t	ndp_transport;	/* ndp_db is the transport's */
	nvmf_databuf_t	ndp_db;
	nvmft_xfer_t	ndp_xfer;
} nvmft_dbuf_priv_t;

/* nvmft_io.c */
nvmft_controller_t *nvmft_task_ctrlr(nvmft_task_priv_t *priv);
void	nvmft_task_listed(nvmft_controller_t *ctrlr, nvmft_task_priv_t *priv,
    scsi_task_t *task);
void	nvmft_task_unlist(nvmft_controller_t *ctrlr, nvmft_task_priv_t *priv);
stmf_status_t nvmft_lport_xfer_data(scsi_task_t *task, stmf_data_buf_t *dbuf,
    uint32_t ioflags);
stmf_status_t nvmft_lport_send_status(scsi_task_t *task, uint32_t ioflags);
void	nvmft_lport_task_free(scsi_task_t *task);
stmf_status_t nvmft_lport_abort(stmf_local_port_t *lport, int abort_cmd,
    void *arg, uint32_t flags);
void	nvmft_lport_task_poll(scsi_task_t *task);

/* nvmft_ns.c */
void	nvmft_internal_xfer_data(scsi_task_t *task, stmf_data_buf_t *dbuf);
void	nvmft_internal_send_status(scsi_task_t *task);
void	nvmft_internal_aborted(nvmft_internal_io_t *iio);
void	nvmft_internal_rele(nvmft_internal_io_t *iio);

/* nvmft_dbuf.c */
stmf_dbuf_store_t *nvmft_dbuf_store_create(void);
void	nvmft_dbuf_store_destroy(stmf_dbuf_store_t *ds);

#ifdef __cplusplus
}
#endif

#endif /* _NVMFT_IO_H */

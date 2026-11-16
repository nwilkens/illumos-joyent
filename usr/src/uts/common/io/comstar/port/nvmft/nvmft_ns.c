/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Namespaces.  NSID n is LUN n - 1 of the association's STMF session, so the
 * session's LUN map (stmfadm add-view) is the namespace list.  STMF does not
 * expose an LU's geometry, so Identify Namespace sends the LU a
 * READ CAPACITY(16) in an internal task, whose data stays in the kernel.
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
#include <sys/scsi/scsi.h>
#include <sys/scsi/generic/commands.h>
#include <sys/scsi/generic/status.h>
#include <sys/scsi/generic/sense.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include <sys/time.h>
#include <sys/stmf.h>
#include <sys/lpif.h>
#include <sys/stmf_ioctl.h>
#include <sys/portif.h>

#include "nvmft_io.h"

/*
 * How many times to reissue the internal READ CAPACITY when the LU reports a
 * pending UNIT ATTENTION.  Each command clears one UA; a handful covers the
 * power-on plus any reported-LUNs/capacity-changed UAs a new session may queue.
 */
#define	NVMFT_READ_CAPACITY_UA_RETRIES	5

/* Seconds that Identify Namespace waits for the LU. */
uint_t nvmft_read_capacity_timeout = 10;

/*
 * Whether a LU is mapped at nsid, and optionally its 16-byte identifier (the
 * LU GUID).
 */
static boolean_t
nvmft_ns_mapped(nvmft_controller_t *ctrlr, uint32_t nsid, uint8_t *guid)
{
	stmf_scsi_session_t *ss = ctrlr->ctrlr_session;
	uint16_t lun;

	if (ss == NULL || nsid == 0 || nsid - 1 > NVMFT_MAX_LUN)
		return (B_FALSE);
	lun = (uint16_t)(nsid - 1);
	return (stmf_session_next_lun(ss, &lun, guid, 16) == STMF_SUCCESS &&
	    lun == nsid - 1);
}

void
nvmft_internal_rele(nvmft_internal_io_t *iio)
{
	boolean_t last;

	mutex_enter(&iio->iio_lock);
	ASSERT3U(iio->iio_refs, >, 0);
	last = (--iio->iio_refs == 0);
	mutex_exit(&iio->iio_lock);
	if (!last)
		return;

	cv_destroy(&iio->iio_cv);
	mutex_destroy(&iio->iio_lock);
	mutex_destroy(&iio->iio_priv.ntp_lock);
	kmem_free(iio, sizeof (*iio));
}

static void
nvmft_internal_done(nvmft_internal_io_t *iio, uint8_t status, uint8_t key)
{
	mutex_enter(&iio->iio_lock);
	if (!iio->iio_done) {
		iio->iio_scsi_status = status;
		iio->iio_sense_key = key;
		iio->iio_done = B_TRUE;
		cv_broadcast(&iio->iio_cv);
	}
	mutex_exit(&iio->iio_lock);
}

/* The data of an internal task: keep it instead of sending it to a host. */
void
nvmft_internal_xfer_data(scsi_task_t *task, stmf_data_buf_t *dbuf)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	nvmft_internal_io_t *iio = priv->ntp_iio;

	if (dbuf->db_flags & DB_DIRECTION_TO_RPORT) {
		uint32_t off = dbuf->db_relative_offset;
		uint32_t resid = dbuf->db_data_size;
		uint16_t i;

		mutex_enter(&iio->iio_lock);
		for (i = 0; i < dbuf->db_sglist_length && resid != 0 &&
		    off < sizeof (iio->iio_buf); i++) {
			uint32_t todo = dbuf->db_sglist[i].seg_length;

			if (todo > resid)
				todo = resid;
			if (todo > sizeof (iio->iio_buf) - off)
				todo = sizeof (iio->iio_buf) - off;
			bcopy(dbuf->db_sglist[i].seg_addr, iio->iio_buf + off,
			    todo);
			off += todo;
			resid -= todo;
		}
		if (off > iio->iio_xfered)
			iio->iio_xfered = off;
		mutex_exit(&iio->iio_lock);
	}
	dbuf->db_xfer_status = STMF_SUCCESS;

	/* With status in the dbuf no lport_send_status() follows. */
	if (dbuf->db_flags & DB_SEND_STATUS_GOOD) {
		nvmft_internal_done(iio, STATUS_GOOD, KEY_NO_SENSE);
		nvmft_task_unlist(iio->iio_ctrlr, priv);
		stmf_data_xfer_done(task, dbuf, STMF_IOF_LPORT_DONE);
		return;
	}
	stmf_data_xfer_done(task, dbuf, 0);
}

void
nvmft_internal_send_status(scsi_task_t *task)
{
	nvmft_task_priv_t *priv = task->task_port_private;
	nvmft_internal_io_t *iio = priv->ntp_iio;
	uint8_t key = KEY_NO_SENSE;

	/* Fixed format sense data has its key in byte 2. */
	if (task->task_scsi_status == STATUS_CHECK &&
	    task->task_sense_data != NULL && task->task_sense_length > 2 &&
	    (task->task_sense_data[0] & 0x7e) == 0x70)
		key = task->task_sense_data[2] & 0x0f;
	nvmft_internal_done(iio, task->task_scsi_status, key);
	nvmft_task_unlist(iio->iio_ctrlr, priv);
	stmf_send_status_done(task, STMF_SUCCESS, STMF_IOF_LPORT_DONE);
}

/* STMF aborted the task, so no status follows. */
void
nvmft_internal_aborted(nvmft_internal_io_t *iio)
{
	nvmft_internal_done(iio, STATUS_TASK_ABORT, KEY_NO_SENSE);
}

/*
 * Send one READ CAPACITY(16) to the LU at lun_id and wait for it.  The task
 * can outlive the wait, and frees the nvmft_internal_io_t if it does.
 */
static int
nvmft_lu_read_capacity_once(nvmft_controller_t *ctrlr, uint32_t lun_id,
    uint64_t *nblocksp, uint32_t *blksizep, uint8_t *keyp)
{
	nvmft_internal_io_t *iio;
	scsi_task_t *task;
	clock_t deadline;
	uint8_t lun[8];
	int rc;

	(void) bzero(lun, sizeof (lun));
	lun[0] = (uint8_t)((lun_id >> 8) & 0x3f);
	lun[1] = (uint8_t)(lun_id & 0xff);

	/* Once the admin queue is stopped, nothing waits for the LU. */
	mutex_enter(&ctrlr->ctrlr_lock);
	if (ctrlr->ctrlr_admin_stopped || ctrlr->ctrlr_session == NULL) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return (ENXIO);
	}
	mutex_exit(&ctrlr->ctrlr_lock);

	task = stmf_task_alloc(ctrlr->ctrlr_np->np_lport, ctrlr->ctrlr_session,
	    lun, 16, 0);
	if (task == NULL)
		return (ENOMEM);

	iio = kmem_zalloc(sizeof (*iio), KM_SLEEP);
	mutex_init(&iio->iio_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&iio->iio_cv, NULL, CV_DRIVER, NULL);
	mutex_init(&iio->iio_priv.ntp_lock, NULL, MUTEX_DRIVER, NULL);
	iio->iio_priv.ntp_iio = iio;
	iio->iio_ctrlr = ctrlr;
	iio->iio_refs = 2;

	task->task_port_private = &iio->iio_priv;
	task->task_flags |= TF_READ_DATA | TF_ATTR_SIMPLE_QUEUE;
	(void) bzero(task->task_cdb, task->task_cdb_length);
	task->task_cdb[0] = SCMD_SVC_ACTION_IN_G4;
	task->task_cdb[1] = SSVC_ACTION_READ_CAPACITY_G4;
	task->task_cdb[13] = sizeof (iio->iio_buf);	/* allocation length */
	task->task_expected_xfer_length = sizeof (iio->iio_buf);
	task->task_cmd_xfer_length = sizeof (iio->iio_buf);
	task->task_max_nbufs = 1;
	task->task_max_xfer_len = sizeof (iio->iio_buf);
	task->task_1st_xfer_len = sizeof (iio->iio_buf);

	nvmft_task_listed(ctrlr, &iio->iio_priv, task);
	stmf_post_task(task, NULL);

	deadline = ddi_get_lbolt() +
	    drv_usectohz((clock_t)nvmft_read_capacity_timeout * MICROSEC);
	mutex_enter(&iio->iio_lock);
	while (!iio->iio_done) {
		if (cv_timedwait(&iio->iio_cv, &iio->iio_lock, deadline) == -1)
			break;
	}
	*keyp = iio->iio_sense_key;
	if (!iio->iio_done) {
		rc = ETIMEDOUT;
	} else if (iio->iio_scsi_status == STATUS_GOOD &&
	    iio->iio_xfered >= 12) {
		/* The last LBA is in bytes 0-7, the block length in 8-11. */
		*nblocksp = BE_IN64(&iio->iio_buf[0]) + 1;
		*blksizep = BE_IN32(&iio->iio_buf[8]);
		rc = 0;
	} else if (iio->iio_scsi_status == STATUS_CHECK) {
		rc = EAGAIN;
	} else {
		rc = EIO;
	}
	mutex_exit(&iio->iio_lock);
	nvmft_internal_rele(iio);
	return (rc);
}

/*
 * The block count and block size of the LU behind nsid.  This waits for the
 * LU, so it needs a thread that may block.
 *
 * The first command on a new STMF session draws a UNIT ATTENTION (29h/00h),
 * and each command reports one, so the command is sent again a bounded
 * number of times.
 */
static int
nvmft_lu_read_capacity(nvmft_controller_t *ctrlr, uint32_t nsid,
    uint64_t *nblocksp, uint32_t *blksizep)
{
	uint_t attempt;
	uint8_t key;
	int rc;

	for (attempt = 0; attempt <= NVMFT_READ_CAPACITY_UA_RETRIES;
	    attempt++) {
		rc = nvmft_lu_read_capacity_once(ctrlr, nsid - 1, nblocksp,
		    blksizep, &key);
		if (rc != EAGAIN || key != KEY_UNIT_ATTENTION)
			break;
	}
	return (rc == EAGAIN ? EIO : rc);
}

/*
 * Build the active-namespace list (Identify CNS 2): every mapped LUN whose NSID
 * exceeds start_nsid, in increasing order.
 */
void
nvmft_build_active_nslist(nvmft_controller_t *ctrlr, uint32_t start_nsid,
    nvme_identify_nsid_list_t *nslist)
{
	stmf_scsi_session_t *ss = ctrlr->ctrlr_session;
	uint_t nitems, count = 0;
	uint16_t lun;

	nitems = sizeof (nslist->nl_nsid) / sizeof (nslist->nl_nsid[0]);
	if (ss == NULL || start_nsid > NVMFT_MAX_LUN)
		return;

	/* The first NSID above start_nsid is the LUN start_nsid. */
	lun = (uint16_t)start_nsid;
	while (count < nitems &&
	    stmf_session_next_lun(ss, &lun, NULL, 0) == STMF_SUCCESS) {
		nslist->nl_nsid[count++] = LE_32((uint32_t)lun + 1);
		if (lun++ == NVMFT_MAX_LUN)
			break;
	}
}

/*
 * Build Identify Namespace (CNS 0) for nsid from the LU geometry.  Returns
 * B_FALSE if nsid is not a mapped/active namespace.
 */
boolean_t
nvmft_build_identify_nsid(nvmft_controller_t *ctrlr, uint32_t nsid,
    nvme_identify_nsid_t *nsdata)
{
	uint64_t nblocks;
	uint32_t blksize;
	uint8_t guid[16];

	if (!nvmft_ns_mapped(ctrlr, nsid, guid))
		return (B_FALSE);
	if (nvmft_lu_read_capacity(ctrlr, nsid, &nblocks, &blksize) != 0)
		return (B_FALSE);
	if (blksize == 0 || (blksize & (blksize - 1)) != 0)
		return (B_FALSE);		/* expect a power-of-two LBA size */

	(void) bzero(nsdata, sizeof (*nsdata));
	nsdata->id_nsize = LE_64(nblocks);
	nsdata->id_ncap = LE_64(nblocks);
	nsdata->id_nuse = LE_64(nblocks);
	nsdata->id_nlbaf = 0;			/* a single LBA format */
	nsdata->id_flbas.lba_format = 0;	/* format 0 in use */
	nsdata->id_lbaf[0].lbaf_lbads = (uint8_t)(highbit(blksize) - 1);
	/* The illumos host builds its devid from the NGUID. */
	(void) memcpy(nsdata->id_nguid, guid, sizeof (guid));
	return (B_TRUE);
}

/*
 * Build the Namespace Identification Descriptor list (CNS 3) for nsid: a single
 * NGUID descriptor from the LU identity, terminated by a zero-length
 * descriptor.  buf is the (zeroed) 4096-byte Identify payload.
 */
boolean_t
nvmft_build_nsid_desc(nvmft_controller_t *ctrlr, uint32_t nsid, uint8_t *buf,
    size_t buflen)
{
	uint8_t guid[16];

	/* NIDT(1) + NIDL(1) + rsvd(2) + NGUID(16), then a zero terminator. */
	if (buflen < 4 + 16 + 1)
		return (B_FALSE);
	if (!nvmft_ns_mapped(ctrlr, nsid, guid))
		return (B_FALSE);

	buf[0] = 0x02;		/* NIDT = NGUID */
	buf[1] = 16;		/* NIDL */
	(void) memcpy(&buf[4], guid, 16);
	/* buf[20] (next NIDT) stays 0: end-of-list terminator. */
	return (B_TRUE);
}

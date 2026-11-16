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
 * Namespaces: NSID n is LUN n - 1 of the association's STMF session.
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
#include "../../stmf/stmf_impl.h"
#include "../../stmf/lun_map.h"

#include "nvmft_io.h"

/*
 * Look up the controller session's LU map entry for nsid (LUN nsid-1).  Returns
 * B_TRUE if a LU is mapped, optionally copying its 16-byte identity (the LU
 * GUID) for the namespace identification descriptor.  Read under iss_lockp.
 */
static boolean_t
nvmft_ns_mapped(nvmft_controller_t *ctrlr, uint32_t nsid, uint8_t *guid)
{
	stmf_scsi_session_t *ss = ctrlr->ctrlr_session;
	stmf_i_scsi_session_t *iss;
	stmf_lun_map_ent_t *ent;
	uint32_t lun_id;
	boolean_t mapped = B_FALSE;

	if (ss == NULL || nsid == 0)
		return (B_FALSE);
	lun_id = nsid - 1;
	if (lun_id > 0x3fff)		/* STMF single-level LUN limit */
		return (B_FALSE);

	iss = (stmf_i_scsi_session_t *)ss->ss_stmf_private;
	rw_enter(iss->iss_lockp, RW_READER);
	/*
	 * iss_sm is NULL during the session create/teardown window (stmf clears
	 * it under iss_lockp); stmf_get_ent_from_map() would deref it.  Guard as
	 * nvmft_build_active_nslist() does.
	 */
	if (iss->iss_sm != NULL) {
		ent = (stmf_lun_map_ent_t *)stmf_get_ent_from_map(iss->iss_sm,
		    (uint16_t)lun_id);
		if (ent != NULL && ent->ent_lu != NULL) {
			mapped = B_TRUE;
			if (guid != NULL) {
				scsi_devid_desc_t *id = ent->ent_lu->lu_id;

				(void) bzero(guid, 16);
				if (id != NULL)
					(void) memcpy(guid, id->ident,
					    MIN(id->ident_length, 16));
			}
		}
	}
	rw_exit(iss->iss_lockp);
	return (mapped);
}

/*
 * How many times to reissue the internal READ CAPACITY when the LU reports a
 * pending UNIT ATTENTION.  Each command clears one UA; a handful covers the
 * power-on plus any reported-LUNs/capacity-changed UAs a new session may queue.
 */
#define	NVMFT_READ_CAPACITY_UA_RETRIES	5

/*
 * Issue an internal READ CAPACITY(16) to the LU backing nsid and return its
 * block count and block size.  Blocks until the LU completes the command on an
 * STMF worker thread (independent of this thread), so it must run in thread
 * context (it is called from the admin-command handler).
 */
static int
nvmft_lu_read_capacity(nvmft_controller_t *ctrlr, uint32_t nsid,
    uint64_t *nblocksp, uint32_t *blksizep)
{
	scsi_task_t *task;
	nvmft_task_priv_t *priv;
	nvmft_internal_io_t iio;
	uint8_t capbuf[32];
	uint8_t lun[8];
	uint32_t lun_id = nsid - 1;
	uint_t attempt;
	int rc;

	if (ctrlr->ctrlr_session == NULL)
		return (ENXIO);

	(void) bzero(lun, sizeof (lun));
	lun[0] = (uint8_t)((lun_id >> 8) & 0x3f);
	lun[1] = (uint8_t)(lun_id & 0xff);

	/*
	 * The first command on a freshly registered STMF session draws a
	 * UNIT ATTENTION (29h/00h, power-on/reset occurred), failing with
	 * CHECK CONDITION and no data.  A SCSI initiator clears a pending UA by
	 * reissuing -- each command reports one UA -- so retry a bounded number
	 * of times.  (Without this the host gets a zero-size namespace on
	 * connect, since nvmf_host issues IDENTIFY only once.)
	 */
	for (attempt = 0; ; attempt++) {
		task = stmf_task_alloc(ctrlr->ctrlr_np->np_lport,
		    ctrlr->ctrlr_session, lun, 16, 0);
		if (task == NULL)
			return (ENOMEM);

		(void) bzero(&iio, sizeof (iio));
		mutex_init(&iio.iio_lock, NULL, MUTEX_DRIVER, NULL);
		cv_init(&iio.iio_cv, NULL, CV_DRIVER, NULL);
		iio.iio_buf = capbuf;
		iio.iio_buflen = sizeof (capbuf);

		priv = kmem_zalloc(sizeof (*priv), KM_SLEEP);
		priv->ntp_iio = &iio;
		task->task_port_private = priv;
		task->task_flags |= TF_READ_DATA | TF_ATTR_SIMPLE_QUEUE;

		(void) bzero(task->task_cdb, task->task_cdb_length);
		task->task_cdb[0] = SCMD_SVC_ACTION_IN_G4;
		task->task_cdb[1] = SSVC_ACTION_READ_CAPACITY_G4;
		/* allocation length (bytes 10-13) */
		task->task_cdb[13] = sizeof (capbuf);
		task->task_expected_xfer_length = sizeof (capbuf);
		task->task_cmd_xfer_length = sizeof (capbuf);
		task->task_max_nbufs = 1;
		task->task_max_xfer_len = sizeof (capbuf);
		task->task_1st_xfer_len = sizeof (capbuf);

		stmf_post_task(task, NULL);

		mutex_enter(&iio.iio_lock);
		while (!iio.iio_done)
			cv_wait(&iio.iio_cv, &iio.iio_lock);
		mutex_exit(&iio.iio_lock);

		/*
		 * READ CAPACITY(16): max LBA at bytes 0-7, block length at 8-11.
		 */
		if (iio.iio_scsi_status == STATUS_GOOD && iio.iio_xfered >= 12) {
			*nblocksp = BE_IN64(&capbuf[0]) + 1;
			*blksizep = BE_IN32(&capbuf[8]);
			rc = 0;
		} else if (iio.iio_scsi_status == STATUS_CHECK &&
		    iio.iio_sense_key == KEY_UNIT_ATTENTION &&
		    attempt < NVMFT_READ_CAPACITY_UA_RETRIES) {
			rc = EAGAIN;	/* UA cleared by this command; retry */
		} else {
			rc = EIO;
		}

		mutex_destroy(&iio.iio_lock);
		cv_destroy(&iio.iio_cv);

		if (rc != EAGAIN)
			return (rc);
	}
}

/*
 * Build the active-namespace list (Identify CNS 2): every mapped LUN whose NSID
 * exceeds start_nsid, in increasing order.  (Replaces the np_active_ns scan,
 * which was never populated.)
 */
void
nvmft_build_active_nslist(nvmft_controller_t *ctrlr, uint32_t start_nsid,
    nvme_identify_nsid_list_t *nslist)
{
	stmf_scsi_session_t *ss = ctrlr->ctrlr_session;
	stmf_i_scsi_session_t *iss;
	uint_t nitems, count = 0;
	uint16_t i;

	nitems = sizeof (nslist->nl_nsid) / sizeof (nslist->nl_nsid[0]);
	if (ss == NULL)
		return;
	iss = (stmf_i_scsi_session_t *)ss->ss_stmf_private;

	rw_enter(iss->iss_lockp, RW_READER);
	if (iss->iss_sm != NULL) {
		for (i = 0; i < iss->iss_sm->lm_nentries && count < nitems; i++) {
			stmf_lun_map_ent_t *ent =
			    (stmf_lun_map_ent_t *)iss->iss_sm->lm_plus[i];
			uint32_t nsid = (uint32_t)i + 1;

			if (ent == NULL || ent->ent_lu == NULL)
				continue;
			if (nsid <= start_nsid)
				continue;
			nslist->nl_nsid[count++] = LE_32(nsid);
		}
	}
	rw_exit(iss->iss_lockp);
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
	/*
	 * Expose the LU GUID as the NVMe 1.2+ namespace globally-unique
	 * identifier (NGUID).  Real controllers populate this in Identify
	 * Namespace as well as in the Namespace Identification Descriptor (CNS
	 * 3); the illumos host builds its devid from the Identify Namespace
	 * NGUID, so without this the blkdev attach fails (no devid).
	 */
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

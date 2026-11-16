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
 * Provenance: ported to illumos from FreeBSD
 * sys/dev/nvmf/controller/nvmft_controller.c.
 *
 * Original: Copyright (c) 2023-2024 Chelsio Communications, Inc.
 *           Written by: John Baldwin <jhb@FreeBSD.org>
 *           SPDX-License-Identifier: BSD-2-Clause
 *
 * Admin commands, the I/O command switch and asynchronous events.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#include <sys/ksynch.h>
#include <sys/kmem.h>
#include <sys/list.h>
#include <sys/taskq.h>
#include <sys/id_space.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/varargs.h>
#include <sys/sdt.h>

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>
#include <sys/nvme/nvmf_transport.h>

#include <sys/time.h>		/* hrtime_t (sys/stmf.h needs it) */
#include <sys/stmf.h>

#include "nvmft_var.h"

/*
 * NVM Verify command opcode (NVMe NVM command set).  io/nvme/nvme_reg.h does not
 * yet define this opcode; the spec fixes it at 0x0c.  (FreeBSD: NVME_OPC_VERIFY.)
 */
#define	NVMFT_OPC_NVM_VERIFY	0x0c

/*
 * Asynchronous Event Configuration enable bit for the Namespace Attribute
 * Notices event (Set Features FID 0x0b, cdw11 bit 8).  illumos's headers do not
 * define this AER-config mask.  (FreeBSD: NVME_ASYNC_EVENT_NS_ATTRIBUTE.)
 */
#define	NVMFT_AER_NS_ATTRIBUTE	(1u << 8)

/* An admin command waiting for ns_admin_taskq, kept in its capsule. */
typedef struct nvmft_admin_cmd {
	list_node_t		nac_link;
	struct nvmf_capsule	*nac_nc;
} nvmft_admin_cmd_t;

CTASSERT(sizeof (nvmft_admin_cmd_t) <=
    sizeof (((struct nvmf_capsule *)0)->nc_consumer));

void
nvmft_admin_init(nvmft_controller_t *ctrlr)
{
	list_create(&ctrlr->ctrlr_admin_cmds, sizeof (nvmft_admin_cmd_t),
	    offsetof(nvmft_admin_cmd_t, nac_link));
}

void
nvmft_admin_fini(nvmft_controller_t *ctrlr)
{
	list_destroy(&ctrlr->ctrlr_admin_cmds);
}

static void
nvmft_admin_task(void *arg)
{
	nvmft_controller_t *ctrlr = arg;
	nvmft_admin_cmd_t *ac;

	mutex_enter(&ctrlr->ctrlr_lock);
	while ((ac = list_remove_head(&ctrlr->ctrlr_admin_cmds)) != NULL) {
		mutex_exit(&ctrlr->ctrlr_lock);
		nvmft_handle_admin_command(ctrlr, ac->nac_nc);
		mutex_enter(&ctrlr->ctrlr_lock);
	}
	ctrlr->ctrlr_admin_running = B_FALSE;
	cv_broadcast(&ctrlr->ctrlr_admin_cv);
	mutex_exit(&ctrlr->ctrlr_lock);
}

/* Run an admin command on ns_admin_taskq; this does not block. */
void
nvmft_queue_admin_command(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc)
{
	nvmft_admin_cmd_t *ac = NVMF_CAPSULE_CONSUMER(nc);

	ac->nac_nc = nc;
	mutex_enter(&ctrlr->ctrlr_lock);
	if (ctrlr->ctrlr_admin_stopped) {
		mutex_exit(&ctrlr->ctrlr_lock);
		nvmf_free_capsule(nc);
		return;
	}
	list_insert_tail(&ctrlr->ctrlr_admin_cmds, ac);
	if (!ctrlr->ctrlr_admin_running) {
		ctrlr->ctrlr_admin_running = B_TRUE;
		taskq_dispatch_ent(nvmft_global->ns_admin_taskq,
		    nvmft_admin_task, ctrlr, 0, &ctrlr->ctrlr_admin_task);
	}
	mutex_exit(&ctrlr->ctrlr_lock);
}

/* Discard queued admin commands and wait out the one running. */
void
nvmft_admin_stop(nvmft_controller_t *ctrlr)
{
	nvmft_admin_cmd_t *ac;
	list_t drain;

	list_create(&drain, sizeof (nvmft_admin_cmd_t),
	    offsetof(nvmft_admin_cmd_t, nac_link));
	mutex_enter(&ctrlr->ctrlr_lock);
	ctrlr->ctrlr_admin_stopped = B_TRUE;
	list_move_tail(&drain, &ctrlr->ctrlr_admin_cmds);
	while (ctrlr->ctrlr_admin_running)
		cv_wait(&ctrlr->ctrlr_admin_cv, &ctrlr->ctrlr_lock);
	mutex_exit(&ctrlr->ctrlr_lock);

	while ((ac = list_remove_head(&drain)) != NULL)
		nvmf_free_capsule(ac->nac_nc);
	list_destroy(&drain);
}

/*
 * Allocate an mblk of len bytes with the leading region filled from src (todo
 * bytes) and the remainder zero-filled.  Replaces the FreeBSD m_getml /
 * m_copyback / m_zero pattern used by the log-page builders.  FreeBSD allocated
 * with M_WAITOK, so block (allocb_wait, STR_NOSIG) rather than fail; NULL is
 * only returned if the request is too large to ever satisfy, which the callers
 * turn into an Internal Error completion.
 */
static mblk_t *
nvmft_alloc_data(const void *src, size_t todo, size_t len)
{
	mblk_t *mp;
	int error = 0;

	/*
	 * STR_NOSIG forces a KM_SLEEP-backed allocation, so this does not return
	 * NULL in practice; callers still treat a NULL result as an Internal
	 * Error completion in case that ever changes.
	 */
	mp = allocb_wait(len, BPRI_MED, STR_NOSIG, &error);
	if (mp == NULL)
		return (NULL);
	if (todo > 0)
		(void) memcpy(mp->b_wptr, src, todo);
	if (len > todo)
		(void) bzero(mp->b_wptr + todo, len - todo);
	mp->b_wptr += len;
	return (mp);
}

/*
 * Allocate a fully zero-filled mblk of len bytes.  Used where the source must
 * be copied in under a lock that cannot be held across the blocking
 * allocb_wait(): the caller allocates here first, then memcpy's the leading
 * todo bytes into mp->b_rptr while holding the lock (FreeBSD allocates the mbuf
 * before mtx_lock for exactly this reason; see the NSCHANGE log page).
 */
static mblk_t *
nvmft_alloc_zeroed(size_t len)
{
	return (nvmft_alloc_data(NULL, 0, len));
}

/*
 * The length of a Get Log Page.  NUMD is the host's, so it is bounded before
 * it sizes an allocation, and it must match the host's SGL, as Linux nvmet
 * requires: a longer SGL would leave the transfer short.
 */
static uint_t
nvmft_log_page_len(uint32_t numd, size_t data_len, size_t *lenp)
{
	size_t len = ((size_t)numd + 1) * 4;

	if (len > NVMFT_MAX_LOGPAGE_LEN)
		return (NVME_CQE_SC_GEN_INV_FLD);
	if (len != data_len)
		return (NVME_CQE_SC_GEN_INV_DSGL_LEN);
	*lenp = len;
	return (NVME_CQE_SC_GEN_SUCCESS);
}

static void
handle_get_log_page(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd)
{
	mblk_t *mp;
	uint64_t offset;
	uint32_t numd;
	size_t len, todo;
	uint_t status;
	uint8_t lid;
	boolean_t rae;

	lid = LE_32(cmd->sqe_cdw10) & 0xff;
	rae = (LE_32(cmd->sqe_cdw10) & (1U << 15)) != 0;
	numd = (LE_32(cmd->sqe_cdw10) >> 16) | (LE_32(cmd->sqe_cdw11) << 16);
	offset = LE_32(cmd->sqe_cdw12) |
	    ((uint64_t)LE_32(cmd->sqe_cdw13) << 32);

	/*
	 * The Get Log Page offset (LPOL/LPOU) must be DWORD aligned.  FreeBSD
	 * uses "offset % 3" here, which is a long-standing upstream bug (it
	 * accepts a misaligned offset of 6 and rejects an aligned offset of 8);
	 * the spec requires a multiple of 4.  DIVERGENCE: fixed here as % 4;
	 * worth reporting upstream so both trees converge.
	 */
	if (offset % 4 != 0) {
		status = NVME_CQE_SC_GEN_INV_FLD;
		goto done;
	}

	status = nvmft_log_page_len(numd, nvmf_capsule_data_len(nc), &len);
	if (status != NVME_CQE_SC_GEN_SUCCESS)
		goto done;

	switch (lid) {
	case NVME_LOGPAGE_ERROR:
		mp = nvmft_alloc_zeroed(len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}
		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	case NVME_LOGPAGE_HEALTH: {
		nvme_health_log_t hip;

		if (offset >= sizeof (hip)) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			goto done;
		}
		todo = sizeof (hip) - offset;
		if (todo > len)
			todo = len;

		mutex_enter(&ctrlr->ctrlr_lock);
		hip = ctrlr->ctrlr_hip;
		/*
		 * FreeBSD reports Controller Busy Time in minutes and Power On
		 * Hours in hours.  ctrlr_busy_total and ctrlr_create_time are
		 * hrtime_t nanoseconds; the low 64 bits of the 128-bit fields are
		 * sufficient.
		 */
		hip.hl_ctrl_busy.lo =
		    LE_64((ctrlr->ctrlr_busy_total / NANOSEC) / 60);
		hip.hl_power_on_hours.lo = LE_64(((gethrtime() -
		    ctrlr->ctrlr_create_time) / NANOSEC) / 3600);
		mutex_exit(&ctrlr->ctrlr_lock);

		mp = nvmft_alloc_data((char *)&hip + offset, todo, len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}
		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	case NVME_LOGPAGE_FWSLOT:
		if (offset >= sizeof (ctrlr->ctrlr_np->np_fp)) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			goto done;
		}
		todo = sizeof (ctrlr->ctrlr_np->np_fp) - offset;
		if (todo > len)
			todo = len;
		mp = nvmft_alloc_data((char *)&ctrlr->ctrlr_np->np_fp + offset,
		    todo, len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}
		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	case NVME_LOGPAGE_NSCHANGE:
		if (offset >= sizeof (*ctrlr->ctrlr_changed_ns)) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			goto done;
		}
		todo = sizeof (*ctrlr->ctrlr_changed_ns) - offset;
		if (todo > len)
			todo = len;

		/*
		 * Allocate the (zero-filled) reply mblk before taking
		 * ctrlr_lock: allocb_wait() can block and sleeping with a driver
		 * mutex held is illegal.  Under the lock we only do the
		 * non-blocking copy/bzero/flag update (FreeBSD likewise allocates
		 * the mbuf before mtx_lock here).
		 */
		mp = nvmft_alloc_zeroed(len);
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			goto done;
		}

		mutex_enter(&ctrlr->ctrlr_lock);
		(void) memcpy(mp->b_rptr,
		    (char *)ctrlr->ctrlr_changed_ns + offset, todo);
		if (offset == 0 && len == sizeof (*ctrlr->ctrlr_changed_ns))
			(void) bzero(ctrlr->ctrlr_changed_ns,
			    sizeof (*ctrlr->ctrlr_changed_ns));
		if (!rae)
			ctrlr->ctrlr_changed_ns_reported = B_FALSE;
		mutex_exit(&ctrlr->ctrlr_lock);

		status = nvmf_send_controller_data(nc, 0, mp, len);
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	/*
	 * PORT-TODO (NVMEOF.md 9.4): add case NVME_LOGPAGE_ANA to serve the ANA
	 * log page from nvmft_ana_build_log_page().
	 */
	default:
		DTRACE_PROBE2(nvmft__log__page__unsupported,
		    nvmft_controller_t *, ctrlr, uint8_t, lid);
		status = NVME_CQE_SC_GEN_INV_FLD;
		break;
	}

done:
	if (status == NVMF_SUCCESS_SENT)
		nvmft_command_completed(ctrlr->ctrlr_admin, nc);
	else
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc, status);
	nvmf_free_capsule(nc);
}

static void
handle_identify_command(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd)
{
	mblk_t *mp;
	size_t data_len;
	uint_t status;
	uint8_t cns;

	cns = LE_32(cmd->sqe_cdw10) & 0xFF;
	data_len = nvmf_capsule_data_len(nc);
	if (data_len != sizeof (ctrlr->ctrlr_cdata)) {
		DTRACE_PROBE3(nvmft__identify__length, nvmft_controller_t *,
		    ctrlr, uint8_t, cns, size_t, data_len);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		return;
	}

	switch (cns) {
	case 0: {	/* Identify Namespace. */
		nvme_identify_nsid_t *nsdata;
		uint32_t nsid = LE_32(cmd->sqe_nsid);

		/*
		 * Synthesize Identify Namespace from the STMF LU geometry
		 * (nvmft_stmf.c).  An inactive/unallocated NSID returns a
		 * zero-filled structure (the spec uses that, not an error, so
		 * the host can probe namespace ids).
		 */
		nsdata = kmem_zalloc(sizeof (*nsdata), KM_SLEEP);
		(void) nvmft_build_identify_nsid(ctrlr, nsid, nsdata);
		mp = nvmft_alloc_data(nsdata, sizeof (*nsdata),
		    sizeof (ctrlr->ctrlr_cdata));
		kmem_free(nsdata, sizeof (*nsdata));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp,
		    sizeof (ctrlr->ctrlr_cdata));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	case 3: {	/* Namespace Identification Descriptor list. */
		uint8_t *desc;
		uint32_t nsid = LE_32(cmd->sqe_nsid);

		desc = kmem_zalloc(sizeof (ctrlr->ctrlr_cdata), KM_SLEEP);
		(void) nvmft_build_nsid_desc(ctrlr, nsid, desc,
		    sizeof (ctrlr->ctrlr_cdata));
		mp = nvmft_alloc_data(desc, sizeof (ctrlr->ctrlr_cdata),
		    sizeof (ctrlr->ctrlr_cdata));
		kmem_free(desc, sizeof (ctrlr->ctrlr_cdata));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp,
		    sizeof (ctrlr->ctrlr_cdata));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	case 1:	/* Controller data. */
		mp = nvmft_alloc_data(&ctrlr->ctrlr_cdata,
		    sizeof (ctrlr->ctrlr_cdata), sizeof (ctrlr->ctrlr_cdata));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp,
		    sizeof (ctrlr->ctrlr_cdata));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	case 2: {	/* Active namespace list. */
		nvme_identify_nsid_list_t *nslist;
		uint32_t nsid;

		nsid = LE_32(cmd->sqe_nsid);
		if (nsid >= 0xfffffffe) {
			status = NVME_CQE_SC_GEN_INV_FLD;
			break;
		}

		nslist = kmem_zalloc(sizeof (*nslist), KM_SLEEP);
		nvmft_build_active_nslist(ctrlr, nsid, nslist);
		mp = nvmft_alloc_data(nslist, sizeof (*nslist),
		    sizeof (*nslist));
		kmem_free(nslist, sizeof (*nslist));
		if (mp == NULL) {
			status = NVME_CQE_SC_GEN_INTERNAL_ERR;
			break;
		}
		status = nvmf_send_controller_data(nc, 0, mp, sizeof (*nslist));
		ASSERT3U(status, !=, NVMF_MORE);
		break;
	}
	default:
		DTRACE_PROBE2(nvmft__identify__unsupported,
		    nvmft_controller_t *, ctrlr, uint8_t, cns);
		status = NVME_CQE_SC_GEN_INV_FLD;
		break;
	}

	if (status == NVMF_SUCCESS_SENT)
		nvmft_command_completed(ctrlr->ctrlr_admin, nc);
	else
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc, status);
	nvmf_free_capsule(nc);
}

static void
handle_set_features(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvme_sqe_t *cmd)
{
	nvme_cqe_t cqe;
	uint8_t fid;

	fid = LE_32(cmd->sqe_cdw10) & 0xff;
	switch (fid) {
	case NVME_FEAT_NQUEUES: {
		uint32_t num_queues;
		nvmft_io_qpair_t *io_qpairs;

		num_queues = LE_32(cmd->sqe_cdw11) & 0xffff;

		/* 5.12.1.7: 65535 is invalid. */
		if (num_queues == 65535)
			goto error;
		/* Fabrics requires the same number of SQs and CQs. */
		if ((LE_32(cmd->sqe_cdw11) >> 16) != num_queues)
			goto error;

		num_queues++;	/* convert to 1's based */

		io_qpairs = kmem_zalloc(num_queues * sizeof (*io_qpairs),
		    KM_SLEEP);

		mutex_enter(&ctrlr->ctrlr_lock);
		if (ctrlr->ctrlr_num_io_queues != 0) {
			mutex_exit(&ctrlr->ctrlr_lock);
			kmem_free(io_qpairs, num_queues * sizeof (*io_qpairs));
			(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
			    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
			nvmf_free_capsule(nc);
			return;
		}
		ctrlr->ctrlr_num_io_queues = num_queues;
		ctrlr->ctrlr_io_qpairs = io_qpairs;
		mutex_exit(&ctrlr->ctrlr_lock);

		nvmft_init_cqe(&cqe, nc, 0);
		cqe.cqe_dw0 = cmd->sqe_cdw11;
		(void) nvmft_send_response(ctrlr->ctrlr_admin, &cqe);
		nvmf_free_capsule(nc);
		return;
	}
	case NVME_FEAT_ASYNC_EVENT: {
		uint32_t aer_mask = LE_32(cmd->sqe_cdw11);

		if ((aer_mask & 0xffffc000) != 0)
			goto error;
		mutex_enter(&ctrlr->ctrlr_lock);
		ctrlr->ctrlr_aer_mask = aer_mask;
		mutex_exit(&ctrlr->ctrlr_lock);
		(void) nvmft_send_success(ctrlr->ctrlr_admin, nc);
		nvmf_free_capsule(nc);
		return;
	}
	default:
		DTRACE_PROBE2(nvmft__feature__unsupported,
		    nvmft_controller_t *, ctrlr, uint8_t, fid);
		goto error;
	}

error:
	(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
	    NVME_CQE_SC_GEN_INV_FLD);
	nvmf_free_capsule(nc);
}

static boolean_t
update_cc(nvmft_controller_t *ctrlr, uint32_t new_cc, boolean_t *need_shutdown)
{
	nvmft_port_t *np = ctrlr->ctrlr_np;
	nvme_reg_cc_t cc;
	nvme_reg_csts_t csts;
	uint32_t changes;

	*need_shutdown = B_FALSE;

	mutex_enter(&ctrlr->ctrlr_lock);

	if (ctrlr->ctrlr_shutdown) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return (B_FALSE);
	}
	if (!_nvmf_validate_cc(np->np_max_io_qsize, np->np_cap, ctrlr->ctrlr_cc,
	    new_cc)) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return (B_FALSE);
	}

	changes = ctrlr->ctrlr_cc ^ new_cc;
	ctrlr->ctrlr_cc = new_cc;
	cc.r = new_cc;
	csts.r = ctrlr->ctrlr_csts;

	/* Handle shutdown requests (CC.SHN). */
	if (NVMFT_CC_SHN(changes) != 0 && cc.b.cc_shn != 0) {
		csts.b.csts_shst = NVME_CSTS_SHN_OCCURING;
		cc.b.cc_en = 0;
		ctrlr->ctrlr_cc = cc.r;
		ctrlr->ctrlr_shutdown = B_TRUE;
		*need_shutdown = B_TRUE;
		(void) nvmft_printf(ctrlr, "shutdown requested\n");
	}

	/* Handle enable/disable (CC.EN). */
	if ((changes & 0x1u) != 0) {
		if (cc.b.cc_en == 0) {
			(void) nvmft_printf(ctrlr, "reset requested\n");
			ctrlr->ctrlr_shutdown = B_TRUE;
			*need_shutdown = B_TRUE;
		} else {
			csts.b.csts_rdy = 1;
		}
	}
	ctrlr->ctrlr_csts = csts.r;
	if (*need_shutdown)
		ctrlr->ctrlr_shutdown_pending = B_TRUE;
	mutex_exit(&ctrlr->ctrlr_lock);

	return (B_TRUE);
}

static void
handle_property_get(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvmf_fabric_prop_get_cmd_t *pget)
{
	nvmf_fabric_prop_get_rsp_t rsp;

	nvmft_init_cqe(&rsp, nc, 0);

	switch (LE_32(pget->nfpg_ofst)) {
	case NVMF_PROP_CAP:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_8)
			goto error;
		rsp.nfpr_value.u64 = LE_64(ctrlr->ctrlr_np->np_cap);
		break;
	case NVMF_PROP_VS:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = ctrlr->ctrlr_cdata.id_ver;
		break;
	case NVMF_PROP_CC:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = LE_32(ctrlr->ctrlr_cc);
		break;
	case NVMF_PROP_CSTS:
		if (pget->nfpg_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		rsp.nfpr_value.u32.low = LE_32(ctrlr->ctrlr_csts);
		break;
	default:
		goto error;
	}

	(void) nvmft_send_response(ctrlr->ctrlr_admin, &rsp);
	return;
error:
	(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
	    NVME_CQE_SC_GEN_INV_FLD);
}

static void
handle_property_set(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvmf_fabric_prop_set_cmd_t *pset)
{
	boolean_t need_shutdown = B_FALSE;

	switch (LE_32(pset->nfps_ofst)) {
	case NVMF_PROP_CC:
		if (pset->nfps_attrib.size != NVMF_PROP_SIZE_4)
			goto error;
		if (!update_cc(ctrlr, LE_32(pset->nfps_value.u32.low),
		    &need_shutdown))
			goto error;
		break;
	default:
		goto error;
	}

	(void) nvmft_send_success(ctrlr->ctrlr_admin, nc);
	if (need_shutdown)
		nvmft_controller_start_shutdown(ctrlr);
	return;
error:
	(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
	    NVME_CQE_SC_GEN_INV_FLD);
}

static void
handle_admin_fabrics_command(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc,
    const nvmf_fabric_cmd_t *fc)
{
	switch (fc->nfc_fctype) {
	case NVMF_FCTYPE_PROPERTY_GET:
		handle_property_get(ctrlr, nc,
		    (const nvmf_fabric_prop_get_cmd_t *)fc);
		break;
	case NVMF_FCTYPE_PROPERTY_SET:
		handle_property_set(ctrlr, nc,
		    (const nvmf_fabric_prop_set_cmd_t *)fc);
		break;
	case NVMF_FCTYPE_CONNECT:
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
		break;
	case NVMF_FCTYPE_DISCONNECT:
		(void) nvmft_send_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SCT_SPECIFIC, NVMF_FABRIC_SC_INVALID_QUEUE_TYPE);
		break;
	default:
		DTRACE_PROBE2(nvmft__fabrics__unsupported,
		    nvmft_controller_t *, ctrlr, uint8_t, fc->nfc_fctype);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_INV_OPC);
		break;
	}
	nvmf_free_capsule(nc);
}

void
nvmft_handle_admin_command(nvmft_controller_t *ctrlr, struct nvmf_capsule *nc)
{
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);

	/* Only permit Fabrics commands while the controller is disabled. */
	if (NVMFT_CC_EN(ctrlr->ctrlr_cc) == 0 &&
	    cmd->sqe_opc != NVMFT_OPC_FABRICS) {
		DTRACE_PROBE2(nvmft__admin__disabled, nvmft_controller_t *,
		    ctrlr, uint8_t, cmd->sqe_opc);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_CMD_SEQ_ERR);
		nvmf_free_capsule(nc);
		return;
	}

	(void) atomic_swap_uint(&ctrlr->ctrlr_ka_active_traffic, 1);

	switch (cmd->sqe_opc) {
	case NVME_OPC_GET_LOG_PAGE:
		handle_get_log_page(ctrlr, nc, cmd);
		break;
	case NVME_OPC_IDENTIFY:
		handle_identify_command(ctrlr, nc, cmd);
		break;
	case NVME_OPC_SET_FEATURES:
		handle_set_features(ctrlr, nc, cmd);
		break;
	case NVME_OPC_ASYNC_EVENT:
		mutex_enter(&ctrlr->ctrlr_lock);
		if (ctrlr->ctrlr_aer_pending > ctrlr->ctrlr_cdata.id_aerl) {
			mutex_exit(&ctrlr->ctrlr_lock);
			(void) nvmft_send_error(ctrlr->ctrlr_admin, nc,
			    NVME_CQE_SCT_SPECIFIC,
			    NVME_CQE_SC_SPC_ASYNC_EVREQ_EXC);
		} else {
			/* NB: store the CID without byte-swapping. */
			ctrlr->ctrlr_aer_cids[ctrlr->ctrlr_aer_pidx] =
			    cmd->sqe_cid;
			ctrlr->ctrlr_aer_pending++;
			ctrlr->ctrlr_aer_pidx =
			    (ctrlr->ctrlr_aer_pidx + 1) % NVMFT_NUM_AER;
			mutex_exit(&ctrlr->ctrlr_lock);
			nvmf_capsule_defer_response(nc);
		}
		nvmf_free_capsule(nc);
		break;
	case NVME_OPC_KEEP_ALIVE:
		(void) nvmft_send_success(ctrlr->ctrlr_admin, nc);
		nvmf_free_capsule(nc);
		break;
	case NVMFT_OPC_FABRICS:
		handle_admin_fabrics_command(ctrlr, nc,
		    (const nvmf_fabric_cmd_t *)cmd);
		break;
	default:
		DTRACE_PROBE2(nvmft__admin__unsupported, nvmft_controller_t *,
		    ctrlr, uint8_t, cmd->sqe_opc);
		(void) nvmft_send_generic_error(ctrlr->ctrlr_admin, nc,
		    NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		break;
	}
}

void
nvmft_handle_io_command(struct nvmft_qpair *qp, uint16_t qid,
    struct nvmf_capsule *nc)
{
	nvmft_controller_t *ctrlr = nvmft_qpair_ctrlr(qp);
	const nvme_sqe_t *cmd = nvmf_capsule_sqe(nc);

	_NOTE(ARGUNUSED(qid));

	(void) atomic_swap_uint(&ctrlr->ctrlr_ka_active_traffic, 1);

	switch (cmd->sqe_opc) {
	case NVME_OPC_NVM_FLUSH:
		if (cmd->sqe_nsid == LE_32(NVME_NSID_BCAST)) {
			(void) nvmft_send_generic_error(qp, nc,
			    NVME_CQE_SC_GEN_INV_NS);
			nvmf_free_capsule(nc);
			break;
		}
		/* FALLTHROUGH */
	case NVME_OPC_NVM_WRITE:
	case NVME_OPC_NVM_READ:
	case NVME_OPC_NVM_WRITE_UNC:
	case NVME_OPC_NVM_COMPARE:
	case NVME_OPC_NVM_WRITE_ZERO:
	case NVME_OPC_NVM_DSET_MGMT:
	case NVMFT_OPC_NVM_VERIFY:
	/*
	 * PORT-TODO (7.4): NVME_OPC_NVM_RESV_* (register/report/acquire/release)
	 * should route to nvmft_resv.c rather than the generic datamove path
	 * (FreeBSD nvmft_handle_io_command does not handle the reservation
	 * opcodes either; they are added here for the COMSTAR reservation work).
	 */
		nvmft_dispatch_command(qp, nc);
		break;
	default:
		DTRACE_PROBE2(nvmft__io__unsupported, nvmft_controller_t *,
		    ctrlr, uint8_t, cmd->sqe_opc);
		(void) nvmft_send_generic_error(qp, nc, NVME_CQE_SC_GEN_INV_OPC);
		nvmf_free_capsule(nc);
		break;
	}
}

static void
nvmft_report_aer(nvmft_controller_t *ctrlr, uint32_t aer_mask, uint_t type,
    uint8_t info, uint8_t log_page_id)
{
	nvme_cqe_t cpl;

	ASSERT3U(type, <=, 7);

	mutex_enter(&ctrlr->ctrlr_lock);
	if ((ctrlr->ctrlr_aer_mask & aer_mask) == 0) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	if (ctrlr->ctrlr_aer_pending == 0) {
		mutex_exit(&ctrlr->ctrlr_lock);
		DTRACE_PROBE4(nvmft__aer__dropped, nvmft_controller_t *, ctrlr,
		    uint_t, type, uint8_t, info, uint8_t, log_page_id);
		return;
	}

	(void) bzero(&cpl, sizeof (cpl));
	cpl.cqe_cid = ctrlr->ctrlr_aer_cids[ctrlr->ctrlr_aer_cidx];
	ctrlr->ctrlr_aer_pending--;
	ctrlr->ctrlr_aer_cidx = (ctrlr->ctrlr_aer_cidx + 1) % NVMFT_NUM_AER;
	mutex_exit(&ctrlr->ctrlr_lock);

	cpl.cqe_dw0 = LE_32(((uint32_t)type & 0x7) |
	    ((uint32_t)info << 8) | ((uint32_t)log_page_id << 16));

	(void) nvmft_send_response(ctrlr->ctrlr_admin, &cpl);
}

void
nvmft_controller_lun_changed(nvmft_controller_t *ctrlr, int lun_id)
{
	nvme_nschange_list_t *nslist;
	uint32_t new_nsid, nsid;
	uint_t i, nitems;

	new_nsid = lun_id + 1;
	nslist = ctrlr->ctrlr_changed_ns;
	nitems = sizeof (nslist->nscl_ns) / sizeof (nslist->nscl_ns[0]);

	mutex_enter(&ctrlr->ctrlr_lock);

	/* If the first entry is 0xffffffff, the list is already full. */
	if (nslist->nscl_ns[0] != 0xffffffff) {
		for (i = 0; i < nitems; i++) {
			nsid = LE_32(nslist->nscl_ns[i]);
			if (nsid == new_nsid) {
				mutex_exit(&ctrlr->ctrlr_lock);
				return;
			}
			if (nsid == 0 || nsid > new_nsid)
				break;
		}

		if (nslist->nscl_ns[nitems - 1] != LE_32(0)) {
			/* List is full. */
			(void) bzero(nslist, sizeof (*nslist));
			nslist->nscl_ns[0] = 0xffffffff;
		} else {
			/*
			 * Not full: nscl_ns[nitems-1] is zero, so the scan above
			 * must have broken (on a zero or a larger NSID) rather
			 * than running to completion; therefore i < nitems and
			 * is a valid in-range insertion point.  The insertion
			 * point may be the trailing-zero slot itself, in which
			 * case we just write there.
			 */
			VERIFY3U(i, <, nitems);
			if (nslist->nscl_ns[i] == LE_32(0)) {
				nslist->nscl_ns[i] = LE_32(new_nsid);
			} else {
				/*
				 * A nonzero slot that is not the trailing zero
				 * implies a free slot exists after it, so i is
				 * strictly below the last index and the shift
				 * stays in-bounds.
				 */
				VERIFY3U(i, <, nitems - 1);
				(void) memmove(&nslist->nscl_ns[i + 1],
				    &nslist->nscl_ns[i],
				    (nitems - i - 1) *
				    sizeof (nslist->nscl_ns[0]));
				nslist->nscl_ns[i] = LE_32(new_nsid);
			}
		}
	}

	if (ctrlr->ctrlr_changed_ns_reported) {
		mutex_exit(&ctrlr->ctrlr_lock);
		return;
	}
	ctrlr->ctrlr_changed_ns_reported = B_TRUE;
	mutex_exit(&ctrlr->ctrlr_lock);

	nvmft_report_aer(ctrlr, NVMFT_AER_NS_ATTRIBUTE, NVME_ASYNC_TYPE_NOTICE,
	    NVME_ASYNC_NOTICE_NS_CHANGE, NVME_LOGPAGE_NSCHANGE);
}

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
 * NEW file.  Asymmetric Namespace Access (ANA) for the NVMe-oF controller.
 *
 * No FreeBSD counterpart exists: ANA/multipath is absent on both ends of the
 * FreeBSD port (NVMEOF.md sections 3 / 9).  This file implements the target
 * half of ANA per NVMEOF.md section 9.4: ANA groups, each namespace's ANAGRPID,
 * the ANA log page (log id 0x0c), the ANA-change AEN, the Identify Controller
 * CMIC/ANACAP bits, and the control ioctl that the distribution control plane
 * uses to set per-group ANA state (paralleling stmfSetAluaState/stmfModifyLu).
 *
 * Design notes (NVMEOF.md 9.1 / 9.2 / 9.4):
 *   - ANA is modeled natively in NVMe terms here (5 states x N groups), NOT
 *     round-tripped through STMF's binary SCSI ALUA (2 states x 2 TPGs); the
 *     state models do not match.  STMF active/standby may feed in as a coarse
 *     input via nvmft_lport_event_handler() but is not the model of record.
 *   - v1 may hardcode every group to Optimized, but all the plumbing (log page,
 *     AEN, Identify bits, control ioctl) ships so the wire surface is correct.
 *
 * Per-path state is per-controller (each association is a path).  In the full
 * model the group's state in a given controller's log page is the group state
 * intersected with that controller's reachability.  v1 does NOT implement that
 * intersection: nvmft_ana_build_log_page() reports the port-global group state
 * identically on every controller, ignoring its ctrlr argument for state.  The
 * per-path intersection is a TODO; until it lands, callers must not assume the
 * builder has already clamped the state toward Inaccessible/Non-Optimized.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/byteorder.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>		/* bcopy, ddi_copyin */

#include <sys/nvme.h>
#include <sys/nvme/nvmf.h>

#include <sys/time.h>		/* hrtime_t (sys/stmf.h needs it) */
#include <sys/stmf.h>

#include "nvmft_var.h"

/* ANA states (NVMe 1.4 figure for the ANA log page Group Descriptor). */
typedef enum {
	NVMFT_ANA_OPTIMIZED		= 0x1,
	NVMFT_ANA_NON_OPTIMIZED		= 0x2,
	NVMFT_ANA_INACCESSIBLE		= 0x3,
	NVMFT_ANA_PERSISTENT_LOSS	= 0x4,
	NVMFT_ANA_CHANGE		= 0xf
} nvmft_ana_state_t;

/*
 * An ANA group: the unit of failover.  Namespaces carry an ANAGRPID that
 * indexes into the port's group table; a group transitions as a whole.
 * (NVMEOF.md 9.1 / 9.4.)
 */
typedef struct nvmft_ana_group {
	uint32_t		nag_grpid;	/* ANAGRPID (1's based) */
	nvmft_ana_state_t	nag_state;
	uint_t			nag_nns;	/* namespaces in this group */
} nvmft_ana_group_t;

/*
 * ANA log page (log id 0x0c) wire structures (NVMe 1.4, figures for the ANA
 * log page header and Group Descriptor).  <sys/nvme.h> defines the log id but
 * not these on-wire layouts, so they live here.  All multi-byte fields are
 * little-endian on the wire.
 */
#pragma pack(1)
typedef struct nvmft_ana_grpdesc {
	uint32_t	agd_grpid;	/* ANA Group Identifier */
	uint32_t	agd_nnsids;	/* Number of NSIDs in this group */
	uint64_t	agd_chgcnt;	/* Per-group change count */
	uint8_t		agd_state;	/* ANA state (low nibble) */
	uint8_t		agd_rsvd[15];
	/* uint32_t	agd_nsids[]; -- follows immediately */
} nvmft_ana_grpdesc_t;

typedef struct nvmft_ana_hdr {
	uint64_t	ah_chgcnt;	/* Change count */
	uint16_t	ah_ngrpdesc;	/* Number of ANA Group Descriptors */
	uint8_t		ah_rsvd[6];
	/* nvmft_ana_grpdesc_t ah_grps[]; -- follows immediately */
} nvmft_ana_hdr_t;
#pragma pack()

CTASSERT(sizeof (nvmft_ana_grpdesc_t) == 32);
CTASSERT(sizeof (nvmft_ana_hdr_t) == 16);

/*
 * Control ioctl argument (parallels stmfSetAluaState/stmfModifyLu).  This is
 * shared with the userland control surface; see header_fields_needed --- the
 * integrator should hoist it into a shared nvmft ioctl header.
 */
typedef struct nvmft_ana_set_state {
	uint16_t	nass_portid;	/* exported subsystem port id */
	uint16_t	nass_pad;
	uint32_t	nass_grpid;	/* ANA Group Identifier */
	uint32_t	nass_state;	/* nvmft_ana_state_t value */
} nvmft_ana_set_state_t;

/*
 * Initialise the ANA group table for a port.  v1 default: a single group, all
 * namespaces in it, state Optimized.
 *
 * PORT-TODO (NVMEOF.md 9.4 / R10): the group design (how namespaces map to
 * failover domains) is a distribution-layer concern delivered via the control
 * ioctl; do not bake a grouping policy in beyond this default.
 */
void
nvmft_ana_init(nvmft_port_t *np)
{
	np->np_num_ana_groups = 1;
	np->np_ana_groups = kmem_zalloc(
	    np->np_num_ana_groups * sizeof (nvmft_ana_group_t), KM_SLEEP);
	np->np_ana_groups[0].nag_grpid = 1;
	np->np_ana_groups[0].nag_state = NVMFT_ANA_OPTIMIZED;
	np->np_ana_changecount = 0;
}

void
nvmft_ana_fini(nvmft_port_t *np)
{
	if (np->np_ana_groups != NULL) {
		kmem_free(np->np_ana_groups,
		    np->np_num_ana_groups * sizeof (nvmft_ana_group_t));
		np->np_ana_groups = NULL;
		np->np_num_ana_groups = 0;
	}
}

/*
 * Set the CMIC (multipath capable) and ANACAP/ANAGRPMAX/NANAGRPID fields in the
 * Identify Controller data so hosts know the subsystem is ANA-capable
 * (NVMEOF.md 9.4).
 */
void
nvmft_ana_init_identify(nvmft_port_t *np, nvme_identify_ctrl_t *cdata)
{
	/*
	 * CMIC (id_mic): the subsystem may have multiple controllers (one per
	 * association/path) and supports ANA reporting.
	 */
	cdata->id_mic.m_multi_ctrl = 1;
	cdata->id_mic.m_anar_sup = 1;

	/*
	 * ANACAP: advertise the ANA states we can report.  The ANAGRPID is
	 * non-zero (groups are 1's based) and does not change when a namespace
	 * is attached (anacap_grpns left 0).
	 */
	cdata->ap_anacap.anacap_opt = 1;	/* Optimized */
	cdata->ap_anacap.anacap_unopt = 1;	/* Non-Optimized */
	cdata->ap_anacap.anacap_inacc = 1;	/* Inaccessible */
	cdata->ap_anacap.anacap_ploss = 1;	/* Persistent Loss */
	cdata->ap_anacap.anacap_chg = 1;	/* Change */
	cdata->ap_anacap.anacap_grpid = 1;	/* Supports Group ID field */

	/* ANA transition time (seconds) the host should expect. */
	cdata->ap_anatt = 10;

	cdata->ap_anagrpmax = LE_32(np->np_num_ana_groups);
	cdata->ap_nanagrpid = LE_32(np->np_num_ana_groups);

	/* OAES: emit the Asymmetric Namespace Access Change notice. */
	cdata->id_oaes.oaes_ansacn = 1;
}

/*
 * Return the ANAGRPID for a namespace, used to populate the per-namespace
 * Identify Namespace ANAGRPID field.
 */
uint32_t
nvmft_ana_grpid_for_nsid(nvmft_port_t *np, uint32_t nsid)
{
	_NOTE(ARGUNUSED(nsid));

	/*
	 * v1 default: a single group owns every namespace.  When the control
	 * plane introduces multiple groups (R10) this becomes a per-namespace
	 * lookup; for now report the first (and only) group's id.
	 */
	if (np->np_num_ana_groups == 0)
		return (1);
	return (np->np_ana_groups[0].nag_grpid);
}

/*
 * Compute the full serialized length of the ANA log page for a port: a fixed
 * header, then for every group a fixed descriptor plus 4 bytes per namespace
 * in that group.  Called with np_lock held.
 */
static size_t
nvmft_ana_log_len(nvmft_port_t *np)
{
	size_t len;

	len = sizeof (nvmft_ana_hdr_t);
	len += (size_t)np->np_num_ana_groups * sizeof (nvmft_ana_grpdesc_t);
	/* v1: every active namespace belongs to the (single) first group. */
	len += (size_t)np->np_num_ns * sizeof (uint32_t);
	return (len);
}

/*
 * Build the ANA log page (log id 0x0c) for a controller (NVMEOF.md 9.4).  The
 * full page is serialized into a scratch buffer, then the [offset, offset+len)
 * window is copied into the caller's buf, honoring partial reads exactly as
 * handle_get_log_page() does for the other log pages.  *outlenp is set to the
 * number of bytes copied.  Returns 0 on success (including a zero-length copy
 * when offset is past the end of the page).
 *
 * The change-notice / RAE handling is the caller's (handle_get_log_page), which
 * already clears the ANA-change pending flag on a non-RAE read; the log page
 * itself is stateless.
 *
 * v1 reports the port-global group state on every controller: the ctrlr
 * argument selects the owning port (ctrlr_np) but is NOT used to clamp the
 * state to that controller's reachability.  The per-controller (per-path)
 * intersection described in the file header is a TODO.
 */
int
nvmft_ana_build_log_page(nvmft_controller_t *ctrlr, void *buf, size_t buflen,
    uint64_t offset, size_t *outlenp)
{
	nvmft_port_t *np = ctrlr->ctrlr_np;
	nvmft_ana_hdr_t *hdr;
	uint8_t *page, *p;
	size_t pagelen, todo;
	uint_t g;

	*outlenp = 0;

	mutex_enter(&np->np_lock);
	pagelen = nvmft_ana_log_len(np);

	page = kmem_zalloc(pagelen, KM_SLEEP);

	hdr = (nvmft_ana_hdr_t *)page;
	hdr->ah_chgcnt = LE_64(np->np_ana_changecount);
	hdr->ah_ngrpdesc = LE_16((uint16_t)np->np_num_ana_groups);

	p = page + sizeof (*hdr);
	for (g = 0; g < np->np_num_ana_groups; g++) {
		nvmft_ana_group_t *grp = &np->np_ana_groups[g];
		nvmft_ana_grpdesc_t *gd = (nvmft_ana_grpdesc_t *)p;
		uint32_t nnsids = 0;
		uint_t i;

		gd->agd_grpid = LE_32(grp->nag_grpid);
		gd->agd_chgcnt = LE_64(np->np_ana_changecount);
		gd->agd_state = (uint8_t)(grp->nag_state & 0xf);

		p += sizeof (*gd);

		/*
		 * Emit the NSID list for this group.  v1 places every active
		 * namespace in the first group; for later multi-group designs
		 * this filters by nvmft_ana_grpid_for_nsid().
		 */
		for (i = 0; i < np->np_num_ns; i++) {
			uint32_t nsid = np->np_active_ns[i];
			uint32_t le_nsid;

			if (nvmft_ana_grpid_for_nsid(np, nsid) !=
			    grp->nag_grpid)
				continue;

			le_nsid = LE_32(nsid);
			(void) bcopy(&le_nsid, p, sizeof (le_nsid));
			p += sizeof (le_nsid);
			nnsids++;
		}
		gd->agd_nnsids = LE_32(nnsids);
	}
	ASSERT3U((size_t)(p - page), ==, pagelen);
	mutex_exit(&np->np_lock);

	/* Honor offset/len exactly as handle_get_log_page() does. */
	if (offset >= pagelen) {
		kmem_free(page, pagelen);
		return (0);
	}

	todo = pagelen - offset;
	if (todo > buflen)
		todo = buflen;

	(void) bcopy(page + offset, buf, todo);
	*outlenp = todo;

	kmem_free(page, pagelen);
	return (0);
}

/*
 * Set the ANA state for a group.  This is the kernel mechanism the distribution
 * control plane drives via the control ioctl (NVMEOF.md 9.4 / 9.5).  The state
 * update and changecount bump are completed here under np_lock; emitting the
 * ANA-change AEN to each controller is deferred to the controller module (see
 * the PORT-TODO below) for lifetime safety and cannot be driven from this file
 * without changes to the shared header / controller module.
 */
int
nvmft_ana_set_group_state(nvmft_port_t *np, uint32_t grpid, uint32_t state)
{
	uint_t i;
	int rv = ENOENT;

	if (state < NVMFT_ANA_OPTIMIZED || state > NVMFT_ANA_PERSISTENT_LOSS)
		return (EINVAL);

	mutex_enter(&np->np_lock);
	for (i = 0; i < np->np_num_ana_groups; i++) {
		if (np->np_ana_groups[i].nag_grpid == grpid) {
			np->np_ana_groups[i].nag_state =
			    (nvmft_ana_state_t)state;
			np->np_ana_changecount++;
			rv = 0;
			break;
		}
	}

	if (rv == 0) {
		NVMFT_DPRINTF_L2("nvmft_ana: group %u -> state %u", grpid,
		    state);
	}
	mutex_exit(&np->np_lock);

	/*
	 * The state update and changecount bump are now durable.  The remaining
	 * step is to emit the ANA-change AEN to every controller (path) on this
	 * port so each host re-reads the log page.  That emission is deliberately
	 * NOT done here, and NOT under np_lock:
	 *
	 *   - nvmft_send_response() on the admin qpair takes the qpair lock and
	 *     does a KM_SLEEP allocation, so it must run with np_lock dropped.
	 *   - The controller teardown path destroys ctrlr_admin
	 *     (nvmft_controller.c) *before* delisting the controller from
	 *     np_controllers, so a raw pointer snapshot taken under np_lock and
	 *     used after the drop is a use-after-free.
	 *
	 * Safe emission needs a controller (admin-qpair-pinning) reference that
	 * keeps ctrlr_admin alive across the np_lock drop.  nvmft_var.h exposes
	 * nvmft_port_ref but no such controller ref, and nvmft_report_aer() is
	 * static to nvmft_controller.c, so the emission cannot be driven from
	 * this file without changes to the shared header / controller module.
	 *
	 * PORT-TODO (header_fields_needed): add a controller ref and a public
	 * nvmft_controller_ana_changed(ctrlr) in nvmft_controller.c that consumes
	 * one pending AER and calls the existing (static) nvmft_report_aer() with
	 * type NVME_ASYNC_TYPE_NOTICE, info NVME_ASYNC_NOTICE_NS_ASYMM, log page
	 * NVME_LOGPAGE_ASYMNS.  Then, under np_lock, snapshot a ref on each
	 * controller into a local array, drop np_lock, call
	 * nvmft_controller_ana_changed() on each, and release the refs.
	 * (FreeBSD has no ANA; the model is nvmft_controller_lun_changed(), which
	 * sets a pending flag then calls nvmft_report_aer() with np_lock dropped.)
	 */

	return (rv);
}

/*
 * Control ioctl entry point for setting ANA state, paralleling
 * stmfSetAluaState/stmfModifyLu (NVMEOF.md 9.4).  Copies in a
 * nvmft_ana_set_state_t, resolves the target subsystem by its STMF port id, and
 * applies the new group state.
 *
 * PORT-TODO: wire this into nvmft_drv_ioctl() in nvmft.c (new ioctl cmd) and
 * hoist nvmft_ana_set_state_t into a shared nvmft ioctl header for the userland
 * control surface (libstmf/stmfadm parallel).
 */
int
nvmft_ana_ioctl(intptr_t arg, int mode)
{
	nvmft_ana_set_state_t req;
	nvmft_port_t *np, *found = NULL;
	int rv;

	if (ddi_copyin((void *)arg, &req, sizeof (req), mode) != 0)
		return (EFAULT);

	/* Resolve the target subsystem by its STMF port id. */
	mutex_enter(&nvmft_global->ns_lock);
	for (np = list_head(&nvmft_global->ns_ports); np != NULL;
	    np = list_next(&nvmft_global->ns_ports, np)) {
		if (np->np_portid == req.nass_portid) {
			found = np;
			nvmft_port_ref(found);
			break;
		}
	}
	mutex_exit(&nvmft_global->ns_lock);

	if (found == NULL)
		return (ENOENT);

	rv = nvmft_ana_set_group_state(found, req.nass_grpid, req.nass_state);

	nvmft_port_rele(found);
	return (rv);
}

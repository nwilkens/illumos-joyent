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
 * The L2 table: the next hop (port, VLAN, MAC) of an offloaded connection.
 * The client resolves the neighbour and asks for an entry; entries with the
 * same next hop are shared and counted.  A new entry is written with a
 * synchronous CPL_L2T_WRITE_REQ on the port's control queue, and the caller
 * waits for the reply before the index may be used.
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ethernet.h>
#include <sys/vlan.h>

#include "common/common.h"
#include "common/t4_msg.h"
#include "t4_ofld.h"

/* The TID field of an L2T write carries the SYNC_WR flag and a queue ID. */
#define	S_T4_L2T_SYNC_WR	12
#define	F_T4_L2T_SYNC_WR	(1U << S_T4_L2T_SYNC_WR)

CTASSERT(T4_OFLD_MAX_L2T <= F_T4_L2T_SYNC_WR);

#define	T4_L2T_TIMEOUT_US	(2 * MICROSEC)

int
t4_l2t_init(t4_ofld_t *of)
{
	t4_l2t_t *l2 = &of->of_l2t;

	mutex_init(&l2->l2_lock, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(of->of_sc->intr_pri));
	cv_init(&l2->l2_cv, NULL, CV_DRIVER, NULL);
	l2->l2_start = of->of_l2t_start;
	l2->l2_size = of->of_l2t_size;
	l2->l2_ent = kmem_zalloc(l2->l2_size * sizeof (t4_l2t_ent_t),
	    KM_SLEEP);
	return (0);
}

void
t4_l2t_fini(t4_ofld_t *of)
{
	t4_l2t_t *l2 = &of->of_l2t;

	if (l2->l2_ent == NULL)
		return;
	kmem_free(l2->l2_ent, l2->l2_size * sizeof (t4_l2t_ent_t));
	cv_destroy(&l2->l2_cv);
	mutex_destroy(&l2->l2_lock);
	bzero(l2, sizeof (*l2));
}

static int
t4_l2t_write(t4_ofld_t *of, t4_l2t_ent_t *e, uint32_t idx)
{
	t4_ofld_port_t *op = &of->of_port[e->le_port];
	struct cpl_l2t_write_req req;
	const uint32_t hwidx = of->of_l2t.l2_start + idx;

	bzero(&req, sizeof (req));
	t4_ofld_init_tp_wr(&req, sizeof (req), 0);
	OPCODE_TID(&req) = BE_32(MK_OPCODE_TID(CPL_L2T_WRITE_REQ,
	    hwidx | F_T4_L2T_SYNC_WR | V_TID_QID(of->of_rxq.iq.tsi_abs_id)));
	req.params = BE_16(V_L2T_W_PORT(op->op_pi->lport) |
	    V_L2T_W_NOREPLY(0));
	req.l2t_idx = BE_16(hwidx);
	req.vlan = BE_16(e->le_vlan);
	bcopy(e->le_dmac, req.dst_mac, ETHERADDRL);
	T4_OFLD_STAT(of, os_l2t_write);
	return (t4_ofld_wr_send(of, &op->op_ctrlq, &req,
	    roundup(sizeof (req), 16)));
}

/*
 * Get an entry for the next hop (port, vlan, dmac) and return its index for
 * the opt0 L2T_IDX field.  vlan is CPL_L2T_VLAN_NONE for untagged traffic.
 * Thread context.
 */
int
t4_l2t_get(t4_ofld_t *of, uint8_t port, uint16_t vlan, const uint8_t *dmac,
    uint32_t *idxp)
{
	t4_l2t_t *l2 = &of->of_l2t;
	const clock_t deadline = ddi_get_lbolt() +
	    drv_usectohz(T4_L2T_TIMEOUT_US);
	static const uint8_t zero[ETHERADDRL] = { 0 };
	t4_l2t_ent_t *e = NULL;
	uint32_t i, idx = 0;
	int rc;

	if (servicing_interrupt())
		return (EWOULDBLOCK);
	if (dmac == NULL || port >= of->of_nports ||
	    (vlan != CPL_L2T_VLAN_NONE && (vlan & ~VLAN_ID_MASK) != 0) ||
	    (dmac[0] & 0x01) != 0 || bcmp(dmac, zero, ETHERADDRL) == 0)
		return (EINVAL);

	mutex_enter(&l2->l2_lock);
again:
	for (i = 0; i < l2->l2_size; i++) {
		t4_l2t_ent_t *c = &l2->l2_ent[i];

		if (c->le_refs != 0 && c->le_port == port &&
		    c->le_vlan == vlan &&
		    bcmp(c->le_dmac, dmac, ETHERADDRL) == 0) {
			if (c->le_state == TLS_WRITING) {
				if (cv_timedwait(&l2->l2_cv, &l2->l2_lock,
				    deadline) == -1) {
					mutex_exit(&l2->l2_lock);
					return (ETIMEDOUT);
				}
				goto again;
			}
			if (c->le_state != TLS_VALID)
				continue;
			c->le_refs++;
			mutex_exit(&l2->l2_lock);
			*idxp = l2->l2_start + i;
			return (0);
		}
	}

	for (i = 0; i < l2->l2_size; i++) {
		idx = (l2->l2_rotor + i) % l2->l2_size;
		if (l2->l2_ent[idx].le_refs == 0 &&
		    l2->l2_ent[idx].le_state != TLS_WRITING) {
			e = &l2->l2_ent[idx];
			break;
		}
	}
	if (e == NULL) {
		mutex_exit(&l2->l2_lock);
		return (ENOSPC);
	}
	l2->l2_rotor = (idx + 1) % l2->l2_size;
	e->le_refs = 1;
	e->le_state = TLS_WRITING;
	e->le_port = port;
	e->le_vlan = vlan;
	bcopy(dmac, e->le_dmac, ETHERADDRL);

	if ((rc = t4_l2t_write(of, e, idx)) != 0) {
		e->le_state = TLS_FAILED;
		e->le_refs = 0;
		cv_broadcast(&l2->l2_cv);
		mutex_exit(&l2->l2_lock);
		T4_OFLD_STAT(of, os_l2t_fail);
		return (rc);
	}
	while (e->le_state == TLS_WRITING) {
		if (cv_timedwait(&l2->l2_cv, &l2->l2_lock, deadline) == -1)
			break;
	}
	if (e->le_state != TLS_VALID) {
		/* A late reply finds the entry unused and is ignored. */
		e->le_state = TLS_FAILED;
		e->le_refs = 0;
		cv_broadcast(&l2->l2_cv);
		mutex_exit(&l2->l2_lock);
		T4_OFLD_STAT(of, os_l2t_fail);
		return (ETIMEDOUT);
	}
	mutex_exit(&l2->l2_lock);
	*idxp = l2->l2_start + idx;
	return (0);
}

void
t4_l2t_put(t4_ofld_t *of, uint32_t hwidx)
{
	t4_l2t_t *l2 = &of->of_l2t;
	const uint32_t idx = hwidx - l2->l2_start;

	if (hwidx < l2->l2_start || idx >= l2->l2_size)
		return;
	mutex_enter(&l2->l2_lock);
	if (l2->l2_ent[idx].le_refs > 0 &&
	    l2->l2_ent[idx].le_state == TLS_VALID)
		l2->l2_ent[idx].le_refs--;
	mutex_exit(&l2->l2_lock);
}

/* Drop every reference, for a client that left. */
void
t4_l2t_reset(t4_ofld_t *of)
{
	t4_l2t_t *l2 = &of->of_l2t;

	mutex_enter(&l2->l2_lock);
	for (uint32_t i = 0; i < l2->l2_size; i++) {
		if (l2->l2_ent[i].le_state == TLS_VALID)
			l2->l2_ent[i].le_refs = 0;
	}
	mutex_exit(&l2->l2_lock);
}

/* Whether hwidx is a held, written entry for port; returns its VLAN. */
boolean_t
t4_l2t_held(t4_ofld_t *of, uint32_t hwidx, uint8_t port, uint16_t *vlanp)
{
	t4_l2t_t *l2 = &of->of_l2t;
	const uint32_t idx = hwidx - l2->l2_start;
	boolean_t ok;

	if (hwidx < l2->l2_start || idx >= l2->l2_size)
		return (B_FALSE);
	mutex_enter(&l2->l2_lock);
	ok = l2->l2_ent[idx].le_refs > 0 &&
	    l2->l2_ent[idx].le_state == TLS_VALID &&
	    l2->l2_ent[idx].le_port == port;
	if (ok && vlanp != NULL)
		*vlanp = l2->l2_ent[idx].le_vlan;
	mutex_exit(&l2->l2_lock);
	return (ok);
}

void
t4_l2t_write_rpl(t4_ofld_t *of, const struct cpl_l2t_write_rpl *rpl)
{
	t4_l2t_t *l2 = &of->of_l2t;
	const uint32_t tid = GET_TID(rpl);
	const uint32_t hwidx = tid & ~(F_T4_L2T_SYNC_WR |
	    V_TID_QID(M_TID_QID));
	const uint32_t idx = hwidx - l2->l2_start;
	t4_l2t_ent_t *e;

	if ((tid & F_T4_L2T_SYNC_WR) == 0 || hwidx < l2->l2_start ||
	    idx >= l2->l2_size) {
		T4_OFLD_STAT(of, os_cpl_badid);
		return;
	}
	mutex_enter(&l2->l2_lock);
	e = &l2->l2_ent[idx];
	if (e->le_state == TLS_WRITING) {
		e->le_state = rpl->status == CPL_ERR_NONE ? TLS_VALID :
		    TLS_FAILED;
		cv_broadcast(&l2->l2_cv);
	}
	mutex_exit(&l2->l2_lock);
}

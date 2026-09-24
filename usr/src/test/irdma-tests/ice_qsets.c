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
 * Run the qset add and delete operations of the ice RDMA peer interface.
 * The child is not trusted with the scheduler: a bad TC, VSI, count, handle
 * or TEID must fail with nothing changed, and a failed firmware command must
 * be reported and owe a reset.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef unsigned int uint_t;
typedef int boolean_t;
typedef int kmutex_t;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint8_t u8;

#define	B_TRUE			1
#define	B_FALSE			0
#define	BIT(n)			(1U << (n))
#define	ICE_SUCCESS		0
#define	ICE_ERR_AQ_ERROR	(-100)
#define	ICE_MAX_TRAFFIC_CLASS	8
#define	ICE_PF_VSI_HANDLE	0
#define	ICE_STATE_PFR_REQ	0x10
#define	ICE_RDMA_MAX_QSETS	8
#define	ICE_RDMA_QSET_TABLE	64

typedef struct ice_rdma_qset {
	uint16_t	irqs_handle;
	uint16_t	irqs_vsi_num;
	uint8_t		irqs_tc;
	uint32_t	irqs_teid;
} ice_rdma_qset_t;

typedef struct ice_rdma_qrec {
	boolean_t	iqr_used;
	uint16_t	iqr_handle;
	uint8_t		iqr_tc;
	uint32_t	iqr_teid;
} ice_rdma_qrec_t;

typedef struct ice_rdma {
	ice_rdma_qrec_t	ir_qsets[ICE_RDMA_QSET_TABLE];
	uint_t		ir_nqsets;
} ice_rdma_t;

struct ice_hw { void *port_info; };
typedef struct ice {
	kmutex_t	ice_rebuild_lock;
	uint32_t	ice_state;
	struct ice_hw	ice_hw;
	struct { uint16_t vi_hw_num; } ice_pf_vsi;
	ice_rdma_t	*ice_rdma;
} ice_t;
typedef ice_t ice_rdma_peer_t;

static ice_t dev;
static ice_rdma_t rdma;
static int hw_state, cfg_fail, ena_fail_at, dis_fail, redispatches;
static int enas, dis, cfgs;
static uint32_t next_teid = 100;
static uint16_t cfg_max;

static void
mutex_enter(kmutex_t *m)
{
	assert(*m == 0);
	*m = 1;
}

static void
mutex_exit(kmutex_t *m)
{
	assert(*m == 1);
	*m = 0;
}

static ice_t *ice_rdma_peer_ice(ice_rdma_peer_t *p) { return (p); }

static int
ice_rdma_hw_ok(ice_t *ice)
{
	assert(ice->ice_rebuild_lock);
	return (hw_state);
}

static int
ice_status_to_errno(ice_t *ice, int status)
{
	(void) ice;
	return (status == ICE_SUCCESS ? 0 : EIO);
}

static int
ice_cfg_vsi_rdma(void *pi, uint16_t vsi, uint16_t tcmap, uint16_t *max)
{
	(void) pi;
	assert(dev.ice_rebuild_lock && vsi == ICE_PF_VSI_HANDLE);
	assert(tcmap == BIT(0));
	cfgs++;
	cfg_max = max[0];
	return (cfg_fail ? ICE_ERR_AQ_ERROR : ICE_SUCCESS);
}

static int
ice_ena_vsi_rdma_qset(void *pi, uint16_t vsi, uint8_t tc, uint16_t *h,
    uint16_t n, uint32_t *teid)
{
	(void) pi;
	(void) h;
	assert(dev.ice_rebuild_lock && vsi == ICE_PF_VSI_HANDLE);
	assert(n == 1 && tc == 0);
	if (++enas == ena_fail_at)
		return (ICE_ERR_AQ_ERROR);
	*teid = next_teid++;
	return (ICE_SUCCESS);
}

static int
ice_dis_vsi_rdma_qset(void *pi, uint16_t n, uint32_t *teid, uint16_t *h)
{
	(void) pi;
	(void) teid;
	(void) h;
	assert(dev.ice_rebuild_lock && n == 1);
	dis++;
	return (dis_fail ? ICE_ERR_AQ_ERROR : ICE_SUCCESS);
}

static void
atomic_or_32(uint32_t *p, uint32_t bits)
{
	*p |= bits;
}

static void
ice_reset_redispatch(ice_t *ice)
{
	assert(ice->ice_rebuild_lock);
	redispatches++;
}

#include "qset_bodies.h"

static ice_rdma_qset_t
q(uint16_t handle, uint8_t tc, uint16_t vsi)
{
	ice_rdma_qset_t s = { handle, vsi, tc, 0 };

	return (s);
}

static void
reset(void)
{
	memset(&rdma, 0, sizeof (rdma));
	memset(&dev, 0, sizeof (dev));
	dev.ice_rdma = &rdma;
	dev.ice_pf_vsi.vi_hw_num = 3;
	hw_state = cfg_fail = ena_fail_at = dis_fail = redispatches = 0;
	enas = dis = cfgs = 0;
}

int
main(void)
{
	ice_rdma_qset_t a[ICE_RDMA_MAX_QSETS + 1], b;
	uint_t i;

	reset();
	a[0] = q(7, 0, 3);
	a[1] = q(8, 0, 3);
	assert(ice_rdma_op_qset_add(&dev, a, 2) == 0);
	assert(rdma.ir_nqsets == 2 && cfg_max == 2);
	assert(a[0].irqs_teid == 100 && a[1].irqs_teid == 101);

	/* Arguments ice rejects before any command. */
	b = q(9, 1, 3);
	assert(ice_rdma_op_qset_add(&dev, &b, 1) == EINVAL);	/* TC 1 */
	b = q(9, 8, 3);
	assert(ice_rdma_op_qset_add(&dev, &b, 1) == EINVAL);	/* TC 8 */
	b = q(9, 0, 4);
	assert(ice_rdma_op_qset_add(&dev, &b, 1) == EINVAL);	/* other VSI */
	b = q(7, 0, 3);
	assert(ice_rdma_op_qset_add(&dev, &b, 1) == EINVAL);	/* in use */
	a[2] = q(9, 0, 3);
	a[3] = q(9, 0, 3);
	assert(ice_rdma_op_qset_add(&dev, &a[2], 2) == EINVAL);	/* twice */
	assert(ice_rdma_op_qset_add(&dev, a, 0) == EINVAL);
	assert(ice_rdma_op_qset_add(&dev, a, ICE_RDMA_MAX_QSETS + 1) ==
	    EINVAL);
	assert(ice_rdma_op_qset_add(&dev, NULL, 1) == EINVAL);
	assert(rdma.ir_nqsets == 2 && cfgs == 1 && enas == 2);

	/* A device that is resetting takes no commands. */
	hw_state = EAGAIN;
	b = q(9, 0, 3);
	assert(ice_rdma_op_qset_add(&dev, &b, 1) == EAGAIN);
	assert(ice_rdma_op_qset_del(&dev, a, 1) == EAGAIN);
	hw_state = 0;
	assert(cfgs == 1 && dis == 0);

	/* A firmware failure midway removes what this call added. */
	a[2] = q(20, 0, 3);
	a[3] = q(21, 0, 3);
	a[4] = q(22, 0, 3);
	ena_fail_at = enas + 3;
	assert(ice_rdma_op_qset_add(&dev, &a[2], 3) == EIO);
	assert(rdma.ir_nqsets == 2 && dis == 2 && redispatches == 0);
	assert(a[2].irqs_teid == 0 && a[3].irqs_teid == 0);
	ena_fail_at = 0;

	/* Delete needs the owner's handle, TC, VSI and TEID. */
	b = a[0];
	b.irqs_teid++;
	assert(ice_rdma_op_qset_del(&dev, &b, 1) == EINVAL);
	b = a[0];
	b.irqs_vsi_num = 4;
	assert(ice_rdma_op_qset_del(&dev, &b, 1) == EINVAL);
	b = q(99, 0, 3);
	b.irqs_teid = 100;
	assert(ice_rdma_op_qset_del(&dev, &b, 1) == EINVAL);
	a[2] = a[0];
	a[3] = a[0];
	assert(ice_rdma_op_qset_del(&dev, &a[2], 2) == EINVAL);
	assert(rdma.ir_nqsets == 2 && dis == 2);

	/* A failed removal still retires the record and owes a reset. */
	dis_fail = 1;
	assert(ice_rdma_op_qset_del(&dev, &a[1], 1) == EIO);
	assert(rdma.ir_nqsets == 1 && (dev.ice_state & ICE_STATE_PFR_REQ));
	assert(redispatches == 1);
	dis_fail = 0;
	assert(ice_rdma_op_qset_del(&dev, &a[0], 1) == 0);
	assert(rdma.ir_nqsets == 0);

	/* The table bounds what one client can hold. */
	reset();
	for (i = 0; i < ICE_RDMA_QSET_TABLE; i++) {
		b = q((uint16_t)(i + 1), 0, 3);
		assert(ice_rdma_op_qset_add(&dev, &b, 1) == 0);
	}
	b = q(1000, 0, 3);
	assert(ice_rdma_op_qset_add(&dev, &b, 1) == ENOSPC);
	assert(!dev.ice_rebuild_lock);

	(void) printf("PASS: qset add and delete validate TC, VSI, counts, "
	    "handles and TEIDs\n");
	return (0);
}

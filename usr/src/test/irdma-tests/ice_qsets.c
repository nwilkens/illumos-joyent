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

#define	ASSERT(x)		assert(x)
#define	MUTEX_HELD(m)		(*(m) != 0)
#define	LE32_TO_CPU(x)		(x)
#define	ICE_AQC_ELEM_TYPE_LEAF	5
#define	ICE_SCHED_NODE_OWNER_LAN	0
#define	ICE_SCHED_NODE_OWNER_RDMA	2

struct ice_sched_node {
	struct ice_sched_node *parent;
	struct ice_sched_node *children[80];
	struct {
		uint32_t node_teid;
		struct { uint8_t elem_type; } data;
	} info;
	uint16_t vsi_handle;
	uint8_t tx_sched_layer;
	uint8_t num_children;
	uint8_t owner;
};

struct ice_port_info {
	struct ice_sched_node *root;
	int sched_lock;
};

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

struct ice_hw {
	struct ice_port_info *port_info;
	uint8_t num_tx_sched_layers;
};
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

/* A scheduler tree: root, an RDMA parent, and a LAN parent with a leaf. */
static struct ice_sched_node root, rparent, lparent, lleaf, leaves[80];
static struct ice_port_info pinfo;
static uint_t nleaves;
static uint32_t forge_teid;
static int errors;

static void
ice_error(ice_t *ice, const char *fmt, ...)
{
	(void) ice;
	(void) fmt;
	errors++;
}

static void
ice_acquire_lock(int *l)
{
	assert(*l == 0);
	*l = 1;
}

static void
ice_release_lock(int *l)
{
	assert(*l == 1);
	*l = 0;
}

static void
link_child(struct ice_sched_node *p, struct ice_sched_node *c)
{
	c->parent = p;
	p->children[p->num_children++] = c;
}

static struct ice_sched_node *
ice_sched_find_node_by_teid(struct ice_sched_node *n, uint32_t teid)
{
	struct ice_sched_node *r;
	uint_t i;

	if (n->info.node_teid == teid)
		return (n);
	for (i = 0; i < n->num_children; i++) {
		r = ice_sched_find_node_by_teid(n->children[i], teid);
		if (r != NULL)
			return (r);
	}
	return (NULL);
}

static void
tree(void)
{
	memset(&root, 0, sizeof (root));
	memset(&rparent, 0, sizeof (rparent));
	memset(&lparent, 0, sizeof (lparent));
	memset(&lleaf, 0, sizeof (lleaf));
	nleaves = 0;
	root.info.node_teid = 1;
	rparent.info.node_teid = 2;
	rparent.owner = ICE_SCHED_NODE_OWNER_RDMA;
	rparent.tx_sched_layer = 4;
	lparent.info.node_teid = 3;
	lparent.tx_sched_layer = 4;
	lleaf.info.node_teid = 50;
	lleaf.tx_sched_layer = 5;
	lleaf.info.data.elem_type = ICE_AQC_ELEM_TYPE_LEAF;
	link_child(&root, &rparent);
	link_child(&root, &lparent);
	link_child(&lparent, &lleaf);
	pinfo.root = &root;
}

static void
add_leaf(uint32_t teid)
{
	struct ice_sched_node *leaf = &leaves[nleaves++];

	memset(leaf, 0, sizeof (*leaf));
	leaf->info.node_teid = teid;
	leaf->tx_sched_layer = 5;
	leaf->info.data.elem_type = ICE_AQC_ELEM_TYPE_LEAF;
	link_child(&rparent, leaf);
}

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
	/* The common code adds whatever TEID firmware returned. */
	if (forge_teid == UINT32_MAX)
		*teid = 0;
	else
		*teid = forge_teid != 0 ? forge_teid : next_teid++;
	add_leaf(*teid);
	return (ICE_SUCCESS);
}

static int
ice_dis_vsi_rdma_qset(void *pi, uint16_t n, uint32_t *teid, uint16_t *h)
{
	(void) pi;
	(void) teid;
	(void) h;
	struct ice_sched_node *node;
	uint_t i;

	assert(dev.ice_rebuild_lock && n == 1);
	dis++;
	if (dis_fail)
		return (ICE_ERR_AQ_ERROR);
	/* Only a recorded RDMA leaf may reach the recursive free. */
	node = ice_sched_find_node_by_teid(&root, *teid);
	assert(node != NULL && node->parent == &rparent);
	for (i = 0; i < rparent.num_children; i++) {
		if (rparent.children[i] == node) {
			rparent.children[i] =
			    rparent.children[--rparent.num_children];
			break;
		}
	}
	return (ICE_SUCCESS);
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
	dev.ice_hw.port_info = &pinfo;
	dev.ice_hw.num_tx_sched_layers = 6;
	tree();
	forge_teid = 0;
	errors = 0;
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

	/*
	 * A firmware failure midway removes what this call added, and owes a
	 * reset for a qset the failed command may have made.
	 */
	a[2] = q(20, 0, 3);
	a[3] = q(21, 0, 3);
	a[4] = q(22, 0, 3);
	ena_fail_at = enas + 3;
	assert(ice_rdma_op_qset_add(&dev, &a[2], 3) == EIO);
	assert(rdma.ir_nqsets == 2 && dis == 2 && redispatches == 1);
	assert(a[2].irqs_teid == 0 && a[3].irqs_teid == 0);
	assert((dev.ice_state & ICE_STATE_PFR_REQ) != 0);
	dev.ice_state = 0;
	ena_fail_at = 0;

	/*
	 * A TEID naming the root, a LAN leaf or a live RDMA qset, or zero, is
	 * never recorded and nothing is deleted by it; a reset is owed.
	 */
	for (i = 0; i < 4; i++) {
		static const uint32_t forged[] = { 1, 50, 100, UINT32_MAX };

		forge_teid = forged[i];
		b = q((uint16_t)(30 + i), 0, 3);
		redispatches = 0;
		assert(ice_rdma_op_qset_add(&dev, &b, 1) == EIO);
		assert(rdma.ir_nqsets == 2 && redispatches == 1);
		assert(b.irqs_teid == 0 && dis == 2);
		assert((dev.ice_state & ICE_STATE_PFR_REQ) != 0);
		dev.ice_state = 0;
	}
	forge_teid = 0;
	assert(errors == 4 && root.num_children == 2);
	assert(lparent.num_children == 1 && lparent.children[0] == &lleaf);
	/* The reset would rebuild the tree; model its result. */
	tree();
	add_leaf(a[0].irqs_teid);
	add_leaf(a[1].irqs_teid);
	redispatches = 0;

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

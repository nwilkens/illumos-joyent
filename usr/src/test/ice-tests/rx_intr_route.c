/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 * A copy of the CDDL is available at http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Execute the production QINT_RQCTL writer against the lifecycle routing
 * transitions and MAC's poll-mode callbacks.  Every write must be composed
 * from ring state under the ring lock, so no interleaving of the two owners
 * can leave the cause enabled on a cleared vector, or re-arm a cause the
 * lifecycle dissociated.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int boolean_t;
typedef int kmutex_t;
typedef void *mac_intr_handle_t;
#define	B_TRUE	1
#define	B_FALSE	0
#define	BIT(n)	(1U << (n))
#define	MAKEMASK(m, s)	((m) << (s))
#define	ASSERT(x)	assert(x)
#define	MUTEX_HELD(m)	(*(m) != 0)

#define	QINT_RQCTL(q)		(q)
#define	QINT_RQCTL_MSIX_INDX_S	0
#define	QINT_RQCTL_MSIX_INDX_M	MAKEMASK(0x7FFU, 0)
#define	QINT_RQCTL_ITR_INDX_S	11
#define	QINT_RQCTL_ITR_INDX_M	MAKEMASK(0x3U, 11)
#define	QINT_RQCTL_CAUSE_ENA_M	BIT(30)
#define	GLINT_DYN_CTL(v)	(0x100 + (v))
#define	ICE_ITR_IDX_0		0
#define	ICE_GLINT_DYN_CTL_REARM	0xdeadU

#define	QUEUE	7
#define	VECTOR	3
#define	ROUTING	((VECTOR << QINT_RQCTL_MSIX_INDX_S) | \
	(ICE_ITR_IDX_0 << QINT_RQCTL_ITR_INDX_S))

struct ice_hw {
	uint32_t regs[0x200];
	unsigned writes, flushes, rearms;
};
typedef struct ice {
	struct ice_hw ice_hw;
} ice_t;
typedef struct ice_rx_ring {
	ice_t *irxr_ice;
	uint32_t irxr_index;
	uint32_t irxr_vec;
	boolean_t irxr_intr_poll;
	boolean_t irxr_intr_routed;
	boolean_t irxr_intr_armed;
	kmutex_t irxr_lock;
} ice_rx_ring_t;

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

static void
wr32(struct ice_hw *hw, uint32_t reg, uint32_t val)
{
	hw->regs[reg] = val;
	hw->writes++;
	if (reg == GLINT_DYN_CTL(VECTOR))
		hw->rearms++;
}

static void
ice_flush(struct ice_hw *hw)
{
	hw->flushes++;
}

#include "rx_intr_route_body.h"

static ice_t ice;
static ice_rx_ring_t ring;

static void
reset(void)
{
	(void) memset(&ice, 0, sizeof (ice));
	(void) memset(&ring, 0, sizeof (ring));
	ring.irxr_ice = &ice;
	ring.irxr_index = QUEUE;
	ring.irxr_vec = VECTOR;
	/* A stale value from before the driver owned the queue. */
	ice.ice_hw.regs[QINT_RQCTL(QUEUE)] = 0x7ff | QINT_RQCTL_CAUSE_ENA_M;
}

static uint32_t
rqctl(void)
{
	assert(ring.irxr_lock == 0);
	/* Every write is flushed before the lock drops. */
	assert(ice.ice_hw.flushes >= ice.ice_hw.writes - ice.ice_hw.rearms);
	return (ice.ice_hw.regs[QINT_RQCTL(QUEUE)]);
}

int
main(void)
{
	unsigned rearms;

	/* Attach/start: route and arm; MAC toggles the cause alone. */
	reset();
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	assert(rqctl() == (ROUTING | QINT_RQCTL_CAUSE_ENA_M));
	(void) ice_ring_rx_intr_disable(&ring);
	assert(rqctl() == ROUTING);
	rearms = ice.ice_hw.rearms;
	(void) ice_ring_rx_intr_enable(&ring);
	assert(rqctl() == (ROUTING | QINT_RQCTL_CAUSE_ENA_M));
	assert(ice.ice_hw.rearms == rearms + 1);
	assert(ice.ice_hw.regs[GLINT_DYN_CTL(VECTOR)] ==
	    ICE_GLINT_DYN_CTL_REARM);

	/*
	 * The reviewed race: MAC leaves poll mode after reset preparation
	 * removed the routing.  A read-modify-write would publish CAUSE_ENA on
	 * vector 0; the composed write leaves the queue unrouted and does not
	 * touch the vector.
	 */
	reset();
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	(void) ice_ring_rx_intr_disable(&ring);
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_UNMAP);
	assert(rqctl() == 0);
	rearms = ice.ice_hw.rearms;
	(void) ice_ring_rx_intr_enable(&ring);
	assert(rqctl() == 0);
	assert(ice.ice_hw.rearms == rearms);
	/* The rebuild's remap then restores routing and MAC's chosen mode. */
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	assert(rqctl() == (ROUTING | QINT_RQCTL_CAUSE_ENA_M));

	/*
	 * The other order: the rebuild remaps while MAC still polls.  MAC owns
	 * the cause and always leaves poll mode through the enable callback.
	 */
	reset();
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	(void) ice_ring_rx_intr_disable(&ring);
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_UNMAP);
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	assert(rqctl() == ROUTING);
	(void) ice_ring_rx_intr_enable(&ring);
	assert(rqctl() == (ROUTING | QINT_RQCTL_CAUSE_ENA_M));

	/*
	 * Stop dissociates the cause but keeps the routing so the software
	 * interrupt can retire an in-flight cause.  MAC's ring stop leaves
	 * poll mode afterwards; that must not re-arm the cause the queue
	 * disable depends on.
	 */
	reset();
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	(void) ice_ring_rx_intr_disable(&ring);
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_DISSOCIATE);
	assert(rqctl() == ROUTING);
	rearms = ice.ice_hw.rearms;
	(void) ice_ring_rx_intr_enable(&ring);
	assert(rqctl() == ROUTING);
	assert(ice.ice_hw.rearms == rearms);
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	assert(rqctl() == (ROUTING | QINT_RQCTL_CAUSE_ENA_M));

	/* Unmap after dissociate clears everything; poll state survives. */
	reset();
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_MAP);
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_DISSOCIATE);
	ice_rx_ring_intr_route(&ring, ICE_RX_INTR_UNMAP);
	assert(rqctl() == 0);
	assert(!ring.irxr_intr_poll);

	(void) printf("PASS: ice rx interrupt routing composed under ring "
	    "lock\n");
	return (0);
}

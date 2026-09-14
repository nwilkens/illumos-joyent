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

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define	B_TRUE	true
#define	B_FALSE	false
#define	ASSERT(x)	assert(x)
#define	MUTEX_HELD(p)	(*(p) != 0)
#define	BIT_ULL(n)	(UINT64_C(1) << (n))
#define	MSEC2NSEC(n)	((int64_t)(n) * 1000000)
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))
#define	DDI_FM_OK	0
#define	DDI_SERVICE_DEGRADED	1
#define	DDI_SERVICE_UNAFFECTED	2

typedef bool boolean_t;
typedef unsigned int uint_t;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t hrtime_t;
typedef int kmutex_t;

#include "ice_stats_read_types.h"

struct ice_port_info { uint8_t lport; };
struct ice_hw { struct ice_port_info *port_info; };
typedef struct ice {
	struct ice_hw ice_hw;
	struct ice_hw_port_stats ice_stat_port_cur, ice_stat_port_prev;
	struct ice_eth_stats ice_stat_vsi_cur, ice_stat_vsi_prev;
	boolean_t ice_stat_port_loaded, ice_stat_vsi_loaded;
	hrtime_t ice_stat_port_last_update;
	kmutex_t ice_stat_lock, ice_rebuild_lock;
	struct { void *ios_reg_handle; } ice_osdep;
	void *ice_dip;
} ice_t;

static ice_t device;
static struct ice_port_info port = { 3 };
static hrtime_t now;
static unsigned int reads, checks, impacts, lock_entries;
static int access_status, last_impact;

static struct counter {
	uint32_t reg;
	unsigned int width;
	uint64_t raw, initial;
} counters[] = {
	{ GLPRT_GORCL(3), 40, 0, BIT_ULL(35) + 101 },
	{ GLPRT_UPRCL(3), 40, 0, 2 },
	{ GLPRT_MPRCL(3), 40, 0, 3 },
	{ GLPRT_BPRCL(3), 40, 0, 5 },
	{ GLPRT_GOTCL(3), 40, 0, BIT_ULL(36) + 211 },
	{ GLPRT_UPTCL(3), 40, 0, 7 },
	{ GLPRT_MPTCL(3), 40, 0, 11 },
	{ GLPRT_BPTCL(3), 40, 0, 13 },
	{ GLPRT_TDOLD(3), 32, 0, 17 },
	{ GLPRT_LXONRXC(3), 32, 0, 19 },
	{ GLPRT_LXOFFRXC(3), 32, 0, 23 },
	{ GLPRT_LXONTXC(3), 32, 0, 29 },
	{ GLPRT_LXOFFTXC(3), 32, 0, 31 },
	{ GLPRT_CRCERRS(3), 32, 0, 37 },
	{ GLPRT_ILLERRC(3), 32, 0, 41 },
	{ GLPRT_MLFC(3), 32, 0, 43 },
	{ GLPRT_MRFC(3), 32, 0, 47 },
	{ GLPRT_RLEC(3), 32, 0, 53 },
	{ GLPRT_RUC(3), 32, 0, 59 },
	{ GLPRT_RFC(3), 32, 0, 61 },
	{ GLPRT_ROC(3), 32, 0, 67 },
	{ GLPRT_RJC(3), 32, 0, 71 }
};

static void
mutex_enter(kmutex_t *lock)
{
	assert(!MUTEX_HELD(lock));
	if (lock == &device.ice_stat_lock)
		assert(MUTEX_HELD(&device.ice_rebuild_lock));
	else
		assert(lock == &device.ice_rebuild_lock);
	*lock = 1;
	lock_entries++;
}

static void
mutex_exit(kmutex_t *lock)
{
	assert(MUTEX_HELD(lock));
	if (lock == &device.ice_rebuild_lock)
		assert(!MUTEX_HELD(&device.ice_stat_lock));
	*lock = 0;
}

static hrtime_t
gethrtime(void)
{
	assert(MUTEX_HELD(&device.ice_rebuild_lock));
	assert(MUTEX_HELD(&device.ice_stat_lock));
	return (now);
}

static uint64_t
read_counter(struct ice_hw *hw, uint32_t reg, unsigned int width)
{
	size_t i;

	assert(hw == &device.ice_hw);
	assert(MUTEX_HELD(&device.ice_rebuild_lock));
	assert(MUTEX_HELD(&device.ice_stat_lock));
	for (i = 0; i < ARRAY_SIZE(counters); i++) {
		if (counters[i].reg == reg) {
			assert(counters[i].width == width);
			reads++;
			return (counters[i].raw);
		}
	}
	assert(!"unexpected register");
	return (0);
}

static uint64_t
rd64(struct ice_hw *hw, uint32_t reg)
{
	return (read_counter(hw, reg, 40));
}

static uint32_t
rd32(struct ice_hw *hw, uint32_t reg)
{
	return ((uint32_t)read_counter(hw, reg, 32));
}

static int
ice_check_acc_handle(ice_t *ice, void *handle)
{
	assert(ice == &device && handle == &device);
	checks++;
	return (access_status);
}

static void
ddi_fm_service_impact(void *dip, int impact)
{
	assert(dip == &device);
	impacts++;
	last_impact = impact;
}

#include "ice_stats_read_bodies.h"

static void
reset(void)
{
	size_t i;

	assert(!MUTEX_HELD(&device.ice_rebuild_lock));
	assert(!MUTEX_HELD(&device.ice_stat_lock));
	memset(&device, 0, sizeof (device));
	device.ice_hw.port_info = &port;
	device.ice_osdep.ios_reg_handle = &device;
	device.ice_dip = &device;
	device.ice_stat_port_loaded = B_TRUE;
	device.ice_stat_vsi_loaded = B_TRUE;
	for (i = 0; i < ARRAY_SIZE(counters); i++)
		counters[i].raw = counters[i].initial;
	now = MSEC2NSEC(1000);
	reads = checks = impacts = lock_entries = 0;
	access_status = DDI_FM_OK;
	last_impact = 0;
}

static void
check_read(uint_t stat, uint64_t expected)
{
	uint64_t value = UINT64_MAX;

	assert(ice_stats_read(&device, stat, &value) == 0);
	assert(value == expected);
	assert(!MUTEX_HELD(&device.ice_rebuild_lock));
	assert(!MUTEX_HELD(&device.ice_stat_lock));
}

static void
selectors(void)
{
	static const struct { uint_t stat; uint64_t value; } cases[] = {
		{ MAC_STAT_RBYTES, BIT_ULL(35) + 101 },
		{ MAC_STAT_IPACKETS, 10 },
		{ MAC_STAT_OBYTES, BIT_ULL(36) + 211 },
		{ MAC_STAT_OPACKETS, 31 },
		{ MAC_STAT_MULTIRCV, 3 },
		{ MAC_STAT_BRDCSTRCV, 5 },
		{ MAC_STAT_MULTIXMT, 11 },
		{ MAC_STAT_BRDCSTXMT, 13 },
		{ MAC_STAT_IERRORS, 131 },
		{ MAC_STAT_UNDERFLOWS, 120 },
		{ MAC_STAT_OVERFLOWS, 138 },
		{ ETHER_STAT_FCS_ERRORS, 37 },
		{ ETHER_STAT_TOOLONG_ERRORS, 67 },
		{ ETHER_STAT_MACRCV_ERRORS, 311 }
	};
	size_t i;

	reset();
	for (i = 0; i < ARRAY_SIZE(cases); i++)
		check_read(cases[i].stat, cases[i].value);
	assert(reads == ARRAY_SIZE(counters));
	assert(checks == ARRAY_SIZE(cases) && impacts == 0);
	assert(lock_entries == 2 * ARRAY_SIZE(cases));
}

static void
unsupported(void)
{
	const uint_t cases[] = {
		UINT32_MAX, MAC_STAT_OERRORS, MAC_STAT_IFSPEED,
		ETHER_STAT_LINK_DUPLEX
	};
	size_t i;

	reset();
	access_status = -1;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		uint64_t value = UINT64_MAX;

		assert(ice_stats_read(&device, cases[i], &value) == ENOTSUP);
		assert(value == UINT64_MAX);
		assert(!MUTEX_HELD(&device.ice_rebuild_lock));
		assert(!MUTEX_HELD(&device.ice_stat_lock));
	}
	assert(reads == 0 && checks == 0 && impacts == 0);
	assert(device.ice_stat_port_last_update == 0);
}

static void
cache(void)
{
	uint64_t initial = counters[0].initial;

	reset();
	check_read(MAC_STAT_RBYTES, initial);
	counters[0].raw += 42;
	now += MSEC2NSEC(10) - 1;
	check_read(MAC_STAT_RBYTES, initial);
	assert(reads == ARRAY_SIZE(counters));
	now++;
	check_read(MAC_STAT_RBYTES, initial + 42);
	assert(reads == 2 * ARRAY_SIZE(counters));
}

static void
faults(void)
{
	uint64_t value;

	reset();
	access_status = -1;
	assert(ice_stats_read(&device, MAC_STAT_RBYTES, &value) == EIO);
	assert(impacts == 1 && last_impact == DDI_SERVICE_DEGRADED);
	assert(reads == ARRAY_SIZE(counters));
	assert(!MUTEX_HELD(&device.ice_rebuild_lock));
	assert(!MUTEX_HELD(&device.ice_stat_lock));
	/* A cached sample retains the MAC path's access-error policy. */
	assert(ice_stats_read(&device, MAC_STAT_RBYTES, &value) == EIO);
	assert(impacts == 2 && reads == ARRAY_SIZE(counters));
	/* The private kstat path keeps its existing weaker service policy. */
	ice_stats_check_acc(&device);
	assert(impacts == 3 && last_impact == DDI_SERVICE_UNAFFECTED);
	access_status = DDI_FM_OK;
	check_read(MAC_STAT_RBYTES, counters[0].initial);
	assert(impacts == 3 && checks == 4);
}

static void
reset_baselines(void)
{
	struct ice_hw_port_stats saved_port, saved_prev;
	struct ice_eth_stats saved_vsi, saved_vsi_prev;
	hrtime_t last;
	unsigned int locks_before;
	size_t i;

	reset();
	check_read(MAC_STAT_RBYTES, counters[0].initial);
	memset(&device.ice_stat_vsi_cur, 0x5a, sizeof (saved_vsi));
	memset(&device.ice_stat_vsi_prev, 0x6b, sizeof (saved_vsi_prev));
	saved_port = device.ice_stat_port_cur;
	saved_prev = device.ice_stat_port_prev;
	saved_vsi = device.ice_stat_vsi_cur;
	saved_vsi_prev = device.ice_stat_vsi_prev;
	last = device.ice_stat_port_last_update;
	mutex_enter(&device.ice_rebuild_lock);
	locks_before = lock_entries;
	ice_stats_reset(&device);
	assert(lock_entries == locks_before + 1);
	assert(MUTEX_HELD(&device.ice_rebuild_lock));
	assert(!MUTEX_HELD(&device.ice_stat_lock));
	assert(!device.ice_stat_port_loaded && !device.ice_stat_vsi_loaded);
	assert(device.ice_stat_port_last_update == last);
	assert(memcmp(&device.ice_stat_port_cur, &saved_port,
	    sizeof (saved_port)) == 0);
	assert(memcmp(&device.ice_stat_port_prev, &saved_prev,
	    sizeof (saved_prev)) == 0);
	assert(memcmp(&device.ice_stat_vsi_cur, &saved_vsi,
	    sizeof (saved_vsi)) == 0);
	assert(memcmp(&device.ice_stat_vsi_prev, &saved_vsi_prev,
	    sizeof (saved_vsi_prev)) == 0);
	mutex_exit(&device.ice_rebuild_lock);
	assert(reads == ARRAY_SIZE(counters));
	for (i = 0; i < ARRAY_SIZE(counters); i++)
		counters[i].raw = 9;
	/* Preserve the existing refresh deadline across reset. */
	check_read(MAC_STAT_RBYTES, saved_port.eth.rx_bytes);
	assert(!device.ice_stat_port_loaded && reads == ARRAY_SIZE(counters));
	now += MSEC2NSEC(10);
	check_read(MAC_STAT_RBYTES, saved_port.eth.rx_bytes);
	assert(device.ice_stat_port_loaded && !device.ice_stat_vsi_loaded);
	assert(reads == 2 * ARRAY_SIZE(counters));
	counters[0].raw += 4;
	now += MSEC2NSEC(10);
	check_read(MAC_STAT_RBYTES, saved_port.eth.rx_bytes + 4);
	assert(reads == 3 * ARRAY_SIZE(counters));
}

static void
missing_port(void)
{
	reset();
	device.ice_hw.port_info = NULL;
	device.ice_stat_port_cur.eth.rx_bytes = 29;
	check_read(MAC_STAT_RBYTES, 29);
	assert(reads == 0 && checks == 1);
	assert(device.ice_stat_port_last_update == 0);
}

int
main(void)
{
	selectors();
	unsupported();
	cache();
	faults();
	reset_baselines();
	missing_port();
	(void) puts("PASS: ICE statistics selector/cache/reset ownership");
	return (0);
}

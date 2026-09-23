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
 * Run the firmware diagnostic ioctls as an attacker would: from a zone,
 * without privilege, with wrong sizes and hostile values, and against a
 * firmware that returns too much data.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int boolean_t;
typedef unsigned int uint_t;
typedef int kmutex_t;
typedef int zoneid_t;

#define	B_TRUE			1
#define	B_FALSE			0
#define	ICE_SUCCESS		0
#define	ICE_ERR_NOT_SUPPORTED	-4
#define	ICE_ERR_AQ_ERROR	-100
#define	GLOBAL_ZONEID		0
#define	KM_SLEEP		0
#define	MUTEX_DRIVER		0
#define	MIN(a, b)		((a) < (b) ? (a) : (b))
#undef	bcopy
#undef	bzero
#define	bcopy(s, d, n)		memmove((d), (s), (n))
#define	bzero(p, n)		memset((p), 0, (n))
#define	ICE_FWLOG_OPTION_ARQ_ENA	0x1
#define	ICE_FWLOG_OPTION_UART_ENA	0x2
#define	ICE_FWLOG_OPTION_IS_REGISTERED	0x8
#define	ICE_AQC_DBG_DUMP_CLUSTER_ID_SW_E810	0
#define	ICE_AQC_DBG_DUMP_CLUSTER_ID_SW_E830	100

#include "ice_ioctl.h"

struct ice_fwlog_module_entry {
	u16 module_id;
	u8 log_level;
};
struct ice_fwlog_cfg {
	struct ice_fwlog_module_entry module_entries[ICE_FWLOG_NMODULES];
	u16 options;
	u16 log_resolution;
};
struct ice_hw {
	bool e830;
};
typedef struct cred {
	zoneid_t zone;
	int devices;
	int config;
} cred_t;
struct iocblk {
	int ioc_cmd;
	cred_t *ioc_cr;
	uint_t ioc_count;
};
typedef struct mblk {
	unsigned char *b_rptr;
	struct mblk *b_cont;
} mblk_t;
typedef int queue_t;

typedef struct ice {
	struct ice_hw ice_hw;
	kmutex_t ice_rebuild_lock;
	kmutex_t ice_fwlog_lock;
	uint8_t *ice_fwlog_buf;
	size_t ice_fwlog_head;
	size_t ice_fwlog_len;
	uint32_t ice_fwlog_dropped;
} ice_t;

static ice_t dev;
static struct ice_fwlog_cfg firmware;
static int fw_status, dump_status;
static unsigned fw_calls, registers, unregisters, acks, naks, allocs;
static int nak_error;
static u16 dump_len;

static void
mutex_init(kmutex_t *m, void *n, int t, void *a)
{
	(void) n;
	(void) t;
	(void) a;
	*m = 0;
}

static void
mutex_destroy(kmutex_t *m)
{
	assert(*m == 0);
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

static void *
kmem_zalloc(size_t n, int f)
{
	void *p = calloc(1, n);

	(void) f;
	assert(p != NULL);
	allocs++;
	return (p);
}

static void
kmem_free(void *p, size_t n)
{
	(void) n;
	allocs--;
	free(p);
}

static zoneid_t
crgetzoneid(const cred_t *cr)
{
	return (cr->zone);
}

static int
drv_priv(cred_t *cr)
{
	return (cr->devices ? 0 : EPERM);
}

static int
secpolicy_sys_config(const cred_t *cr, boolean_t quiet)
{
	(void) quiet;
	return (cr->config ? 0 : EPERM);
}

static int
miocpullup(mblk_t *mp, size_t n)
{
	(void) n;
	return (mp->b_cont == NULL ? EINVAL : 0);
}

static void
miocack(queue_t *q, mblk_t *mp, int count, int rval)
{
	struct iocblk *iocp = (struct iocblk *)(void *)mp->b_rptr;

	(void) q;
	(void) rval;
	assert((uint_t)count == iocp->ioc_count);
	acks++;
}

static void
miocnak(queue_t *q, mblk_t *mp, int count, int error)
{
	(void) q;
	(void) mp;
	(void) count;
	nak_error = error;
	naks++;
}

static bool
ice_is_e830(struct ice_hw *hw)
{
	return (hw->e830);
}

/* Installed by ice_fwlog_get() to model a concurrent enable. */
static uint8_t *race;

static int
ice_fwlog_get(struct ice_hw *hw, struct ice_fwlog_cfg *cfg)
{
	(void) hw;
	assert(dev.ice_rebuild_lock == 1);
	if (race != NULL) {
		dev.ice_fwlog_buf = race;
		race = NULL;
	}
	fw_calls++;
	if (fw_status == ICE_SUCCESS)
		*cfg = firmware;
	return (fw_status);
}

static int
ice_fwlog_set(struct ice_hw *hw, struct ice_fwlog_cfg *cfg)
{
	unsigned i;

	(void) hw;
	assert(dev.ice_rebuild_lock == 1);
	for (i = 0; i < ICE_FWLOG_NMODULES; i++)
		assert(cfg->module_entries[i].module_id == i);
	fw_calls++;
	firmware = *cfg;
	return (ICE_SUCCESS);
}

static int
ice_fwlog_register(struct ice_hw *hw)
{
	(void) hw;
	assert(dev.ice_rebuild_lock == 1);
	registers++;
	firmware.options |= ICE_FWLOG_OPTION_IS_REGISTERED;
	return (ICE_SUCCESS);
}

static int
ice_fwlog_unregister(struct ice_hw *hw)
{
	(void) hw;
	assert(dev.ice_rebuild_lock == 1);
	unregisters++;
	firmware.options &= ~ICE_FWLOG_OPTION_IS_REGISTERED;
	return (ICE_SUCCESS);
}

static int
ice_aq_get_internal_data(struct ice_hw *hw, u16 cluster, u16 table,
    u32 start, void *buf, u16 size, u16 *ret_size, u16 *next_cluster,
    u16 *next_table, u32 *next_index, void *cd)
{
	(void) hw;
	(void) cd;
	assert(dev.ice_rebuild_lock == 1);
	assert(size == ICE_IOC_BUFSZ);
	fw_calls++;
	if (dump_status != ICE_SUCCESS)
		return (dump_status);
	memset(buf, 0x5a, MIN(dump_len, size));
	*ret_size = dump_len;
	*next_cluster = cluster;
	*next_table = (u16)(table + 1);
	*next_index = start + 7;
	return (ICE_SUCCESS);
}

#include "diag_ioctl_body.h"

/* One I_STR ioctl.  The payload starts as a canary the reply must replace. */
static struct {
	struct iocblk ioc;
	mblk_t mp, data;
	union {
		ice_ioc_fwlog_cfg_t cfg;
		ice_ioc_fwlog_read_t rd;
		ice_ioc_fwdump_t dump;
		unsigned char raw[sizeof (ice_ioc_fwdump_t) + 8];
	} u;
} req;

static cred_t root = { GLOBAL_ZONEID, 1, 1 };

static int
call(int cmd, cred_t *cr, uint_t count)
{
	queue_t q = 0;

	acks = naks = 0;
	nak_error = 0;
	req.ioc.ioc_cmd = cmd;
	req.ioc.ioc_cr = cr;
	req.ioc.ioc_count = count;
	req.mp.b_rptr = (unsigned char *)&req.ioc;
	req.mp.b_cont = &req.data;
	req.data.b_rptr = req.u.raw;
	req.data.b_cont = NULL;
	assert(ice_diag_ioctl(&dev, &q, &req.mp));
	assert(acks + naks == 1);
	assert(dev.ice_rebuild_lock == 0 && dev.ice_fwlog_lock == 0);
	return (acks == 1 ? 0 : nak_error);
}

static int
set(uint32_t module, uint32_t level, uint32_t flags, uint32_t res)
{
	memset(&req.u, 0, sizeof (req.u));
	req.u.cfg.ifc_module = module;
	req.u.cfg.ifc_level = level;
	req.u.cfg.ifc_flags = flags;
	req.u.cfg.ifc_resolution = res;
	return (call(ICE_IOC_FWLOG_SET, &root, sizeof (req.u.cfg)));
}

static int
dump(uint32_t cluster, uint32_t table)
{
	memset(&req.u, 0xaa, sizeof (req.u));
	req.u.dump.ifd_cluster = cluster;
	req.u.dump.ifd_table = table;
	req.u.dump.ifd_offset = 3;
	return (call(ICE_IOC_FWDUMP, &root, sizeof (req.u.dump)));
}

static void
check_access(void)
{
	cred_t zone = { 5, 1, 1 }, nodev = { GLOBAL_ZONEID, 0, 1 };
	cred_t noconf = { GLOBAL_ZONEID, 1, 0 };
	const int cmds[] = { ICE_IOC_FWLOG_GET, ICE_IOC_FWLOG_SET,
	    ICE_IOC_FWLOG_READ, ICE_IOC_FWDUMP };
	unsigned i;
	queue_t q = 0;

	/* Other commands are left to the caller untouched. */
	req.ioc.ioc_cmd = ICE_IOC | 0x7f;
	req.mp.b_rptr = (unsigned char *)&req.ioc;
	assert(!ice_diag_ioctl(&dev, &q, &req.mp));

	fw_calls = 0;
	for (i = 0; i < sizeof (cmds) / sizeof (cmds[0]); i++) {
		/* A zone fails even with every privilege. */
		assert(call(cmds[i], &zone, sizeof (ice_ioc_fwdump_t)) ==
		    EPERM);
		assert(call(cmds[i], &nodev, sizeof (ice_ioc_fwdump_t)) ==
		    EPERM);
		assert(call(cmds[i], &noconf, sizeof (ice_ioc_fwdump_t)) ==
		    EPERM);
		/* TRANSPARENT and wrong sizes carry no usable payload. */
		assert(call(cmds[i], &root, (uint_t)-1) == EINVAL);
		assert(call(cmds[i], &root, 0) == EINVAL);
		assert(call(cmds[i], &root, sizeof (ice_ioc_fwdump_t) + 1) ==
		    EINVAL);
	}
	assert(call(ICE_IOC_FWLOG_SET, &root, sizeof (req.u.cfg) - 1) ==
	    EINVAL);
	assert(fw_calls == 0);
}

static void
check_fwlog_cfg(void)
{
	fw_calls = 0;
	assert(set(ICE_FWLOG_NMODULES, 1, 0, 1) == EINVAL);
	assert(set(0, ICE_FWLOG_LEVEL_MAX + 1, 0, 1) == EINVAL);
	assert(set(0, 1, 0, 0) == EINVAL);
	assert(set(0, 1, 0, ICE_FWLOG_RES_MAX + 1) == EINVAL);
	assert(set(0, 1, ICE_FWLOG_F_REGISTERED, 1) == EINVAL);
	assert(set(0, 1, 0x80000000u, 1) == EINVAL);
	assert(fw_calls == 0 && allocs == 0);

	/* Firmware UART logging is preserved; ARQ follows the request. */
	firmware.options = ICE_FWLOG_OPTION_UART_ENA;
	assert(set(ICE_FWLOG_MODULE_ALL, 4, ICE_FWLOG_F_ARQ, 10) == 0);
	assert(registers == 1 && dev.ice_fwlog_buf != NULL && allocs == 1);
	assert(firmware.module_entries[31].log_level == 4);
	assert(firmware.log_resolution == 10);
	assert(firmware.options ==
	    (ICE_FWLOG_OPTION_UART_ENA | ICE_FWLOG_OPTION_ARQ_ENA |
	    ICE_FWLOG_OPTION_IS_REGISTERED));

	assert(set(3, 1, 0, 1) == 0);
	assert(unregisters == 1 && firmware.module_entries[3].log_level == 1);
	assert(firmware.module_entries[4].log_level == 4);
	assert((firmware.options & ICE_FWLOG_OPTION_ARQ_ENA) == 0);
	/* The ring stays until detach; a second enable does not leak. */
	assert(set(3, 2, ICE_FWLOG_F_ARQ, 1) == 0 && allocs == 1);
	/* An enable that loses the race frees its own ring. */
	{
		uint8_t *kept = dev.ice_fwlog_buf;

		dev.ice_fwlog_buf = NULL;
		race = kept;
		assert(set(3, 2, ICE_FWLOG_F_ARQ, 1) == 0);
		assert(dev.ice_fwlog_buf == kept && allocs == 1);
	}

	memset(&req.u, 0xaa, sizeof (req.u));
	req.u.cfg.ifc_module = 3;
	assert(call(ICE_IOC_FWLOG_GET, &root, sizeof (req.u.cfg)) == 0);
	assert(req.u.cfg.ifc_level == 2 && req.u.cfg.ifc_resolution == 1);
	assert(req.u.cfg.ifc_flags ==
	    (ICE_FWLOG_F_ARQ | ICE_FWLOG_F_REGISTERED));
	req.u.cfg.ifc_module = ICE_FWLOG_MODULE_ALL;
	assert(call(ICE_IOC_FWLOG_GET, &root, sizeof (req.u.cfg)) ==
	    EINVAL);

	/* Unsupported firmware logging reports ENOTSUP, not EIO. */
	fw_status = ICE_ERR_NOT_SUPPORTED;
	assert(set(0, 1, 0, 1) == ENOTSUP);
	fw_status = ICE_ERR_AQ_ERROR;
	assert(set(0, 1, 0, 1) == EIO);
	fw_status = ICE_SUCCESS;
}

static void
check_fwlog_ring(void)
{
	static uint8_t event[ICE_IOC_BUFSZ];
	size_t i, total = 0;
	uint32_t dropped = 0;

	for (i = 0; i < sizeof (event); i++)
		event[i] = (uint8_t)i;

	/* Fill the ring with 4 KB events; the one that does not fit drops. */
	for (i = 0; i < ICE_FWLOG_RING_SIZE / sizeof (event) + 2; i++)
		ice_diag_fwlog_event(&dev, event, sizeof (event));
	assert(dev.ice_fwlog_len == ICE_FWLOG_RING_SIZE);
	ice_diag_fwlog_event(&dev, event, 0);

	for (;;) {
		memset(&req.u, 0xaa, sizeof (req.u));
		assert(call(ICE_IOC_FWLOG_READ, &root, sizeof (req.u.rd)) ==
		    0);
		dropped += req.u.rd.ifr_dropped;
		if (req.u.rd.ifr_len == 0)
			break;
		assert(req.u.rd.ifr_len == sizeof (event));
		assert(memcmp(req.u.rd.ifr_buf, event, sizeof (event)) == 0);
		total += req.u.rd.ifr_len;
	}
	assert(total == ICE_FWLOG_RING_SIZE && dropped == 2);

	/* A wrapped event reads back intact, and no stale byte follows it. */
	ice_diag_fwlog_event(&dev, event, 100);
	ice_diag_fwlog_event(&dev, event + 100, 3000);
	memset(&req.u, 0xaa, sizeof (req.u));
	assert(call(ICE_IOC_FWLOG_READ, &root, sizeof (req.u.rd)) == 0);
	assert(req.u.rd.ifr_len == 3100);
	assert(memcmp(req.u.rd.ifr_buf, event, 3100) == 0);
	for (i = 3100; i < sizeof (req.u.rd.ifr_buf); i++)
		assert(req.u.rd.ifr_buf[i] == 0);
}

static void
check_fwdump(void)
{
	size_t i;

	fw_calls = 0;
	/* EMP DRAM, AUX registers, queue manager and full CSR are refused. */
	assert(dump(4, 0) == EINVAL && dump(6, 0) == EINVAL);
	assert(dump(9, 0) == EINVAL && dump(21, 0) == EINVAL);
	assert(dump(23, 0) == EINVAL && dump(0xffffffffu, 0) == EINVAL);
	assert(dump(100, 0) == EINVAL);
	assert(dump(0, 0x10000) == EINVAL);
	assert(fw_calls == 0);

	dump_len = 10;
	assert(dump(22, 4) == 0);
	assert(req.u.dump.ifd_len == 10 && req.u.dump.ifd_next_table == 5);
	assert(req.u.dump.ifd_next_offset == 10);
	assert(req.u.dump.ifd_next_cluster == 22);
	for (i = 0; i < sizeof (req.u.dump.ifd_buf); i++)
		assert(req.u.dump.ifd_buf[i] == (i < 10 ? 0x5a : 0));

	/* A firmware length beyond the buffer is an error, not a copy. */
	dump_len = ICE_IOC_BUFSZ + 1;
	assert(dump(0, 0) == EIO);
	dump_len = 10;
	dump_status = ICE_ERR_AQ_ERROR;
	assert(dump(0, 0) == EIO);
	dump_status = ICE_SUCCESS;

	/* E830 clusters start at 100. */
	dev.ice_hw.e830 = true;
	assert(dump(0, 0) == EINVAL && dump(22, 0) == EINVAL);
	assert(dump(100, 0) == 0 && dump(108, 0) == 0);
	assert(dump(104, 0) == EINVAL && dump(109, 0) == EINVAL);
	dev.ice_hw.e830 = false;
}

int
main(void)
{
	ice_diag_init(&dev);
	check_access();
	check_fwlog_cfg();
	check_fwlog_ring();
	check_fwdump();
	ice_diag_fini(&dev);
	assert(allocs == 0);
	(void) puts("PASS: diagnostic ioctl access, bounds and replies");
	return (0);
}

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
#define	ICE_ERR_CFG		-12
#define	ICE_ERR_DOES_NOT_EXIST	-15
#define	ICE_ERR_AQ_ERROR	-100
#define	GLOBAL_ZONEID		0
#define	KM_SLEEP		0
#define	MUTEX_DRIVER		0
#define	MIN(a, b)		((a) < (b) ? (a) : (b))
#define	ASSERT(x)		assert(x)
#define	ASSERT3U(a, op, b)	assert((a) op(b))
#define	MUTEX_HELD(m)		(*(m) != 0)
#define	LE16_TO_CPU(x)		(x)
#define	CPU_TO_LE16(x)		(x)
#define	ICE_AQ_MAX_BUF_LEN	4096
#define	ICE_AQ_FLAG_RD		0x0400
#define	ICE_AQ_FLAG_SI		0x2000
#define	ice_aqc_opc_fw_logs_config	0xFF30
#define	ice_aqc_opc_fw_logs_query	0xFF32
#define	ICE_AQC_FW_LOG_CONF_UART_EN	0x01
#define	ICE_AQC_FW_LOG_CONF_AQ_EN	0x02
#define	ICE_AQC_FW_LOG_QUERY_REGISTERED	0x04
#define	ICE_AQC_FW_LOG_CONF_SET_VALID	0x08
#define	ICE_AQC_FW_LOG_AQ_QUERY		0x04
#undef	bcopy
#undef	bzero
#define	bcopy(s, d, n)		memmove((d), (s), (n))
#define	bzero(p, n)		memset((p), 0, (n))
#define	ICE_AQC_DBG_DUMP_CLUSTER_ID_SW_E810	0
#define	ICE_AQC_DBG_DUMP_CLUSTER_ID_SW_E830	100

#include "ice_ioctl.h"

struct ice_aqc_fw_log {
	u8 cmd_flags;
	u8 rsp_flag;
	u16 fw_rt_msb;
	union {
		struct {
			u16 log_resolution;
			u16 mdl_cnt;
		} cfg;
	} ops;
	u32 addr_high;
	u32 addr_low;
};
struct ice_aq_desc {
	u16 flags;
	u16 opcode;
	u16 datalen;
	union {
		struct ice_aqc_fw_log fw_log;
	} params;
};
struct ice_aqc_fw_log_cfg_resp {
	u16 module_identifier;
	u8 log_level;
	u8 rsvd0;
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
static int fw_status, dump_status;
static unsigned fw_calls, registers, unregisters, acks, naks, allocs;
static unsigned configs;
static int nak_error;
static u16 dump_len;
static unsigned errors;

static void
ice_error(ice_t *ice, const char *fmt, ...)
{
	(void) ice;
	(void) fmt;
	errors++;
}

/*
 * The firmware: the modules it lists, in its own order, and the count it
 * reports.  A hostile count may exceed what it lists.
 */
#define	FW_SLOTS	40
static struct {
	bool supported;
	u16 n;
	u16 count;
	u16 ids[FW_SLOTS];
	u8 levels[FW_SLOTS];
	u16 resolution;
	u8 flags;
	bool short_reply;
} fw;

/* Every module, in reverse order. */
static void
firmware_modules(void)
{
	unsigned i;

	memset(&fw, 0, sizeof (fw));
	fw.supported = true;
	fw.n = fw.count = ICE_FWLOG_NMODULES;
	fw.resolution = 1;
	for (i = 0; i < ICE_FWLOG_NMODULES; i++) {
		fw.ids[i] = (u16)(ICE_FWLOG_NMODULES - 1 - i);
		fw.levels[i] = (u8)(i % 5);
	}
}

/* A short reply: only these modules, as newer or older firmware may send. */
static void
firmware_short(const u16 *ids, unsigned n)
{
	unsigned i;

	firmware_modules();
	fw.n = fw.count = (u16)n;
	for (i = 0; i < n; i++) {
		fw.ids[i] = ids[i];
		fw.levels[i] = (u8)((i + 1) % 5);
	}
}

static int
firmware_slot(unsigned module)
{
	unsigned i;

	for (i = 0; i < fw.n; i++) {
		if (fw.ids[i] == module)
			return ((int)i);
	}
	return (-1);
}

static u8
firmware_level(unsigned module)
{
	int slot = firmware_slot(module);

	assert(slot >= 0);
	return (fw.levels[slot]);
}

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

static void *
kmem_alloc(size_t n, int f)
{
	return (kmem_zalloc(n, f));
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

/* Installed by the query to model a concurrent enable. */
static uint8_t *race;

static bool
ice_fwlog_supported(struct ice_hw *hw)
{
	(void) hw;
	return (fw.supported);
}

static void
ice_fill_dflt_direct_cmd_desc(struct ice_aq_desc *desc, u16 opcode)
{
	memset(desc, 0, sizeof (*desc));
	desc->opcode = opcode;
	desc->flags = ICE_AQ_FLAG_SI;
}

/*
 * Query (0xFF32) and configure (0xFF30).  A configuration may carry only
 * modules the firmware listed, each once.
 */
static int
ice_aq_send_cmd(struct ice_hw *hw, struct ice_aq_desc *desc, void *buf,
    u16 size, void *cd)
{
	struct ice_aqc_fw_log *cmd = &desc->params.fw_log;
	struct ice_aqc_fw_log_cfg_resp *mods = buf;
	unsigned i, n;
	uint32_t sent = 0;

	(void) hw;
	(void) cd;
	assert(dev.ice_rebuild_lock == 1);
	if (race != NULL) {
		dev.ice_fwlog_buf = race;
		race = NULL;
	}
	fw_calls++;
	if (fw_status != ICE_SUCCESS)
		return (fw_status);

	switch (desc->opcode) {
	case ice_aqc_opc_fw_logs_query:
		assert(cmd->cmd_flags == ICE_AQC_FW_LOG_AQ_QUERY);
		assert(size == ICE_AQ_MAX_BUF_LEN && buf != NULL);
		n = MIN(fw.n, size / sizeof (*mods));
		for (i = 0; i < FW_SLOTS && i < size / sizeof (*mods); i++) {
			/* Past the listed count the buffer holds garbage. */
			mods[i].module_identifier = i < n ? fw.ids[i] : 3;
			mods[i].log_level = i < n ? fw.levels[i] : 0xff;
		}
		cmd->ops.cfg.mdl_cnt = fw.count;
		desc->datalen = (u16)(n * sizeof (*mods));
		if (fw.short_reply && n > 0) {
			/* The control queue copies only datalen bytes. */
			desc->datalen -= sizeof (*mods);
			memset(&mods[n - 1], 0, sizeof (*mods));
		}
		cmd->ops.cfg.log_resolution = fw.resolution;
		cmd->cmd_flags = fw.flags;
		return (ICE_SUCCESS);
	case ice_aqc_opc_fw_logs_config:
		assert((desc->flags & ICE_AQ_FLAG_RD) != 0);
		assert((cmd->cmd_flags & ICE_AQC_FW_LOG_CONF_SET_VALID) != 0);
		n = cmd->ops.cfg.mdl_cnt;
		assert(n > 0 && n == fw.n && size == n * sizeof (*mods));
		for (i = 0; i < n; i++) {
			int slot = firmware_slot(mods[i].module_identifier);

			assert(slot >= 0);
			assert((sent & (1u << slot)) == 0);
			assert(mods[i].log_level <= ICE_FWLOG_LEVEL_MAX);
			sent |= 1u << slot;
			fw.levels[slot] = mods[i].log_level;
		}
		fw.resolution = cmd->ops.cfg.log_resolution;
		fw.flags = (u8)((fw.flags & ICE_AQC_FW_LOG_QUERY_REGISTERED) |
		    (cmd->cmd_flags & (ICE_AQC_FW_LOG_CONF_AQ_EN |
		    ICE_AQC_FW_LOG_CONF_UART_EN)));
		configs++;
		return (ICE_SUCCESS);
	default:
		assert(0);
		return (ICE_ERR_AQ_ERROR);
	}
}

static int
ice_fwlog_register(struct ice_hw *hw)
{
	(void) hw;
	assert(dev.ice_rebuild_lock == 1);
	registers++;
	fw.flags |= ICE_AQC_FW_LOG_QUERY_REGISTERED;
	return (ICE_SUCCESS);
}

static int
ice_fwlog_unregister(struct ice_hw *hw)
{
	(void) hw;
	assert(dev.ice_rebuild_lock == 1);
	unregisters++;
	fw.flags &= ~ICE_AQC_FW_LOG_QUERY_REGISTERED;
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

static int
get(uint32_t module)
{
	memset(&req.u, 0xaa, sizeof (req.u));
	req.u.cfg.ifc_module = module;
	return (call(ICE_IOC_FWLOG_GET, &root, sizeof (req.u.cfg)));
}

static void
check_fwlog_cfg(void)
{
	unsigned i, calls;

	firmware_modules();
	fw_calls = 0;
	assert(set(ICE_FWLOG_NMODULES, 1, 0, 1) == EINVAL);
	assert(set(0, ICE_FWLOG_LEVEL_MAX + 1, 0, 1) == EINVAL);
	assert(set(0, 1, 0, 0) == EINVAL);
	assert(set(0, 1, 0, ICE_FWLOG_RES_MAX + 1) == EINVAL);
	assert(set(0, 1, ICE_FWLOG_F_REGISTERED, 1) == EINVAL);
	assert(set(0, 1, 0x80000000u, 1) == EINVAL);
	assert(get(ICE_FWLOG_NMODULES) == EINVAL);
	assert(get(ICE_FWLOG_MODULE_ALL) == EINVAL);
	assert(fw_calls == 0 && allocs == 0);

	/* Firmware UART logging is preserved; ARQ follows the request. */
	fw.flags = ICE_AQC_FW_LOG_CONF_UART_EN;
	assert(set(ICE_FWLOG_MODULE_ALL, 4, ICE_FWLOG_F_ARQ, 10) == 0);
	assert(registers == 1 && dev.ice_fwlog_buf != NULL && allocs == 1);
	for (i = 0; i < ICE_FWLOG_NMODULES; i++)
		assert(fw.levels[i] == 4);
	assert(fw.resolution == 10 && configs == 1);
	assert(fw.flags == (ICE_AQC_FW_LOG_CONF_UART_EN |
	    ICE_AQC_FW_LOG_CONF_AQ_EN | ICE_AQC_FW_LOG_QUERY_REGISTERED));

	assert(set(3, 1, 0, 1) == 0);
	assert(unregisters == 1 && firmware_level(3) == 1);
	assert(firmware_level(4) == 4);

	/* A reply in another order is read and rewritten by module ID. */
	firmware_modules();
	assert(get(3) == 0 && req.u.cfg.ifc_level == firmware_level(3) &&
	    req.u.cfg.ifc_level == (28 % 5));
	assert(set(3, 4, 0, 1) == 0);
	for (i = 0; i < ICE_FWLOG_NMODULES; i++)
		assert(fw.levels[i] == (fw.ids[i] == 3 ? 4 : i % 5));

	/*
	 * A short reply is used as given: the listed modules read and set,
	 * the others are ENOENT and nothing is sent for them.
	 */
	{
		static const u16 ids[] = { 9, 2, 17, 30, 4 };
		unsigned n = sizeof (ids) / sizeof (ids[0]);

		firmware_short(ids, n);
		assert(get(2) == 0 && req.u.cfg.ifc_level == 2);
		assert(get(4) == 0 && req.u.cfg.ifc_level == 0);
		calls = configs;
		errors = 0;
		assert(get(0) == ENOENT && get(3) == ENOENT);
		assert(set(0, 1, 0, 1) == ENOENT && set(31, 1, 0, 1) == ENOENT);
		assert(configs == calls && errors == 0);
		assert(set(17, 4, 0, 1) == 0 && configs == calls + 1);
		for (i = 0; i < n; i++) {
			assert(fw.levels[i] ==
			    (ids[i] == 17 ? 4 : (i + 1) % 5));
		}
		assert(set(ICE_FWLOG_MODULE_ALL, 3, 0, 2) == 0);
		for (i = 0; i < n; i++)
			assert(fw.levels[i] == 3);
		assert(fw.resolution == 2 && configs == calls + 2);

		/* No modules at all: nothing to read or set. */
		firmware_short(ids, 0);
		assert(get(9) == ENOENT);
		assert(set(ICE_FWLOG_MODULE_ALL, 1, 0, 1) == ENOENT);
		assert(configs == calls + 2);
	}

	/*
	 * A count past the module space, a duplicate or unknown ID, or a
	 * level out of range in a listed entry fails before any set.
	 * Entries past the count are not read.
	 */
	{
		static const struct {
			unsigned slot;
			u16 id;
			u8 level;
		} bad[] = {
			{ 5, 7, 0 },			/* slot 24 has ID 7 */
			{ 0, ICE_FWLOG_NMODULES, 0 },	/* out of range */
			{ 9, 0xffff, 0 },
			{ 2, 29, ICE_FWLOG_LEVEL_MAX + 1 },
		};
		static const u16 two[] = { 6, 8 };

		for (i = 0; i <= sizeof (bad) / sizeof (bad[0]) + 1; i++) {
			firmware_modules();
			if (i < sizeof (bad) / sizeof (bad[0])) {
				fw.ids[bad[i].slot] = bad[i].id;
				fw.levels[bad[i].slot] = bad[i].level;
			} else if (i == sizeof (bad) / sizeof (bad[0])) {
				fw.n = ICE_FWLOG_NMODULES;
				fw.count = ICE_FWLOG_NMODULES + 1;
			} else {
				/* A count the returned length cannot hold. */
				fw.short_reply = true;
			}
			errors = 0;
			calls = configs;
			assert(set(1, 1, 0, 1) == EIO);
			assert(errors == 1 && configs == calls);
			assert(get(1) == EIO);
		}

		firmware_short(two, 2);
		assert(get(6) == 0 && get(8) == 0);
		assert(set(8, 2, 0, 1) == 0);
	}

	/*
	 * A resolution outside 1 to 128 or a reply flag the query does not
	 * define is neither returned nor sent back.
	 */
	{
		static const struct {
			u16 resolution;
			u8 flags;
		} bad[] = {
			{ 0, 0 },
			{ ICE_FWLOG_RES_MAX + 1, 0 },
			{ 0xffff, 0 },
			{ 1, ICE_AQC_FW_LOG_CONF_SET_VALID },
			{ 1, 0x80 },
		};

		for (i = 0; i < sizeof (bad) / sizeof (bad[0]); i++) {
			firmware_modules();
			fw.resolution = bad[i].resolution;
			fw.flags = bad[i].flags;
			errors = 0;
			calls = configs;
			assert(get(1) == EIO && errors == 1);
			assert(set(1, 1, 0, 1) == EIO && configs == calls);
		}
		firmware_modules();
		fw.resolution = ICE_FWLOG_RES_MAX;
		fw.flags = ICE_AQC_FW_LOG_CONF_UART_EN |
		    ICE_AQC_FW_LOG_CONF_AQ_EN | ICE_AQC_FW_LOG_QUERY_REGISTERED;
		assert(get(1) == 0);
		assert(req.u.cfg.ifc_resolution == ICE_FWLOG_RES_MAX);
	}

	firmware_modules();
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

	assert(get(3) == 0);
	assert(req.u.cfg.ifc_level == 2 && req.u.cfg.ifc_resolution == 1);
	assert(req.u.cfg.ifc_flags ==
	    (ICE_FWLOG_F_ARQ | ICE_FWLOG_F_REGISTERED));

	/* Unsupported firmware logging reports ENOTSUP, not EIO. */
	fw.supported = false;
	calls = fw_calls;
	assert(set(0, 1, 0, 1) == ENOTSUP && get(0) == ENOTSUP);
	assert(fw_calls == calls);
	fw.supported = true;
	fw_status = ICE_ERR_NOT_SUPPORTED;
	assert(set(0, 1, 0, 1) == ENOTSUP);
	fw_status = ICE_ERR_AQ_ERROR;
	assert(set(0, 1, 0, 1) == EIO && get(0) == EIO);
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

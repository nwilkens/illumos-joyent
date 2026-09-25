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
 * Copyright 2026 RackTop Systems, Inc.
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * Firmware diagnostic ioctls: firmware logging and internal debug dump.  The
 * interface is in ice_ioctl.h.  The command set and structures follow the ice
 * driver by Jason King; the cluster limits follow FreeBSD.
 *
 * Every value from the caller and from firmware is bounded here.  Each
 * command copies its structure into a zeroed kernel buffer, fills it, and
 * copies it back, so a reply never carries kernel memory the command did not
 * write.
 */

#include <sys/cred.h>
#include <sys/policy.h>
#include <sys/stream.h>
#include <sys/strsun.h>
#include <sys/zone.h>

#include "ice.h"
#include "ice_common.h"
#include "ice_ioctl.h"

CTASSERT(ICE_FWLOG_NMODULES == ICE_AQC_FW_LOG_ID_MAX);
CTASSERT(ICE_FWLOG_NMODULES <= 32);
CTASSERT(ICE_FWLOG_LEVEL_MAX + 1 == ICE_FWLOG_LEVEL_INVALID);
CTASSERT(ICE_FWLOG_RES_MIN == ICE_AQC_FW_LOG_MIN_RESOLUTION);
CTASSERT(ICE_FWLOG_RES_MAX == ICE_AQC_FW_LOG_MAX_RESOLUTION);
CTASSERT(ICE_IOC_BUFSZ <= ICE_AQ_MAX_BUF_LEN);
CTASSERT(ICE_IOC_BUFSZ <= UINT16_MAX);

/* The Query FW Logging reply flags (datasheet 0xFF32). */
#define	ICE_DIAG_FWLOG_QUERY_FLAGS	(ICE_AQC_FW_LOG_CONF_UART_EN | \
	ICE_AQC_FW_LOG_CONF_AQ_EN | ICE_AQC_FW_LOG_QUERY_REGISTERED)

/*
 * Debug dump clusters FreeBSD permits (ICE_FW_DEBUG_DUMP_VALID_CLUSTER_MASK_*
 * in ice_lib.h), as bits above the family's first cluster ID.  The excluded
 * clusters dump firmware memory, raw registers, or queue manager state.
 */
#define	ICE_FWDUMP_MASK_E810	0x4001AFu
#define	ICE_FWDUMP_MASK_E830	0x1AFu

void
ice_diag_init(ice_t *ice)
{
	mutex_init(&ice->ice_fwlog_lock, NULL, MUTEX_DRIVER, NULL);
}

/*
 * Runs after the OICR worker is gone, so no event can append to the ring.
 */
void
ice_diag_fini(ice_t *ice)
{
	if (ice->ice_fwlog_buf != NULL) {
		kmem_free(ice->ice_fwlog_buf, ICE_FWLOG_RING_SIZE);
		ice->ice_fwlog_buf = NULL;
	}
	mutex_destroy(&ice->ice_fwlog_lock);
}

/*
 * Queue one firmware log event from the admin receive queue.  An event that
 * does not fit is dropped whole, so the stream never holds part of one.
 */
void
ice_diag_fwlog_event(ice_t *ice, const uint8_t *data, size_t len)
{
	size_t tail, first;

	mutex_enter(&ice->ice_fwlog_lock);
	if (ice->ice_fwlog_buf == NULL || len == 0) {
		mutex_exit(&ice->ice_fwlog_lock);
		return;
	}
	if (len > ICE_FWLOG_RING_SIZE - ice->ice_fwlog_len) {
		if (ice->ice_fwlog_dropped < UINT32_MAX)
			ice->ice_fwlog_dropped++;
		mutex_exit(&ice->ice_fwlog_lock);
		return;
	}

	tail = (ice->ice_fwlog_head + ice->ice_fwlog_len) % ICE_FWLOG_RING_SIZE;
	first = MIN(len, ICE_FWLOG_RING_SIZE - tail);
	bcopy(data, ice->ice_fwlog_buf + tail, first);
	bcopy(data + first, ice->ice_fwlog_buf, len - first);
	ice->ice_fwlog_len += len;
	mutex_exit(&ice->ice_fwlog_lock);
}

static int
ice_diag_fwlog_read(ice_t *ice, ice_ioc_fwlog_read_t *rd)
{
	size_t n, first;

	mutex_enter(&ice->ice_fwlog_lock);
	n = MIN(sizeof (rd->ifr_buf), ice->ice_fwlog_len);
	first = MIN(n, ICE_FWLOG_RING_SIZE - ice->ice_fwlog_head);
	if (n != 0) {
		bcopy(ice->ice_fwlog_buf + ice->ice_fwlog_head, rd->ifr_buf,
		    first);
		bcopy(ice->ice_fwlog_buf, rd->ifr_buf + first, n - first);
	}
	ice->ice_fwlog_head = (ice->ice_fwlog_head + n) % ICE_FWLOG_RING_SIZE;
	ice->ice_fwlog_len -= n;
	rd->ifr_len = (uint32_t)n;
	rd->ifr_dropped = ice->ice_fwlog_dropped;
	ice->ice_fwlog_dropped = 0;
	mutex_exit(&ice->ice_fwlog_lock);

	return (0);
}

/*
 * The logging configuration firmware reports, limited to the modules it
 * listed.  The common code's ice_fwlog_get() fills the entries it was not
 * given with module 0, so the driver issues the query itself to learn the
 * count.
 */
typedef struct ice_diag_fwlog {
	uint16_t	idf_nmods;
	uint16_t	idf_ids[ICE_FWLOG_NMODULES];
	uint8_t		idf_levels[ICE_FWLOG_NMODULES];
	uint16_t	idf_resolution;
	uint8_t		idf_flags;	/* ICE_AQC_FW_LOG_CONF_* */
} ice_diag_fwlog_t;

/* Look up a module in the reply; -1 if firmware did not list it. */
static int
ice_diag_fwlog_find(const ice_diag_fwlog_t *q, uint32_t module)
{
	uint_t i;

	for (i = 0; i < q->idf_nmods; i++) {
		if (q->idf_ids[i] == module)
			return ((int)i);
	}
	return (-1);
}

/*
 * Query the firmware logging configuration (0xFF32).  Firmware need not list
 * the modules in order or list all of them, but each one it lists must be a
 * known module, listed once, at a known level, and the resolution and flags
 * must be ones the command defines.
 */
static int
ice_diag_fwlog_query(ice_t *ice, uint8_t *buf, ice_diag_fwlog_t *q)
{
	struct ice_hw *hw = &ice->ice_hw;
	const struct ice_aqc_fw_log_cfg_resp *resp =
	    (const struct ice_aqc_fw_log_cfg_resp *)buf;
	struct ice_aqc_fw_log *cmd;
	struct ice_aq_desc desc;
	uint32_t seen = 0;
	uint16_t nmods, resolution;
	uint8_t flags;
	uint_t i;
	int status;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));
	if (!ice_fwlog_supported(hw))
		return (ICE_ERR_NOT_SUPPORTED);

	bzero(buf, ICE_AQ_MAX_BUF_LEN);
	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_fw_logs_query);
	cmd = &desc.params.fw_log;
	cmd->cmd_flags = ICE_AQC_FW_LOG_AQ_QUERY;
	status = ice_aq_send_cmd(hw, &desc, buf, ICE_AQ_MAX_BUF_LEN, NULL);
	if (status != ICE_SUCCESS)
		return (status);

	nmods = LE16_TO_CPU(cmd->ops.cfg.mdl_cnt);
	resolution = LE16_TO_CPU(cmd->ops.cfg.log_resolution);
	flags = cmd->cmd_flags;
	/* The common code copies only the returned length into buf. */
	if (nmods > ICE_FWLOG_NMODULES ||
	    LE16_TO_CPU(desc.datalen) < nmods * sizeof (*resp)) {
		ice_error(ice, "firmware lists %u log modules in %u bytes",
		    nmods, LE16_TO_CPU(desc.datalen));
		return (ICE_ERR_CFG);
	}
	if (resolution < ICE_FWLOG_RES_MIN || resolution > ICE_FWLOG_RES_MAX ||
	    (flags & ~ICE_DIAG_FWLOG_QUERY_FLAGS) != 0) {
		ice_error(ice, "firmware log configuration has resolution %u "
		    "and flags 0x%x", resolution, flags);
		return (ICE_ERR_CFG);
	}

	bzero(q, sizeof (*q));
	q->idf_nmods = nmods;
	q->idf_resolution = resolution;
	q->idf_flags = flags;

	for (i = 0; i < q->idf_nmods; i++) {
		uint16_t id = LE16_TO_CPU(resp[i].module_identifier);
		uint8_t level = resp[i].log_level;

		if (id >= ICE_FWLOG_NMODULES || (seen & (1u << id)) != 0 ||
		    level > ICE_FWLOG_LEVEL_MAX) {
			ice_error(ice, "firmware log configuration has an "
			    "invalid entry: module %u level %u", id, level);
			return (ICE_ERR_CFG);
		}
		seen |= 1u << id;
		q->idf_ids[i] = id;
		q->idf_levels[i] = level;
	}

	return (ICE_SUCCESS);
}

/* Set Firmware Logging Configuration (0xFF30) for the listed modules only. */
static int
ice_diag_fwlog_config(ice_t *ice, uint8_t *buf, const ice_diag_fwlog_t *q)
{
	struct ice_hw *hw = &ice->ice_hw;
	struct ice_aqc_fw_log_cfg_resp *mods =
	    (struct ice_aqc_fw_log_cfg_resp *)buf;
	struct ice_aqc_fw_log *cmd;
	struct ice_aq_desc desc;
	uint_t i;

	ASSERT(MUTEX_HELD(&ice->ice_rebuild_lock));
	ASSERT3U(q->idf_nmods, >, 0);
	ASSERT3U(q->idf_nmods, <=, ICE_FWLOG_NMODULES);

	bzero(buf, q->idf_nmods * sizeof (*mods));
	for (i = 0; i < q->idf_nmods; i++) {
		mods[i].module_identifier = CPU_TO_LE16(q->idf_ids[i]);
		mods[i].log_level = q->idf_levels[i];
	}

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_fw_logs_config);
	desc.flags |= CPU_TO_LE16(ICE_AQ_FLAG_RD);
	cmd = &desc.params.fw_log;
	cmd->cmd_flags = ICE_AQC_FW_LOG_CONF_SET_VALID |
	    (q->idf_flags & (ICE_AQC_FW_LOG_CONF_AQ_EN |
	    ICE_AQC_FW_LOG_CONF_UART_EN));
	cmd->ops.cfg.log_resolution = CPU_TO_LE16(q->idf_resolution);
	cmd->ops.cfg.mdl_cnt = CPU_TO_LE16(q->idf_nmods);

	return (ice_aq_send_cmd(hw, &desc, buf,
	    (uint16_t)(q->idf_nmods * sizeof (*mods)), NULL));
}

static int
ice_diag_fwlog_get(ice_t *ice, ice_ioc_fwlog_cfg_t *cfg)
{
	ice_diag_fwlog_t q;
	uint32_t module = cfg->ifc_module;
	uint8_t *buf;
	int status, slot;

	if (module >= ICE_FWLOG_NMODULES)
		return (EINVAL);

	buf = kmem_alloc(ICE_AQ_MAX_BUF_LEN, KM_SLEEP);
	mutex_enter(&ice->ice_rebuild_lock);
	status = ice_diag_fwlog_query(ice, buf, &q);
	mutex_exit(&ice->ice_rebuild_lock);
	kmem_free(buf, ICE_AQ_MAX_BUF_LEN);
	if (status == ICE_ERR_NOT_SUPPORTED)
		return (ENOTSUP);
	if (status != ICE_SUCCESS)
		return (EIO);
	if ((slot = ice_diag_fwlog_find(&q, module)) < 0)
		return (ENOENT);

	bzero(cfg, sizeof (*cfg));
	cfg->ifc_module = module;
	cfg->ifc_level = q.idf_levels[slot];
	cfg->ifc_resolution = q.idf_resolution;
	if ((q.idf_flags & ICE_AQC_FW_LOG_CONF_AQ_EN) != 0)
		cfg->ifc_flags |= ICE_FWLOG_F_ARQ;
	if ((q.idf_flags & ICE_AQC_FW_LOG_QUERY_REGISTERED) != 0)
		cfg->ifc_flags |= ICE_FWLOG_F_REGISTERED;

	return (0);
}

/*
 * Only the modules firmware listed are sent.  A module it did not list is
 * ENOENT, and so is ICE_FWLOG_MODULE_ALL when it listed none.
 */
static int
ice_diag_fwlog_set(ice_t *ice, const ice_ioc_fwlog_cfg_t *cfg)
{
	struct ice_hw *hw = &ice->ice_hw;
	ice_diag_fwlog_t q;
	boolean_t arq = (cfg->ifc_flags & ICE_FWLOG_F_ARQ) != 0;
	uint8_t level = (uint8_t)cfg->ifc_level;
	uint8_t *ring = NULL, *buf;
	int status, slot;
	uint_t i;

	if ((cfg->ifc_module >= ICE_FWLOG_NMODULES &&
	    cfg->ifc_module != ICE_FWLOG_MODULE_ALL) ||
	    cfg->ifc_level > ICE_FWLOG_LEVEL_MAX ||
	    cfg->ifc_resolution < ICE_FWLOG_RES_MIN ||
	    cfg->ifc_resolution > ICE_FWLOG_RES_MAX ||
	    (cfg->ifc_flags & ~ICE_FWLOG_F_ARQ) != 0)
		return (EINVAL);

	/* Allocate before the lock; the ring then lives until detach. */
	if (arq && ice->ice_fwlog_buf == NULL)
		ring = kmem_zalloc(ICE_FWLOG_RING_SIZE, KM_SLEEP);
	buf = kmem_alloc(ICE_AQ_MAX_BUF_LEN, KM_SLEEP);

	mutex_enter(&ice->ice_rebuild_lock);
	status = ice_diag_fwlog_query(ice, buf, &q);
	if (status != ICE_SUCCESS)
		goto out;

	if (cfg->ifc_module == ICE_FWLOG_MODULE_ALL) {
		if (q.idf_nmods == 0) {
			status = ICE_ERR_DOES_NOT_EXIST;
			goto out;
		}
		for (i = 0; i < q.idf_nmods; i++)
			q.idf_levels[i] = level;
	} else {
		if ((slot = ice_diag_fwlog_find(&q, cfg->ifc_module)) < 0) {
			status = ICE_ERR_DOES_NOT_EXIST;
			goto out;
		}
		q.idf_levels[slot] = level;
	}
	q.idf_resolution = (uint16_t)cfg->ifc_resolution;
	q.idf_flags &= ~ICE_AQC_FW_LOG_CONF_AQ_EN;
	if (arq)
		q.idf_flags |= ICE_AQC_FW_LOG_CONF_AQ_EN;

	status = ice_diag_fwlog_config(ice, buf, &q);
	if (status != ICE_SUCCESS)
		goto out;

	/* A concurrent enable can win the race; the loser frees its ring. */
	mutex_enter(&ice->ice_fwlog_lock);
	if (ring != NULL && ice->ice_fwlog_buf == NULL) {
		ice->ice_fwlog_buf = ring;
		ice->ice_fwlog_head = ice->ice_fwlog_len = 0;
		ring = NULL;
	}
	mutex_exit(&ice->ice_fwlog_lock);
	status = arq ? ice_fwlog_register(hw) : ice_fwlog_unregister(hw);

out:
	mutex_exit(&ice->ice_rebuild_lock);
	kmem_free(buf, ICE_AQ_MAX_BUF_LEN);
	if (ring != NULL)
		kmem_free(ring, ICE_FWLOG_RING_SIZE);

	if (status == ICE_ERR_NOT_SUPPORTED)
		return (ENOTSUP);
	if (status == ICE_ERR_DOES_NOT_EXIST)
		return (ENOENT);
	return (status == ICE_SUCCESS ? 0 : EIO);
}

/*
 * Accept only the clusters FreeBSD reads.  The E830 cluster IDs start at a
 * different base.
 */
static boolean_t
ice_diag_cluster_ok(struct ice_hw *hw, uint32_t cluster)
{
	uint32_t base, mask;

	if (ice_is_e830(hw)) {
		base = ICE_AQC_DBG_DUMP_CLUSTER_ID_SW_E830;
		mask = ICE_FWDUMP_MASK_E830;
	} else {
		base = ICE_AQC_DBG_DUMP_CLUSTER_ID_SW_E810;
		mask = ICE_FWDUMP_MASK_E810;
	}

	if (cluster < base || cluster - base >= 32)
		return (B_FALSE);
	return ((mask & (1u << (cluster - base))) != 0);
}

static int
ice_diag_fwdump(ice_t *ice, ice_ioc_fwdump_t *dump)
{
	struct ice_hw *hw = &ice->ice_hw;
	uint16_t len = 0, next_cluster = 0, next_table = 0;
	uint32_t next_offset = 0;
	uint8_t *buf;
	int status;

	if (!ice_diag_cluster_ok(hw, dump->ifd_cluster) ||
	    dump->ifd_table > UINT16_MAX)
		return (EINVAL);

	buf = kmem_zalloc(ICE_IOC_BUFSZ, KM_SLEEP);
	mutex_enter(&ice->ice_rebuild_lock);
	status = ice_aq_get_internal_data(hw, (uint16_t)dump->ifd_cluster,
	    (uint16_t)dump->ifd_table, dump->ifd_offset, buf, ICE_IOC_BUFSZ,
	    &len, &next_cluster, &next_table, &next_offset, NULL);
	mutex_exit(&ice->ice_rebuild_lock);

	/* The returned length is firmware data; it must fit the buffer. */
	if (status != ICE_SUCCESS || len > ICE_IOC_BUFSZ) {
		kmem_free(buf, ICE_IOC_BUFSZ);
		return (EIO);
	}

	bzero(dump->ifd_buf, sizeof (dump->ifd_buf));
	bcopy(buf, dump->ifd_buf, len);
	dump->ifd_len = len;
	dump->ifd_next_cluster = next_cluster;
	dump->ifd_next_table = next_table;
	dump->ifd_next_offset = next_offset;
	kmem_free(buf, ICE_IOC_BUFSZ);

	return (0);
}

/*
 * Firmware logging and debug data are card-wide, so a zone must not reach
 * them even when it owns the link.
 */
static int
ice_diag_priv(cred_t *cr)
{
	if (crgetzoneid(cr) != GLOBAL_ZONEID)
		return (EPERM);
	if (drv_priv(cr) != 0)
		return (EPERM);
	return (secpolicy_sys_config(cr, B_FALSE));
}

/*
 * Handle a diagnostic ioctl.  Returns B_FALSE, and leaves the message alone,
 * when the command is not one of these.
 */
boolean_t
ice_diag_ioctl(ice_t *ice, queue_t *q, mblk_t *mp)
{
	struct iocblk *iocp = (struct iocblk *)(uintptr_t)mp->b_rptr;
	size_t size;
	void *arg;
	int error;

	switch (iocp->ioc_cmd) {
	case ICE_IOC_FWLOG_GET:
	case ICE_IOC_FWLOG_SET:
		size = sizeof (ice_ioc_fwlog_cfg_t);
		break;
	case ICE_IOC_FWLOG_READ:
		size = sizeof (ice_ioc_fwlog_read_t);
		break;
	case ICE_IOC_FWDUMP:
		size = sizeof (ice_ioc_fwdump_t);
		break;
	default:
		return (B_FALSE);
	}

	if ((error = ice_diag_priv(iocp->ioc_cr)) != 0)
		goto nak;
	/* A TRANSPARENT ioctl carries no data; only I_STR is accepted. */
	if (iocp->ioc_count != size) {
		error = EINVAL;
		goto nak;
	}
	if ((error = miocpullup(mp, size)) != 0)
		goto nak;

	arg = kmem_zalloc(size, KM_SLEEP);
	bcopy(mp->b_cont->b_rptr, arg, size);

	switch (iocp->ioc_cmd) {
	case ICE_IOC_FWLOG_GET:
		error = ice_diag_fwlog_get(ice, arg);
		break;
	case ICE_IOC_FWLOG_SET:
		error = ice_diag_fwlog_set(ice, arg);
		break;
	case ICE_IOC_FWLOG_READ:
		bzero(arg, size);
		error = ice_diag_fwlog_read(ice, arg);
		break;
	default:
		error = ice_diag_fwdump(ice, arg);
		break;
	}

	if (error == 0)
		bcopy(arg, mp->b_cont->b_rptr, size);
	kmem_free(arg, size);
	if (error != 0)
		goto nak;

	miocack(q, mp, (int)size, 0);
	return (B_TRUE);

nak:
	miocnak(q, mp, 0, error);
	return (B_TRUE);
}

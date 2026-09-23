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
 * mdb support for ice(4D).  The walker and dcmd structure follows the ice
 * module by Jason King.
 *
 * The driver's structures are read through CTF with mdb_ctf_vread(), so the
 * mdb_* types below name only the members this module uses and a change in
 * layout does not break it.  The descriptor fields come from the imported
 * common code, which does not build outside the kernel, so they are copied
 * here; usr/src/test/ice-tests/mdb_module.py checks that they still match.
 */

#include <sys/mdb_modapi.h>
#include <mdb/mdb_ctf.h>
#include <sys/byteorder.h>
#include <sys/sysmacros.h>

/* ice_state_t (ice.h) */
#define	ICE_STATE_ATTACHED	(1 << 0)
#define	ICE_STATE_RESET_PENDING	(1 << 1)
#define	ICE_STATE_ERROR		(1 << 2)
#define	ICE_STATE_STARTED	(1 << 3)
#define	ICE_STATE_PFR_REQ	(1 << 4)
#define	ICE_STATE_RESET_FAILED	(1 << 5)
#define	ICE_STATE_MDD_PENDING	(1 << 6)

/* Transmit descriptor fields (core/ice_lan_tx_rx.h) */
#define	ICE_TXD_QW1_DTYPE_S	0
#define	ICE_TXD_QW1_DTYPE_M	(0xFULL << ICE_TXD_QW1_DTYPE_S)
#define	ICE_TXD_QW1_CMD_S	4
#define	ICE_TXD_QW1_CMD_M	(0xFFFULL << ICE_TXD_QW1_CMD_S)
#define	ICE_TXD_QW1_OFFSET_S	16
#define	ICE_TXD_QW1_OFFSET_M	(0x3FFFFULL << ICE_TXD_QW1_OFFSET_S)
#define	ICE_TXD_QW1_TX_BUF_SZ_S	34
#define	ICE_TXD_QW1_TX_BUF_SZ_M	(0x3FFFULL << ICE_TXD_QW1_TX_BUF_SZ_S)
#define	ICE_TXD_CTX_QW1_CMD_S	4
#define	ICE_TXD_CTX_QW1_CMD_M	(0x7FULL << ICE_TXD_CTX_QW1_CMD_S)
#define	ICE_TXD_CTX_QW1_TSO_LEN_S	30
#define	ICE_TXD_CTX_QW1_TSO_LEN_M	\
	(0x3FFFFULL << ICE_TXD_CTX_QW1_TSO_LEN_S)
#define	ICE_TXD_CTX_QW1_MSS_S	50
#define	ICE_TXD_CTX_QW1_MSS_M	(0x3FFFULL << ICE_TXD_CTX_QW1_MSS_S)
#define	ICE_TX_DESC_DTYPE_DATA	0x0
#define	ICE_TX_DESC_DTYPE_CTX	0x1
#define	ICE_TX_DESC_DTYPE_DESC_DONE	0xF
#define	ICE_TX_DESC_CMD_EOP	0x0001
#define	ICE_TX_DESC_CMD_RS	0x0002
#define	ICE_TX_CTX_DESC_TSO	0x01

/* Receive flex descriptor write-back fields (core/ice_lan_tx_rx.h) */
#define	ICE_RX_FLEX_DESC_PTYPE_M	0x3FF
#define	ICE_RX_FLX_DESC_PKT_LEN_M	0x3FFF
#define	ICE_RX_FLEX_DESC_STATUS0_DD_S	0
#define	ICE_RX_FLEX_DESC_STATUS0_EOF_S	1
#define	ICE_RX_FLEX_DESC_STATUS0_RXE_S	10

#define	ICE_TX_DESC_SIZE	16
#define	ICE_RX_DESC_SIZE	32

typedef struct mdb_ice {
	int		ice_instance;
	uint32_t	ice_state;
	uint16_t	ice_nqueues;
	uint_t		ice_num_txr;
	uint_t		ice_num_rxr;
	uintptr_t	ice_txr;
	uintptr_t	ice_rxr;
	uint32_t	ice_mtu;
	boolean_t	ice_safe_mode;
	boolean_t	ice_tx_lso_enable;
	boolean_t	ice_led_ident;
	uint64_t	ice_link_speed;
} mdb_ice_t;

typedef struct mdb_ice_tx_ring {
	uint32_t	itxr_index;
	uint32_t	itxr_vec;
	boolean_t	itxr_quiesce;
	boolean_t	itxr_blocked;
	uint_t		itxr_tx_active;
	uintptr_t	itxr_descs;
	uint16_t	itxr_size;
	uint16_t	itxr_avail;
	uint16_t	itxr_head;
	uint16_t	itxr_tail;
	uintptr_t	itxr_tcbs;
} mdb_ice_tx_ring_t;

typedef struct mdb_ice_rx_ring {
	uint32_t	irxr_index;
	uint32_t	irxr_vec;
	boolean_t	irxr_shutdown;
	boolean_t	irxr_started;
	boolean_t	irxr_intr_poll;
	uintptr_t	irxr_descs;
	uint16_t	irxr_size;
	uint16_t	irxr_head;
	uint16_t	irxr_tail;
	uint_t		irxr_nfree;
	uint_t		irxr_nloaned;
} mdb_ice_rx_ring_t;

static const mdb_bitmask_t ice_state_bits[] = {
	{ "ATTACHED", ICE_STATE_ATTACHED, ICE_STATE_ATTACHED },
	{ "RESET_PENDING", ICE_STATE_RESET_PENDING, ICE_STATE_RESET_PENDING },
	{ "ERROR", ICE_STATE_ERROR, ICE_STATE_ERROR },
	{ "STARTED", ICE_STATE_STARTED, ICE_STATE_STARTED },
	{ "PFR_REQ", ICE_STATE_PFR_REQ, ICE_STATE_PFR_REQ },
	{ "RESET_FAILED", ICE_STATE_RESET_FAILED, ICE_STATE_RESET_FAILED },
	{ "MDD_PENDING", ICE_STATE_MDD_PENDING, ICE_STATE_MDD_PENDING },
	{ NULL, 0, 0 }
};

/*
 * Walk the attached instances through the driver's soft state.
 */
static int
ice_walk_init(mdb_walk_state_t *wsp)
{
	uintptr_t addr;

	if (wsp->walk_addr != 0) {
		mdb_warn("::walk ice only supports global walks\n");
		return (WALK_ERR);
	}

	if (mdb_readvar(&addr, "ice_state_p") == -1) {
		mdb_warn("failed to read ice_state_p");
		return (WALK_ERR);
	}

	wsp->walk_addr = addr;
	if (mdb_layered_walk("softstate", wsp) != 0) {
		mdb_warn("failed to walk softstate");
		return (WALK_ERR);
	}

	return (WALK_NEXT);
}

static int
ice_walk_step(mdb_walk_state_t *wsp)
{
	return (wsp->walk_callback(wsp->walk_addr, wsp->walk_layer,
	    wsp->walk_cbdata));
}

static int
ice_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	mdb_ice_t ice;

	if (argc != 0)
		return (DCMD_USAGE);

	if ((flags & DCMD_ADDRSPEC) == 0)
		return (mdb_walk_dcmd("ice", "ice", argc, argv));

	if ((flags & DCMD_PIPE_OUT) != 0) {
		mdb_printf("%#lr\n", addr);
		return (DCMD_OK);
	}

	if (mdb_ctf_vread(&ice, "ice_t", "mdb_ice_t", addr, 0) != 0) {
		mdb_warn("failed to read ice_t at %p", addr);
		return (DCMD_ERR);
	}

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%<u>%-?s %4s %5s %5s %6s %-4s %s%</u>\n", "ADDR",
		    "INST", "QUEUE", "MTU", "SPEED", "OPTS", "STATE");
	}

	mdb_printf("%0?p %4d %5u %5u %6llu %c%c%c  %b\n", addr,
	    ice.ice_instance, ice.ice_nqueues, ice.ice_mtu,
	    (u_longlong_t)ice.ice_link_speed,
	    ice.ice_safe_mode ? 'S' : '-', ice.ice_tx_lso_enable ? 'L' : '-',
	    ice.ice_led_ident ? 'I' : '-', ice.ice_state, ice_state_bits);

	return (DCMD_OK);
}

static void
ice_dcmd_help(void)
{
	mdb_printf("Print the attached ice(4D) instances.  SPEED is the link "
	    "speed in Mb/s.\nOPTS: S = DDP safe mode, L = LSO enabled, "
	    "I = LED identify on.\n");
}

typedef struct ice_walk_rings {
	uintptr_t	iwr_end;
	size_t		iwr_size;
} ice_walk_rings_t;

static int
ice_walk_rings_init(mdb_walk_state_t *wsp, boolean_t tx)
{
	const char *type = tx ? "ice_tx_ring_t" : "ice_rx_ring_t";
	ice_walk_rings_t *iwr;
	mdb_ice_t ice;
	ssize_t size;
	uint_t n;

	if (wsp->walk_addr == 0) {
		mdb_warn("the %s walker needs the address of an ice_t\n",
		    tx ? "ice_txq" : "ice_rxq");
		return (WALK_ERR);
	}

	if (mdb_ctf_vread(&ice, "ice_t", "mdb_ice_t", wsp->walk_addr, 0) !=
	    0) {
		mdb_warn("failed to read ice_t at %p", wsp->walk_addr);
		return (WALK_ERR);
	}

	if ((size = mdb_ctf_sizeof_by_name(type)) <= 0) {
		mdb_warn("failed to find the size of %s", type);
		return (WALK_ERR);
	}

	n = tx ? ice.ice_num_txr : ice.ice_num_rxr;
	iwr = mdb_zalloc(sizeof (*iwr), UM_SLEEP);
	iwr->iwr_size = (size_t)size;
	wsp->walk_addr = tx ? ice.ice_txr : ice.ice_rxr;
	iwr->iwr_end = wsp->walk_addr + n * iwr->iwr_size;
	wsp->walk_data = iwr;

	return (WALK_NEXT);
}

static int
ice_walk_txq_init(mdb_walk_state_t *wsp)
{
	return (ice_walk_rings_init(wsp, B_TRUE));
}

static int
ice_walk_rxq_init(mdb_walk_state_t *wsp)
{
	return (ice_walk_rings_init(wsp, B_FALSE));
}

static int
ice_walk_rings_step(mdb_walk_state_t *wsp)
{
	ice_walk_rings_t *iwr = wsp->walk_data;
	int ret;

	if (wsp->walk_addr >= iwr->iwr_end)
		return (WALK_DONE);

	ret = wsp->walk_callback(wsp->walk_addr, NULL, wsp->walk_cbdata);
	wsp->walk_addr += iwr->iwr_size;
	return (ret);
}

static void
ice_walk_rings_fini(mdb_walk_state_t *wsp)
{
	mdb_free(wsp->walk_data, sizeof (ice_walk_rings_t));
}

static int
ice_txq_cb(uintptr_t addr, const void *data __unused, void *arg __unused)
{
	mdb_ice_tx_ring_t txr;

	if (mdb_ctf_vread(&txr, "ice_tx_ring_t", "mdb_ice_tx_ring_t", addr,
	    0) != 0) {
		mdb_warn("failed to read ice_tx_ring_t at %p", addr);
		return (WALK_ERR);
	}

	mdb_printf("%0?p %4u %4u %5u %5u %5u %5u %6u %s%s\n", addr,
	    txr.itxr_index, txr.itxr_vec, txr.itxr_size,
	    txr.itxr_size - txr.itxr_avail, txr.itxr_head, txr.itxr_tail,
	    txr.itxr_tx_active, txr.itxr_blocked ? "BLOCKED " : "",
	    txr.itxr_quiesce ? "QUIESCED" : "");

	return (WALK_NEXT);
}

static int
ice_txq_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	if (argc != 0 || (flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%<u>%-?s %4s %4s %5s %5s %5s %5s %6s %s%</u>\n",
		    "ADDR", "IDX", "VEC", "SIZE", "INUSE", "HEAD", "TAIL",
		    "ACTIVE", "FLAGS");
	}

	return (mdb_pwalk("ice_txq", ice_txq_cb, NULL, addr) == 0 ?
	    DCMD_OK : DCMD_ERR);
}

static int
ice_rxq_cb(uintptr_t addr, const void *data __unused, void *arg __unused)
{
	mdb_ice_rx_ring_t rxr;

	if (mdb_ctf_vread(&rxr, "ice_rx_ring_t", "mdb_ice_rx_ring_t", addr,
	    0) != 0) {
		mdb_warn("failed to read ice_rx_ring_t at %p", addr);
		return (WALK_ERR);
	}

	mdb_printf("%0?p %4u %4u %5u %5u %5u %5u %6u %s%s%s\n", addr,
	    rxr.irxr_index, rxr.irxr_vec, rxr.irxr_size, rxr.irxr_head,
	    rxr.irxr_tail, rxr.irxr_nfree, rxr.irxr_nloaned,
	    rxr.irxr_started ? "STARTED " : "",
	    rxr.irxr_intr_poll ? "POLL " : "",
	    rxr.irxr_shutdown ? "SHUTDOWN" : "");

	return (WALK_NEXT);
}

static int
ice_rxq_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	if (argc != 0 || (flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%<u>%-?s %4s %4s %5s %5s %5s %5s %6s %s%</u>\n",
		    "ADDR", "IDX", "VEC", "SIZE", "HEAD", "TAIL", "FREE",
		    "LOANED", "FLAGS");
	}

	return (mdb_pwalk("ice_rxq", ice_rxq_cb, NULL, addr) == 0 ?
	    DCMD_OK : DCMD_ERR);
}

static const char *
ice_tx_dtype(uint64_t qw1)
{
	switch ((qw1 & ICE_TXD_QW1_DTYPE_M) >> ICE_TXD_QW1_DTYPE_S) {
	case ICE_TX_DESC_DTYPE_DATA:
		return ("DATA");
	case ICE_TX_DESC_DTYPE_CTX:
		return ("CTX");
	case ICE_TX_DESC_DTYPE_DESC_DONE:
		return ("DONE");
	default:
		return ("?");
	}
}

/*
 * Decode one transmit descriptor.  Data descriptors show the buffer, its
 * length and the header offsets; TSO context descriptors show the TSO length
 * and MSS.
 */
static int
ice_tx_desc_dcmd(uintptr_t addr, uint_t flags, int argc,
    const mdb_arg_t *argv)
{
	uint64_t desc[2], qw1, off, cmd;

	if (argc != 0 || (flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	if (mdb_vread(desc, sizeof (desc), addr) != sizeof (desc)) {
		mdb_warn("failed to read tx descriptor at %p", addr);
		return (DCMD_ERR);
	}
	qw1 = LE_64(desc[1]);

	mdb_printf("%0?p %-4s", addr, ice_tx_dtype(qw1));
	switch ((qw1 & ICE_TXD_QW1_DTYPE_M) >> ICE_TXD_QW1_DTYPE_S) {
	case ICE_TX_DESC_DTYPE_DATA:
		cmd = (qw1 & ICE_TXD_QW1_CMD_M) >> ICE_TXD_QW1_CMD_S;
		off = (qw1 & ICE_TXD_QW1_OFFSET_M) >> ICE_TXD_QW1_OFFSET_S;
		mdb_printf(" buf=%llx len=%llu maclen=%llu iplen=%llu "
		    "l4len=%llu%s%s\n", (u_longlong_t)LE_64(desc[0]),
		    (u_longlong_t)((qw1 & ICE_TXD_QW1_TX_BUF_SZ_M) >>
		    ICE_TXD_QW1_TX_BUF_SZ_S),
		    (u_longlong_t)(off & 0x7f) * 2,
		    (u_longlong_t)((off >> 7) & 0x7f) * 4,
		    (u_longlong_t)((off >> 14) & 0xf) * 4,
		    (cmd & ICE_TX_DESC_CMD_EOP) != 0 ? " EOP" : "",
		    (cmd & ICE_TX_DESC_CMD_RS) != 0 ? " RS" : "");
		break;
	case ICE_TX_DESC_DTYPE_CTX:
		cmd = (qw1 & ICE_TXD_CTX_QW1_CMD_M) >> ICE_TXD_CTX_QW1_CMD_S;
		if ((cmd & ICE_TX_CTX_DESC_TSO) != 0) {
			mdb_printf(" TSO tlen=%llu mss=%llu\n",
			    (u_longlong_t)((qw1 & ICE_TXD_CTX_QW1_TSO_LEN_M) >>
			    ICE_TXD_CTX_QW1_TSO_LEN_S),
			    (u_longlong_t)((qw1 & ICE_TXD_CTX_QW1_MSS_M) >>
			    ICE_TXD_CTX_QW1_MSS_S));
		} else {
			mdb_printf(" cmd=%llx\n", (u_longlong_t)cmd);
		}
		break;
	default:
		mdb_printf(" qw0=%llx qw1=%llx\n", (u_longlong_t)LE_64(desc[0]),
		    (u_longlong_t)qw1);
		break;
	}

	return (DCMD_OK);
}

/*
 * Print every slot of a transmit ring: the control block the slot holds, the
 * descriptor type, and the software head and tail.
 */
static int
ice_tx_ring_dcmd(uintptr_t addr, uint_t flags, int argc,
    const mdb_arg_t *argv)
{
	mdb_ice_tx_ring_t txr;
	uint_t i;

	if (argc != 0 || (flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	if (mdb_ctf_vread(&txr, "ice_tx_ring_t", "mdb_ice_tx_ring_t", addr,
	    0) != 0) {
		mdb_warn("failed to read ice_tx_ring_t at %p", addr);
		return (DCMD_ERR);
	}
	if (txr.itxr_descs == 0 || txr.itxr_tcbs == 0) {
		mdb_warn("ring %p has no descriptor memory\n", addr);
		return (DCMD_ERR);
	}

	mdb_printf("%<u>%5s %-?s %-?s %-4s%</u>\n", "SLOT", "TCB", "DESC",
	    "TYPE");
	for (i = 0; i < txr.itxr_size; i++) {
		uintptr_t tcb, desc = txr.itxr_descs + i * ICE_TX_DESC_SIZE;
		uint64_t qw[2];

		if (mdb_vread(&tcb, sizeof (tcb), txr.itxr_tcbs +
		    i * sizeof (uintptr_t)) != sizeof (tcb) ||
		    mdb_vread(qw, sizeof (qw), desc) != sizeof (qw)) {
			mdb_warn("failed to read slot %u", i);
			return (DCMD_ERR);
		}
		mdb_printf("%5u %0?p %0?p %-4s%s%s\n", i, tcb, desc,
		    ice_tx_dtype(LE_64(qw[1])),
		    i == txr.itxr_head ? " HEAD" : "",
		    i == txr.itxr_tail ? " TAIL" : "");
	}

	return (DCMD_OK);
}

/*
 * Decode a receive flex descriptor after write-back.  Before write-back the
 * same memory holds buffer addresses, so DD tells which view applies.
 */
static int
ice_rx_desc_dcmd(uintptr_t addr, uint_t flags, int argc,
    const mdb_arg_t *argv)
{
	uint8_t d[ICE_RX_DESC_SIZE];
	uint16_t ptype, len, status;

	if (argc != 0 || (flags & DCMD_ADDRSPEC) == 0)
		return (DCMD_USAGE);

	if (mdb_vread(d, sizeof (d), addr) != sizeof (d)) {
		mdb_warn("failed to read rx descriptor at %p", addr);
		return (DCMD_ERR);
	}

	ptype = (d[2] | (d[3] << 8)) & ICE_RX_FLEX_DESC_PTYPE_M;
	len = (d[4] | (d[5] << 8)) & ICE_RX_FLX_DESC_PKT_LEN_M;
	status = d[8] | (d[9] << 8);

	if ((status & (1 << ICE_RX_FLEX_DESC_STATUS0_DD_S)) == 0) {
		uint64_t pkt;

		bcopy(d, &pkt, sizeof (pkt));
		mdb_printf("%0?p posted buf=%llx\n", addr,
		    (u_longlong_t)LE_64(pkt));
		return (DCMD_OK);
	}

	mdb_printf("%0?p done rxdid=%u ptype=%u len=%u status=%#x%s%s\n",
	    addr, d[0], ptype, len, status,
	    (status & (1 << ICE_RX_FLEX_DESC_STATUS0_EOF_S)) != 0 ? " EOF" : "",
	    (status & (1 << ICE_RX_FLEX_DESC_STATUS0_RXE_S)) != 0 ? " RXE" :
	    "");

	return (DCMD_OK);
}

static const mdb_dcmd_t ice_dcmds[] = {
	{ "ice", NULL, "print ice instances", ice_dcmd, ice_dcmd_help },
	{ "ice_txq", ":", "print the transmit rings of an ice_t",
	    ice_txq_dcmd },
	{ "ice_rxq", ":", "print the receive rings of an ice_t",
	    ice_rxq_dcmd },
	{ "ice_tx_ring", ":", "print each slot of an ice_tx_ring_t",
	    ice_tx_ring_dcmd },
	{ "ice_tx_desc", ":", "decode a transmit descriptor",
	    ice_tx_desc_dcmd },
	{ "ice_rx_desc", ":", "decode a receive descriptor",
	    ice_rx_desc_dcmd },
	{ NULL }
};

static const mdb_walker_t ice_walkers[] = {
	{ "ice", "walk ice instances", ice_walk_init, ice_walk_step },
	{ "ice_txq", "walk the transmit rings of an ice_t", ice_walk_txq_init,
	    ice_walk_rings_step, ice_walk_rings_fini },
	{ "ice_rxq", "walk the receive rings of an ice_t", ice_walk_rxq_init,
	    ice_walk_rings_step, ice_walk_rings_fini },
	{ NULL }
};

static const mdb_modinfo_t ice_modinfo = {
	MDB_API_VERSION, ice_dcmds, ice_walkers
};

const mdb_modinfo_t *
_mdb_init(void)
{
	return (&ice_modinfo);
}

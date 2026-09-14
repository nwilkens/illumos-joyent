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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/* Only kernel, DMA, pool-return and MMIO boundaries are substituted. */
typedef unsigned int uint_t;
typedef int boolean_t;
typedef char *caddr_t;
typedef void *ddi_acc_handle_t;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef struct { uint_t frees; } mblk_t;
typedef struct {
	uint64_t dmac_laddress;
	uint32_t dmac_size;
} ddi_dma_cookie_t;
typedef struct fake_dma {
	ddi_dma_cookie_t cookies[3];
	uint_t ncookies, reads, unbinds;
	boolean_t bound, fault;
} *ddi_dma_handle_t;

#define	B_FALSE	0
#define	B_TRUE	1
#define	DDI_FM_OK	0
#define	DDI_DMA_SYNC_FORDEV	1
#define	DDI_DMA_SYNC_FORKERNEL	2
#define	DDI_SERVICE_DEGRADED	3
#define	RING_SIZE	8
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#define	ASSERT(x)	assert(x)
#define	ASSERT0(x)	assert((x) == 0)
/* CSTYLED */
#define	ASSERT3U(a, op, b)	assert((a) op (b))
/* CSTYLED */
#define	VERIFY3U(a, op, b)	assert((a) op (b))
#define	MUTEX_HELD(lock)	(*(lock))
#define	QTX_COMM_DBELL(index)	(0x1000U + (index) * 4)

static uint64_t
little_endian(uint64_t value)
{
	uint64_t result;
	unsigned char bytes[8];
	uint_t i;

	for (i = 0; i < 8; i++)
		bytes[i] = (unsigned char)(value >> (i * 8));
	(void) memcpy(&result, bytes, sizeof (result));
	return (result);
}

static uint64_t
read_le64(const void *ptr)
{
	const unsigned char *bytes = ptr;
	uint64_t value = 0;
	uint_t i;

	for (i = 0; i < 8; i++)
		value |= (uint64_t)bytes[i] << (i * 8);
	return (value);
}

#define	CPU_TO_LE64(value)	little_endian(value)
#define	LE64_TO_CPU(value)	read_le64(&(value))

#include "ice_tx_emit_types.h"

struct ice_hw { uint_t unused; };
typedef struct ice {
	struct ice_hw ice_hw;
	struct { ddi_acc_handle_t ios_reg_handle; } ice_osdep;
	void *ice_dip, *ice_mac_hdl;
	uint32_t ice_state;
} ice_t;
typedef struct ice_tx_ring {
	ice_t *itxr_ice;
	boolean_t itxr_lock, itxr_tcb_lock;
	boolean_t itxr_quiesce, itxr_blocked;
	uint16_t itxr_size, itxr_head, itxr_tail, itxr_avail;
	uint16_t itxr_rs_pidx, itxr_rs_cidx;
	uint16_t itxr_rsq[RING_SIZE];
	uint_t itxr_index, itxr_tcb_nfree;
	ice_tx_ctrl_block_t *itxr_tcbs[RING_SIZE];
	ice_tx_ctrl_block_t *itxr_tcb_free_list[RING_SIZE];
	struct ice_tx_desc itxr_descs[RING_SIZE];
	ice_dma_buffer_t itxr_dma;
	void *itxr_mactxring;
} ice_tx_ring_t;

static struct {
	ice_t ice;
	ice_tx_ring_t ring;
	ice_tx_ctrl_block_t blocks[3], *chain[3];
	ice_dma_buffer_t buffers[2];
	struct fake_dma dma[5];
	ddi_dma_handle_t active;
	ice_tx_ctx_t ctx;
	mblk_t mp;
	struct {
		ddi_dma_handle_t handle;
		off_t offset;
		size_t len;
		uint_t direction;
	} syncs[32];
	uint_t nsyncs, doorbells, impacts, notices, returns[2];
	uint16_t start, end;
	uint_t ndesc;
	boolean_t access_fault;
} f;

static void
mutex_enter(boolean_t *held)
{
	assert(!*held);
	*held = B_TRUE;
}

static void
mutex_exit(boolean_t *held)
{
	assert(*held);
	*held = B_FALSE;
}

static uint_t
ddi_dma_ncookies(ddi_dma_handle_t handle)
{
	assert(handle->bound);
	return (handle->ncookies);
}

static const ddi_dma_cookie_t *
ddi_dma_cookie_get(ddi_dma_handle_t handle, uint_t index)
{
	assert(handle->bound && index < handle->ncookies);
	handle->reads++;
	return (&handle->cookies[index]);
}

static int
ddi_dma_sync(ddi_dma_handle_t handle, off_t offset, size_t len,
    uint_t direction)
{
	uint_t n = f.nsyncs++;

	assert(n < sizeof (f.syncs) / sizeof (f.syncs[0]));
	f.syncs[n].handle = handle;
	f.syncs[n].offset = offset;
	f.syncs[n].len = len;
	f.syncs[n].direction = direction;
	return (0);
}

static int
ice_check_dma_handle(ddi_dma_handle_t handle)
{
	return (handle->fault ? -1 : DDI_FM_OK);
}

static int
ddi_dma_unbind_handle(ddi_dma_handle_t handle)
{
	assert(handle->bound);
	handle->bound = B_FALSE;
	assert(++handle->unbinds == 1);
	return (0);
}

static void
ice_buf_free(ice_dma_buffer_t *buf)
{
	uint_t index;

	assert(buf == &f.buffers[0] || buf == &f.buffers[1]);
	index = (buf == &f.buffers[1]);
	assert(++f.returns[index] == 1);
}

static void
freemsg(mblk_t *mp)
{
	assert(mp == &f.mp);
	assert(++mp->frees == 1);
}

static void
ddi_fm_service_impact(void *dip, int impact)
{
	assert(dip == f.ice.ice_dip && impact == DDI_SERVICE_DEGRADED);
	f.impacts++;
}

static void
atomic_or_32(uint32_t *state, uint32_t bits)
{
	*state |= bits;
}

static void
mac_tx_ring_update(void *mac, void *ring)
{
	assert(mac == f.ice.ice_mac_hdl && ring == f.ring.itxr_mactxring);
	f.notices++;
}

static void
wr32(struct ice_hw *hw, uint_t reg, uint_t value)
{
	assert(hw == &f.ice.ice_hw);
	assert(reg == QTX_COMM_DBELL(f.ring.itxr_index));
	assert(value == f.end && f.ring.itxr_tail == f.end);
	assert(f.ring.itxr_avail == RING_SIZE - f.ndesc);
	assert(f.ring.itxr_rs_pidx == 0);
	assert(f.chain[2]->itcb_mp == &f.mp);
	assert(f.nsyncs >= 4);
	assert(f.syncs[f.nsyncs - 1].handle == &f.dma[0]);
	f.doorbells++;
}

static int
ice_check_acc_handle(ice_t *ice, ddi_acc_handle_t handle)
{
	assert(ice == &f.ice && handle == ice->ice_osdep.ios_reg_handle);
	assert(f.doorbells == 1);
	return (f.access_fault ? -1 : DDI_FM_OK);
}

#include "ice_tx_emit_body.h"

static void
setup(ice_tcb_type_t copy_type, boolean_t lso, uint16_t start,
    boolean_t bound_last)
{
	uint_t i;
	ice_tx_ctrl_block_t *bound;

	(void) memset(&f, 0, sizeof (f));
	f.start = start;
	f.ndesc = 5 + lso;
	f.end = (start + f.ndesc) % RING_SIZE;
	f.ring.itxr_ice = &f.ice;
	f.ring.itxr_lock = B_TRUE;
	f.ring.itxr_size = f.ring.itxr_avail = RING_SIZE;
	f.ring.itxr_head = f.ring.itxr_tail = start;
	f.ring.itxr_rs_cidx = f.ring.itxr_rs_pidx = RING_SIZE - 1;
	f.ring.itxr_index = 2;
	f.ring.itxr_dma.idb_dma_handle = &f.dma[0];
	for (i = 0; i < RING_SIZE; i++)
		f.ring.itxr_rsq[i] = UINT16_MAX;
	f.ctx.itc_use_ctx = lso;
	f.ctx.itc_mss = 512;
	f.ctx.itc_tsolen = 3087;
	f.ctx.itc_data_cmd = ICE_TX_DESC_CMD_IIPT_IPV4_CSUM |
	    ICE_TX_DESC_CMD_L4T_EOFT_TCP;
	f.ctx.itc_data_off = 7 | (5 << 7) | (5 << 14);

	for (i = 0; i < 2; i++) {
		ice_tx_ctrl_block_t *copy = &f.blocks[i * 2];

		f.buffers[i].idb_dma_handle = &f.dma[3 + i];
		f.buffers[i].idb_cookie.dmac_laddress =
		    UINT64_C(0x100000010) + i * 0x200;
		copy->itcb_type = copy_type;
		copy->itcb_buf = &f.buffers[i];
		copy->itcb_len = i == 0 ? 54 : 257;
	}
	bound = &f.blocks[1];
	bound->itcb_type = lso ? ITCB_LSO_BIND : ITCB_BIND;
	bound->itcb_dmah = &f.dma[1];
	bound->itcb_lso_dmah = &f.dma[2];
	bound->itcb_len = 2830;
	f.active = &f.dma[lso ? 2 : 1];
	f.active->bound = B_TRUE;
	/* Distinct inactive cookies catch selecting the wrong bind handle. */
	for (i = 1; i <= 2; i++) {
		f.dma[i].ncookies = 3;
		f.dma[i].cookies[0].dmac_laddress =
		    UINT64_C(0x200000037) + i * 0x10000;
		f.dma[i].cookies[0].dmac_size = 769;
		f.dma[i].cookies[1].dmac_laddress =
		    UINT64_C(0x300000000) + i * 0x10000;
		f.dma[i].cookies[1].dmac_size = 2048;
		f.dma[i].cookies[2].dmac_laddress =
		    UINT64_C(0x4000000f3) + i * 0x10000;
		f.dma[i].cookies[2].dmac_size = 13;
	}
	f.chain[0] = &f.blocks[0];
	f.chain[1] = &f.blocks[bound_last ? 2 : 1];
	f.chain[2] = &f.blocks[bound_last ? 1 : 2];
}

static void
check_sync(uint_t index, ddi_dma_handle_t handle, off_t offset, size_t len,
    uint_t direction)
{
	assert(index < f.nsyncs);
	assert(f.syncs[index].handle == handle);
	assert(f.syncs[index].offset == offset);
	assert(f.syncs[index].len == len);
	assert(f.syncs[index].direction == direction);
}

/* Decode each hardware field independently of the descriptor writer. */
static void
check_data(uint16_t slot, uint64_t address, uint_t len, boolean_t last)
{
	struct ice_tx_desc *desc = &f.ring.itxr_descs[slot];
	uint64_t bits = read_le64(&desc->cmd_type_offset_bsz);
	uint64_t cmd = f.ctx.itc_data_cmd;

	if (last)
		cmd |= ICE_TX_DESC_CMD_EOP | ICE_TX_DESC_CMD_RS;
	assert(read_le64(&desc->buf_addr) == address);
	assert((bits & 0xf) == ICE_TX_DESC_DTYPE_DATA);
	assert(((bits >> 4) & 0xfff) == cmd);
	assert(((bits >> 16) & 0x3ffff) == f.ctx.itc_data_off);
	assert(((bits >> 34) & 0x3fff) == len);
	assert((bits >> 48) == 0);
}

static void
check_emitted(void)
{
	uint16_t slot = f.start;
	uint_t i, j;
	uint_t first_span = MIN(f.ndesc, RING_SIZE - f.start);

	if (f.ctx.itc_use_ctx) {
		struct ice_tx_desc *desc = &f.ring.itxr_descs[slot];
		uint64_t bits = read_le64(&desc->cmd_type_offset_bsz);
		uint64_t expected = ICE_TX_DESC_DTYPE_CTX |
		    ((uint64_t)ICE_TX_CTX_DESC_TSO << 4) |
		    ((uint64_t)f.ctx.itc_tsolen << 30) |
		    ((uint64_t)f.ctx.itc_mss << 50);

		assert(bits == expected && read_le64(&desc->buf_addr) == 0);
		assert(f.ring.itxr_tcbs[slot] == NULL);
		slot = (slot + 1) % RING_SIZE;
	}
	for (i = 0; i < 3; i++) {
		ice_tx_ctrl_block_t *tcb = f.chain[i];
		boolean_t bound = (tcb == &f.blocks[1]);
		uint_t count = bound ? 3 : 1;
		ddi_dma_handle_t handle = bound ? f.active :
		    tcb->itcb_buf->idb_dma_handle;

		check_sync(i, handle, 0, 0, DDI_DMA_SYNC_FORDEV);
		assert(tcb->itcb_mp == (i == 2 ? &f.mp : NULL));
		for (j = 0; j < count; j++) {
			uint64_t address = bound ?
			    f.active->cookies[j].dmac_laddress :
			    tcb->itcb_buf->idb_cookie.dmac_laddress;
			uint_t len = bound ? f.active->cookies[j].dmac_size :
			    tcb->itcb_len;

			check_data(slot, address, len,
			    i == 2 && j == count - 1);
			assert(f.ring.itxr_tcbs[slot] == (j == 0 ? tcb : NULL));
			slot = (slot + 1) % RING_SIZE;
		}
	}
	assert(slot == f.end && f.ring.itxr_tail == f.end);
	assert(f.active->reads == 3);
	assert(f.dma[f.active == &f.dma[1] ? 2 : 1].reads == 0);
	assert(f.ring.itxr_rsq[RING_SIZE - 1] ==
	    (f.end + RING_SIZE - 1) % RING_SIZE);
	assert(f.ring.itxr_rs_pidx == 0);
	assert(f.ring.itxr_rs_cidx == RING_SIZE - 1);
	assert(f.ring.itxr_avail == RING_SIZE - f.ndesc);
	check_sync(3, &f.dma[0], f.start * sizeof (struct ice_tx_desc),
	    first_span * sizeof (struct ice_tx_desc), DDI_DMA_SYNC_FORDEV);
	if (first_span != f.ndesc) {
		check_sync(4, &f.dma[0], 0,
		    (f.ndesc - first_span) * sizeof (struct ice_tx_desc),
		    DDI_DMA_SYNC_FORDEV);
	}
	assert(f.nsyncs == (first_span == f.ndesc ? 4U : 5U));
	assert(f.doorbells == 1 && f.mp.frees == 0);
	assert(f.ring.itxr_tcb_nfree == 0 && f.active->unbinds == 0);
}

static void
check_released(void)
{
	uint_t i, j;

	assert(f.ring.itxr_tcb_nfree == 3);
	assert(f.active->unbinds == 1 && !f.active->bound);
	assert(f.dma[f.active == &f.dma[1] ? 2 : 1].unbinds == 0);
	assert(f.returns[0] == 1 && f.returns[1] == 1);
	assert(f.mp.frees == 1);
	for (i = 0; i < 3; i++) {
		uint_t found = 0;

		assert(f.blocks[i].itcb_type == ITCB_NOT_USED);
		assert(f.blocks[i].itcb_len == 0);
		assert(f.blocks[i].itcb_mp == NULL);
		assert(f.blocks[i].itcb_buf == NULL);
		for (j = 0; j < 3; j++)
			found += f.ring.itxr_tcb_free_list[j] == &f.blocks[i];
		assert(found == 1);
	}
}

static void
check_empty_slots(void)
{
	uint_t i;

	for (i = 0; i < RING_SIZE; i++) {
		assert(f.ring.itxr_tcbs[i] == NULL);
		assert(f.ring.itxr_descs[i].buf_addr == 0);
		assert(f.ring.itxr_descs[i].cmd_type_offset_bsz == 0);
	}
}

static void
complete_packet(void)
{
	uint16_t last = (f.end + RING_SIZE - 1) % RING_SIZE;
	uint64_t bits;
	uint_t before;

	/* Completion before the packet's RS descriptor must free nothing. */
	assert(ice_tx_recycle(&f.ring) == 0);
	assert(f.ring.itxr_tcb_nfree == 0 && f.mp.frees == 0);
	bits = read_le64(&f.ring.itxr_descs[last].cmd_type_offset_bsz);
	f.ring.itxr_descs[last].cmd_type_offset_bsz =
	    little_endian((bits & ~UINT64_C(0xf)) | 0xf);
	f.ring.itxr_blocked = B_TRUE;
	before = f.nsyncs;
	assert(ice_tx_recycle(&f.ring) == f.ndesc);
	check_sync(before, &f.dma[0], last * sizeof (struct ice_tx_desc),
	    sizeof (struct ice_tx_desc), DDI_DMA_SYNC_FORKERNEL);
	assert(f.ring.itxr_head == f.end && f.ring.itxr_avail == RING_SIZE);
	assert(f.ring.itxr_rs_cidx == f.ring.itxr_rs_pidx);
	assert(!f.ring.itxr_blocked && f.notices == 1);
	check_released();
	check_empty_slots();
	assert(ice_tx_recycle(&f.ring) == 0);
	check_released();
}

static void
success_cases(void)
{
	const ice_tcb_type_t types[] = {
		ITCB_SMALL_COPY, ITCB_COPY, ITCB_LSO_COPY
	};
	uint_t mode, wrap, bound_last;

	for (mode = 0; mode < 3; mode++) {
		for (wrap = 0; wrap < 2; wrap++) {
			for (bound_last = 0; bound_last < 2; bound_last++) {
				setup(types[mode], mode == 2, wrap ? 6 : 1,
				    bound_last);
				assert(ice_tx_emit(&f.ring, f.chain, 3, f.ndesc,
				    &f.mp, &f.ctx));
				check_emitted();
				assert(f.impacts == 0 && f.ice.ice_state == 0);
				complete_packet();
			}
		}
	}
}

static void
failure_cases(void)
{
	uint_t lso, fault, i;

	/* Fail copy, bind, or descriptor DMA sync before publication. */
	for (lso = 0; lso < 2; lso++) {
		for (fault = 0; fault < 3; fault++) {
			setup(lso ? ITCB_LSO_COPY : ITCB_COPY, lso, 6, B_FALSE);
			(fault == 0 ? &f.dma[3] : fault == 1 ? f.active :
			    &f.dma[0])->fault = B_TRUE;
			assert(!ice_tx_emit(&f.ring, f.chain, 3, f.ndesc,
			    &f.mp, &f.ctx));
			assert(f.doorbells == 0 && f.ring.itxr_tail == f.start);
			assert(f.ring.itxr_avail == RING_SIZE);
			assert(f.ring.itxr_rs_pidx == RING_SIZE - 1);
			assert(f.ring.itxr_rs_cidx == RING_SIZE - 1);
			assert(f.ring.itxr_rsq[RING_SIZE - 1] == UINT16_MAX);
			assert(f.ring.itxr_tcb_nfree == 0 && f.mp.frees == 0);
			assert(f.impacts == 1 &&
			    (f.ice.ice_state & ICE_STATE_ERROR) != 0);
			assert(f.nsyncs == (fault == 0 ? 1U : fault == 1 ?
			    2U : 5U));
			check_empty_slots();
			/* The caller still owns every TCB and the message. */
			for (i = 0; i < 3; i++) {
				assert(f.chain[i]->itcb_mp == NULL);
				ice_tcb_free(&f.ring, f.chain[i]);
			}
			freemsg(&f.mp);
			check_released();
		}
		setup(lso ? ITCB_LSO_COPY : ITCB_COPY, lso, 6, B_TRUE);
		f.access_fault = B_TRUE;
		assert(ice_tx_emit(&f.ring, f.chain, 3, f.ndesc,
		    &f.mp, &f.ctx));
		check_emitted();
		assert(f.impacts == 1 &&
		    (f.ice.ice_state & ICE_STATE_ERROR) != 0);
		complete_packet();
	}
}

int
main(void)
{
	success_cases();
	failure_cases();
	(void) puts("PASS: ICE TX cookies, wrap, completion ownership "
	    "and failures");
	return (0);
}

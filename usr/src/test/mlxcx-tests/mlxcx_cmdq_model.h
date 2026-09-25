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
 * Copyright 2026 MNX Cloud, Inc.
 */

/*
 * A scripted model of the HCA command interface. The test decides, per
 * doorbell, when and how the device answers, and may inject stray or
 * hostile completion events. The model checks the ownership rules that the
 * driver must follow: a doorbell only for an entry the driver handed over,
 * never for a slot the device still owns, and no device write to freed DMA
 * memory.
 */

#ifndef _MLXCX_CMDQ_MODEL_H
#define	_MLXCX_CMDQ_MODEL_H

/* Registers. */
static uint32_t model_cmd_low = (5 << 4) | 6;	/* 32 entries, 64 bytes */
static uint32_t model_init_busy;
static uint64_t model_q_pa;

/* Per slot device state. */
typedef struct {
	int		ms_busy;
	uint8_t		ms_token;
	uint16_t	ms_op;
	uint16_t	ms_opmod;
	uint32_t	ms_inlen;
	uint32_t	ms_outlen;
	uint64_t	ms_in_mbox;
	uint64_t	ms_out_mbox;
	uint8_t		ms_in[64 * 1024];
} model_slot_t;

static model_slot_t model_slots[32];
static uint_t model_doorbells;
static boolean_t model_eq;
static mlxcx_t *model_mlxp;

/* Scheduled device events. */
typedef enum {
	EV_COMPLETE,
	EV_EQE,
	EV_CALL
} model_ev_type_t;

typedef struct {
	clock_t		me_when;
	model_ev_type_t	me_type;
	uint_t		me_slot;
	uint32_t	me_vec;
	void		(*me_func)(void);
} model_ev_t;

static model_ev_t model_evs[256];
static uint_t model_nevs;

static void
model_schedule(clock_t when, model_ev_type_t type, uint_t slot, uint32_t vec,
    void (*func)(void))
{
	if (model_nevs == 256)
		stub_fail("model event overflow");
	model_evs[model_nevs].me_when = when;
	model_evs[model_nevs].me_type = type;
	model_evs[model_nevs].me_slot = slot;
	model_evs[model_nevs].me_vec = vec;
	model_evs[model_nevs].me_func = func;
	model_nevs++;
}

/* Test hooks. */
static void model_on_doorbell(uint_t slot, model_slot_t *ms, uint_t seq);
static void model_output(uint_t slot, model_slot_t *ms, uint8_t *out,
    uint8_t *delivery);

static uint8_t *
model_entry(uint_t slot)
{
	uint_t stride_l2 = model_cmd_low & 0xf;

	return (stub_dma_va(model_q_pa + ((uint64_t)slot << stride_l2),
	    sizeof (mlxcx_cmd_ent_t), B_TRUE));
}

static void
model_gather_input(model_slot_t *ms, mlxcx_cmd_ent_t *ent)
{
	uint32_t rem = ms->ms_inlen, copy, off = 0, blk = 0;
	uint64_t pa = ms->ms_in_mbox;

	if (rem > sizeof (ms->ms_in))
		stub_fail("model input too long: %u", rem);
	copy = MIN(rem, MLXCX_CMD_INLINE_INPUT_LEN);
	memcpy(ms->ms_in, ent->mce_input, copy);
	rem -= copy;
	off += copy;
	while (rem > 0) {
		mlxcx_cmd_mailbox_t *mb;

		if (pa == 0)
			stub_fail("input mailbox chain too short");
		mb = stub_dma_va(pa, sizeof (*mb), B_FALSE);
		if (from_be32(mb->mlxb_blockno) != blk)
			stub_fail("input mailbox block number wrong");
		if (mb->mlxb_token != ms->ms_token)
			stub_fail("input mailbox token wrong");
		copy = MIN(rem, MLXCX_CMD_MAILBOX_LEN);
		memcpy(ms->ms_in + off, mb->mlxb_data, copy);
		rem -= copy;
		off += copy;
		pa = from_be64(mb->mlxb_nextp);
		blk++;
	}
}

static void
model_doorbell(uint32_t val)
{
	uint_t slot;

	for (slot = 0; slot < 32; slot++) {
		model_slot_t *ms = &model_slots[slot];
		mlxcx_cmd_ent_t *ent;

		if ((val & (1U << slot)) == 0)
			continue;
		if (slot >= (1U << ((model_cmd_low >> 4) & 0xf)))
			stub_fail("doorbell for slot %u outside the queue",
			    slot);
		if (ms->ms_busy)
			stub_fail("doorbell for slot %u, which the device "
			    "still owns (slot aliasing)", slot);
		ent = (mlxcx_cmd_ent_t *)model_entry(slot);
		if ((ent->mce_status & MLXCX_CMD_HW_OWNED) == 0)
			stub_fail("doorbell for slot %u without ownership",
			    slot);
		memset(ms, 0, offsetof(model_slot_t, ms_in));
		ms->ms_busy = 1;
		ms->ms_token = ent->mce_token;
		ms->ms_inlen = from_be32(ent->mce_in_length);
		ms->ms_outlen = from_be32(ent->mce_out_length);
		ms->ms_in_mbox = from_be64(ent->mce_in_mbox);
		ms->ms_out_mbox = from_be64(ent->mce_out_mbox);
		model_gather_input(ms, ent);
		ms->ms_op = (ms->ms_in[0] << 8) | ms->ms_in[1];
		ms->ms_opmod = (ms->ms_in[6] << 8) | ms->ms_in[7];
		model_on_doorbell(slot, ms, model_doorbells++);
	}
}

/* Finish the command in a slot: write the output, then hand it back. */
static void
model_finish(uint_t slot)
{
	model_slot_t *ms = &model_slots[slot];
	mlxcx_cmd_ent_t *ent;
	uint8_t *out;
	uint8_t delivery = 0;
	uint32_t rem, copy, off = 0, blk = 0;
	uint64_t pa;

	if (!ms->ms_busy)
		stub_fail("model finished an idle slot %u", slot);
	out = calloc(1, ms->ms_outlen + MLXCX_CMD_MAILBOX_LEN);
	model_output(slot, ms, out, &delivery);

	ent = (mlxcx_cmd_ent_t *)model_entry(slot);
	rem = ms->ms_outlen;
	copy = MIN(rem, MLXCX_CMD_INLINE_OUTPUT_LEN);
	memcpy(ent->mce_output, out, copy);
	rem -= copy;
	off += copy;
	pa = ms->ms_out_mbox;
	while (rem > 0) {
		mlxcx_cmd_mailbox_t *mb;

		if (pa == 0)
			stub_fail("output mailbox chain too short");
		mb = stub_dma_va(pa, sizeof (*mb), B_TRUE);
		if (from_be32(mb->mlxb_blockno) != blk)
			stub_fail("output mailbox block number wrong");
		copy = MIN(rem, MLXCX_CMD_MAILBOX_LEN);
		memcpy(mb->mlxb_data, out + off, copy);
		rem -= copy;
		off += copy;
		pa = from_be64(mb->mlxb_nextp);
		blk++;
	}
	free(out);
	ent->mce_status = delivery << 1;
	ms->ms_busy = 0;
}

static void
model_eqe(uint32_t vec)
{
	mlxcx_eventq_ent_t eqe;

	memset(&eqe, 0, sizeof (eqe));
	eqe.mleqe_event_type = MLXCX_EVENT_CMD_COMPLETION;
	eqe.mleqe_cmd_completion.mled_cmd_completion_vec = to_be32(vec);
	mlxcx_cmd_completion(model_mlxp, &eqe);
}

static boolean_t
stub_sleep_hook(clock_t deadline)
{
	uint_t i, best = UINT_MAX;
	model_ev_t ev;

	for (i = 0; i < model_nevs; i++) {
		if (deadline >= 0 && model_evs[i].me_when > deadline)
			continue;
		if (best == UINT_MAX ||
		    model_evs[i].me_when < model_evs[best].me_when)
			best = i;
	}
	if (best == UINT_MAX)
		return (B_FALSE);
	ev = model_evs[best];
	model_evs[best] = model_evs[--model_nevs];
	if (ev.me_when > stub_now)
		stub_now = ev.me_when;

	switch (ev.me_type) {
	case EV_COMPLETE:
		model_finish(ev.me_slot);
		if (model_eq)
			model_eqe(1U << ev.me_slot);
		break;
	case EV_EQE:
		model_eqe(ev.me_vec);
		break;
	case EV_CALL:
		ev.me_func();
		break;
	}
	return (B_TRUE);
}

static uint32_t
mlxcx_get32(mlxcx_t *mlxp, uintptr_t off)
{
	(void) mlxp;
	switch (off) {
	case MLXCX_ISS_FIRMWARE:
		return ((22 << 16) | 16);
	case MLXCX_ISS_FW_CMD:
		return ((MLXCX_CMD_REVISION << 16) | 1000);
	case MLXCX_ISS_CMD_LOW:
		return (model_cmd_low);
	case MLXCX_ISS_INIT:
		return (model_init_busy << 31);
	default:
		return (0);
	}
}

static void
mlxcx_put32(mlxcx_t *mlxp, uintptr_t off, uint32_t val)
{
	(void) mlxp;
	switch (off) {
	case MLXCX_ISS_CMD_HIGH:
		model_q_pa = (model_q_pa & UINT32_MAX) | ((uint64_t)val << 32);
		break;
	case MLXCX_ISS_CMD_LOW:
		model_q_pa = (model_q_pa & ~(uint64_t)UINT32_MAX) |
		    (val & ~0xfffU);
		break;
	case MLXCX_ISS_CMD_DOORBELL:
		model_doorbell(val);
		break;
	default:
		break;
	}
}

/* Common setup: a command queue in the requested mode. */
static mlxcx_t model_mlx;

static void
model_attach(boolean_t events)
{
	model_mlxp = &model_mlx;
	if (!mlxcx_cmd_queue_init(model_mlxp))
		stub_fail("mlxcx_cmd_queue_init failed");
	if (events) {
		mlxcx_cmd_eq_enable(model_mlxp);
		model_eq = B_TRUE;
	}
}

/* Complete the command after the given number of ticks. */
static void
model_complete_in(uint_t slot, clock_t ticks)
{
	model_schedule(stub_now + ticks, EV_COMPLETE, slot, 0, NULL);
}

static void
model_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static void
model_put_be64(uint8_t *p, uint64_t v)
{
	model_put_be32(p, v >> 32);
	model_put_be32(p + 4, (uint32_t)v);
}

static uint32_t
model_get_be32(const uint8_t *p)
{
	return (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3]);
}

#endif /* _MLXCX_CMDQ_MODEL_H */

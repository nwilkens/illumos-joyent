// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2004-2007 Intel Corporation.  All rights reserved.
 * Copyright (c) 2004 Topspin Corporation.  All rights reserved.
 * Copyright (c) 2004, 2005 Voltaire Corporation.  All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2019, Mellanox Technologies inc.  All rights reserved.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * The IB CM connection states.  Each case names the Linux cm.c handler
 * whose rule it follows; the retransmit and timewait times come from
 * cm_convert_to_ms() and cm_ack_timeout().
 */

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#endif

#include "rdk_ibcm_fsm.h"

static const char *const rdk_ibcm_state_names[] = {
	[IBCS_IDLE] = "idle",
	[IBCS_REQ_SENT] = "req sent",
	[IBCS_MRA_REQ_RCVD] = "mra req rcvd",
	[IBCS_REP_RCVD] = "rep rcvd",
	[IBCS_REQ_RCVD] = "req rcvd",
	[IBCS_REP_SENT] = "rep sent",
	[IBCS_MRA_REP_RCVD] = "mra rep rcvd",
	[IBCS_ESTABLISHED] = "established",
	[IBCS_DREQ_SENT] = "dreq sent",
	[IBCS_TIMEWAIT] = "timewait",
	[IBCS_DONE] = "done"
};

const char *
rdk_ibcm_state_name(rdk_ibcm_state_t s)
{
	if ((unsigned int)s < sizeof (rdk_ibcm_state_names) /
	    sizeof (rdk_ibcm_state_names[0]))
		return (rdk_ibcm_state_names[s]);
	return ("?");
}

/* cm_convert_to_ms(): 4.096 us times 2^t, roughly, in ms. */
uint32_t
rdk_ibcm_time_ms(uint8_t t)
{
	if (t > IBCM_TIME_MAX)
		t = IBCM_TIME_MAX;
	return (1U << (t > 8 ? t - 8 : 0));
}

/*
 * cm_ack_timeout(): 4.096 us x 2^ack = 4.096 us x 2^delay + 2 x 4.096 us x
 * 2^life, rounded to one of the two terms.
 */
uint8_t
rdk_ibcm_ack_timeout(uint8_t ca_ack_delay, uint8_t life)
{
	int ack = life + 1;

	if (ack >= ca_ack_delay)
		ack += (ca_ack_delay >= ack - 1);
	else
		ack = ca_ack_delay + (ack >= ca_ack_delay - 1);
	return ((uint8_t)(ack > IBCM_TIME_MAX ? IBCM_TIME_MAX : ack));
}

void
rdk_ibcm_fsm_init(rdk_ibcm_fsm_t *f, boolean_t active, uint8_t max_retries,
    uint32_t resp_ms, uint32_t tw_ms, uint32_t life_ms)
{
	bzero(f, sizeof (*f));
	f->f_state = active ? IBCS_IDLE : IBCS_REQ_RCVD;
	f->f_active = active;
	f->f_max_retries = max_retries > 15 ? 15 : max_retries;
	f->f_resp_ms = resp_ms;
	f->f_tw_ms = tw_ms;
	f->f_life_ms = life_ms;
}

static void
rdk_ibcm_ev(rdk_ibcm_fsm_t *f, rdk_ibcm_act_t *a, rdk_ibcm_ev_t ev,
    int status, boolean_t final)
{
	if (!f->f_attached)
		return;
	a->ia_ev = ev;
	a->ia_status = status;
	if (final) {
		a->ia_final = B_TRUE;
		f->f_attached = B_FALSE;
	}
}

static void
rdk_ibcm_send(rdk_ibcm_fsm_t *f, rdk_ibcm_act_t *a, rdk_ibcm_send_t msg,
    boolean_t timed)
{
	a->ia_send = msg;
	if (timed) {
		f->f_retries = f->f_max_retries;
		a->ia_timer_ms = f->f_resp_ms;
	}
}

static void
rdk_ibcm_rej(rdk_ibcm_act_t *a, uint16_t reason, uint8_t msg)
{
	a->ia_send = IBCM_SEND_REJ;
	a->ia_rej_reason = reason;
	a->ia_rej_msg = msg;
}

/* cm_enter_timewait() */
static void
rdk_ibcm_timewait(rdk_ibcm_fsm_t *f, rdk_ibcm_act_t *a)
{
	f->f_state = IBCS_TIMEWAIT;
	a->ia_timer_stop = B_TRUE;
	a->ia_timer_ms = f->f_tw_ms;
	a->ia_drop_remote = B_TRUE;
}

/* cm_reset_to_idle(): nothing more is owed to the peer. */
static void
rdk_ibcm_done(rdk_ibcm_fsm_t *f, rdk_ibcm_act_t *a)
{
	f->f_state = IBCS_DONE;
	a->ia_timer_stop = B_TRUE;
	a->ia_drop_remote = B_TRUE;
	a->ia_free = B_TRUE;
}

/* A retransmit timer went off: send again, or give up. */
static boolean_t
rdk_ibcm_retry(rdk_ibcm_fsm_t *f, rdk_ibcm_act_t *a)
{
	if (f->f_retries == 0)
		return (B_FALSE);
	f->f_retries--;
	a->ia_resend = B_TRUE;
	a->ia_timer_ms = f->f_resp_ms;
	return (B_TRUE);
}

/* cm_destroy_id(), and cm_process_send_error() for the deadline. */
static void
rdk_ibcm_abort(rdk_ibcm_fsm_t *f, rdk_ibcm_act_t *a, boolean_t qp)
{
	switch (f->f_state) {
	case IBCS_REQ_SENT:
	case IBCS_MRA_REQ_RCVD:
		rdk_ibcm_rej(a, IBCM_REJ_TIMEOUT, IBCM_MSG_RESPONSE_OTHER);
		rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNABORTED, B_TRUE);
		rdk_ibcm_done(f, a);
		break;
	case IBCS_REP_RCVD:
		rdk_ibcm_rej(a, IBCM_REJ_CONSUMER_DEFINED,
		    IBCM_MSG_RESPONSE_REP);
		a->ia_qp_err = qp;
		rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNABORTED, B_TRUE);
		rdk_ibcm_timewait(f, a);
		break;
	case IBCS_REQ_RCVD:
		rdk_ibcm_rej(a, IBCM_REJ_CONSUMER_DEFINED,
		    IBCM_MSG_RESPONSE_REQ);
		rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNABORTED, B_TRUE);
		rdk_ibcm_done(f, a);
		break;
	case IBCS_REP_SENT:
	case IBCS_MRA_REP_RCVD:
		rdk_ibcm_rej(a, IBCM_REJ_CONSUMER_DEFINED,
		    IBCM_MSG_RESPONSE_OTHER);
		a->ia_qp_err = qp;
		rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNABORTED, B_TRUE);
		rdk_ibcm_timewait(f, a);
		break;
	case IBCS_ESTABLISHED:
		a->ia_qp_err = qp;
		a->ia_send = IBCM_SEND_DREQ;
		rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNABORTED, B_TRUE);
		rdk_ibcm_timewait(f, a);
		break;
	case IBCS_DREQ_SENT:
		rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNABORTED, B_TRUE);
		rdk_ibcm_timewait(f, a);
		break;
	case IBCS_TIMEWAIT:
		rdk_ibcm_ev(f, a, IBCE_CLOSE, 0, B_TRUE);
		break;
	case IBCS_IDLE:
		rdk_ibcm_done(f, a);
		break;
	default:
		break;
	}
}

void
rdk_ibcm_fsm_step(rdk_ibcm_fsm_t *f, const rdk_ibcm_in_t *in,
    rdk_ibcm_act_t *a)
{
	const rdk_ibcm_state_t s = f->f_state;
	uint32_t ms;

	bzero(a, sizeof (*a));
	switch (in->ii_input) {
	case IBCI_CONNECT:		/* ib_send_cm_req() */
		if (s != IBCS_IDLE || !f->f_active)
			break;
		f->f_attached = B_TRUE;
		f->f_state = IBCS_REQ_SENT;
		rdk_ibcm_send(f, a, IBCM_SEND_REQ, B_TRUE);
		break;

	case IBCI_ACCEPT:		/* ib_send_cm_rep() */
		if (s != IBCS_REQ_RCVD)
			break;
		f->f_attached = B_TRUE;
		f->f_state = IBCS_REP_SENT;
		rdk_ibcm_send(f, a, IBCM_SEND_REP, B_TRUE);
		break;

	case IBCI_REJECT:		/* ib_send_cm_rej() */
		if (s != IBCS_REQ_RCVD)
			break;
		rdk_ibcm_rej(a, in->ii_rej_reason != 0 ? in->ii_rej_reason :
		    IBCM_REJ_CONSUMER_DEFINED, IBCM_MSG_RESPONSE_REQ);
		rdk_ibcm_done(f, a);
		break;

	case IBCI_DISCONNECT:		/* ib_send_cm_dreq() */
		if (s != IBCS_ESTABLISHED)
			break;
		a->ia_qp_err = B_TRUE;
		f->f_state = IBCS_DREQ_SENT;
		rdk_ibcm_send(f, a, IBCM_SEND_DREQ, B_TRUE);
		break;

	case IBCI_ABORT:
		rdk_ibcm_abort(f, a, B_TRUE);
		break;

	case IBCI_QP_GONE:
		if (s == IBCS_ESTABLISHED) {
			f->f_state = IBCS_DREQ_SENT;
			rdk_ibcm_send(f, a, IBCM_SEND_DREQ, B_TRUE);
			rdk_ibcm_ev(f, a, IBCE_DISCONNECT, ECONNRESET,
			    B_FALSE);
		} else if (s != IBCS_DREQ_SENT && s != IBCS_TIMEWAIT) {
			rdk_ibcm_abort(f, a, B_FALSE);
		}
		break;

	case IBCI_REP:			/* cm_rep_handler() */
		if (s == IBCS_REQ_SENT || s == IBCS_MRA_REQ_RCVD) {
			a->ia_timer_stop = B_TRUE;
			a->ia_qp_rtr_rts = B_TRUE;
			f->f_state = IBCS_REP_RCVD;
		} else if (s == IBCS_ESTABLISHED && f->f_active) {
			/* cm_dup_rep_handler(): the RTU was lost. */
			a->ia_send = IBCM_SEND_RTU;
		}
		break;

	case IBCI_QP_READY:		/* cma_rep_recv() */
		if (s != IBCS_REP_RCVD)
			break;
		f->f_state = IBCS_ESTABLISHED;
		a->ia_send = IBCM_SEND_RTU;
		rdk_ibcm_ev(f, a, IBCE_REPLY, 0, B_FALSE);
		break;

	case IBCI_QP_FAILED:
		if (s != IBCS_REP_RCVD)
			break;
		rdk_ibcm_rej(a, IBCM_REJ_CONSUMER_DEFINED,
		    IBCM_MSG_RESPONSE_REP);
		a->ia_qp_err = B_TRUE;
		rdk_ibcm_ev(f, a, IBCE_REPLY, EIO, B_TRUE);
		rdk_ibcm_timewait(f, a);
		break;

	case IBCI_REJ:			/* cm_rej_handler() */
		switch (s) {
		case IBCS_REQ_SENT:
		case IBCS_MRA_REQ_RCVD:
			rdk_ibcm_ev(f, a, IBCE_REPLY, ECONNREFUSED, B_TRUE);
			a->ia_reason = in->ii_rej_reason;
			if (in->ii_rej_reason == IBCM_REJ_STALE_CONN)
				rdk_ibcm_timewait(f, a);
			else
				rdk_ibcm_done(f, a);
			break;
		case IBCS_REP_SENT:
		case IBCS_MRA_REP_RCVD:
			a->ia_qp_err = B_TRUE;
			rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNREFUSED, B_TRUE);
			a->ia_reason = in->ii_rej_reason;
			if (in->ii_rej_reason == IBCM_REJ_STALE_CONN)
				rdk_ibcm_timewait(f, a);
			else
				rdk_ibcm_done(f, a);
			break;
		case IBCS_REQ_RCVD:
			rdk_ibcm_done(f, a);
			break;
		case IBCS_REP_RCVD:
			a->ia_qp_err = B_TRUE;
			rdk_ibcm_ev(f, a, IBCE_REPLY, ECONNREFUSED, B_TRUE);
			a->ia_reason = in->ii_rej_reason;
			rdk_ibcm_timewait(f, a);
			break;
		case IBCS_ESTABLISHED:
		case IBCS_DREQ_SENT:
			a->ia_qp_err = B_TRUE;
			rdk_ibcm_ev(f, a, IBCE_DISCONNECT, ECONNRESET,
			    B_FALSE);
			rdk_ibcm_timewait(f, a);
			break;
		default:
			break;
		}
		break;

	case IBCI_MRA:			/* cm_mra_handler() */
		ms = rdk_ibcm_time_ms(in->ii_mra_timeout) + f->f_life_ms;
		if (f->f_mra_max_ms != 0 && ms > f->f_mra_max_ms)
			ms = f->f_mra_max_ms;
		if (s == IBCS_REQ_SENT &&
		    in->ii_mra_msg == IBCM_MSG_RESPONSE_REQ) {
			f->f_state = IBCS_MRA_REQ_RCVD;
			a->ia_timer_ms = ms;
		} else if (s == IBCS_REP_SENT &&
		    in->ii_mra_msg == IBCM_MSG_RESPONSE_REP) {
			f->f_state = IBCS_MRA_REP_RCVD;
			a->ia_timer_ms = ms;
		}
		break;

	case IBCI_RTU:			/* cm_rtu_handler() */
	case IBCI_COMM_EST:		/* cm_establish() */
		if (s != IBCS_REP_SENT && s != IBCS_MRA_REP_RCVD)
			break;
		a->ia_timer_stop = B_TRUE;
		f->f_state = IBCS_ESTABLISHED;
		rdk_ibcm_ev(f, a, IBCE_ESTABLISHED, 0, B_FALSE);
		break;

	case IBCI_DREQ:			/* cm_dreq_handler() */
		switch (s) {
		case IBCS_ESTABLISHED:
			a->ia_qp_err = B_TRUE;
			a->ia_send = IBCM_SEND_DREP;
			rdk_ibcm_ev(f, a, IBCE_DISCONNECT, 0, B_FALSE);
			rdk_ibcm_timewait(f, a);
			break;
		case IBCS_DREQ_SENT:
			a->ia_send = IBCM_SEND_DREP;
			rdk_ibcm_ev(f, a, IBCE_DISCONNECT, 0, B_FALSE);
			rdk_ibcm_timewait(f, a);
			break;
		case IBCS_REP_SENT:
		case IBCS_MRA_REP_RCVD:
			a->ia_qp_err = B_TRUE;
			a->ia_send = IBCM_SEND_DREP;
			rdk_ibcm_ev(f, a, IBCE_CLOSE, ECONNRESET, B_TRUE);
			rdk_ibcm_timewait(f, a);
			break;
		case IBCS_TIMEWAIT:
			a->ia_send = IBCM_SEND_DREP;
			break;
		default:
			break;
		}
		break;

	case IBCI_DREP:			/* cm_drep_handler() */
		if (s != IBCS_DREQ_SENT)
			break;
		rdk_ibcm_ev(f, a, IBCE_DISCONNECT, 0, B_FALSE);
		rdk_ibcm_timewait(f, a);
		break;

	case IBCI_DUP_REQ:		/* cm_dup_req_handler() */
		if (s == IBCS_REQ_RCVD) {
			a->ia_send = IBCM_SEND_MRA;
			a->ia_mra_msg = IBCM_MSG_RESPONSE_REQ;
			f->f_mra_sent = B_TRUE;
		} else if (s == IBCS_TIMEWAIT) {
			rdk_ibcm_rej(a, IBCM_REJ_STALE_CONN,
			    IBCM_MSG_RESPONSE_OTHER);
		}
		break;

	case IBCI_DUP_REP:		/* cm_dup_rep_handler() */
		if (s == IBCS_ESTABLISHED && f->f_active)
			a->ia_send = IBCM_SEND_RTU;
		break;

	case IBCI_TIMER:		/* cm_process_send_error() */
		switch (s) {
		case IBCS_REQ_SENT:
		case IBCS_MRA_REQ_RCVD:
			if (rdk_ibcm_retry(f, a))
				break;
			rdk_ibcm_ev(f, a, IBCE_REPLY, ETIMEDOUT, B_TRUE);
			rdk_ibcm_done(f, a);
			break;
		case IBCS_REP_SENT:
		case IBCS_MRA_REP_RCVD:
			if (rdk_ibcm_retry(f, a))
				break;
			a->ia_qp_err = B_TRUE;
			rdk_ibcm_ev(f, a, IBCE_CLOSE, ETIMEDOUT, B_TRUE);
			rdk_ibcm_done(f, a);
			break;
		case IBCS_DREQ_SENT:
			if (rdk_ibcm_retry(f, a))
				break;
			rdk_ibcm_ev(f, a, IBCE_DISCONNECT, ETIMEDOUT,
			    B_FALSE);
			rdk_ibcm_timewait(f, a);
			break;
		case IBCS_TIMEWAIT:	/* cm_timewait_handler() */
			rdk_ibcm_ev(f, a, IBCE_CLOSE, 0, B_TRUE);
			f->f_state = IBCS_DONE;
			a->ia_free = B_TRUE;
			a->ia_drop_remote = B_TRUE;
			break;
		default:
			break;
		}
		break;

	case IBCI_FLUSH:
		if (s == IBCS_DONE)
			break;
		rdk_ibcm_ev(f, a, IBCE_CLOSE, ENXIO, B_TRUE);
		rdk_ibcm_done(f, a);
		break;

	default:
		break;
	}
}

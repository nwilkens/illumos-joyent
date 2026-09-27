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
 * The IB CM state machine: the setup and teardown sequences, retries and
 * their limits, MRA extensions, timewait, an abort in every state, and a
 * seeded run of random inputs checked against a model of the timer, the
 * QP and the events the ID gets.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdk_ibcm_fsm.h"

#define	CHECK(x)	do {						\
	if (!(x)) {							\
		(void) fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__,	\
		    __LINE__, #x);					\
		exit(1);						\
	}								\
} while (0)

#define	RESP_MS		100
#define	TW_MS		500
#define	LIFE_MS		3

static uint64_t seed = 0xd1b54a32d192ed03ULL;

static uint32_t
rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return ((uint32_t)(seed >> 11));
}

/* What the kernel side would hold for one connection. */
typedef struct model {
	rdk_ibcm_fsm_t	f;
	rdk_ibcm_act_t	a;
	uint32_t	timer;		/* armed for this long, or 0 */
	int		attached;	/* the ID waits for its final */
	int		finals;
	int		events;
	int		freed;
	int		qp_rts;
	int		qp_err;
	int		sends[IBCM_SEND_DREP + 1];
	int		resends;
} model_t;

static void
start(model_t *m, boolean_t active, uint8_t retries)
{
	memset(m, 0, sizeof (*m));
	rdk_ibcm_fsm_init(&m->f, active, retries, RESP_MS, TW_MS, LIFE_MS);
}

static int
waits(rdk_ibcm_state_t s)
{
	return (s == IBCS_REQ_SENT || s == IBCS_MRA_REQ_RCVD ||
	    s == IBCS_REP_SENT || s == IBCS_MRA_REP_RCVD ||
	    s == IBCS_DREQ_SENT || s == IBCS_TIMEWAIT);
}

static void
step(model_t *m, rdk_ibcm_input_t input, uint16_t reason, uint8_t msg,
    uint8_t to)
{
	rdk_ibcm_in_t in = { input, reason, msg, to };
	const rdk_ibcm_state_t s0 = m->f.f_state;
	rdk_ibcm_act_t *a = &m->a;

	if (input == IBCI_TIMER) {
		if (m->timer == 0)
			return;
		m->timer = 0;
	}
	rdk_ibcm_fsm_step(&m->f, &in, a);

	if (s0 == IBCS_DONE) {
		rdk_ibcm_act_t zero;

		memset(&zero, 0, sizeof (zero));
		CHECK(memcmp(a, &zero, sizeof (zero)) == 0);
		CHECK(m->f.f_state == IBCS_DONE);
		return;
	}
	if ((input == IBCI_CONNECT && s0 == IBCS_IDLE && m->f.f_active) ||
	    (input == IBCI_ACCEPT && s0 == IBCS_REQ_RCVD)) {
		CHECK(!m->attached);
		m->attached = 1;
	}

	/* Events go only to an attached ID, and the final is the last. */
	if (a->ia_ev != IBCE_NONE) {
		CHECK(m->attached);
		m->events++;
	}
	CHECK(!a->ia_final || a->ia_ev != IBCE_NONE);
	if (a->ia_final) {
		m->attached = 0;
		m->finals++;
	}
	CHECK(m->attached == (m->f.f_attached != B_FALSE));

	if (a->ia_timer_stop)
		m->timer = 0;
	if (a->ia_timer_ms != 0) {
		CHECK(a->ia_timer_ms == RESP_MS || a->ia_timer_ms == TW_MS ||
		    input == IBCI_MRA);
		m->timer = a->ia_timer_ms;
	}
	CHECK(!a->ia_resend || (input == IBCI_TIMER && a->ia_timer_ms ==
	    RESP_MS && a->ia_send == IBCM_SEND_NONE));
	if (a->ia_resend)
		m->resends++;
	if (a->ia_send != IBCM_SEND_NONE)
		m->sends[a->ia_send]++;
	if (m->f.f_active) {
		CHECK(a->ia_send != IBCM_SEND_REP &&
		    a->ia_send != IBCM_SEND_MRA);
	} else {
		CHECK(a->ia_send != IBCM_SEND_REQ &&
		    a->ia_send != IBCM_SEND_RTU && !a->ia_qp_rtr_rts);
	}
	if (a->ia_qp_rtr_rts) {
		CHECK(!m->qp_err && m->qp_rts == 0);
		m->qp_rts++;
	}
	if (a->ia_qp_err)
		m->qp_err = 1;

	if (a->ia_free) {
		CHECK(!m->freed && m->f.f_state == IBCS_DONE && !m->attached);
		CHECK(a->ia_drop_remote);
		m->freed = 1;
	}
	CHECK((m->f.f_state == IBCS_DONE) == m->freed);
	/* A state that waits on the peer has a timer; no other one does. */
	CHECK(waits(m->f.f_state) == (m->timer != 0));
}

static void
t_active(void)
{
	model_t m;
	int i;

	start(&m, B_TRUE, 3);
	step(&m, IBCI_CONNECT, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_REQ && m.timer == RESP_MS);
	for (i = 0; i < 3; i++) {
		step(&m, IBCI_TIMER, 0, 0, 0);
		CHECK(m.a.ia_resend && m.f.f_state == IBCS_REQ_SENT);
	}
	step(&m, IBCI_REP, 0, 0, 0);
	CHECK(m.a.ia_qp_rtr_rts && m.f.f_state == IBCS_REP_RCVD);
	step(&m, IBCI_QP_READY, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_RTU && m.a.ia_ev == IBCE_REPLY &&
	    m.a.ia_status == 0 && !m.a.ia_final);
	step(&m, IBCI_DUP_REP, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_RTU);
	step(&m, IBCI_DISCONNECT, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_DREQ && m.a.ia_qp_err);
	step(&m, IBCI_TIMER, 0, 0, 0);
	CHECK(m.a.ia_resend);
	step(&m, IBCI_DREP, 0, 0, 0);
	CHECK(m.a.ia_ev == IBCE_DISCONNECT && m.a.ia_drop_remote &&
	    m.timer == TW_MS);
	step(&m, IBCI_DREQ, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_DREP && m.timer == TW_MS);
	step(&m, IBCI_TIMER, 0, 0, 0);
	CHECK(m.a.ia_ev == IBCE_CLOSE && m.a.ia_final && m.a.ia_free);
	CHECK(m.finals == 1 && m.events == 3 && m.resends == 4);
}

/* One REQ and max_retries resends, then the ID hears ETIMEDOUT. */
static void
t_retries(void)
{
	model_t m;
	uint8_t r;
	int i;

	for (r = 0; r <= 17; r++) {
		start(&m, B_TRUE, r);
		step(&m, IBCI_CONNECT, 0, 0, 0);
		for (i = 0; m.f.f_state != IBCS_DONE; i++) {
			CHECK(i <= 16);
			step(&m, IBCI_TIMER, 0, 0, 0);
		}
		CHECK(m.resends == (r > 15 ? 15 : r));
		CHECK(m.a.ia_ev == IBCE_REPLY && m.a.ia_status == ETIMEDOUT &&
		    m.a.ia_final && m.a.ia_send == IBCM_SEND_NONE);

		start(&m, B_FALSE, r);
		step(&m, IBCI_ACCEPT, 0, 0, 0);
		while (m.f.f_state != IBCS_DONE)
			step(&m, IBCI_TIMER, 0, 0, 0);
		CHECK(m.resends == (r > 15 ? 15 : r) && m.sends[IBCM_SEND_REP]);
		CHECK(m.a.ia_ev == IBCE_CLOSE && m.a.ia_status == ETIMEDOUT &&
		    m.a.ia_qp_err && m.finals == 1);

		/* A lost DREP still ends in timewait and a close. */
		start(&m, B_FALSE, r);
		step(&m, IBCI_ACCEPT, 0, 0, 0);
		step(&m, IBCI_RTU, 0, 0, 0);
		step(&m, IBCI_DISCONNECT, 0, 0, 0);
		while (m.f.f_state == IBCS_DREQ_SENT)
			step(&m, IBCI_TIMER, 0, 0, 0);
		CHECK(m.a.ia_ev == IBCE_DISCONNECT &&
		    m.a.ia_status == ETIMEDOUT && m.timer == TW_MS);
		step(&m, IBCI_TIMER, 0, 0, 0);
		CHECK(m.freed && m.finals == 1 && m.events == 3);
		CHECK(m.resends == (r > 15 ? 15 : r));
	}
}

static void
t_mra(void)
{
	model_t m;

	start(&m, B_TRUE, 2);
	step(&m, IBCI_CONNECT, 0, 0, 0);
	step(&m, IBCI_MRA, 0, IBCM_MSG_RESPONSE_REP, 20);
	CHECK(m.f.f_state == IBCS_REQ_SENT && m.timer == RESP_MS);
	step(&m, IBCI_MRA, 0, IBCM_MSG_RESPONSE_REQ, 20);
	CHECK(m.f.f_state == IBCS_MRA_REQ_RCVD);
	CHECK(m.timer == rdk_ibcm_time_ms(20) + LIFE_MS);
	CHECK(rdk_ibcm_time_ms(20) == 4096 && rdk_ibcm_time_ms(3) == 1 &&
	    rdk_ibcm_time_ms(40) == 1U << 23);
	/* A repeated MRA does not extend it again. */
	step(&m, IBCI_MRA, 0, IBCM_MSG_RESPONSE_REQ, 25);
	CHECK(m.timer == 4096 + LIFE_MS && m.a.ia_timer_ms == 0);
	step(&m, IBCI_TIMER, 0, 0, 0);
	CHECK(m.a.ia_resend && m.timer == RESP_MS);
	step(&m, IBCI_REP, 0, 0, 0);
	CHECK(m.a.ia_qp_rtr_rts && m.timer == 0);
	step(&m, IBCI_QP_FAILED, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_REJ && m.a.ia_rej_msg ==
	    IBCM_MSG_RESPONSE_REP && m.a.ia_status == EIO && m.a.ia_final &&
	    m.timer == TW_MS);

	start(&m, B_FALSE, 2);
	step(&m, IBCI_DUP_REQ, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_MRA && m.a.ia_mra_msg ==
	    IBCM_MSG_RESPONSE_REQ && m.a.ia_ev == IBCE_NONE);
	step(&m, IBCI_ACCEPT, 0, 0, 0);
	step(&m, IBCI_MRA, 0, IBCM_MSG_RESPONSE_REP, 18);
	CHECK(m.f.f_state == IBCS_MRA_REP_RCVD && m.timer == 1024 + LIFE_MS);
	step(&m, IBCI_COMM_EST, 0, 0, 0);
	CHECK(m.a.ia_ev == IBCE_ESTABLISHED && m.timer == 0);
	step(&m, IBCI_RTU, 0, 0, 0);
	CHECK(m.a.ia_ev == IBCE_NONE);

	/* A capped MRA waits no longer than the cap. */
	start(&m, B_FALSE, 2);
	m.f.f_mra_max_ms = 200;
	step(&m, IBCI_ACCEPT, 0, 0, 0);
	step(&m, IBCI_MRA, 0, IBCM_MSG_RESPONSE_REP, 31);
	CHECK(m.f.f_state == IBCS_MRA_REP_RCVD && m.timer == 200);

	CHECK(rdk_ibcm_ack_timeout(15, 16) == 17);
	CHECK(rdk_ibcm_ack_timeout(17, 16) == 18);
	CHECK(rdk_ibcm_ack_timeout(20, 16) == 20);
	CHECK(rdk_ibcm_ack_timeout(18, 16) == 19);
	CHECK(rdk_ibcm_ack_timeout(31, 30) == 31);
	CHECK(rdk_ibcm_ack_timeout(0, 0) == 2);
}

static void
t_rej(void)
{
	model_t m;

	start(&m, B_TRUE, 2);
	step(&m, IBCI_CONNECT, 0, 0, 0);
	step(&m, IBCI_REJ, IBCM_REJ_CONSUMER_DEFINED, 0, 0);
	CHECK(m.a.ia_ev == IBCE_REPLY && m.a.ia_status == ECONNREFUSED &&
	    m.a.ia_reason == IBCM_REJ_CONSUMER_DEFINED && m.freed);

	start(&m, B_TRUE, 2);
	step(&m, IBCI_CONNECT, 0, 0, 0);
	step(&m, IBCI_REJ, IBCM_REJ_STALE_CONN, 0, 0);
	CHECK(m.f.f_state == IBCS_TIMEWAIT && m.finals == 1);
	step(&m, IBCI_DUP_REQ, 0, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_REJ &&
	    m.a.ia_rej_reason == IBCM_REJ_STALE_CONN);
	step(&m, IBCI_TIMER, 0, 0, 0);
	CHECK(m.freed && m.a.ia_ev == IBCE_NONE);

	/* The consumer refuses: a REJ, and no event, since no ID attached. */
	start(&m, B_FALSE, 2);
	step(&m, IBCI_REJECT, 28, 0, 0);
	CHECK(m.a.ia_send == IBCM_SEND_REJ && m.a.ia_rej_reason == 28 &&
	    m.a.ia_rej_msg == IBCM_MSG_RESPONSE_REQ && m.events == 0 &&
	    m.freed);

	start(&m, B_FALSE, 2);
	step(&m, IBCI_ACCEPT, 0, 0, 0);
	step(&m, IBCI_RTU, 0, 0, 0);
	step(&m, IBCI_REJ, IBCM_REJ_CONSUMER_DEFINED, 0, 0);
	CHECK(m.a.ia_ev == IBCE_DISCONNECT && m.a.ia_qp_err &&
	    m.f.f_state == IBCS_TIMEWAIT);
	step(&m, IBCI_TIMER, 0, 0, 0);
	CHECK(m.finals == 1 && m.freed);
}

/* Drive a connection into each state, abort it, and let it run out. */
static void
t_abort(void)
{
	static const rdk_ibcm_input_t ends[] = {
		IBCI_ABORT, IBCI_QP_GONE, IBCI_FLUSH
	};
	static const struct {
		boolean_t		active;
		rdk_ibcm_input_t	path[4];
		rdk_ibcm_state_t	state;
	} to[] = {
		{ B_TRUE, { 0 }, IBCS_IDLE },
		{ B_TRUE, { IBCI_CONNECT }, IBCS_REQ_SENT },
		{ B_TRUE, { IBCI_CONNECT, IBCI_MRA }, IBCS_MRA_REQ_RCVD },
		{ B_TRUE, { IBCI_CONNECT, IBCI_REP }, IBCS_REP_RCVD },
		{ B_TRUE, { IBCI_CONNECT, IBCI_REP, IBCI_QP_READY },
		    IBCS_ESTABLISHED },
		{ B_TRUE, { IBCI_CONNECT, IBCI_REP, IBCI_QP_READY,
		    IBCI_DISCONNECT }, IBCS_DREQ_SENT },
		{ B_TRUE, { IBCI_CONNECT, IBCI_REP, IBCI_QP_READY,
		    IBCI_DREQ }, IBCS_TIMEWAIT },
		{ B_FALSE, { 0 }, IBCS_REQ_RCVD },
		{ B_FALSE, { IBCI_ACCEPT }, IBCS_REP_SENT },
		{ B_FALSE, { IBCI_ACCEPT, IBCI_MRA }, IBCS_MRA_REP_RCVD },
		{ B_FALSE, { IBCI_ACCEPT, IBCI_RTU }, IBCS_ESTABLISHED },
		{ B_FALSE, { IBCI_ACCEPT, IBCI_RTU, IBCI_DISCONNECT },
		    IBCS_DREQ_SENT },
	};
	model_t m;
	unsigned int i, e, j;
	int n, att;

	for (i = 0; i < sizeof (to) / sizeof (to[0]); i++) {
		for (e = 0; e < 3; e++) {
			start(&m, to[i].active, 4);
			for (j = 0; j < 4 && to[i].path[j] != 0; j++) {
				step(&m, to[i].path[j], 0, to[i].active ?
				    IBCM_MSG_RESPONSE_REQ :
				    IBCM_MSG_RESPONSE_REP, 10);
			}
			CHECK(m.f.f_state == to[i].state);
			att = m.attached;
			step(&m, ends[e], 0, 0, 0);
			if (ends[e] != IBCI_QP_GONE) {
				/* Nothing more is sent, or the ID waits. */
				CHECK(m.f.f_state == IBCS_TIMEWAIT ||
				    m.f.f_state == IBCS_DONE);
				CHECK(!m.attached);
			}
			if (ends[e] == IBCI_FLUSH)
				CHECK(m.freed && m.a.ia_send == IBCM_SEND_NONE);
			if (to[i].state == IBCS_REQ_SENT && ends[e] ==
			    IBCI_ABORT) {
				CHECK(m.a.ia_send == IBCM_SEND_REJ &&
				    m.a.ia_rej_reason == IBCM_REJ_TIMEOUT);
			}
			for (n = 0; !m.freed; n++) {
				CHECK(n < 8);
				step(&m, IBCI_TIMER, 0, 0, 0);
				step(&m, IBCI_ABORT, 0, 0, 0);
			}
			CHECK(m.finals == att && !m.attached);
		}
	}
}

static void
t_random(int runs)
{
	model_t m;
	int r, i, steps = 0, finals = 0, events = 0;
	rdk_ibcm_input_t in;

	for (r = 0; r < runs; r++) {
		start(&m, rnd() % 2, rnd() % 18);
		for (i = 0; i < 64 && !m.freed; i++) {
			in = (rdk_ibcm_input_t)(1 + rnd() % IBCI_FLUSH);
			if (in == IBCI_FLUSH && rnd() % 4 != 0)
				in = IBCI_TIMER;
			step(&m, in, (uint16_t)(rnd() % 40),
			    (uint8_t)(rnd() % 4), (uint8_t)(rnd() % 40));
			steps++;
		}
		/* Every connection can be ended, and ends once. */
		step(&m, IBCI_ABORT, 0, 0, 0);
		for (i = 0; !m.freed; i++) {
			CHECK(i < 20);
			step(&m, IBCI_TIMER, 0, 0, 0);
		}
		CHECK(!m.attached && m.finals <= 1);
		finals += m.finals;
		events += m.events;
	}
	(void) printf("random: %d connections, %d inputs, %d events, "
	    "%d finals\n", runs, steps, events, finals);
}

int
main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 200000;

	t_active();
	t_retries();
	t_mra();
	t_rej();
	t_abort();
	t_random(n);
	(void) printf("setup, teardown, retries 0..17, MRA, REJ, abort and "
	    "flush in 12 states checked\n");
	return (0);
}

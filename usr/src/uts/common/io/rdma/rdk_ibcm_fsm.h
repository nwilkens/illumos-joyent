/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
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

#ifndef _RDK_IBCM_FSM_H
#define	_RDK_IBCM_FSM_H

/*
 * The state machine of one IB CM connection, without I/O: each input gives
 * the actions to take.  The states, the retransmission and timewait rules
 * and the answer to each message in each state follow Linux
 * drivers/infiniband/core/cm.c; see README.illumos.  Builds in the kernel
 * and on the host for tests.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rdk_ibcm_state {
	IBCS_IDLE = 0,
	IBCS_REQ_SENT,
	IBCS_MRA_REQ_RCVD,
	IBCS_REP_RCVD,		/* the QP moves to RTR and RTS */
	IBCS_REQ_RCVD,		/* the consumer has not decided */
	IBCS_REP_SENT,
	IBCS_MRA_REP_RCVD,
	IBCS_ESTABLISHED,
	IBCS_DREQ_SENT,
	IBCS_TIMEWAIT,
	IBCS_DONE
} rdk_ibcm_state_t;

typedef enum rdk_ibcm_input {
	IBCI_CONNECT = 1,	/* send the REQ */
	IBCI_ACCEPT,		/* the QP is in RTS: send the REP */
	IBCI_REJECT,		/* the consumer refuses the request */
	IBCI_DISCONNECT,
	IBCI_ABORT,		/* destroy, cancel or deadline */
	IBCI_QP_GONE,		/* the consumer destroyed the QP */
	IBCI_REP,
	IBCI_QP_READY,
	IBCI_QP_FAILED,
	IBCI_RTU,
	IBCI_REJ,
	IBCI_MRA,
	IBCI_DREQ,
	IBCI_DREP,
	IBCI_DUP_REQ,
	IBCI_DUP_REP,
	IBCI_COMM_EST,
	IBCI_TIMER,
	IBCI_FLUSH		/* the device is going: end without a word */
} rdk_ibcm_input_t;

/* The message an action sends. */
typedef enum rdk_ibcm_send {
	IBCM_SEND_NONE = 0,
	IBCM_SEND_REQ,
	IBCM_SEND_REP,
	IBCM_SEND_RTU,
	IBCM_SEND_REJ,
	IBCM_SEND_MRA,
	IBCM_SEND_DREQ,
	IBCM_SEND_DREP
} rdk_ibcm_send_t;

/* What the connection tells its ID; mirrors rdk_cm_tev_t. */
typedef enum rdk_ibcm_ev {
	IBCE_NONE = 0,
	IBCE_REPLY,
	IBCE_ESTABLISHED,
	IBCE_DISCONNECT,
	IBCE_CLOSE
} rdk_ibcm_ev_t;

/* REJ reasons and MRA/REJ message codes (IBTA vol 1, 12.6.7 and 12.6.6). */
#define	IBCM_REJ_TIMEOUT		4
#define	IBCM_REJ_INVALID_COMM_ID	6
#define	IBCM_REJ_INVALID_SERVICE_ID	8
#define	IBCM_REJ_INVALID_TRANSPORT	9
#define	IBCM_REJ_STALE_CONN		10
#define	IBCM_REJ_INVALID_GID		12
#define	IBCM_REJ_INVALID_MTU		26
#define	IBCM_REJ_CONSUMER_DEFINED	28
#define	IBCM_REJ_INVALID_CLASS_VERSION	31

#define	IBCM_MSG_RESPONSE_REQ		0
#define	IBCM_MSG_RESPONSE_REP		1
#define	IBCM_MSG_RESPONSE_OTHER		2

/* IB time values: 4.096 us times 2^t; the CM's approximation in ms. */
#define	IBCM_TIME_MAX			31

typedef struct rdk_ibcm_act {
	rdk_ibcm_send_t	ia_send;
	boolean_t	ia_resend;	/* send the saved message again */
	uint16_t	ia_rej_reason;
	uint8_t		ia_rej_msg;
	uint8_t		ia_mra_msg;
	boolean_t	ia_timer_stop;
	uint32_t	ia_timer_ms;	/* nonzero: arm */
	boolean_t	ia_qp_rtr_rts;
	boolean_t	ia_qp_err;
	rdk_ibcm_ev_t	ia_ev;
	int		ia_status;
	uint32_t	ia_reason;
	boolean_t	ia_final;	/* the ID lets go after this event */
	boolean_t	ia_drop_remote;	/* out of the remote ID tables */
	boolean_t	ia_free;
} rdk_ibcm_act_t;

typedef struct rdk_ibcm_fsm {
	rdk_ibcm_state_t	f_state;
	boolean_t		f_active;
	boolean_t		f_attached;	/* an ID is owed its final */
	boolean_t		f_mra_sent;
	uint8_t			f_retries;	/* left for the saved message */
	uint8_t			f_max_retries;
	uint32_t		f_resp_ms;	/* retransmit interval */
	uint32_t		f_tw_ms;	/* timewait */
	uint32_t		f_life_ms;	/* packet lifetime */
} rdk_ibcm_fsm_t;

typedef struct rdk_ibcm_in {
	rdk_ibcm_input_t	ii_input;
	uint16_t		ii_rej_reason;	/* IBCI_REJ, IBCI_REJECT */
	uint8_t			ii_mra_msg;	/* IBCI_MRA */
	uint8_t			ii_mra_timeout;	/* IBCI_MRA, IB time */
} rdk_ibcm_in_t;

extern uint32_t rdk_ibcm_time_ms(uint8_t);
extern uint8_t rdk_ibcm_ack_timeout(uint8_t, uint8_t);
extern void rdk_ibcm_fsm_init(rdk_ibcm_fsm_t *, boolean_t, uint8_t, uint32_t,
    uint32_t, uint32_t);
extern void rdk_ibcm_fsm_step(rdk_ibcm_fsm_t *, const rdk_ibcm_in_t *,
    rdk_ibcm_act_t *);
extern const char *rdk_ibcm_state_name(rdk_ibcm_state_t);

#ifdef __cplusplus
}
#endif

#endif /* _RDK_IBCM_FSM_H */

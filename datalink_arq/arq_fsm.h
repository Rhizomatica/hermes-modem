/* HERMES Modem — ARQ FSM: state/event types and session structure
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_FSM_H_
#define ARQ_FSM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arq.h"  /* CALLSIGN_MAX_SIZE, arq_action_t/type, arq_info */

/* ======================================================================
 * Level 1 — Connection FSM states
 * ====================================================================== */

typedef enum
{
    ARQ_CONN_DISCONNECTED  = 0, /* no session; idle                              */
    ARQ_CONN_LISTENING     = 1, /* waiting for incoming CALL frame               */
    ARQ_CONN_CALLING       = 2, /* outgoing CALL sent; awaiting ACCEPT           */
    ARQ_CONN_ACCEPTING     = 3, /* ACCEPT sent; awaiting first data/ACK          */
    ARQ_CONN_CONNECTED     = 4, /* data-flow sub-FSM is active                   */
    ARQ_CONN_DISCONNECTING = 5, /* DISCONNECT frame being exchanged              */
    ARQ_CONN__COUNT
} arq_conn_state_t;

/* ======================================================================
 * Level 2 — Data-flow sub-FSM states (active only in ARQ_CONN_CONNECTED)
 * ====================================================================== */

typedef enum
{
    ARQ_DFLOW_IDLE_ISS        =  0, /* ISS: no pending frame; waiting for data   */
    ARQ_DFLOW_DATA_TX         =  1, /* ISS: frame queued/transmitting            */
    ARQ_DFLOW_WAIT_ACK        =  2, /* ISS: PTT-OFF; waiting for peer ACK        */
    ARQ_DFLOW_IDLE_IRS        =  3, /* IRS: waiting for peer data frame          */
    ARQ_DFLOW_DATA_RX         =  4, /* IRS: data frame decoded; ACK pending      */
    ARQ_DFLOW_ACK_TX          =  5, /* IRS: ACK frame being transmitted          */
    ARQ_DFLOW_TURN_REQ_TX     =  6, /* IRS→ISS: TURN_REQ being transmitted       */
    ARQ_DFLOW_TURN_REQ_WAIT   =  7, /* IRS→ISS: waiting for TURN_ACK            */
    ARQ_DFLOW_TURN_ACK_TX     =  8, /* ISS→IRS: TURN_ACK being transmitted       */
    ARQ_DFLOW_MODE_REQ_TX     =  9, /* mode upgrade: MODE_REQ being transmitted  */
    ARQ_DFLOW_MODE_REQ_WAIT   = 10, /* mode upgrade: waiting for MODE_ACK        */
    ARQ_DFLOW_MODE_ACK_TX     = 11, /* mode upgrade: MODE_ACK being transmitted  */
    ARQ_DFLOW_KEEPALIVE_TX    = 12, /* KEEPALIVE being transmitted               */
    ARQ_DFLOW_KEEPALIVE_WAIT  = 13, /* waiting for KEEPALIVE_ACK                 */
    ARQ_DFLOW__COUNT
} arq_dflow_state_t;

/* ======================================================================
 * Caller/callee role (set at connect time, stays for session lifetime)
 * ====================================================================== */

typedef enum
{
    ARQ_ROLE_NONE   = 0,
    ARQ_ROLE_CALLER = 1,  /* originated the call (starts as ISS) */
    ARQ_ROLE_CALLEE = 2   /* received the call   (starts as IRS) */
} arq_role_t;

/* ======================================================================
 * Events
 * ====================================================================== */

typedef enum
{
    /* Application events (from TCP interface via channel bus) */
    ARQ_EV_APP_LISTEN         =  0,  /* LISTEN ON received            */
    ARQ_EV_APP_STOP_LISTEN    =  1,  /* LISTEN OFF received           */
    ARQ_EV_APP_CONNECT        =  2,  /* CONNECT <dst> received        */
    ARQ_EV_APP_DISCONNECT     =  3,  /* DISCONNECT received           */
    ARQ_EV_APP_DATA_READY     =  4,  /* TX data available in buffer   */

    /* Radio RX events (from modem worker) */
    ARQ_EV_RX_CALL            =  5,  /* CALL frame decoded            */
    ARQ_EV_RX_ACCEPT          =  6,  /* ACCEPT frame decoded          */
    ARQ_EV_RX_ACK             =  7,  /* ACK frame decoded             */
    ARQ_EV_RX_DATA            =  8,  /* DATA frame decoded            */
    ARQ_EV_RX_DISCONNECT      =  9,  /* DISCONNECT frame decoded      */
    ARQ_EV_RX_TURN_REQ        = 10,  /* TURN_REQ frame decoded        */
    ARQ_EV_RX_TURN_ACK        = 11,  /* TURN_ACK frame decoded        */
    ARQ_EV_RX_MODE_REQ        = 12,  /* MODE_REQ frame decoded        */
    ARQ_EV_RX_MODE_ACK        = 13,  /* MODE_ACK frame decoded        */
    ARQ_EV_RX_KEEPALIVE       = 14,  /* KEEPALIVE frame decoded       */
    ARQ_EV_RX_KEEPALIVE_ACK   = 15,  /* KEEPALIVE_ACK frame decoded   */

    /* Timer events */
    ARQ_EV_TIMER_RETRY        = 16,  /* retry deadline expired        */
    ARQ_EV_TIMER_TIMEOUT      = 17,  /* session/call timeout expired  */
    ARQ_EV_TIMER_ACK          = 18,  /* ACK wait deadline expired     */
    ARQ_EV_TIMER_PEER_BACKLOG = 19,  /* peer-backlog hold expired     */
    ARQ_EV_TIMER_KEEPALIVE    = 20,  /* keepalive interval expired    */

    /* Modem events */
    ARQ_EV_TX_STARTED         = 21,  /* PTT ON (frame on air)         */
    ARQ_EV_TX_COMPLETE        = 22,  /* PTT OFF (TX finished)         */

    ARQ_EV__COUNT
} arq_event_id_t;

/**
 * @brief ARQ event with all possible payload fields.
 *
 * Callers fill only the fields relevant to their event type.
 * Unset numeric fields are 0; unset bool fields are false.
 */
typedef struct
{
    arq_event_id_t id;

    /* Frame-derived fields (set on RX events) */
    uint8_t  session_id;
    uint8_t  seq;
    uint8_t  ack_seq;
    uint8_t  rx_flags;        /* ARQ_FLAG_TURN_REQ / HAS_DATA / HAS_SNR bits  */
    int8_t   snr_encoded;     /* as received from frame header                */
    uint16_t ack_delay_raw;   /* as received (10ms units, 0=unknown)          */

    /* Mode negotiation */
    int      mode;            /* requested/applied FreeDV mode                */
    size_t   data_bytes;      /* payload byte count (DATA frames)             */

    /* Call setup */
    char     remote_call[CALLSIGN_MAX_SIZE];
} arq_event_t;

/* ======================================================================
 * Session structure — replaces arq_ctx_t
 *
 * All state is in this struct; no hidden global flags.
 * Monotonic timestamps are uint64_t milliseconds from hermes_log startup.
 * ====================================================================== */

typedef struct
{
    /* --- State machine fields --- */
    arq_conn_state_t  conn_state;      /* Level 1 connection state             */
    arq_dflow_state_t dflow_state;     /* Level 2 data-flow state              */
    arq_role_t        role;            /* CALLER or CALLEE                     */

    /* --- Identifiers --- */
    uint8_t  session_id;               /* random byte chosen by caller         */
    char     remote_call[CALLSIGN_MAX_SIZE];

    /* --- Sequence numbers --- */
    uint8_t  tx_seq;                   /* next seq we will send                */
    uint8_t  rx_expected;              /* next seq we expect from peer         */

    /* --- Mode / speed --- */
    int      payload_mode;             /* current data TX FreeDV mode          */
    int      control_mode;             /* always FREEDV_MODE_DATAC13           */
    int      speed_level;              /* mode selection ladder index          */
    int      mode_upgrade_count;       /* hysteresis counter for upgrade       */

    /* --- Retry/timeout bookkeeping --- */
    int      tx_retries_left;          /* retries remaining for current frame  */
    uint64_t deadline_ms;             /* next absolute monotonic deadline      */
    uint64_t state_enter_ms;          /* when current conn_state was entered   */
    uint64_t startup_deadline_ms;     /* end of DATAC13-only startup period    */

    /* --- Peer state observed from frames --- */
    bool     peer_has_data;            /* peer's HAS_DATA flag in last frame   */
    int      peer_snr_x10;            /* peer-reported SNR * 10 (integer)     */
    int      local_snr_x10;           /* local SNR EMA * 10                   */
    uint64_t peer_busy_until_ms;      /* remote TX busy guard expiry          */

    /* --- Data bookkeeping --- */
    int      tx_backlog_bytes;         /* bytes pending in TX buffer           */

    /* --- Teardown flags --- */
    bool     disconnect_to_no_client;  /* after disconnect: clear arq_info     */

    /* --- Keepalive tracking --- */
    int      keepalive_miss_count;
    uint64_t last_rx_ms;              /* last successful frame decode time     */
} arq_session_t;

/* ======================================================================
 * FSM public API (implemented in arq_fsm.c)
 * ====================================================================== */

/**
 * @brief Initialise a session structure to DISCONNECTED state.
 * @param sess Session to initialise.
 */
void arq_fsm_init(arq_session_t *sess);

/**
 * @brief Dispatch an event through both FSM levels.
 *
 * Runs transition logic, calls action callbacks, and updates deadlines.
 * Must be called from the single ARQ event-loop thread (no locking inside).
 *
 * @param sess  Active session.
 * @param event Event to process.
 */
void arq_fsm_dispatch(arq_session_t *sess, const arq_event_t *event);

/**
 * @brief Return milliseconds until the next deadline, or INT_MAX if idle.
 *
 * Used by the event loop's blocking wait to set a poll timeout.
 *
 * @param sess Active session.
 * @param now  Current monotonic time in milliseconds.
 * @return Milliseconds to wait (0 = fire immediately, INT_MAX = no deadline).
 */
int arq_fsm_timeout_ms(const arq_session_t *sess, uint64_t now);

/**
 * @brief Human-readable name for a connection state (for log output).
 */
const char *arq_conn_state_name(arq_conn_state_t s);

/**
 * @brief Human-readable name for a data-flow state (for log output).
 */
const char *arq_dflow_state_name(arq_dflow_state_t s);

/**
 * @brief Human-readable name for an event (for log output).
 */
const char *arq_event_name(arq_event_id_t ev);

#endif /* ARQ_FSM_H_ */

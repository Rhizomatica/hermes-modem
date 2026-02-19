/* HERMES Modem — ARQ FSM implementation (Phase 1 stub)
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_fsm.h"

#include <limits.h>
#include <string.h>

/* ---- state/event name tables ---- */

const char *arq_conn_state_name(arq_conn_state_t s)
{
    static const char *names[] = {
        [ARQ_CONN_DISCONNECTED]  = "DISCONNECTED",
        [ARQ_CONN_LISTENING]     = "LISTENING",
        [ARQ_CONN_CALLING]       = "CALLING",
        [ARQ_CONN_ACCEPTING]     = "ACCEPTING",
        [ARQ_CONN_CONNECTED]     = "CONNECTED",
        [ARQ_CONN_DISCONNECTING] = "DISCONNECTING",
    };
    if ((unsigned)s < ARQ_CONN__COUNT)
        return names[s];
    return "UNKNOWN";
}

const char *arq_dflow_state_name(arq_dflow_state_t s)
{
    static const char *names[] = {
        [ARQ_DFLOW_IDLE_ISS]       = "IDLE_ISS",
        [ARQ_DFLOW_DATA_TX]        = "DATA_TX",
        [ARQ_DFLOW_WAIT_ACK]       = "WAIT_ACK",
        [ARQ_DFLOW_IDLE_IRS]       = "IDLE_IRS",
        [ARQ_DFLOW_DATA_RX]        = "DATA_RX",
        [ARQ_DFLOW_ACK_TX]         = "ACK_TX",
        [ARQ_DFLOW_TURN_REQ_TX]    = "TURN_REQ_TX",
        [ARQ_DFLOW_TURN_REQ_WAIT]  = "TURN_REQ_WAIT",
        [ARQ_DFLOW_TURN_ACK_TX]    = "TURN_ACK_TX",
        [ARQ_DFLOW_MODE_REQ_TX]    = "MODE_REQ_TX",
        [ARQ_DFLOW_MODE_REQ_WAIT]  = "MODE_REQ_WAIT",
        [ARQ_DFLOW_MODE_ACK_TX]    = "MODE_ACK_TX",
        [ARQ_DFLOW_KEEPALIVE_TX]   = "KEEPALIVE_TX",
        [ARQ_DFLOW_KEEPALIVE_WAIT] = "KEEPALIVE_WAIT",
    };
    if ((unsigned)s < ARQ_DFLOW__COUNT)
        return names[s];
    return "UNKNOWN";
}

const char *arq_event_name(arq_event_id_t ev)
{
    static const char *names[] = {
        [ARQ_EV_APP_LISTEN]         = "APP_LISTEN",
        [ARQ_EV_APP_STOP_LISTEN]    = "APP_STOP_LISTEN",
        [ARQ_EV_APP_CONNECT]        = "APP_CONNECT",
        [ARQ_EV_APP_DISCONNECT]     = "APP_DISCONNECT",
        [ARQ_EV_APP_DATA_READY]     = "APP_DATA_READY",
        [ARQ_EV_RX_CALL]            = "RX_CALL",
        [ARQ_EV_RX_ACCEPT]          = "RX_ACCEPT",
        [ARQ_EV_RX_ACK]             = "RX_ACK",
        [ARQ_EV_RX_DATA]            = "RX_DATA",
        [ARQ_EV_RX_DISCONNECT]      = "RX_DISCONNECT",
        [ARQ_EV_RX_TURN_REQ]        = "RX_TURN_REQ",
        [ARQ_EV_RX_TURN_ACK]        = "RX_TURN_ACK",
        [ARQ_EV_RX_MODE_REQ]        = "RX_MODE_REQ",
        [ARQ_EV_RX_MODE_ACK]        = "RX_MODE_ACK",
        [ARQ_EV_RX_KEEPALIVE]       = "RX_KEEPALIVE",
        [ARQ_EV_RX_KEEPALIVE_ACK]   = "RX_KEEPALIVE_ACK",
        [ARQ_EV_TIMER_RETRY]        = "TIMER_RETRY",
        [ARQ_EV_TIMER_TIMEOUT]      = "TIMER_TIMEOUT",
        [ARQ_EV_TIMER_ACK]          = "TIMER_ACK",
        [ARQ_EV_TIMER_PEER_BACKLOG] = "TIMER_PEER_BACKLOG",
        [ARQ_EV_TIMER_KEEPALIVE]    = "TIMER_KEEPALIVE",
        [ARQ_EV_TX_STARTED]         = "TX_STARTED",
        [ARQ_EV_TX_COMPLETE]        = "TX_COMPLETE",
    };
    if ((unsigned)ev < ARQ_EV__COUNT)
        return names[ev];
    return "UNKNOWN";
}

/* ---- public API stubs ---- */

void arq_fsm_init(arq_session_t *sess)
{
    memset(sess, 0, sizeof(*sess));
    sess->conn_state  = ARQ_CONN_DISCONNECTED;
    sess->dflow_state = ARQ_DFLOW_IDLE_ISS;
    sess->role        = ARQ_ROLE_NONE;
    sess->deadline_ms = UINT64_MAX;
}

void arq_fsm_dispatch(arq_session_t *sess, const arq_event_t *event)
{
    /* TODO Phase 4: implement table-driven FSM dispatch */
    (void)sess;
    (void)event;
}

int arq_fsm_timeout_ms(const arq_session_t *sess, uint64_t now)
{
    /* TODO Phase 4: return time until sess->deadline_ms */
    if (sess->deadline_ms == UINT64_MAX)
        return INT_MAX;
    if (sess->deadline_ms <= now)
        return 0;
    uint64_t diff = sess->deadline_ms - now;
    return (diff > (uint64_t)INT_MAX) ? INT_MAX : (int)diff;
}

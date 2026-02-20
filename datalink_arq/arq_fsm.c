/* HERMES Modem — ARQ FSM implementation
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_fsm.h"
#include "arq_protocol.h"
#include "arq_timing.h"

#include <limits.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "../common/hermes_log.h"
#include "../modem/framer.h"
#include "../modem/freedv/freedv_api.h"

#define LOG_COMP  "arq-fsm"
#define INT_BUFFER_SIZE 4096

/* ======================================================================
 * State/event name tables
 * ====================================================================== */

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
    if ((unsigned)s < ARQ_CONN__COUNT) return names[s];
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
    if ((unsigned)s < ARQ_DFLOW__COUNT) return names[s];
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
    if ((unsigned)ev < ARQ_EV__COUNT) return names[ev];
    return "UNKNOWN";
}

/* ======================================================================
 * Callbacks and timing context registry
 * ====================================================================== */

static arq_fsm_callbacks_t g_cbs;
static arq_timing_ctx_t   *g_timing;

void arq_fsm_set_callbacks(const arq_fsm_callbacks_t *cbs)
{
    if (cbs) g_cbs = *cbs;
}

void arq_fsm_set_timing(arq_timing_ctx_t *timing)
{
    g_timing = timing;
}

/* ======================================================================
 * arq_fsm_init / arq_fsm_timeout_ms
 * ====================================================================== */

void arq_fsm_init(arq_session_t *sess)
{
    memset(sess, 0, sizeof(*sess));
    sess->conn_state     = ARQ_CONN_DISCONNECTED;
    sess->dflow_state    = ARQ_DFLOW_IDLE_ISS;
    sess->role           = ARQ_ROLE_NONE;
    sess->deadline_ms    = UINT64_MAX;
    sess->deadline_event = ARQ_EV_TIMER_RETRY;
    sess->control_mode   = FREEDV_MODE_DATAC13;
    sess->payload_mode   = FREEDV_MODE_DATAC4;
}

int arq_fsm_timeout_ms(const arq_session_t *sess, uint64_t now)
{
    if (sess->deadline_ms == UINT64_MAX) return INT_MAX;
    if (sess->deadline_ms <= now)        return 0;
    uint64_t diff = sess->deadline_ms - now;
    return (diff > (uint64_t)INT_MAX) ? INT_MAX : (int)diff;
}

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

static void sess_enter(arq_session_t *sess, arq_conn_state_t new_state,
                       uint64_t deadline_ms, arq_event_id_t deadline_event)
{
    HLOGD(LOG_COMP, "conn: %s -> %s",
          arq_conn_state_name(sess->conn_state),
          arq_conn_state_name(new_state));
    sess->conn_state     = new_state;
    sess->state_enter_ms = hermes_uptime_ms();
    sess->deadline_ms    = deadline_ms;
    sess->deadline_event = deadline_event;
}

static void dflow_enter(arq_session_t *sess, arq_dflow_state_t new_state,
                        uint64_t deadline_ms, arq_event_id_t deadline_event)
{
    if (sess->dflow_state != new_state)
        HLOGD(LOG_COMP, "dflow: %s -> %s",
              arq_dflow_state_name(sess->dflow_state),
              arq_dflow_state_name(new_state));
    sess->dflow_state    = new_state;
    sess->deadline_ms    = deadline_ms;
    sess->deadline_event = deadline_event;
}

static void send_frame(int ptype, int mode, size_t len, const uint8_t *frame)
{
    if (g_cbs.send_tx_frame)
        g_cbs.send_tx_frame(ptype, mode, len, frame);
}

static uint64_t deadline_from_s(float seconds)
{
    return hermes_uptime_ms() + (uint64_t)(seconds * 1000.0f + 0.5f);
}

static void send_call_accept(arq_session_t *sess, bool is_accept)
{
    uint8_t frame[INT_BUFFER_SIZE];
    int n;
    if (is_accept)
        n = arq_protocol_build_accept(frame, sizeof(frame), sess->session_id,
                                      sess->remote_call, sess->remote_call);
    else
        n = arq_protocol_build_call(frame, sizeof(frame), sess->session_id,
                                    sess->remote_call, sess->remote_call);
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CALL, sess->control_mode, (size_t)n, frame);
}

static void send_ctrl_frame(arq_session_t *sess, arq_subtype_t subtype)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    int n = -1;
    switch (subtype)
    {
    case ARQ_SUBTYPE_DISCONNECT:
        n = arq_protocol_build_disconnect(frame, sizeof(frame),
                                          sess->session_id, snr_raw); break;
    case ARQ_SUBTYPE_KEEPALIVE:
        n = arq_protocol_build_keepalive(frame, sizeof(frame),
                                         sess->session_id, snr_raw); break;
    case ARQ_SUBTYPE_KEEPALIVE_ACK:
        n = arq_protocol_build_keepalive_ack(frame, sizeof(frame),
                                             sess->session_id, snr_raw); break;
    case ARQ_SUBTYPE_TURN_REQ:
        n = arq_protocol_build_turn_req(frame, sizeof(frame),
                                        sess->session_id,
                                        sess->rx_expected, snr_raw); break;
    case ARQ_SUBTYPE_TURN_ACK:
        n = arq_protocol_build_turn_ack(frame, sizeof(frame),
                                        sess->session_id, snr_raw); break;
    default:
        return;
    }
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame);
}

static void send_ack(arq_session_t *sess, uint8_t ack_delay_raw)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t flags   = 0;
    uint8_t snr_raw = 0;

    if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
        flags |= ARQ_FLAG_HAS_DATA;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    int n = arq_protocol_build_ack(frame, sizeof(frame), sess->session_id,
                                   sess->rx_expected, flags, snr_raw, ack_delay_raw);
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame);

    if (g_timing)
        arq_timing_record_ack_tx(g_timing, (int)sess->rx_expected);
}

static void send_data_frame(arq_session_t *sess)
{
    if (!g_cbs.tx_read || !g_cbs.tx_backlog)
        return;

    const arq_mode_timing_t *tm = arq_protocol_mode_timing(sess->payload_mode);
    if (!tm)
        return;

    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t payload[INT_BUFFER_SIZE];
    int payload_len = g_cbs.tx_read(payload, (size_t)tm->payload_bytes);
    if (payload_len <= 0)
        return;

    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    int n = arq_protocol_build_data(frame, sizeof(frame),
                                    sess->session_id, sess->tx_seq,
                                    sess->rx_expected, 0, snr_raw,
                                    payload, (size_t)payload_len);
    if (n > 0)
    {
        send_frame(PACKET_TYPE_ARQ_DATA, sess->payload_mode, (size_t)n, frame);
        if (g_timing)
            arq_timing_record_tx_queue(g_timing, (int)sess->tx_seq,
                                       sess->payload_mode,
                                       g_cbs.tx_backlog());
    }
}

/* ======================================================================
 * Level 1 FSM per-state handlers
 * ====================================================================== */

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev);

static void enter_idle_iss(arq_session_t *sess)
{
    if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
    {
        dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        send_data_frame(sess);
    }
    else
    {
        dflow_enter(sess, ARQ_DFLOW_IDLE_ISS, UINT64_MAX, ARQ_EV_TIMER_RETRY);
    }
}

static void enter_idle_irs(arq_session_t *sess)
{
    dflow_enter(sess, ARQ_DFLOW_IDLE_IRS,
                deadline_from_s(ARQ_PEER_PAYLOAD_HOLD_S),
                ARQ_EV_TIMER_PEER_BACKLOG);
}

static void fsm_disconnected(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_APP_LISTEN:
        sess_enter(sess, ARQ_CONN_LISTENING, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_APP_CONNECT:
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        sess->session_id      = (uint8_t)(hermes_uptime_ms() & 0x7F) | 0x01;
        sess->tx_retries_left = ARQ_CALL_RETRY_SLOTS;
        send_call_accept(sess, false);
        {
            const arq_mode_timing_t *tm =
                arq_protocol_mode_timing(sess->control_mode);
            float interval = tm ? tm->retry_interval_s : 7.0f;
            sess_enter(sess, ARQ_CONN_CALLING,
                       deadline_from_s(interval), ARQ_EV_TIMER_RETRY);
        }
        break;

    default:
        break;
    }
}

static void fsm_listening(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_CALL:
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        sess->session_id      = ev->session_id;
        sess->tx_retries_left = ARQ_ACCEPT_RETRY_SLOTS;
        /* Do NOT send ACCEPT immediately: the caller's PTT-OFF may not have
         * happened yet when we decode the last samples of their CALL frame.
         * Wait ARQ_CHANNEL_GUARD_MS so their relay is in RX before we TX. */
        sess_enter(sess, ARQ_CONN_ACCEPTING,
                   hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_APP_CONNECT:
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        fsm_disconnected(sess, ev);
        break;

    case ARQ_EV_APP_STOP_LISTEN:
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    default:
        break;
    }
}

static void fsm_calling(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_RX_ACCEPT:
        if (ev->session_id == sess->session_id)
        {
            sess->role        = ARQ_ROLE_CALLER;
            sess->tx_seq      = 0;
            sess->rx_expected = 0;
            sess->startup_deadline_ms =
                hermes_uptime_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
            if (g_cbs.notify_connected)
                g_cbs.notify_connected(sess->remote_call);
            if (g_timing)
                arq_timing_record_connect(g_timing, sess->control_mode);
            sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
            enter_idle_iss(sess);   /* caller sends data first */
        }
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_call_accept(sess, false);
            tm = arq_protocol_mode_timing(sess->control_mode);
            sess->deadline_ms = deadline_from_s(tm ? tm->retry_interval_s : 7.0f);
        }
        else
        {
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_EV_APP_DISCONNECT:
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    default:
        break;
    }
}

static void fsm_accepting(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_RX_DATA:
    case ARQ_EV_RX_ACK:
        sess->role        = ARQ_ROLE_CALLEE;
        sess->tx_seq      = 0;
        sess->rx_expected = 0;
        sess->startup_deadline_ms =
            hermes_uptime_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
        if (g_cbs.notify_connected)
            g_cbs.notify_connected(sess->remote_call);
        if (g_timing)
            arq_timing_record_connect(g_timing, sess->control_mode);
        sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        enter_idle_irs(sess);       /* callee receives first; process incoming data */
        if (ev->id == ARQ_EV_RX_DATA)
            fsm_dflow(sess, ev);
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_call_accept(sess, true);
            tm = arq_protocol_mode_timing(sess->control_mode);
            sess->deadline_ms = deadline_from_s(tm ? tm->retry_interval_s : 7.0f);
        }
        else
        {
            sess_enter(sess, ARQ_CONN_LISTENING, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_EV_APP_DISCONNECT:
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    default:
        break;
    }
}

static void fsm_disconnecting(arq_session_t *sess, const arq_event_t *ev)
{
    bool to_no_client = sess->disconnect_to_no_client;
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_RX_DISCONNECT:
        HLOGI(LOG_COMP, "Disconnect finalized (peer ack)");
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(to_no_client);
        if (g_timing) arq_timing_record_disconnect(g_timing, "peer_ack");
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
            tm = arq_protocol_mode_timing(sess->control_mode);
            sess->deadline_ms = deadline_from_s(tm ? tm->retry_interval_s : 7.0f);
            HLOGD(LOG_COMP, "Disconnect tx retry=%d", sess->tx_retries_left);
        }
        else
        {
            HLOGI(LOG_COMP, "Disconnect finalized (timeout)");
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(to_no_client);
            if (g_timing) arq_timing_record_disconnect(g_timing, "timeout");
            sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        break;

    default:
        break;
    }
}

static void fsm_connected(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_APP_DISCONNECT:
        send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess->disconnect_to_no_client = false;
        tm = arq_protocol_mode_timing(sess->control_mode);
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                   ARQ_EV_TIMER_RETRY);
        return;

    case ARQ_EV_RX_DISCONNECT:
        send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        if (g_timing) arq_timing_record_disconnect(g_timing, "rx_disconnect");
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        return;

    case ARQ_EV_TIMER_KEEPALIVE:
        send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE);
        tm = arq_protocol_mode_timing(sess->control_mode);
        dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_TX,
                    deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                    ARQ_EV_TIMER_RETRY);
        return;

    default:
        break;
    }

    fsm_dflow(sess, ev);
}

/* ======================================================================
 * Level 2 data-flow sub-FSM
 * ====================================================================== */

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (sess->dflow_state)
    {
    case ARQ_DFLOW_IDLE_ISS:
        if (ev->id == ARQ_EV_APP_DATA_READY && g_cbs.tx_backlog &&
            g_cbs.tx_backlog() > 0)
        {
            dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
            send_data_frame(sess);
        }
        break;

    case ARQ_DFLOW_DATA_TX:
        if (ev->id == ARQ_EV_TX_STARTED)
        {
            if (g_timing)
                arq_timing_record_tx_start(g_timing, (int)sess->tx_seq,
                                           sess->payload_mode,
                                           g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);
        }
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (g_timing)
                arq_timing_record_tx_end(g_timing, (int)sess->tx_seq);
            tm = arq_protocol_mode_timing(sess->payload_mode);
            sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
            dflow_enter(sess, ARQ_DFLOW_WAIT_ACK,
                        deadline_from_s(tm ? tm->ack_timeout_s : 9.0f),
                        ARQ_EV_TIMER_ACK);
        }
        break;

    case ARQ_DFLOW_WAIT_ACK:
        if (ev->id == ARQ_EV_RX_ACK)
        {
            if (ev->snr_encoded != 0)
                sess->peer_snr_x10 =
                    (int)(arq_protocol_decode_snr((uint8_t)ev->snr_encoded) * 10.0f);
            if (g_timing)
                arq_timing_record_ack_rx(g_timing, (int)sess->tx_seq,
                                         (uint8_t)ev->ack_delay_raw,
                                         sess->peer_snr_x10);
            sess->tx_seq++;
            sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
            if (g_cbs.send_buffer_status)
                g_cbs.send_buffer_status(g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);

            if (sess->peer_has_data)
            {
                if (g_timing) arq_timing_record_turn(g_timing, false, "piggyback");
                enter_idle_irs(sess);
            }
            else
            {
                enter_idle_iss(sess);
            }
        }
        else if (ev->id == ARQ_EV_TIMER_ACK)
        {
            if (sess->tx_retries_left > 0)
            {
                sess->tx_retries_left--;
                if (g_timing)
                    arq_timing_record_retry(g_timing, (int)sess->tx_seq,
                                            ARQ_DATA_RETRY_SLOTS - sess->tx_retries_left,
                                            "ack_timeout");
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_frame(sess);
            }
            else
            {
                HLOGW(LOG_COMP, "Data retry exhausted seq=%d — disconnecting",
                      (int)sess->tx_seq);
                send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
                sess->tx_retries_left = ARQ_DISCONNECT_RETRY_SLOTS;
                tm = arq_protocol_mode_timing(sess->control_mode);
                sess_enter(sess, ARQ_CONN_DISCONNECTING,
                           deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                           ARQ_EV_TIMER_RETRY);
            }
        }
        else if (ev->id == ARQ_EV_RX_TURN_REQ)
        {
            send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_ACK);
            if (g_timing) arq_timing_record_turn(g_timing, false, "turn_req");
            dflow_enter(sess, ARQ_DFLOW_TURN_ACK_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_DFLOW_IDLE_IRS:
        if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Record data arrival and deliver payload */
            if (g_timing)
                arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                          (int)ev->data_bytes,
                                          sess->local_snr_x10);
            sess->rx_expected   = ev->seq + 1;
            sess->last_rx_ms    = hermes_uptime_ms();
            sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;

            /* Guard: allow ARQ_CHANNEL_GUARD_MS for the ISS relay to switch
             * back to RX before our ACK preamble arrives.  ACK is sent
             * when TIMER_ACK fires in DATA_RX. */
            dflow_enter(sess, ARQ_DFLOW_DATA_RX,
                        hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                        ARQ_EV_TIMER_ACK);
        }
        else if (ev->id == ARQ_EV_TIMER_PEER_BACKLOG)
        {
            if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
            {
                send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_REQ);
                sess->tx_retries_left = ARQ_TURN_REQ_RETRIES;
                tm = arq_protocol_mode_timing(sess->control_mode);
                dflow_enter(sess, ARQ_DFLOW_TURN_REQ_TX,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
            else
            {
                enter_idle_irs(sess);
            }
        }
        else if (ev->id == ARQ_EV_APP_DATA_READY)
        {
            send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_REQ);
            sess->tx_retries_left = ARQ_TURN_REQ_RETRIES;
            tm = arq_protocol_mode_timing(sess->control_mode);
            dflow_enter(sess, ARQ_DFLOW_TURN_REQ_TX,
                        deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                        ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_DFLOW_DATA_RX:
        if (ev->id == ARQ_EV_TIMER_ACK)
        {
            /* Channel guard elapsed — now safe to transmit ACK */
            uint32_t delay_ms = (uint32_t)(hermes_uptime_ms() - sess->last_rx_ms);
            send_ack(sess, arq_protocol_encode_ack_delay(delay_ms));
            if (g_timing)
                arq_timing_record_ack_tx(g_timing, (int)sess->rx_expected - 1);
            dflow_enter(sess, ARQ_DFLOW_ACK_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Another frame arrived during guard window; update bookkeeping */
            if (g_timing)
                arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                          (int)ev->data_bytes,
                                          sess->local_snr_x10);
            sess->rx_expected   = ev->seq + 1;
            sess->last_rx_ms    = hermes_uptime_ms();
            sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
        }
        break;

    case ARQ_DFLOW_ACK_TX:
        if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (sess->peer_has_data)
                enter_idle_irs(sess);
            else if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
            {
                if (g_timing) arq_timing_record_turn(g_timing, true, "piggyback");
                enter_idle_iss(sess);
            }
            else
                enter_idle_irs(sess);
        }
        break;

    case ARQ_DFLOW_TURN_REQ_TX:
        if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            tm = arq_protocol_mode_timing(sess->control_mode);
            dflow_enter(sess, ARQ_DFLOW_TURN_REQ_WAIT,
                        deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                        ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_DFLOW_TURN_REQ_WAIT:
        if (ev->id == ARQ_EV_RX_TURN_ACK)
        {
            if (g_timing) arq_timing_record_turn(g_timing, true, "turn_ack");
            enter_idle_iss(sess);
        }
        else if (ev->id == ARQ_EV_TIMER_RETRY)
        {
            if (sess->tx_retries_left > 0)
            {
                sess->tx_retries_left--;
                send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_REQ);
                tm = arq_protocol_mode_timing(sess->control_mode);
                dflow_enter(sess, ARQ_DFLOW_TURN_REQ_TX,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
            else
            {
                enter_idle_irs(sess);
            }
        }
        break;

    case ARQ_DFLOW_TURN_ACK_TX:
        if (ev->id == ARQ_EV_TX_COMPLETE)
            enter_idle_irs(sess);
        break;

    case ARQ_DFLOW_KEEPALIVE_TX:
        if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            tm = arq_protocol_mode_timing(sess->control_mode);
            dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_WAIT,
                        deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                        ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_DFLOW_KEEPALIVE_WAIT:
        if (ev->id == ARQ_EV_RX_KEEPALIVE_ACK)
        {
            sess->keepalive_miss_count = 0;
            if (sess->role == ARQ_ROLE_CALLER)
                enter_idle_irs(sess);
            else
                enter_idle_iss(sess);
        }
        else if (ev->id == ARQ_EV_RX_KEEPALIVE)
        {
            send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE_ACK);
            sess->keepalive_miss_count = 0;
            if (sess->role == ARQ_ROLE_CALLER)
                enter_idle_irs(sess);
            else
                enter_idle_iss(sess);
        }
        else if (ev->id == ARQ_EV_TIMER_RETRY)
        {
            sess->keepalive_miss_count++;
            if (sess->keepalive_miss_count >= ARQ_KEEPALIVE_MISS_LIMIT)
            {
                HLOGW(LOG_COMP, "Keepalive miss limit — disconnecting");
                send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
                sess->tx_retries_left = ARQ_DISCONNECT_RETRY_SLOTS;
                tm = arq_protocol_mode_timing(sess->control_mode);
                sess_enter(sess, ARQ_CONN_DISCONNECTING,
                           deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                           ARQ_EV_TIMER_RETRY);
            }
            else
            {
                send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE);
                tm = arq_protocol_mode_timing(sess->control_mode);
                dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_TX,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
        }
        break;

    case ARQ_DFLOW_MODE_REQ_TX:
    case ARQ_DFLOW_MODE_REQ_WAIT:
    case ARQ_DFLOW_MODE_ACK_TX:
        if (ev->id == ARQ_EV_TX_COMPLETE || ev->id == ARQ_EV_TIMER_RETRY)
            enter_idle_iss(sess);
        break;

    default:
        break;
    }
}

/* ======================================================================
 * Top-level dispatch
 * ====================================================================== */

void arq_fsm_dispatch(arq_session_t *sess, const arq_event_t *ev)
{
    if (!sess || !ev)
        return;

    HLOGD(LOG_COMP, "state=%s dflow=%s ev=%s",
          arq_conn_state_name(sess->conn_state),
          arq_dflow_state_name(sess->dflow_state),
          arq_event_name(ev->id));

    /* Track last RX time and local SNR from any received frame */
    switch (ev->id)
    {
    case ARQ_EV_RX_DATA:
    case ARQ_EV_RX_ACK:
    case ARQ_EV_RX_CALL:
    case ARQ_EV_RX_ACCEPT:
    case ARQ_EV_RX_DISCONNECT:
    case ARQ_EV_RX_TURN_REQ:
    case ARQ_EV_RX_TURN_ACK:
    case ARQ_EV_RX_KEEPALIVE:
    case ARQ_EV_RX_KEEPALIVE_ACK:
        sess->last_rx_ms = hermes_uptime_ms();
        if (ev->snr_encoded != 0)
            sess->local_snr_x10 =
                (int)(arq_protocol_decode_snr((uint8_t)ev->snr_encoded) * 10.0f);
        break;
    default:
        break;
    }

    switch (sess->conn_state)
    {
    case ARQ_CONN_DISCONNECTED:  fsm_disconnected(sess, ev);  break;
    case ARQ_CONN_LISTENING:     fsm_listening(sess, ev);     break;
    case ARQ_CONN_CALLING:       fsm_calling(sess, ev);       break;
    case ARQ_CONN_ACCEPTING:     fsm_accepting(sess, ev);     break;
    case ARQ_CONN_CONNECTED:     fsm_connected(sess, ev);     break;
    case ARQ_CONN_DISCONNECTING: fsm_disconnecting(sess, ev); break;
    default:                                                   break;
    }
}

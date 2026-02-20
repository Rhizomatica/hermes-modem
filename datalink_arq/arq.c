/* HERMES Modem — ARQ datalink entry point (FSM-based rewrite)
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq.h"
#include "arq_fsm.h"
#include "arq_protocol.h"
#include "arq_timing.h"
#include "arq_modem.h"
#include "arq_channels.h"
#include "fsm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#include "../common/hermes_log.h"
#include "../common/defines_modem.h"
#include "../common/ring_buffer_posix.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../modem/framer.h"
#include "../modem/freedv/freedv_api.h"

#define LOG_COMP "arq"

/* ======================================================================
 * Globals required by arq.h
 * ====================================================================== */

arq_info   arq_conn;
fsm_handle arq_fsm;   /* legacy stub kept for link-time compatibility */

/* ======================================================================
 * Module-private state
 * ====================================================================== */

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_tx_buffer_arq_control;
extern cbuf_handle_t data_rx_buffer_arq;

extern void tnc_send_connected(void);
extern void tnc_send_disconnected(void);
extern void tnc_send_buffer(uint32_t bytes);

extern void init_model(void);

static arq_session_t    g_sess;
static arq_timing_ctx_t g_timing;

/* App TX ring buffer (data from TCP client) */
#define APP_TX_BUF_SIZE (64 * 1024)
static uint8_t         g_app_tx_storage[APP_TX_BUF_SIZE];
static cbuf_handle_t   g_app_tx_buf;
static pthread_mutex_t g_app_tx_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Internal event queue */
#define ARQ_EV_QUEUE_CAP 64
static arq_event_t     g_evq[ARQ_EV_QUEUE_CAP];
static size_t          g_evq_head;
static size_t          g_evq_tail;
static size_t          g_evq_count;
static pthread_mutex_t g_evq_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_evq_cond  = PTHREAD_COND_INITIALIZER;

static arq_channel_bus_t g_bus;

static pthread_t g_loop_tid;
static pthread_t g_cmd_tid;
static pthread_t g_payload_tid;

static volatile bool g_running;
static volatile bool g_initialized;

/* ======================================================================
 * Event queue helpers
 * ====================================================================== */

static void evq_push(const arq_event_t *ev)
{
    pthread_mutex_lock(&g_evq_lock);
    if (g_evq_count < ARQ_EV_QUEUE_CAP)
    {
        g_evq[g_evq_tail] = *ev;
        g_evq_tail = (g_evq_tail + 1) % ARQ_EV_QUEUE_CAP;
        g_evq_count++;
        pthread_cond_signal(&g_evq_cond);
    }
    else
    {
        HLOGW(LOG_COMP, "Event queue full — dropped %s",
              arq_event_name(ev->id));
    }
    pthread_mutex_unlock(&g_evq_lock);
}

/* Must be called with g_evq_lock held */
static bool evq_pop_locked(arq_event_t *ev)
{
    if (g_evq_count == 0) return false;
    *ev = g_evq[g_evq_head];
    g_evq_head = (g_evq_head + 1) % ARQ_EV_QUEUE_CAP;
    g_evq_count--;
    return true;
}

/* ======================================================================
 * PTT injection
 * ====================================================================== */

static void ptt_event_inject(int mode, bool ptt_on)
{
    arq_event_t ev = {0};
    ev.id   = ptt_on ? ARQ_EV_TX_STARTED : ARQ_EV_TX_COMPLETE;
    ev.mode = mode;
    evq_push(&ev);
}

/* ======================================================================
 * FSM callbacks
 * ====================================================================== */

static void cb_send_tx_frame(int packet_type, int mode,
                              size_t frame_size, const uint8_t *frame)
{
    if (!frame || frame_size == 0 || frame_size > INT_BUFFER_SIZE)
        return;

    cbuf_handle_t dst = (packet_type == PACKET_TYPE_ARQ_DATA)
                        ? data_tx_buffer_arq
                        : data_tx_buffer_arq_control;

    if (write_buffer(dst, (uint8_t *)frame, frame_size) != 0)
    {
        HLOGW(LOG_COMP, "TX buffer write failed (ptype=%d size=%zu)",
              packet_type, frame_size);
        return;
    }

    arq_action_t action = {
        .type       = (packet_type == PACKET_TYPE_ARQ_DATA)
                      ? ARQ_ACTION_TX_PAYLOAD : ARQ_ACTION_TX_CONTROL,
        .mode       = mode,
        .frame_size = frame_size,
    };
    arq_modem_enqueue(&action);
}

static void cb_notify_connected(const char *remote_call)
{
    snprintf(arq_conn.dst_addr, CALLSIGN_MAX_SIZE, "%s", remote_call);
    arq_conn.TRX = RX;
    tnc_send_connected();
    HLOGI(LOG_COMP, "Connected to %s", remote_call);
}

static void cb_notify_disconnected(bool to_no_client)
{
    (void)to_no_client;
    memset(arq_conn.dst_addr, 0, sizeof(arq_conn.dst_addr));
    arq_conn.TRX = RX;
    tnc_send_disconnected();
    HLOGI(LOG_COMP, "Disconnected");
}

static void cb_deliver_rx_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > INT_BUFFER_SIZE)
        return;
    write_buffer(data_rx_buffer_arq, (uint8_t *)data, len);
}

static int cb_tx_backlog(void)
{
    pthread_mutex_lock(&g_app_tx_mtx);
    int n = (int)size_buffer(g_app_tx_buf);
    pthread_mutex_unlock(&g_app_tx_mtx);
    return n;
}

static int cb_tx_read(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return 0;
    pthread_mutex_lock(&g_app_tx_mtx);
    size_t avail = size_buffer(g_app_tx_buf);
    if (avail > len) avail = len;
    int n = 0;
    if (avail > 0)
        n = (read_buffer(g_app_tx_buf, buf, avail) == 0) ? (int)avail : 0;
    pthread_mutex_unlock(&g_app_tx_mtx);
    return n;
}

static void cb_send_buffer_status(int backlog_bytes)
{
    tnc_send_buffer((uint32_t)(backlog_bytes < 0 ? 0 : backlog_bytes));
}

/* ======================================================================
 * CMD bridge worker
 * ====================================================================== */

static void handle_cmd(const arq_cmd_msg_t *msg)
{
    arq_event_t ev = {0};

    switch (msg->type)
    {
    case ARQ_CMD_SET_CALLSIGN:
        snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s", msg->arg0);
        HLOGI(LOG_COMP, "My callsign: %s", arq_conn.my_call_sign);
        return;

    case ARQ_CMD_SET_BANDWIDTH:
        arq_conn.bw = msg->value;
        return;

    case ARQ_CMD_LISTEN_ON:
        arq_conn.listen = true;
        ev.id = ARQ_EV_APP_LISTEN;
        break;

    case ARQ_CMD_LISTEN_OFF:
        arq_conn.listen = false;
        ev.id = ARQ_EV_APP_STOP_LISTEN;
        break;

    case ARQ_CMD_CONNECT:
        snprintf(ev.remote_call, CALLSIGN_MAX_SIZE, "%s", msg->arg0);
        ev.id = ARQ_EV_APP_CONNECT;
        break;

    case ARQ_CMD_DISCONNECT:
    case ARQ_CMD_CLIENT_DISCONNECT:
        ev.id = ARQ_EV_APP_DISCONNECT;
        break;

    case ARQ_CMD_CLIENT_CONNECT:
        HLOGD(LOG_COMP, "Client (re)connected");
        return;

    case ARQ_CMD_SET_PUBLIC:
    case ARQ_CMD_NONE:
    default:
        return;
    }

    evq_push(&ev);
}

static void *arq_cmd_bridge_worker(void *arg)
{
    arq_cmd_msg_t msg;
    (void)arg;
    while (arq_channel_bus_recv_cmd(&g_bus, &msg) == 0)
        handle_cmd(&msg);
    return NULL;
}

/* ======================================================================
 * Payload bridge worker
 * ====================================================================== */

static void *arq_payload_bridge_worker(void *arg)
{
    arq_bytes_msg_t payload;
    (void)arg;
    while (arq_channel_bus_recv_payload(&g_bus, &payload) == 0)
    {
        if (payload.len == 0 || payload.len > INT_BUFFER_SIZE)
            continue;
        pthread_mutex_lock(&g_app_tx_mtx);
        write_buffer(g_app_tx_buf, payload.data, payload.len);
        pthread_mutex_unlock(&g_app_tx_mtx);

        arq_event_t ev = { .id = ARQ_EV_APP_DATA_READY };
        evq_push(&ev);
    }
    return NULL;
}

/* ======================================================================
 * Main ARQ event loop
 * ====================================================================== */

static void *arq_event_loop_worker(void *arg)
{
    (void)arg;
    HLOGI(LOG_COMP, "Event loop started");

    while (g_running)
    {
        uint64_t now   = hermes_uptime_ms();
        int timeout_ms = arq_fsm_timeout_ms(&g_sess, now);
        if (timeout_ms > 500 || timeout_ms < 0)
            timeout_ms = 500;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000LL;
        if (ts.tv_nsec >= 1000000000LL) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000LL;
        }

        pthread_mutex_lock(&g_evq_lock);
        if (g_evq_count == 0)
            pthread_cond_timedwait(&g_evq_cond, &g_evq_lock, &ts);

        arq_event_t events[ARQ_EV_QUEUE_CAP];
        size_t n = 0;
        arq_event_t ev;
        while (evq_pop_locked(&ev) && n < ARQ_EV_QUEUE_CAP)
            events[n++] = ev;
        pthread_mutex_unlock(&g_evq_lock);

        for (size_t i = 0; i < n; i++)
            arq_fsm_dispatch(&g_sess, &events[i]);

        /* Fire deadline */
        now = hermes_uptime_ms();
        if (g_sess.deadline_ms != UINT64_MAX && now >= g_sess.deadline_ms)
        {
            g_sess.deadline_ms = UINT64_MAX;
            arq_event_t tev = { .id = g_sess.deadline_event };
            arq_fsm_dispatch(&g_sess, &tev);
        }
    }

    HLOGI(LOG_COMP, "Event loop stopped");
    return NULL;
}

/* ======================================================================
 * Incoming frame handling (called from modem.c worker)
 * ====================================================================== */

bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size)
{
    if (!data || frame_size < 2) return false;

    bool is_accept = (data[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG) != 0;
    uint8_t session_id;
    char src[CALLSIGN_MAX_SIZE] = {0};
    char dst[CALLSIGN_MAX_SIZE] = {0};

    int rc = is_accept
             ? arq_protocol_parse_accept(data, frame_size, &session_id, src, dst)
             : arq_protocol_parse_call  (data, frame_size, &session_id, src, dst);

    if (rc < 0)
    {
        HLOGD(LOG_COMP, "CALL/ACCEPT parse failed");
        return false;
    }

    arq_event_t ev = {0};
    ev.id         = is_accept ? ARQ_EV_RX_ACCEPT : ARQ_EV_RX_CALL;
    ev.session_id = session_id;
    /* src = transmitting side's callsign */
    snprintf(ev.remote_call, CALLSIGN_MAX_SIZE, "%s", src);
    evq_push(&ev);
    return true;
}

void arq_handle_incoming_frame(uint8_t *data, size_t frame_size)
{
    if (!data || frame_size < ARQ_FRAME_HDR_SIZE) return;

    arq_frame_hdr_t hdr;
    if (arq_protocol_decode_hdr(data, frame_size, &hdr) < 0)
    {
        HLOGD(LOG_COMP, "Frame header decode failed");
        return;
    }

    arq_event_t ev = {0};
    ev.session_id    = hdr.session_id;
    ev.seq           = hdr.tx_seq;
    ev.ack_seq       = hdr.rx_ack_seq;
    ev.rx_flags      = hdr.flags;
    ev.snr_encoded   = (int8_t)hdr.snr_raw;
    ev.ack_delay_raw = hdr.ack_delay_raw;

    if (hdr.packet_type == PACKET_TYPE_ARQ_DATA)
    {
        ev.id         = ARQ_EV_RX_DATA;
        size_t payload_len = (frame_size > ARQ_FRAME_HDR_SIZE)
                             ? (frame_size - ARQ_FRAME_HDR_SIZE) : 0;
        ev.data_bytes = payload_len;
        /* Deliver payload bytes immediately to RX buffer */
        if (payload_len > 0)
            cb_deliver_rx_data(data + ARQ_FRAME_HDR_SIZE, payload_len);
    }
    else if (hdr.packet_type == PACKET_TYPE_ARQ_CONTROL)
    {
        switch (hdr.subtype)
        {
        case ARQ_SUBTYPE_ACK:          ev.id = ARQ_EV_RX_ACK;           break;
        case ARQ_SUBTYPE_DISCONNECT:   ev.id = ARQ_EV_RX_DISCONNECT;    break;
        case ARQ_SUBTYPE_TURN_REQ:     ev.id = ARQ_EV_RX_TURN_REQ;      break;
        case ARQ_SUBTYPE_TURN_ACK:     ev.id = ARQ_EV_RX_TURN_ACK;      break;
        case ARQ_SUBTYPE_KEEPALIVE:    ev.id = ARQ_EV_RX_KEEPALIVE;     break;
        case ARQ_SUBTYPE_KEEPALIVE_ACK: ev.id = ARQ_EV_RX_KEEPALIVE_ACK; break;
        case ARQ_SUBTYPE_MODE_REQ:
            ev.id   = ARQ_EV_RX_MODE_REQ;
            ev.mode = (frame_size > ARQ_FRAME_HDR_SIZE)
                      ? (int)data[ARQ_FRAME_HDR_SIZE] : 0;
            break;
        case ARQ_SUBTYPE_MODE_ACK:
            ev.id   = ARQ_EV_RX_MODE_ACK;
            ev.mode = (frame_size > ARQ_FRAME_HDR_SIZE)
                      ? (int)data[ARQ_FRAME_HDR_SIZE] : 0;
            break;
        default:
            return;
        }
    }
    else
    {
        return;
    }

    evq_push(&ev);
}

/* ======================================================================
 * Public arq.h API
 * ====================================================================== */

int arq_init(size_t frame_size, int mode)
{
    if (frame_size == 0 || frame_size > INT_BUFFER_SIZE)
    {
        HLOGE(LOG_COMP, "Init failed: bad frame_size=%zu", frame_size);
        return -1;
    }

    memset(&arq_conn, 0, sizeof(arq_conn));
    arq_conn.frame_size      = frame_size;
    arq_conn.mode            = mode;
    arq_conn.call_burst_size = 1;

    init_model();

    g_app_tx_buf = circular_buf_init(g_app_tx_storage, APP_TX_BUF_SIZE);
    if (!g_app_tx_buf)
    {
        HLOGE(LOG_COMP, "Failed to init app TX buffer");
        return -1;
    }

    arq_timing_init(&g_timing);
    arq_fsm_init(&g_sess);
    /* payload_mode and control_mode are set by arq_fsm_init().
     * arq_set_active_modem_mode() will update payload_mode dynamically
     * as the modem switches modes during the session. */

    static const arq_fsm_callbacks_t cbs = {
        .send_tx_frame       = cb_send_tx_frame,
        .notify_connected    = cb_notify_connected,
        .notify_disconnected = cb_notify_disconnected,
        .deliver_rx_data     = cb_deliver_rx_data,
        .tx_backlog          = cb_tx_backlog,
        .tx_read             = cb_tx_read,
        .send_buffer_status  = cb_send_buffer_status,
    };
    arq_fsm_set_callbacks(&cbs);
    arq_fsm_set_timing(&g_timing);
    arq_modem_set_event_fn(ptt_event_inject);
    arq_modem_queue_init(64);

    if (arq_channel_bus_init(&g_bus) < 0)
    {
        HLOGE(LOG_COMP, "Channel bus init failed");
        return -1;
    }

    g_running = true;
    if (pthread_create(&g_loop_tid, NULL, arq_event_loop_worker, NULL) != 0)
    {
        HLOGE(LOG_COMP, "Failed to start event loop thread");
        arq_channel_bus_dispose(&g_bus);
        return -1;
    }

    if (pthread_create(&g_cmd_tid, NULL, arq_cmd_bridge_worker, NULL) != 0 ||
        pthread_create(&g_payload_tid, NULL, arq_payload_bridge_worker, NULL) != 0)
    {
        HLOGE(LOG_COMP, "Failed to start bridge threads");
        g_running = false;
        pthread_mutex_lock(&g_evq_lock);
        pthread_cond_broadcast(&g_evq_cond);
        pthread_mutex_unlock(&g_evq_lock);
        arq_channel_bus_close(&g_bus);
        pthread_join(g_loop_tid, NULL);
        arq_channel_bus_dispose(&g_bus);
        return -1;
    }

    g_initialized = true;
    HLOGI(LOG_COMP, "ARQ initialized (frame=%zu mode=%d)", frame_size, mode);
    return 0;
}

void arq_shutdown(void)
{
    if (!g_initialized) return;
    g_initialized = false;
    g_running     = false;

    arq_channel_bus_close(&g_bus);

    pthread_mutex_lock(&g_evq_lock);
    pthread_cond_broadcast(&g_evq_cond);
    pthread_mutex_unlock(&g_evq_lock);

    arq_modem_queue_shutdown();

    pthread_join(g_loop_tid, NULL);
    pthread_join(g_cmd_tid, NULL);
    pthread_join(g_payload_tid, NULL);

    arq_channel_bus_dispose(&g_bus);

    if (g_app_tx_buf)
    {
        circular_buf_free(g_app_tx_buf);
        g_app_tx_buf = NULL;
    }

    HLOGI(LOG_COMP, "ARQ shutdown complete");
}

void arq_tick_1hz(void) { }

void arq_post_event(int event) { (void)event; }

bool arq_is_link_connected(void)
{
    return g_sess.conn_state == ARQ_CONN_CONNECTED;
}

int arq_queue_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return -1;
    pthread_mutex_lock(&g_app_tx_mtx);
    int rc = write_buffer(g_app_tx_buf, (uint8_t *)data, len);
    pthread_mutex_unlock(&g_app_tx_mtx);
    if (rc == 0)
    {
        arq_event_t ev = { .id = ARQ_EV_APP_DATA_READY };
        evq_push(&ev);
    }
    return rc;
}

int arq_get_tx_backlog_bytes(void)  { return cb_tx_backlog(); }
int arq_get_speed_level(void)       { return g_sess.speed_level; }
int arq_get_payload_mode(void)      { return g_sess.payload_mode; }
int arq_get_control_mode(void)      { return g_sess.control_mode; }
int arq_get_preferred_rx_mode(void) { return arq_modem_preferred_rx_mode(&g_sess); }
int arq_get_preferred_tx_mode(void) { return arq_modem_preferred_tx_mode(&g_sess); }

void arq_set_active_modem_mode(int mode, size_t frame_size)
{
    g_sess.payload_mode = mode;
    arq_conn.mode       = mode;
    arq_conn.frame_size = frame_size;
}

void arq_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded)
{
    (void)sync; (void)rx_status; (void)frame_decoded;
    if (snr > -100.0f && snr < 100.0f)
    {
        if (g_sess.local_snr_x10 == 0)
            g_sess.local_snr_x10 = (int)(snr * 10.0f);
        else
            g_sess.local_snr_x10 = (g_sess.local_snr_x10 * 3 + (int)(snr * 10.0f)) / 4;
    }
}

bool arq_try_dequeue_action(arq_action_t *action)
{
    if (!action) return false;
    return arq_modem_dequeue(action, 0);
}

bool arq_wait_dequeue_action(arq_action_t *action, int timeout_ms)
{
    if (!action) return false;
    return arq_modem_dequeue(action, timeout_ms);
}

bool arq_get_runtime_snapshot(arq_runtime_snapshot_t *snapshot)
{
    if (!snapshot || !g_initialized) return false;
    snapshot->initialized      = true;
    snapshot->connected        = arq_is_link_connected();
    snapshot->trx              = arq_conn.TRX;
    snapshot->tx_backlog_bytes = cb_tx_backlog();
    snapshot->speed_level      = g_sess.speed_level;
    snapshot->payload_mode     = g_sess.payload_mode;
    snapshot->control_mode     = g_sess.control_mode;
    snapshot->preferred_rx_mode = arq_modem_preferred_rx_mode(&g_sess);
    snapshot->preferred_tx_mode = arq_modem_preferred_tx_mode(&g_sess);
    return true;
}

int arq_submit_tcp_cmd(const arq_cmd_msg_t *cmd)
{
    if (!cmd || !g_initialized) return -1;
    return arq_channel_bus_try_send_cmd(&g_bus, cmd);
}

int arq_submit_tcp_payload(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || !g_initialized) return -1;
    return arq_channel_bus_try_send_payload(&g_bus, data, len);
}

void clear_connection_data(void)
{
    pthread_mutex_lock(&g_app_tx_mtx);
    clear_buffer(g_app_tx_buf);
    pthread_mutex_unlock(&g_app_tx_mtx);
    clear_buffer(data_rx_buffer_arq);
    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_tx_buffer_arq_control);
}

void reset_arq_info(arq_info *conn)
{
    if (!conn) return;
    char my_call[CALLSIGN_MAX_SIZE];
    snprintf(my_call, CALLSIGN_MAX_SIZE, "%s", conn->my_call_sign);
    int bw = conn->bw;
    bool listen = conn->listen;
    memset(conn, 0, sizeof(*conn));
    snprintf(conn->my_call_sign, CALLSIGN_MAX_SIZE, "%s", my_call);
    conn->bw              = bw;
    conn->listen          = listen;
    conn->call_burst_size = 1;
}

void call_remote(void)         { }
void callee_accept_connection(void) { }

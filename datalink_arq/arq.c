/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "arq.h"
#include "defines_modem.h"
#include "framer.h"
#include "freedv_api.h"
#include "ring_buffer_posix.h"
#include "tcp_interfaces.h"

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_rx_buffer_arq;

arq_info arq_conn;
fsm_handle arq_fsm;

typedef struct {
    bool initialized;
    uint8_t session_id;
    uint8_t tx_seq;
    uint8_t rx_expected_seq;
    uint8_t outstanding_seq;
    bool waiting_ack;

    size_t outstanding_len;
    size_t outstanding_app_len;
    uint8_t outstanding_frame[INT_BUFFER_SIZE];

    size_t app_tx_len;
    uint8_t app_tx_queue[DATA_TX_BUFFER_SIZE];

    uint16_t my_callsign_hash;
    uint16_t peer_callsign_hash;

    int gear;
    int max_gear;
    int success_streak;
    int failure_streak;
    float snr_ema;

    int retry_interval_s;
    int ack_timeout_s;
    int connect_timeout_s;
    int max_call_retries;
    int max_data_retries;

    time_t connect_deadline;
    time_t next_call_retry_at;
    int call_retries_left;

    time_t next_accept_retry_at;
    int accept_retries_left;

    time_t ack_deadline;
    int data_retries_left;
} arq_ctx_t;

static arq_ctx_t arq_ctx;

enum {
    ARQ_SUBTYPE_CALL = 1,
    ARQ_SUBTYPE_ACCEPT = 2,
    ARQ_SUBTYPE_ACK = 3,
    ARQ_SUBTYPE_DISCONNECT = 4,
    ARQ_SUBTYPE_DATA = 5
};

#define ARQ_PROTO_VERSION 2
#define ARQ_HDR_VERSION_IDX 1
#define ARQ_HDR_SUBTYPE_IDX 2
#define ARQ_HDR_SESSION_IDX 3
#define ARQ_HDR_SEQ_IDX 4
#define ARQ_HDR_ACK_IDX 5
#define ARQ_HDR_GEAR_IDX 6
#define ARQ_HDR_LEN_HI_IDX 7
#define ARQ_HDR_LEN_LO_IDX 8
#define ARQ_PAYLOAD_OFFSET 9

#define ARQ_CALL_RETRY_WINDOW 3
#define ARQ_ACCEPT_RETRY_WINDOW 2

#define ARQ_BASE_CONNECT_TIMEOUT_S 2
#define ARQ_BASE_ACK_TIMEOUT_S 1

static void state_no_connected_client(int event);
static void state_idle(int event);
static void state_listen(int event);
static void state_calling_wait_accept(int event);
static void state_connected(int event);

static inline void arq_lock(void)
{
    pthread_mutex_lock(&arq_fsm.lock);
}

static inline void arq_unlock(void)
{
    pthread_mutex_unlock(&arq_fsm.lock);
}

static bool arq_state_is(fsm_state state)
{
    bool matches;
    arq_lock();
    matches = (arq_ctx.initialized && arq_fsm.current == state);
    arq_unlock();
    return matches;
}

static fsm_state arq_get_idle_or_listen_state(void)
{
    return arq_conn.listen ? state_listen : state_idle;
}

static uint16_t callsign_hash16(const char *callsign)
{
    uint16_t h = 0x6A5D;
    size_t len = strnlen(callsign, CALLSIGN_MAX_SIZE - 1);

    for (size_t i = 0; i < len; i++)
    {
        uint8_t c = (uint8_t)toupper((unsigned char)callsign[i]);
        h = (uint16_t)((h << 5) | (h >> 11));
        h ^= c;
    }

    return h;
}

static int compute_mode_retry_interval_s(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        return 6;
    case FREEDV_MODE_DATAC3:
        return 3;
    case FREEDV_MODE_DATAC4:
        return 2;
    case FREEDV_MODE_DATAC0:
    case FREEDV_MODE_DATAC13:
    case FREEDV_MODE_DATAC14:
    case FREEDV_MODE_FSK_LDPC:
    default:
        return 2;
    }
}

static int compute_max_gear(size_t frame_size)
{
    if (frame_size >= 510)
        return 2;
    if (frame_size >= 126)
        return 1;
    return 0;
}

static size_t chunk_size_for_gear(void)
{
    size_t payload_cap = arq_conn.frame_size - ARQ_PAYLOAD_OFFSET;
    size_t chunk = payload_cap;

    if (arq_ctx.gear <= 0)
        chunk = payload_cap / 4;
    else if (arq_ctx.gear == 1)
        chunk = payload_cap / 2;

    if (chunk < 16)
        chunk = payload_cap;
    if (chunk == 0)
        chunk = payload_cap;

    return chunk;
}

static void maybe_shift_up_locked(void)
{
    if (arq_ctx.gear >= arq_ctx.max_gear)
        return;
    if (arq_ctx.success_streak < 16)
        return;
    if (arq_ctx.snr_ema != 0.0f && arq_ctx.snr_ema < 8.0f)
        return;

    arq_ctx.gear++;
    arq_ctx.success_streak = 0;
    fprintf(stderr, "ARQ gear up -> %d\n", arq_ctx.gear);
}

static void maybe_shift_down_locked(void)
{
    if (arq_ctx.gear <= 0)
        return;
    if (arq_ctx.failure_streak < 2 &&
        !(arq_ctx.snr_ema != 0.0f && arq_ctx.snr_ema < 2.0f))
    {
        return;
    }

    arq_ctx.gear--;
    arq_ctx.failure_streak = 0;
    arq_ctx.success_streak = 0;
    fprintf(stderr, "ARQ gear down -> %d\n", arq_ctx.gear);
}

static void mark_success_locked(void)
{
    arq_ctx.success_streak++;
    arq_ctx.failure_streak = 0;
    maybe_shift_up_locked();
}

static void mark_failure_locked(void)
{
    arq_ctx.failure_streak++;
    arq_ctx.success_streak = 0;
    maybe_shift_down_locked();
}

static int build_frame_locked(uint8_t packet_type,
                              uint8_t subtype,
                              uint8_t seq,
                              uint8_t ack,
                              const uint8_t *payload,
                              uint16_t payload_len,
                              uint8_t *out_frame)
{
    if (arq_conn.frame_size < ARQ_PAYLOAD_OFFSET || payload_len > (arq_conn.frame_size - ARQ_PAYLOAD_OFFSET))
        return -1;

    memset(out_frame, 0, arq_conn.frame_size);
    out_frame[ARQ_HDR_VERSION_IDX] = ARQ_PROTO_VERSION;
    out_frame[ARQ_HDR_SUBTYPE_IDX] = subtype;
    out_frame[ARQ_HDR_SESSION_IDX] = arq_ctx.session_id;
    out_frame[ARQ_HDR_SEQ_IDX] = seq;
    out_frame[ARQ_HDR_ACK_IDX] = ack;
    out_frame[ARQ_HDR_GEAR_IDX] = (uint8_t)arq_ctx.gear;
    out_frame[ARQ_HDR_LEN_HI_IDX] = (uint8_t)((payload_len >> 8) & 0xff);
    out_frame[ARQ_HDR_LEN_LO_IDX] = (uint8_t)(payload_len & 0xff);

    if (payload_len > 0)
        memcpy(out_frame + ARQ_PAYLOAD_OFFSET, payload, payload_len);

    write_frame_header(out_frame, packet_type, arq_conn.frame_size);
    return 0;
}

static int queue_frame_locked(const uint8_t *frame)
{
    return write_buffer(data_tx_buffer_arq, (uint8_t *)frame, arq_conn.frame_size);
}

static int build_callsign_payload_locked(const char *src,
                                         const char *dst,
                                         uint8_t *payload,
                                         uint16_t *payload_len)
{
    size_t src_len = strnlen(src, CALLSIGN_MAX_SIZE - 1);
    size_t dst_len = strnlen(dst, CALLSIGN_MAX_SIZE - 1);

    if (src_len == 0 || dst_len == 0)
        return -1;
    if ((size_t)ARQ_PAYLOAD_OFFSET + 2 + src_len + dst_len > arq_conn.frame_size)
        return -1;

    payload[0] = (uint8_t)src_len;
    payload[1] = (uint8_t)dst_len;
    memcpy(payload + 2, src, src_len);
    memcpy(payload + 2 + src_len, dst, dst_len);
    *payload_len = (uint16_t)(2 + src_len + dst_len);
    return 0;
}

static int send_call_locked(void)
{
    uint8_t payload[2 + (CALLSIGN_MAX_SIZE * 2)];
    uint16_t payload_len = 0;
    uint8_t frame[INT_BUFFER_SIZE];

    if (build_callsign_payload_locked(arq_conn.src_addr, arq_conn.dst_addr, payload, &payload_len) < 0)
        return -1;
    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL, ARQ_SUBTYPE_CALL, 0, 0, payload, payload_len, frame) < 0)
        return -1;

    return queue_frame_locked(frame);
}

static int send_accept_locked(void)
{
    uint8_t payload[2 + (CALLSIGN_MAX_SIZE * 2)];
    uint16_t payload_len = 0;
    uint8_t frame[INT_BUFFER_SIZE];

    if (build_callsign_payload_locked(arq_conn.my_call_sign, arq_conn.dst_addr, payload, &payload_len) < 0)
        return -1;
    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL, ARQ_SUBTYPE_ACCEPT, 0, 0, payload, payload_len, frame) < 0)
        return -1;

    return queue_frame_locked(frame);
}

static int send_ack_locked(uint8_t ack_seq)
{
    uint8_t frame[INT_BUFFER_SIZE];

    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL, ARQ_SUBTYPE_ACK, 0, ack_seq, NULL, 0, frame) < 0)
        return -1;
    return queue_frame_locked(frame);
}

static int send_disconnect_locked(void)
{
    uint8_t frame[INT_BUFFER_SIZE];

    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL, ARQ_SUBTYPE_DISCONNECT, 0, 0, NULL, 0, frame) < 0)
        return -1;
    return queue_frame_locked(frame);
}

static void clear_runtime_locked(void)
{
    int retry_interval_s = arq_ctx.retry_interval_s;
    int ack_timeout_s = arq_ctx.ack_timeout_s;
    int connect_timeout_s = arq_ctx.connect_timeout_s;
    int max_call_retries = arq_ctx.max_call_retries;
    int max_data_retries = arq_ctx.max_data_retries;
    int max_gear = arq_ctx.max_gear;

    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_rx_buffer_arq);

    memset(&arq_ctx, 0, sizeof(arq_ctx));
    arq_ctx.initialized = true;
    arq_ctx.retry_interval_s = retry_interval_s;
    arq_ctx.ack_timeout_s = ack_timeout_s;
    arq_ctx.connect_timeout_s = connect_timeout_s;
    arq_ctx.max_call_retries = max_call_retries;
    arq_ctx.max_data_retries = max_data_retries;
    arq_ctx.max_gear = max_gear;
    arq_ctx.gear = 0;

    arq_conn.TRX = RX;
    arq_conn.encryption = false;
    arq_conn.listen = false;
    arq_conn.bw = 0;
    arq_conn.call_burst_size = 1;
    arq_conn.my_call_sign[0] = 0;
    arq_conn.src_addr[0] = 0;
    arq_conn.dst_addr[0] = 0;
}

static void notify_disconnected_locked(void)
{
    arq_ctx.waiting_ack = false;
    arq_ctx.outstanding_len = 0;
    arq_ctx.outstanding_app_len = 0;
    arq_ctx.app_tx_len = 0;
    arq_ctx.call_retries_left = 0;
    arq_ctx.accept_retries_left = 0;
    arq_ctx.data_retries_left = 0;
    arq_ctx.connect_deadline = 0;
    arq_ctx.next_call_retry_at = 0;
    arq_ctx.next_accept_retry_at = 0;
    arq_ctx.ack_deadline = 0;
    tnc_send_disconnected();
}

static void enter_connected_locked(void)
{
    arq_ctx.call_retries_left = 0;
    arq_ctx.connect_deadline = 0;
    arq_ctx.next_call_retry_at = 0;
    arq_ctx.failure_streak = 0;
    arq_ctx.success_streak = 0;
    arq_fsm.current = state_connected;
    tnc_send_connected();
}

static void start_calling_locked(void)
{
    time_t now = time(NULL);
    arq_ctx.session_id++;
    arq_ctx.tx_seq = 0;
    arq_ctx.rx_expected_seq = 0;
    arq_ctx.waiting_ack = false;
    arq_ctx.outstanding_len = 0;
    arq_ctx.outstanding_app_len = 0;
    arq_ctx.call_retries_left = arq_ctx.max_call_retries;
    arq_ctx.connect_deadline = now + arq_ctx.connect_timeout_s;
    arq_ctx.next_call_retry_at = now + arq_ctx.retry_interval_s;

    arq_ctx.my_callsign_hash = callsign_hash16(arq_conn.src_addr);
    arq_ctx.peer_callsign_hash = callsign_hash16(arq_conn.dst_addr);

    send_call_locked();
    arq_fsm.current = state_calling_wait_accept;
}

static void maybe_send_next_data_locked(void)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t chunk;
    time_t now;

    if (arq_fsm.current != state_connected)
        return;
    if (arq_ctx.waiting_ack)
        return;
    if (arq_ctx.app_tx_len == 0)
        return;

    chunk = chunk_size_for_gear();
    if (chunk > arq_ctx.app_tx_len)
        chunk = arq_ctx.app_tx_len;

    if (build_frame_locked(PACKET_TYPE_ARQ_DATA,
                           ARQ_SUBTYPE_DATA,
                           arq_ctx.tx_seq,
                           (uint8_t)(arq_ctx.rx_expected_seq - 1),
                           arq_ctx.app_tx_queue,
                           (uint16_t)chunk,
                           frame) < 0)
    {
        return;
    }

    if (queue_frame_locked(frame) < 0)
        return;

    memcpy(arq_ctx.outstanding_frame, frame, arq_conn.frame_size);
    arq_ctx.outstanding_len = arq_conn.frame_size;
    arq_ctx.outstanding_seq = arq_ctx.tx_seq;
    arq_ctx.outstanding_app_len = chunk;
    arq_ctx.waiting_ack = true;
    arq_ctx.tx_seq++;
    arq_ctx.data_retries_left = arq_ctx.max_data_retries;
    now = time(NULL);
    arq_ctx.ack_deadline = now + arq_ctx.ack_timeout_s;
}

static bool parse_callsign_payload(const uint8_t *payload,
                                   size_t payload_len,
                                   char *src,
                                   char *dst)
{
    if (payload_len < 2)
        return false;

    size_t src_len = payload[0];
    size_t dst_len = payload[1];

    if (src_len == 0 || dst_len == 0)
        return false;
    if (src_len >= CALLSIGN_MAX_SIZE || dst_len >= CALLSIGN_MAX_SIZE)
        return false;
    if (2 + src_len + dst_len > payload_len)
        return false;

    memcpy(src, payload + 2, src_len);
    src[src_len] = 0;
    memcpy(dst, payload + 2 + src_len, dst_len);
    dst[dst_len] = 0;
    return true;
}

static void state_no_connected_client(int event)
{
    if (event != EV_CLIENT_CONNECT)
        return;

    clear_runtime_locked();
    arq_fsm.current = state_idle;
}

static void state_idle(int event)
{
    switch (event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        arq_fsm.current = state_listen;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_CALL_REMOTE:
        if (arq_conn.my_call_sign[0] != 0 && arq_conn.src_addr[0] == 0)
        {
            strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
            arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
        }
        if (arq_conn.src_addr[0] == 0 || arq_conn.dst_addr[0] == 0)
            break;
        start_calling_locked();
        break;
    case EV_CLIENT_DISCONNECT:
        clear_runtime_locked();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

static void state_listen(int event)
{
    switch (event)
    {
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        arq_fsm.current = state_idle;
        break;
    case EV_LINK_CALL_REMOTE:
        if (arq_conn.my_call_sign[0] != 0 && arq_conn.src_addr[0] == 0)
        {
            strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
            arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
        }
        if (arq_conn.src_addr[0] == 0 || arq_conn.dst_addr[0] == 0)
            break;
        start_calling_locked();
        break;
    case EV_CLIENT_DISCONNECT:
        clear_runtime_locked();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

static void state_calling_wait_accept(int event)
{
    switch (event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_DISCONNECT:
        send_disconnect_locked();
        notify_disconnected_locked();
        arq_fsm.current = arq_get_idle_or_listen_state();
        break;
    case EV_CLIENT_DISCONNECT:
        send_disconnect_locked();
        notify_disconnected_locked();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

static void state_connected(int event)
{
    switch (event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_DISCONNECT:
        send_disconnect_locked();
        notify_disconnected_locked();
        arq_fsm.current = arq_get_idle_or_listen_state();
        break;
    case EV_CLIENT_DISCONNECT:
        send_disconnect_locked();
        notify_disconnected_locked();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

int arq_init(size_t frame_size, int mode)
{
    if (frame_size < 64 || frame_size > INT_BUFFER_SIZE)
    {
        fprintf(stderr, "ARQ init failed: unsupported modem frame size %zu\n", frame_size);
        return EXIT_FAILURE;
    }

    memset(&arq_conn, 0, sizeof(arq_conn));
    memset(&arq_ctx, 0, sizeof(arq_ctx));

    arq_conn.frame_size = frame_size;
    arq_conn.mode = mode;
    arq_conn.call_burst_size = 1;
    arq_ctx.initialized = true;
    arq_ctx.retry_interval_s = compute_mode_retry_interval_s(mode);
    arq_ctx.ack_timeout_s = arq_ctx.retry_interval_s + ARQ_BASE_ACK_TIMEOUT_S;
    arq_ctx.connect_timeout_s = ARQ_BASE_CONNECT_TIMEOUT_S + (arq_ctx.retry_interval_s * (ARQ_CALL_RETRY_WINDOW + 1));
    arq_ctx.max_call_retries = ARQ_CALL_RETRY_WINDOW;
    arq_ctx.max_data_retries = 3;
    arq_ctx.gear = 0;
    arq_ctx.max_gear = compute_max_gear(frame_size);

    fsm_init(&arq_fsm, state_no_connected_client);
    return EXIT_SUCCESS;
}

void arq_shutdown(void)
{
    if (!arq_ctx.initialized)
        return;

    arq_lock();
    arq_ctx.initialized = false;
    arq_unlock();
    fsm_destroy(&arq_fsm);
}

bool arq_is_link_connected(void)
{
    return arq_state_is(state_connected);
}

void arq_tick_1hz(void)
{
    time_t now = time(NULL);

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return;
    }

    if (arq_fsm.current == state_calling_wait_accept)
    {
        if (arq_ctx.call_retries_left > 0 && now >= arq_ctx.next_call_retry_at)
        {
            send_call_locked();
            arq_ctx.call_retries_left--;
            arq_ctx.next_call_retry_at = now + arq_ctx.retry_interval_s;
        }

        if (now >= arq_ctx.connect_deadline)
        {
            mark_failure_locked();
            notify_disconnected_locked();
            arq_fsm.current = arq_get_idle_or_listen_state();
        }
    }

    if (arq_ctx.accept_retries_left > 0 &&
        (arq_fsm.current == state_connected || arq_fsm.current == state_calling_wait_accept) &&
        now >= arq_ctx.next_accept_retry_at)
    {
        send_accept_locked();
        arq_ctx.accept_retries_left--;
        arq_ctx.next_accept_retry_at = now + arq_ctx.retry_interval_s;
    }

    if (arq_fsm.current == state_connected && arq_ctx.waiting_ack && now >= arq_ctx.ack_deadline)
    {
        if (arq_ctx.data_retries_left > 0)
        {
            queue_frame_locked(arq_ctx.outstanding_frame);
            arq_ctx.data_retries_left--;
            arq_ctx.ack_deadline = now + arq_ctx.ack_timeout_s;
            mark_failure_locked();
        }
        else
        {
            send_disconnect_locked();
            notify_disconnected_locked();
            arq_fsm.current = arq_get_idle_or_listen_state();
        }
    }

    arq_unlock();
}

int arq_queue_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0)
        return 0;

    arq_lock();
    if (!arq_ctx.initialized || arq_fsm.current != state_connected)
    {
        arq_unlock();
        return 0;
    }

    size_t free_space = DATA_TX_BUFFER_SIZE - arq_ctx.app_tx_len;
    if (len > free_space)
        len = free_space;

    if (len == 0)
    {
        arq_unlock();
        return 0;
    }

    memcpy(arq_ctx.app_tx_queue + arq_ctx.app_tx_len, data, len);
    arq_ctx.app_tx_len += len;
    maybe_send_next_data_locked();
    arq_unlock();

    return (int)len;
}

static void handle_control_frame_locked(uint8_t subtype,
                                        uint8_t session_id,
                                        uint8_t ack,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    char src[CALLSIGN_MAX_SIZE] = {0};
    char dst[CALLSIGN_MAX_SIZE] = {0};

    switch (subtype)
    {
    case ARQ_SUBTYPE_CALL:
        if (!parse_callsign_payload(payload, payload_len, src, dst))
            return;
        if (arq_conn.my_call_sign[0] == 0 || strcasecmp(dst, arq_conn.my_call_sign) != 0)
            return;
        if (arq_fsm.current != state_listen && arq_fsm.current != state_connected)
            return;

        strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
        arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
        strncpy(arq_conn.dst_addr, src, CALLSIGN_MAX_SIZE - 1);
        arq_conn.dst_addr[CALLSIGN_MAX_SIZE - 1] = 0;

        arq_ctx.session_id = session_id;
        arq_ctx.peer_callsign_hash = callsign_hash16(src);
        arq_ctx.my_callsign_hash = callsign_hash16(arq_conn.my_call_sign);
        arq_ctx.rx_expected_seq = 0;
        arq_ctx.waiting_ack = false;

        send_accept_locked();
        arq_ctx.accept_retries_left = ARQ_ACCEPT_RETRY_WINDOW;
        arq_ctx.next_accept_retry_at = time(NULL) + arq_ctx.retry_interval_s;

        if (arq_fsm.current != state_connected)
            enter_connected_locked();
        return;

    case ARQ_SUBTYPE_ACCEPT:
        if (arq_fsm.current != state_calling_wait_accept)
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (!parse_callsign_payload(payload, payload_len, src, dst))
            return;
        if (strcasecmp(src, arq_conn.dst_addr) != 0)
            return;
        if (strcasecmp(dst, arq_conn.src_addr) != 0)
            return;

        enter_connected_locked();
        mark_success_locked();
        maybe_send_next_data_locked();
        return;

    case ARQ_SUBTYPE_ACK:
        if (arq_fsm.current != state_connected || !arq_ctx.waiting_ack)
            return;
        if (session_id != arq_ctx.session_id || ack != arq_ctx.outstanding_seq)
            return;

        arq_ctx.waiting_ack = false;
        if (arq_ctx.outstanding_app_len <= arq_ctx.app_tx_len)
        {
            memmove(arq_ctx.app_tx_queue,
                    arq_ctx.app_tx_queue + arq_ctx.outstanding_app_len,
                    arq_ctx.app_tx_len - arq_ctx.outstanding_app_len);
            arq_ctx.app_tx_len -= arq_ctx.outstanding_app_len;
        }
        arq_ctx.outstanding_app_len = 0;
        arq_ctx.outstanding_len = 0;
        mark_success_locked();
        maybe_send_next_data_locked();
        return;

    case ARQ_SUBTYPE_DISCONNECT:
        if (arq_fsm.current == state_connected || arq_fsm.current == state_calling_wait_accept)
        {
            notify_disconnected_locked();
            arq_fsm.current = arq_get_idle_or_listen_state();
        }
        return;

    default:
        return;
    }
}

static void handle_data_frame_locked(uint8_t session_id,
                                     uint8_t seq,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    if (arq_fsm.current != state_connected)
        return;
    if (session_id != arq_ctx.session_id)
        return;

    if (seq == arq_ctx.rx_expected_seq)
    {
        write_buffer(data_rx_buffer_arq, (uint8_t *)payload, payload_len);
        arq_ctx.rx_expected_seq++;
        send_ack_locked(seq);
        mark_success_locked();
        return;
    }

    if ((uint8_t)(arq_ctx.rx_expected_seq - 1) == seq)
    {
        send_ack_locked(seq);
    }
}

void arq_handle_incoming_frame(uint8_t *data, size_t frame_size)
{
    uint8_t packet_type;
    uint8_t version;
    uint8_t subtype;
    uint8_t session_id;
    uint8_t seq;
    uint8_t ack;
    uint16_t payload_len;
    const uint8_t *payload;

    if (!data || frame_size < ARQ_PAYLOAD_OFFSET)
        return;

    packet_type = (data[0] >> 6) & 0x3;
    version = data[ARQ_HDR_VERSION_IDX];
    subtype = data[ARQ_HDR_SUBTYPE_IDX];
    session_id = data[ARQ_HDR_SESSION_IDX];
    seq = data[ARQ_HDR_SEQ_IDX];
    ack = data[ARQ_HDR_ACK_IDX];
    payload_len = (uint16_t)(((uint16_t)data[ARQ_HDR_LEN_HI_IDX] << 8) | data[ARQ_HDR_LEN_LO_IDX]);

    if (version != ARQ_PROTO_VERSION)
        return;
    if ((size_t)ARQ_PAYLOAD_OFFSET + payload_len > frame_size)
        return;

    payload = data + ARQ_PAYLOAD_OFFSET;

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return;
    }

    if (packet_type == PACKET_TYPE_ARQ_CONTROL)
        handle_control_frame_locked(subtype, session_id, ack, payload, payload_len);
    else if (packet_type == PACKET_TYPE_ARQ_DATA && subtype == ARQ_SUBTYPE_DATA)
        handle_data_frame_locked(session_id, seq, payload, payload_len);

    arq_unlock();
}

void arq_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded)
{
    (void)sync;

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return;
    }

    if (snr > -20.0f && snr < 30.0f)
    {
        if (arq_ctx.snr_ema == 0.0f)
            arq_ctx.snr_ema = snr;
        else
            arq_ctx.snr_ema = (0.8f * arq_ctx.snr_ema) + (0.2f * snr);
    }

    if (!frame_decoded && (rx_status & 0x4))
        mark_failure_locked();

    arq_unlock();
}

void clear_connection_data(void)
{
    arq_lock();
    if (arq_ctx.initialized)
        clear_runtime_locked();
    arq_unlock();
}

void reset_arq_info(arq_info *arq_conn_i)
{
    if (!arq_conn_i)
        return;

    arq_conn_i->TRX = RX;
    arq_conn_i->my_call_sign[0] = 0;
    arq_conn_i->src_addr[0] = 0;
    arq_conn_i->dst_addr[0] = 0;
    arq_conn_i->encryption = false;
    arq_conn_i->call_burst_size = 1;
    arq_conn_i->listen = false;
    arq_conn_i->bw = 0;
    arq_conn_i->frame_size = 0;
    arq_conn_i->mode = 0;
}

void call_remote(void)
{
    arq_lock();
    if (arq_ctx.initialized && arq_conn.src_addr[0] != 0 && arq_conn.dst_addr[0] != 0)
        send_call_locked();
    arq_unlock();
}

void callee_accept_connection(void)
{
    arq_lock();
    if (arq_ctx.initialized && arq_conn.my_call_sign[0] != 0 && arq_conn.dst_addr[0] != 0)
        send_accept_locked();
    arq_unlock();
}

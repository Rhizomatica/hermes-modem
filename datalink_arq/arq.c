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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <limits.h>

#include "arq.h"
#include "defines_modem.h"
#include "framer.h"
#include "freedv_api.h"
#include "ring_buffer_posix.h"
#include "tcp_interfaces.h"

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_tx_buffer_arq_control;
extern cbuf_handle_t data_rx_buffer_arq;
extern void init_model(void);
extern int arithmetic_encode(const char *msg, uint8_t *output);
extern int arithmetic_decode(uint8_t *input, int max_len, char *output);

arq_info arq_conn;
fsm_handle arq_fsm;

typedef enum {
    ARQ_ROLE_NONE = 0,
    ARQ_ROLE_CALLER = 1,
    ARQ_ROLE_CALLEE = 2
} arq_role_t;

typedef struct {
    bool initialized;
    arq_role_t role;

    uint8_t session_id;
    uint8_t tx_seq;
    uint8_t rx_expected_seq;
    uint8_t outstanding_seq;

    int slot_len_s;
    int tx_period_s;
    int connect_timeout_s;
    int ack_timeout_s;
    int max_call_retries;
    int max_accept_retries;
    int max_data_retries;

    time_t connect_deadline;
    time_t next_role_tx_at;
    time_t remote_busy_until;
    int call_retries_left;
    int accept_retries_left;

    bool waiting_ack;
    int data_retries_left;
    time_t ack_deadline;
    size_t outstanding_frame_len;
    size_t outstanding_app_len;
    uint8_t outstanding_frame[INT_BUFFER_SIZE];

    bool pending_call;
    bool pending_accept;
    bool pending_ack;
    bool pending_disconnect;
    bool pending_keepalive;
    bool pending_keepalive_ack;
    uint8_t pending_ack_seq;
    bool keepalive_waiting;
    int keepalive_misses;
    int keepalive_interval_s;
    int keepalive_miss_limit;
    time_t keepalive_deadline;
    time_t last_keepalive_rx;
    time_t last_keepalive_tx;
    time_t last_phy_activity;

    bool disconnect_in_progress;
    bool disconnect_to_no_client;
    int disconnect_retries_left;
    time_t disconnect_deadline;

    size_t app_tx_len;
    uint8_t app_tx_queue[DATA_TX_BUFFER_SIZE];

    int gear;
    int max_gear;
    int success_streak;
    int failure_streak;
    float snr_ema;
    int payload_mode;
    int control_mode;
} arq_ctx_t;

static arq_ctx_t arq_ctx;

enum {
    ARQ_SUBTYPE_CALL = 1,
    ARQ_SUBTYPE_ACCEPT = 2,
    ARQ_SUBTYPE_ACK = 3,
    ARQ_SUBTYPE_DISCONNECT = 4,
    ARQ_SUBTYPE_DATA = 5,
    ARQ_SUBTYPE_KEEPALIVE = 6,
    ARQ_SUBTYPE_KEEPALIVE_ACK = 7
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

#define ARQ_CONNECT_SESSION_IDX 1
#define ARQ_CONNECT_PAYLOAD_IDX 2
#define ARQ_CONNECT_META_SIZE 2
#define ARQ_CONNECT_MAX_ENCODED (14 - ARQ_CONNECT_META_SIZE)
#define ARQ_ARITH_BUFFER_SIZE 4096

#define ARQ_CALL_RETRY_SLOTS 4
#define ARQ_ACCEPT_RETRY_SLOTS 3
#define ARQ_DATA_RETRY_SLOTS 6
#define ARQ_CONNECT_GRACE_SLOTS 2
#define ARQ_CHANNEL_GUARD_S 1
#define ARQ_CONNECT_BUSY_EXT_S 2
#define ARQ_DISCONNECT_RETRY_SLOTS 2
#define ARQ_KEEPALIVE_INTERVAL_S 10
#define ARQ_KEEPALIVE_MISS_LIMIT 5
#define ARQ_SNR_HYST_DB 1.0f

static void state_no_connected_client(int event);
static void state_idle(int event);
static void state_listen(int event);
static void state_calling_wait_accept(int event);
static void state_connected(int event);
static void state_disconnecting(int event);

static inline void arq_lock(void)
{
    pthread_mutex_lock(&arq_fsm.lock);
}

static inline void arq_unlock(void)
{
    pthread_mutex_unlock(&arq_fsm.lock);
}

static fsm_state idle_or_listen_state_locked(void)
{
    return arq_conn.listen ? state_listen : state_idle;
}

static int mode_slot_len_s(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        return 7;
    case FREEDV_MODE_DATAC3:
        return 4;
    case FREEDV_MODE_DATAC4:
        return 3;
    case FREEDV_MODE_DATAC0:
    case FREEDV_MODE_DATAC13:
    case FREEDV_MODE_DATAC14:
    case FREEDV_MODE_FSK_LDPC:
    default:
        return 2;
    }
}

static const char *mode_name(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        return "DATAC1";
    case FREEDV_MODE_DATAC3:
        return "DATAC3";
    case FREEDV_MODE_DATAC4:
        return "DATAC4";
    case FREEDV_MODE_DATAC0:
        return "DATAC0";
    case FREEDV_MODE_DATAC13:
        return "DATAC13";
    case FREEDV_MODE_DATAC14:
        return "DATAC14";
    default:
        return "OTHER";
    }
}

static int payload_mode_from_snr(float snr)
{
    if (snr >= 5.0f)
        return FREEDV_MODE_DATAC1;
    if (snr >= 0.0f)
        return FREEDV_MODE_DATAC3;
    return FREEDV_MODE_DATAC4;
}

static void update_payload_mode_locked(void)
{
    int old_mode = arq_ctx.payload_mode;
    int new_mode = old_mode;
    float snr = arq_ctx.snr_ema;

    if (snr == 0.0f)
        return;

    switch (old_mode)
    {
    case FREEDV_MODE_DATAC1:
        if (snr < (5.0f - ARQ_SNR_HYST_DB))
            new_mode = FREEDV_MODE_DATAC3;
        break;
    case FREEDV_MODE_DATAC3:
        if (snr >= (5.0f + ARQ_SNR_HYST_DB))
            new_mode = FREEDV_MODE_DATAC1;
        else if (snr < (0.0f - ARQ_SNR_HYST_DB))
            new_mode = FREEDV_MODE_DATAC4;
        break;
    case FREEDV_MODE_DATAC4:
    default:
        if (snr >= (0.0f + ARQ_SNR_HYST_DB))
            new_mode = FREEDV_MODE_DATAC3;
        break;
    }

    if (old_mode == 0)
        new_mode = payload_mode_from_snr(snr);

    if (new_mode == old_mode)
        return;

    arq_ctx.payload_mode = new_mode;
    arq_ctx.slot_len_s = mode_slot_len_s(new_mode);
    arq_ctx.tx_period_s = arq_ctx.slot_len_s;
    arq_ctx.ack_timeout_s = (arq_ctx.slot_len_s * 2) + ARQ_CHANNEL_GUARD_S;
    fprintf(stderr, "ARQ payload mode -> %s (snr=%.1f)\n", mode_name(new_mode), snr);
}

static int max_gear_for_frame_size(size_t frame_size)
{
    if (frame_size >= 510)
        return 2;
    if (frame_size >= 126)
        return 1;
    return 0;
}

static size_t chunk_size_for_gear_locked(void)
{
    size_t cap = arq_conn.frame_size - ARQ_PAYLOAD_OFFSET;
    return cap;
}

static size_t control_frame_size_locked(void)
{
    return 14; /* DATAC13 payload bytes per modem frame */
}

static int initial_gear_locked(void)
{
    if (arq_ctx.max_gear >= 2)
    {
        if (arq_ctx.snr_ema >= 8.0f)
            return 2;
        return 1;
    }

    if (arq_ctx.max_gear == 1 && arq_ctx.snr_ema >= 6.0f)
        return 1;

    return 0;
}

static void maybe_gear_up_locked(void)
{
    if (arq_ctx.gear >= arq_ctx.max_gear)
        return;
    if (arq_ctx.success_streak < 6)
        return;
    if (arq_ctx.snr_ema != 0.0f && arq_ctx.snr_ema < 8.0f)
        return;

    arq_ctx.gear++;
    arq_ctx.success_streak = 0;
    fprintf(stderr, "ARQ gear up -> %d\n", arq_ctx.gear);
}

static void maybe_gear_down_locked(void)
{
    if (arq_ctx.gear <= 0)
        return;
    if (arq_ctx.failure_streak < 3 &&
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
    maybe_gear_up_locked();
}

static void mark_failure_locked(void)
{
    arq_ctx.failure_streak++;
    arq_ctx.success_streak = 0;
    maybe_gear_down_locked();
}

static void mark_link_activity_locked(time_t now)
{
    arq_ctx.last_keepalive_rx = now;
    arq_ctx.keepalive_waiting = false;
    arq_ctx.keepalive_misses = 0;
}

static int compute_inter_frame_interval_locked(time_t now, bool with_jitter)
{
    int interval = arq_ctx.slot_len_s;
    if (interval < 1)
        interval = 1;

    if (arq_ctx.snr_ema != 0.0f && arq_ctx.snr_ema < 2.0f)
        interval += 1;
    if (with_jitter)
        interval += (int)(now & 0x1);

    return interval;
}

static void schedule_next_tx_locked(time_t now, bool with_jitter)
{
    arq_ctx.next_role_tx_at = now + compute_inter_frame_interval_locked(now, with_jitter);
}

static bool defer_tx_if_busy_locked(time_t now)
{
    if (now >= arq_ctx.remote_busy_until)
        return false;

    if (arq_ctx.next_role_tx_at < arq_ctx.remote_busy_until)
        arq_ctx.next_role_tx_at = arq_ctx.remote_busy_until;
    return true;
}

static int queue_frame_locked(const uint8_t *frame, size_t frame_size, bool control_plane)
{
    cbuf_handle_t tx_buffer = control_plane ? data_tx_buffer_arq_control : data_tx_buffer_arq;
    return write_buffer(tx_buffer, (uint8_t *)frame, frame_size);
}

static int build_frame_locked(uint8_t packet_type,
                              uint8_t subtype,
                              uint8_t seq,
                              uint8_t ack,
                              const uint8_t *payload,
                              uint16_t payload_len,
                              uint8_t *out_frame,
                              size_t frame_size)
{
    if (frame_size < ARQ_PAYLOAD_OFFSET)
        return -1;
    if ((size_t)ARQ_PAYLOAD_OFFSET + payload_len > frame_size)
        return -1;

    memset(out_frame, 0, frame_size);
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

    write_frame_header(out_frame, packet_type, frame_size);
    return 0;
}

static int encode_connect_callsign_payload(const char *msg, uint8_t *encoded, size_t encoded_cap)
{
    uint8_t encoded_full[ARQ_ARITH_BUFFER_SIZE];
    char normalized[(CALLSIGN_MAX_SIZE * 2) + 2];
    int n;

    n = snprintf(normalized, sizeof(normalized), "%s", msg);
    if (n <= 0 || n >= (int)sizeof(normalized))
        return -1;

    for (int i = 0; i < n; i++)
    {
        if (normalized[i] >= 'a' && normalized[i] <= 'z')
            normalized[i] = (char)(normalized[i] - ('a' - 'A'));
    }

    init_model();
    int enc_len = arithmetic_encode(normalized, encoded_full);
    if (enc_len <= 0)
        return -1;

    if ((size_t)enc_len > encoded_cap)
        enc_len = (int)encoded_cap;

    memcpy(encoded, encoded_full, (size_t)enc_len);
    return enc_len;
}

static bool decode_connect_callsign_payload(const uint8_t *encoded, char *decoded)
{
    init_model();
    if (arithmetic_decode((uint8_t *)encoded, ARQ_CONNECT_MAX_ENCODED, decoded) < 0)
        return false;
    return decoded[0] != 0;
}

static int build_connect_call_accept_frame_locked(uint8_t subtype,
                                                  uint8_t session_id,
                                                  const char *msg,
                                                  uint8_t *frame,
                                                  size_t frame_size)
{
    uint8_t encoded[ARQ_CONNECT_MAX_ENCODED];
    int encoded_len;

    if (frame_size != 14)
        return -1;
    if (subtype != ARQ_SUBTYPE_CALL && subtype != ARQ_SUBTYPE_ACCEPT)
        return -1;

    encoded_len = encode_connect_callsign_payload(msg, encoded, sizeof(encoded));
    if (encoded_len <= 0)
        return -1;

    memset(frame, 0, frame_size);
    frame[ARQ_CONNECT_SESSION_IDX] = session_id;
    memcpy(frame + ARQ_CONNECT_PAYLOAD_IDX, encoded, (size_t)encoded_len);
    write_frame_header(frame, PACKET_TYPE_ARQ_DATA, frame_size);
    return 0;
}

static bool parse_callsign_pair_payload(const uint8_t *payload,
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

static int send_call_locked(void)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t frame_size = control_frame_size_locked();
    char callsigns[(CALLSIGN_MAX_SIZE * 2) + 2];

    if (snprintf(callsigns, sizeof(callsigns), "%s|%s", arq_conn.dst_addr, arq_conn.src_addr) <= 0)
        return -1;

    if (build_connect_call_accept_frame_locked(ARQ_SUBTYPE_CALL,
                                               arq_ctx.session_id,
                                               callsigns,
                                               frame,
                                               frame_size) < 0)
        return -1;
    return queue_frame_locked(frame, frame_size, true);
}

static int send_accept_locked(void)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t frame_size = control_frame_size_locked();
    char callsign[CALLSIGN_MAX_SIZE];

    if (snprintf(callsign, sizeof(callsign), "%s", arq_conn.my_call_sign) <= 0)
        return -1;

    if (build_connect_call_accept_frame_locked(ARQ_SUBTYPE_ACCEPT,
                                               arq_ctx.session_id,
                                               callsign,
                                               frame,
                                               frame_size) < 0)
        return -1;
    return queue_frame_locked(frame, frame_size, true);
}

static int send_ack_locked(uint8_t ack_seq)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t frame_size = control_frame_size_locked();
    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL,
                           ARQ_SUBTYPE_ACK,
                           0,
                           ack_seq,
                           NULL,
                           0,
                           frame,
                           frame_size) < 0)
        return -1;
    return queue_frame_locked(frame, frame_size, true);
}

static int send_disconnect_locked(void)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t frame_size = control_frame_size_locked();
    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL,
                           ARQ_SUBTYPE_DISCONNECT,
                           0,
                           0,
                           NULL,
                           0,
                           frame,
                           frame_size) < 0)
        return -1;
    return queue_frame_locked(frame, frame_size, true);
}

static int send_keepalive_locked(uint8_t subtype)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t frame_size = control_frame_size_locked();
    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL,
                           subtype,
                           0,
                           0,
                           NULL,
                           0,
                           frame,
                           frame_size) < 0)
        return -1;
    return queue_frame_locked(frame, frame_size, true);
}

static void reset_runtime_locked(bool clear_peer_addresses)
{
    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_tx_buffer_arq_control);
    clear_buffer(data_rx_buffer_arq);

    arq_ctx.role = ARQ_ROLE_NONE;
    arq_ctx.session_id = 0;
    arq_ctx.tx_seq = 0;
    arq_ctx.rx_expected_seq = 0;
    arq_ctx.outstanding_seq = 0;
    arq_ctx.connect_deadline = 0;
    arq_ctx.next_role_tx_at = 0;
    arq_ctx.remote_busy_until = 0;
    arq_ctx.call_retries_left = 0;
    arq_ctx.accept_retries_left = 0;
    arq_ctx.waiting_ack = false;
    arq_ctx.data_retries_left = 0;
    arq_ctx.ack_deadline = 0;
    arq_ctx.outstanding_frame_len = 0;
    arq_ctx.outstanding_app_len = 0;
    arq_ctx.pending_call = false;
    arq_ctx.pending_accept = false;
    arq_ctx.pending_ack = false;
    arq_ctx.pending_disconnect = false;
    arq_ctx.pending_keepalive = false;
    arq_ctx.pending_keepalive_ack = false;
    arq_ctx.pending_ack_seq = 0;
    arq_ctx.keepalive_waiting = false;
    arq_ctx.keepalive_misses = 0;
    arq_ctx.keepalive_interval_s = ARQ_KEEPALIVE_INTERVAL_S;
    arq_ctx.keepalive_miss_limit = ARQ_KEEPALIVE_MISS_LIMIT;
    arq_ctx.keepalive_deadline = 0;
    arq_ctx.last_keepalive_rx = 0;
    arq_ctx.last_keepalive_tx = 0;
    arq_ctx.last_phy_activity = 0;
    arq_ctx.disconnect_in_progress = false;
    arq_ctx.disconnect_to_no_client = false;
    arq_ctx.disconnect_retries_left = 0;
    arq_ctx.disconnect_deadline = 0;
    arq_ctx.app_tx_len = 0;
    arq_ctx.gear = 0;
    arq_ctx.success_streak = 0;
    arq_ctx.failure_streak = 0;
    arq_ctx.snr_ema = 0.0f;
    arq_ctx.payload_mode = 0;
    arq_ctx.control_mode = FREEDV_MODE_DATAC13;

    arq_conn.TRX = RX;
    arq_conn.encryption = false;
    arq_conn.call_burst_size = 1;

    if (clear_peer_addresses)
    {
        arq_conn.src_addr[0] = 0;
        arq_conn.dst_addr[0] = 0;
    }
}

static void notify_disconnected_locked(void)
{
    tnc_send_disconnected();
    reset_runtime_locked(true);
}

static void finalize_disconnect_locked(void)
{
    fsm_state next_state = arq_ctx.disconnect_to_no_client ?
                           state_no_connected_client :
                           idle_or_listen_state_locked();
    fprintf(stderr, "ARQ disconnect finalized\n");
    notify_disconnected_locked();
    arq_fsm.current = next_state;
}

static void start_disconnect_locked(bool to_no_client)
{
    time_t now = time(NULL);

    arq_ctx.disconnect_in_progress = true;
    arq_ctx.disconnect_to_no_client = to_no_client;
    arq_ctx.disconnect_retries_left = ARQ_DISCONNECT_RETRY_SLOTS + 1;
    arq_ctx.pending_disconnect = true;
    arq_ctx.disconnect_deadline = now + (arq_ctx.slot_len_s * 2) + 2;
    arq_ctx.next_role_tx_at = now;
    arq_fsm.current = state_disconnecting;
    fprintf(stderr, "ARQ disconnect start (to_no_client=%d)\n", to_no_client ? 1 : 0);
}

static void enter_connected_locked(void)
{
    time_t now = time(NULL);
    arq_ctx.call_retries_left = 0;
    arq_ctx.accept_retries_left = 0;
    arq_ctx.pending_call = false;
    arq_ctx.pending_accept = false;
    arq_ctx.pending_keepalive = false;
    arq_ctx.pending_keepalive_ack = false;
    arq_ctx.keepalive_waiting = false;
    arq_ctx.keepalive_misses = 0;
    arq_ctx.last_keepalive_rx = now;
    arq_ctx.last_keepalive_tx = now;
    arq_ctx.last_phy_activity = now;
    arq_ctx.disconnect_in_progress = false;
    arq_ctx.disconnect_retries_left = 0;
    arq_ctx.disconnect_deadline = 0;
    arq_ctx.connect_deadline = 0;
    arq_ctx.success_streak = 0;
    arq_ctx.failure_streak = 0;
    arq_ctx.gear = initial_gear_locked();
    arq_fsm.current = state_connected;
    tnc_send_connected();
}

static void start_outgoing_call_locked(void)
{
    time_t now = time(NULL);
    arq_ctx.role = ARQ_ROLE_CALLER;
    arq_ctx.session_id++;
    arq_ctx.tx_seq = 0;
    arq_ctx.rx_expected_seq = 0;
    arq_ctx.waiting_ack = false;
    arq_ctx.outstanding_frame_len = 0;
    arq_ctx.outstanding_app_len = 0;
    arq_ctx.call_retries_left = arq_ctx.max_call_retries + 1;
    arq_ctx.pending_call = true;
    arq_ctx.pending_accept = false;
    arq_ctx.pending_ack = false;
    arq_ctx.pending_disconnect = false;
    arq_ctx.pending_keepalive = false;
    arq_ctx.pending_keepalive_ack = false;
    arq_ctx.keepalive_waiting = false;
    arq_ctx.keepalive_misses = 0;
    arq_ctx.last_keepalive_rx = now;
    arq_ctx.last_keepalive_tx = now;
    arq_ctx.disconnect_in_progress = false;
    arq_ctx.disconnect_to_no_client = false;
    arq_ctx.disconnect_retries_left = 0;
    arq_ctx.disconnect_deadline = 0;
    arq_ctx.next_role_tx_at = now;
    arq_ctx.remote_busy_until = 0;
    arq_ctx.connect_deadline = now + arq_ctx.connect_timeout_s;
    arq_fsm.current = state_calling_wait_accept;
}

static void queue_next_data_frame_locked(void)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t chunk;

    if (arq_fsm.current != state_connected)
        return;
    if (arq_ctx.waiting_ack)
        return;
    if (arq_ctx.app_tx_len == 0)
        return;

    chunk = chunk_size_for_gear_locked();
    if (chunk == 0)
        return;
    if (chunk > arq_ctx.app_tx_len)
        chunk = arq_ctx.app_tx_len;

    if (build_frame_locked(PACKET_TYPE_ARQ_DATA,
                           ARQ_SUBTYPE_DATA,
                           arq_ctx.tx_seq,
                           (uint8_t)(arq_ctx.rx_expected_seq - 1),
                           arq_ctx.app_tx_queue,
                           (uint16_t)chunk,
                           frame,
                           arq_conn.frame_size) < 0)
    {
        return;
    }

    if (queue_frame_locked(frame, arq_conn.frame_size, false) < 0)
        return;

    memcpy(arq_ctx.outstanding_frame, frame, arq_conn.frame_size);
    arq_ctx.outstanding_frame_len = arq_conn.frame_size;
    arq_ctx.outstanding_seq = arq_ctx.tx_seq;
    arq_ctx.outstanding_app_len = chunk;
    arq_ctx.waiting_ack = true;
    arq_ctx.data_retries_left = arq_ctx.max_data_retries;
    arq_ctx.ack_deadline = time(NULL) + arq_ctx.ack_timeout_s;
    arq_ctx.tx_seq++;
}

static bool do_slot_tx_locked(time_t now)
{
    if (arq_ctx.role == ARQ_ROLE_NONE)
        return false;
    if (now < arq_ctx.next_role_tx_at)
        return false;
    if (defer_tx_if_busy_locked(now))
        return false;

    if (arq_ctx.pending_disconnect)
    {
        if (arq_ctx.disconnect_retries_left <= 0)
        {
            arq_ctx.pending_disconnect = false;
            return false;
        }
        send_disconnect_locked();
        fprintf(stderr, "ARQ disconnect tx retry=%d\n", arq_ctx.disconnect_retries_left);
        arq_ctx.disconnect_retries_left--;
        if (arq_ctx.disconnect_retries_left <= 0)
            arq_ctx.pending_disconnect = false;
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_fsm.current == state_calling_wait_accept &&
        arq_ctx.role == ARQ_ROLE_CALLER &&
        arq_ctx.pending_call &&
        arq_ctx.call_retries_left > 0)
    {
        send_call_locked();
        arq_ctx.call_retries_left--;
        if (arq_ctx.call_retries_left <= 0)
            arq_ctx.pending_call = false;
        schedule_next_tx_locked(now, true);
        return true;
    }

    if (arq_ctx.pending_accept)
    {
        send_accept_locked();
        arq_ctx.pending_accept = false;
        arq_ctx.accept_retries_left = 0;
        if (arq_fsm.current != state_connected)
            enter_connected_locked();
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_ctx.pending_ack)
    {
        send_ack_locked(arq_ctx.pending_ack_seq);
        arq_ctx.pending_ack = false;
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_ctx.pending_keepalive_ack)
    {
        send_keepalive_locked(ARQ_SUBTYPE_KEEPALIVE_ACK);
        fprintf(stderr, "ARQ keepalive ack tx\n");
        arq_ctx.pending_keepalive_ack = false;
        arq_ctx.last_keepalive_tx = now;
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_ctx.pending_keepalive)
    {
        send_keepalive_locked(ARQ_SUBTYPE_KEEPALIVE);
        fprintf(stderr, "ARQ keepalive tx\n");
        arq_ctx.pending_keepalive = false;
        arq_ctx.keepalive_waiting = true;
        arq_ctx.keepalive_deadline = now + arq_ctx.keepalive_interval_s;
        arq_ctx.last_keepalive_tx = now;
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_fsm.current == state_connected)
    {
        if (arq_ctx.waiting_ack && now >= arq_ctx.ack_deadline)
        {
            if (arq_ctx.data_retries_left > 0 && arq_ctx.outstanding_frame_len == arq_conn.frame_size)
            {
                queue_frame_locked(arq_ctx.outstanding_frame, arq_ctx.outstanding_frame_len, false);
                arq_ctx.data_retries_left--;
                arq_ctx.ack_deadline = now + arq_ctx.ack_timeout_s;
                mark_failure_locked();
                schedule_next_tx_locked(now, true);
                return true;
            }

            start_disconnect_locked(false);
            return true;
        }

        if (!arq_ctx.waiting_ack && arq_ctx.app_tx_len > 0)
        {
            queue_next_data_frame_locked();
            if (arq_ctx.waiting_ack)
            {
                schedule_next_tx_locked(now, false);
                return true;
            }
        }
    }

    return false;
}

static void state_no_connected_client(int event)
{
    if (event != EV_CLIENT_CONNECT)
        return;

    reset_runtime_locked(true);
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
        start_outgoing_call_locked();
        break;
    case EV_CLIENT_DISCONNECT:
        reset_runtime_locked(true);
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
        start_outgoing_call_locked();
        break;
    case EV_CLIENT_DISCONNECT:
        reset_runtime_locked(true);
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
        start_disconnect_locked(false);
        break;
    case EV_CLIENT_DISCONNECT:
        start_disconnect_locked(true);
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
        start_disconnect_locked(false);
        break;
    case EV_CLIENT_DISCONNECT:
        start_disconnect_locked(true);
        break;
    default:
        break;
    }
}

static void state_disconnecting(int event)
{
    switch (event)
    {
    case EV_CLIENT_DISCONNECT:
        arq_ctx.disconnect_to_no_client = true;
        break;
    default:
        break;
    }
}

int arq_init(size_t frame_size, int mode)
{
    if (frame_size < ARQ_PAYLOAD_OFFSET + 8 || frame_size > INT_BUFFER_SIZE)
    {
        fprintf(stderr, "ARQ init failed: unsupported frame size %zu\n", frame_size);
        return EXIT_FAILURE;
    }

    memset(&arq_conn, 0, sizeof(arq_conn));
    memset(&arq_ctx, 0, sizeof(arq_ctx));
    init_model();

    arq_conn.frame_size = frame_size;
    arq_conn.mode = mode;
    arq_conn.call_burst_size = 1;

    arq_ctx.initialized = true;
    if (mode == FREEDV_MODE_DATAC1 || mode == FREEDV_MODE_DATAC3 || mode == FREEDV_MODE_DATAC4)
        arq_ctx.payload_mode = mode;
    else
        arq_ctx.payload_mode = FREEDV_MODE_DATAC3;
    arq_ctx.control_mode = FREEDV_MODE_DATAC13;

    arq_ctx.slot_len_s = mode_slot_len_s(arq_ctx.payload_mode);
    arq_ctx.tx_period_s = arq_ctx.slot_len_s;
    arq_ctx.max_call_retries = ARQ_CALL_RETRY_SLOTS;
    arq_ctx.max_accept_retries = ARQ_ACCEPT_RETRY_SLOTS;
    arq_ctx.max_data_retries = ARQ_DATA_RETRY_SLOTS;
    arq_ctx.ack_timeout_s = (arq_ctx.slot_len_s * 2) + ARQ_CHANNEL_GUARD_S;
    arq_ctx.connect_timeout_s =
        (arq_ctx.tx_period_s * (arq_ctx.max_call_retries + 2)) +
        ARQ_CONNECT_GRACE_SLOTS;
    arq_ctx.max_gear = max_gear_for_frame_size(frame_size);
    arq_ctx.keepalive_interval_s = ARQ_KEEPALIVE_INTERVAL_S;
    arq_ctx.keepalive_miss_limit = ARQ_KEEPALIVE_MISS_LIMIT;

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
    bool connected;

    arq_lock();
    connected = arq_ctx.initialized && (arq_fsm.current == state_connected);
    arq_unlock();
    return connected;
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

    if (arq_fsm.current == state_calling_wait_accept && now >= arq_ctx.connect_deadline)
    {
        mark_failure_locked();
        notify_disconnected_locked();
        arq_fsm.current = idle_or_listen_state_locked();
        arq_unlock();
        return;
    }

    if (arq_fsm.current == state_disconnecting && now >= arq_ctx.disconnect_deadline)
    {
        finalize_disconnect_locked();
        arq_unlock();
        return;
    }

    if (arq_fsm.current == state_connected)
    {
        time_t last_link_activity = arq_ctx.last_keepalive_tx;
        if (arq_ctx.last_keepalive_rx > last_link_activity)
            last_link_activity = arq_ctx.last_keepalive_rx;
        if (arq_ctx.last_phy_activity > last_link_activity)
            last_link_activity = arq_ctx.last_phy_activity;

        bool link_idle =
            !arq_ctx.waiting_ack &&
            arq_ctx.app_tx_len == 0 &&
            !arq_ctx.pending_ack &&
            !arq_ctx.pending_accept &&
            !arq_ctx.pending_call &&
            !arq_ctx.pending_disconnect &&
            !arq_ctx.pending_keepalive &&
            !arq_ctx.pending_keepalive_ack;

        if (arq_ctx.keepalive_waiting && now >= arq_ctx.keepalive_deadline)
        {
            arq_ctx.keepalive_waiting = false;
            arq_ctx.keepalive_misses++;
            fprintf(stderr, "ARQ keepalive miss=%d\n", arq_ctx.keepalive_misses);
            if (arq_ctx.keepalive_misses >= arq_ctx.keepalive_miss_limit)
                start_disconnect_locked(false);
        }

        if (link_idle &&
            arq_ctx.role == ARQ_ROLE_CALLER &&
            !arq_ctx.keepalive_waiting &&
            (now - last_link_activity) >= arq_ctx.keepalive_interval_s)
        {
            arq_ctx.pending_keepalive = true;
        }
    }

    do_slot_tx_locked(now);
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
    arq_unlock();
    return (int)len;
}

int arq_get_tx_backlog_bytes(void)
{
    size_t pending = 0;

    arq_lock();
    if (arq_ctx.initialized)
    {
        pending = arq_ctx.app_tx_len;
    }
    arq_unlock();

    if (pending > (size_t)INT_MAX)
        return INT_MAX;

    return (int)pending;
}

int arq_get_speed_level(void)
{
    int gear = 0;

    arq_lock();
    if (arq_ctx.initialized)
        gear = arq_ctx.gear;
    arq_unlock();

    if (gear < 0)
        gear = 0;
    return gear;
}

int arq_get_payload_mode(void)
{
    int mode;

    arq_lock();
    mode = arq_ctx.payload_mode ? arq_ctx.payload_mode : arq_conn.mode;
    arq_unlock();
    return mode;
}

int arq_get_control_mode(void)
{
    int mode;

    arq_lock();
    mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    arq_unlock();
    return mode;
}

int arq_get_preferred_rx_mode(void)
{
    int mode;

    arq_lock();
    mode = arq_ctx.payload_mode ? arq_ctx.payload_mode : arq_conn.mode;
    if (arq_ctx.initialized && arq_fsm.current == state_connected)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    if (arq_ctx.initialized && arq_fsm.current == state_calling_wait_accept)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    else if (arq_ctx.initialized && arq_ctx.pending_accept)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    else if (arq_ctx.initialized &&
             arq_fsm.current == state_connected &&
             !arq_ctx.disconnect_in_progress &&
             !arq_ctx.pending_disconnect &&
             !arq_ctx.pending_accept &&
             !arq_ctx.pending_ack &&
             !arq_ctx.pending_keepalive &&
             !arq_ctx.pending_keepalive_ack &&
             !arq_ctx.keepalive_waiting &&
             !arq_ctx.waiting_ack &&
             arq_ctx.app_tx_len == 0)
    {
        mode = arq_ctx.payload_mode ? arq_ctx.payload_mode : arq_conn.mode;
    }
    arq_unlock();
    return mode;
}

int arq_get_preferred_tx_mode(void)
{
    int mode;

    arq_lock();
    mode = arq_ctx.payload_mode ? arq_ctx.payload_mode : arq_conn.mode;
    if (arq_ctx.initialized && arq_fsm.current == state_connected)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    if (arq_ctx.initialized &&
        arq_fsm.current == state_calling_wait_accept &&
        arq_ctx.pending_call)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    else if (arq_ctx.initialized && arq_ctx.pending_accept)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    else if (arq_ctx.initialized &&
             arq_fsm.current == state_connected &&
             !arq_ctx.disconnect_in_progress &&
             !arq_ctx.pending_disconnect &&
             !arq_ctx.pending_accept &&
             !arq_ctx.pending_ack &&
             !arq_ctx.pending_keepalive &&
             !arq_ctx.pending_keepalive_ack &&
             (arq_ctx.waiting_ack || arq_ctx.app_tx_len > 0))
    {
        mode = arq_ctx.payload_mode ? arq_ctx.payload_mode : arq_conn.mode;
    }
    arq_unlock();
    return mode;
}

void arq_set_active_modem_mode(int mode, size_t frame_size)
{
    arq_lock();
    if (mode == FREEDV_MODE_DATAC1 ||
        mode == FREEDV_MODE_DATAC3 ||
        mode == FREEDV_MODE_DATAC4)
    {
        arq_conn.mode = mode;
        arq_conn.frame_size = frame_size;
        arq_ctx.payload_mode = mode;
        arq_ctx.max_gear = max_gear_for_frame_size(frame_size);
        if (arq_ctx.gear > arq_ctx.max_gear)
            arq_ctx.gear = arq_ctx.max_gear;
    }
    arq_unlock();
}

static void handle_control_frame_locked(uint8_t subtype,
                                        uint8_t session_id,
                                        uint8_t ack,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    char src[CALLSIGN_MAX_SIZE] = {0};
    char dst[CALLSIGN_MAX_SIZE] = {0};
    time_t now = time(NULL);

    switch (subtype)
    {
    case ARQ_SUBTYPE_CALL:
        if (!parse_callsign_pair_payload(payload, payload_len, src, dst))
            return;
        if (arq_conn.my_call_sign[0] == 0 || strcasecmp(dst, arq_conn.my_call_sign) != 0)
            return;
        if (arq_fsm.current != state_listen && arq_fsm.current != state_connected)
            return;

        strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
        arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
        strncpy(arq_conn.dst_addr, src, CALLSIGN_MAX_SIZE - 1);
        arq_conn.dst_addr[CALLSIGN_MAX_SIZE - 1] = 0;

        arq_ctx.role = ARQ_ROLE_CALLEE;
        arq_ctx.session_id = session_id;
        arq_ctx.tx_seq = 0;
        arq_ctx.rx_expected_seq = 0;
        arq_ctx.waiting_ack = false;
        arq_ctx.outstanding_frame_len = 0;
        arq_ctx.outstanding_app_len = 0;
        arq_ctx.pending_accept = true;
        arq_ctx.accept_retries_left = 1;
        arq_ctx.next_role_tx_at = now + ARQ_CHANNEL_GUARD_S;
        arq_ctx.remote_busy_until = now + ARQ_CHANNEL_GUARD_S;
        return;

    case ARQ_SUBTYPE_ACCEPT:
        if (arq_fsm.current != state_calling_wait_accept || arq_ctx.role != ARQ_ROLE_CALLER)
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (!parse_callsign_pair_payload(payload, payload_len, src, dst))
            return;
        if (strcasecmp(src, arq_conn.dst_addr) != 0)
            return;
        if (strcasecmp(dst, arq_conn.src_addr) != 0)
            return;

        enter_connected_locked();
        mark_link_activity_locked(now);
        mark_success_locked();
        return;

    case ARQ_SUBTYPE_ACK:
        if (arq_fsm.current != state_connected || !arq_ctx.waiting_ack)
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (ack != arq_ctx.outstanding_seq)
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
        arq_ctx.outstanding_frame_len = 0;
        mark_link_activity_locked(now);
        mark_success_locked();
        return;

    case ARQ_SUBTYPE_DISCONNECT:
        if (session_id != arq_ctx.session_id)
            return;
        if (arq_fsm.current == state_connected ||
            arq_fsm.current == state_calling_wait_accept ||
            arq_fsm.current == state_disconnecting)
            finalize_disconnect_locked();
        return;

    case ARQ_SUBTYPE_KEEPALIVE:
        if (arq_fsm.current != state_connected)
            return;
        if (session_id != arq_ctx.session_id)
            return;
        mark_link_activity_locked(now);
        arq_ctx.pending_keepalive_ack = true;
        if (arq_ctx.next_role_tx_at < now + ARQ_CHANNEL_GUARD_S)
            arq_ctx.next_role_tx_at = now + ARQ_CHANNEL_GUARD_S;
        return;

    case ARQ_SUBTYPE_KEEPALIVE_ACK:
        if (arq_fsm.current != state_connected)
            return;
        if (session_id != arq_ctx.session_id)
            return;
        mark_link_activity_locked(now);
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
    time_t now = time(NULL);

    if (arq_fsm.current != state_connected)
        return;
    if (session_id != arq_ctx.session_id)
        return;

    if (seq == arq_ctx.rx_expected_seq)
    {
        mark_link_activity_locked(now);
        write_buffer(data_rx_buffer_arq, (uint8_t *)payload, payload_len);
        arq_ctx.rx_expected_seq++;
        arq_ctx.pending_ack = true;
        arq_ctx.pending_ack_seq = seq;
        if (arq_ctx.next_role_tx_at < now + ARQ_CHANNEL_GUARD_S)
            arq_ctx.next_role_tx_at = now + ARQ_CHANNEL_GUARD_S;
        mark_success_locked();
        return;
    }

    if ((uint8_t)(arq_ctx.rx_expected_seq - 1) == seq)
    {
        mark_link_activity_locked(now);
        arq_ctx.pending_ack = true;
        arq_ctx.pending_ack_seq = seq;
        if (arq_ctx.next_role_tx_at < now + ARQ_CHANNEL_GUARD_S)
            arq_ctx.next_role_tx_at = now + ARQ_CHANNEL_GUARD_S;
    }
}

bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size)
{
    uint8_t subtype;
    uint8_t session_id;
    char decoded[(CALLSIGN_MAX_SIZE * 2) + 2] = {0};
    char dst[CALLSIGN_MAX_SIZE] = {0};
    char src[CALLSIGN_MAX_SIZE] = {0};
    char *sep;
    time_t now;

    if (!data || frame_size != 14)
        return false;

    session_id = data[ARQ_CONNECT_SESSION_IDX];
    if (!decode_connect_callsign_payload(data + ARQ_CONNECT_PAYLOAD_IDX, decoded))
        return false;

    if (strchr(decoded, '|') != NULL)
        subtype = ARQ_SUBTYPE_CALL;
    else
        subtype = ARQ_SUBTYPE_ACCEPT;

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return true;
    }

    now = time(NULL);
    if (subtype == ARQ_SUBTYPE_CALL)
    {
        sep = strchr(decoded, '|');
        if (!sep)
        {
            arq_unlock();
            return true;
        }

        *sep = 0;
        sep++;
        strncpy(src, sep, CALLSIGN_MAX_SIZE - 1);
        src[CALLSIGN_MAX_SIZE - 1] = 0;
        snprintf(dst, sizeof(dst), "%.*s", CALLSIGN_MAX_SIZE - 1, decoded);

        if (dst[0] != 0 &&
            arq_conn.my_call_sign[0] != 0 &&
            strncasecmp(arq_conn.my_call_sign, dst, strnlen(dst, CALLSIGN_MAX_SIZE - 1)) == 0 &&
            (arq_fsm.current == state_listen || arq_fsm.current == state_connected))
        {
            strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
            arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
            strncpy(arq_conn.dst_addr, src, CALLSIGN_MAX_SIZE - 1);
            arq_conn.dst_addr[CALLSIGN_MAX_SIZE - 1] = 0;

            arq_ctx.role = ARQ_ROLE_CALLEE;
            arq_ctx.session_id = session_id;
            arq_ctx.tx_seq = 0;
            arq_ctx.rx_expected_seq = 0;
            arq_ctx.waiting_ack = false;
            arq_ctx.outstanding_frame_len = 0;
            arq_ctx.outstanding_app_len = 0;
            arq_ctx.pending_accept = true;
            arq_ctx.accept_retries_left = 1;
            arq_ctx.next_role_tx_at = now + ARQ_CHANNEL_GUARD_S;
            arq_ctx.remote_busy_until = now + ARQ_CHANNEL_GUARD_S;
        }
        arq_unlock();
        return true;
    }

    if (arq_fsm.current == state_calling_wait_accept &&
        arq_ctx.role == ARQ_ROLE_CALLER &&
        session_id == arq_ctx.session_id &&
        decoded[0] != 0 &&
        strncasecmp(arq_conn.dst_addr, decoded, strnlen(decoded, CALLSIGN_MAX_SIZE - 1)) == 0)
    {
        enter_connected_locked();
        mark_link_activity_locked(now);
        mark_success_locked();
    }

    arq_unlock();
    return true;
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

    if (!data || frame_size < HEADER_SIZE)
        return;

    packet_type = (data[0] >> 6) & 0x3;

    if (frame_size < ARQ_PAYLOAD_OFFSET)
        return;

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
    time_t now = time(NULL);

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return;
    }

    if (frame_decoded && snr > -20.0f && snr < 30.0f)
    {
        if (arq_ctx.snr_ema == 0.0f)
            arq_ctx.snr_ema = snr;
        else
            arq_ctx.snr_ema = (0.8f * arq_ctx.snr_ema) + (0.2f * snr);

        if (arq_fsm.current == state_connected)
            update_payload_mode_locked();
    }

    if (!frame_decoded && (rx_status & 0x4))
        mark_failure_locked();

    if (sync || frame_decoded)
    {
        arq_ctx.last_phy_activity = now;
        time_t busy_until = now + ARQ_CHANNEL_GUARD_S;
        if (sync && !frame_decoded)
            busy_until += 1;
        if (arq_ctx.remote_busy_until < busy_until)
            arq_ctx.remote_busy_until = busy_until;
        if (arq_ctx.next_role_tx_at < arq_ctx.remote_busy_until)
            arq_ctx.next_role_tx_at = arq_ctx.remote_busy_until;
    }

    if (arq_fsm.current == state_calling_wait_accept &&
        arq_ctx.role == ARQ_ROLE_CALLER &&
        (sync || frame_decoded))
    {
        time_t min_deadline = now + arq_ctx.slot_len_s + ARQ_CONNECT_BUSY_EXT_S;
        if (arq_ctx.connect_deadline < min_deadline)
            arq_ctx.connect_deadline = min_deadline;
    }

    arq_unlock();
}

void clear_connection_data(void)
{
    arq_lock();
    if (arq_ctx.initialized)
        reset_runtime_locked(true);
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
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return;
    }

    if (arq_conn.my_call_sign[0] != 0 && arq_conn.src_addr[0] == 0)
    {
        strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
        arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
    }
    if (arq_conn.src_addr[0] != 0 && arq_conn.dst_addr[0] != 0)
        start_outgoing_call_locked();

    arq_unlock();
}

void callee_accept_connection(void)
{
    arq_lock();
    if (arq_ctx.initialized && arq_conn.my_call_sign[0] != 0 && arq_conn.dst_addr[0] != 0)
    {
        arq_ctx.role = ARQ_ROLE_CALLEE;
        arq_ctx.pending_accept = true;
        arq_ctx.accept_retries_left = 1;
        arq_ctx.next_role_tx_at = time(NULL) + ARQ_CHANNEL_GUARD_S;
    }
    arq_unlock();
}

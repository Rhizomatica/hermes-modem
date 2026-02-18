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

/* ARQv2 backend symbol remap */
#define arq_init arq_v2_init
#define arq_shutdown arq_v2_shutdown
#define arq_tick_1hz arq_v2_tick_1hz
#define arq_post_event arq_v2_post_event
#define arq_is_link_connected arq_v2_is_link_connected
#define arq_queue_data arq_v2_queue_data
#define arq_get_tx_backlog_bytes arq_v2_get_tx_backlog_bytes
#define arq_get_speed_level arq_v2_get_speed_level
#define arq_get_payload_mode arq_v2_get_payload_mode
#define arq_get_control_mode arq_v2_get_control_mode
#define arq_get_preferred_rx_mode arq_v2_get_preferred_rx_mode
#define arq_get_preferred_tx_mode arq_v2_get_preferred_tx_mode
#define arq_set_active_modem_mode arq_v2_set_active_modem_mode
#define arq_handle_incoming_connect_frame arq_v2_handle_incoming_connect_frame
#define arq_handle_incoming_frame arq_v2_handle_incoming_frame
#define arq_update_link_metrics arq_v2_update_link_metrics
#define arq_try_dequeue_action arq_v2_try_dequeue_action
#define arq_wait_dequeue_action arq_v2_wait_dequeue_action
#define arq_get_runtime_snapshot arq_v2_get_runtime_snapshot
#define arq_submit_tcp_cmd arq_v2_submit_tcp_cmd
#define arq_submit_tcp_payload arq_v2_submit_tcp_payload
#define clear_connection_data arq_v2_clear_connection_data
#define reset_arq_info arq_v2_reset_arq_info
#define call_remote arq_v2_call_remote
#define callee_accept_connection arq_v2_callee_accept_connection

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <limits.h>
#include <errno.h>

#include "arq.h"
#include "defines_modem.h"
#include "framer.h"
#include "freedv_api.h"
#include "ring_buffer_posix.h"
#include "tcp_interfaces.h"
#include "arq_channels.h"
#include "hermes_log.h"

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_tx_buffer_arq_control;
extern cbuf_handle_t data_rx_buffer_arq;
extern void init_model(void);
extern int arithmetic_encode(const char *msg, uint8_t *output);
extern int arithmetic_decode(uint8_t *input, int max_len, char *output);

arq_info arq_conn;
fsm_handle arq_fsm;

#define ARQ_ACTION_QUEUE_CAPACITY 256

typedef enum {
    ARQ_ROLE_NONE = 0,
    ARQ_ROLE_CALLER = 1,
    ARQ_ROLE_CALLEE = 2
} arq_role_t;

typedef enum {
    ARQ_TURN_NONE = 0,
    ARQ_TURN_ISS = 1,
    ARQ_TURN_IRS = 2
} arq_turn_t;

typedef enum {
    ARQ_MODE_FSM_IDLE = 0,
    ARQ_MODE_FSM_REQ_PENDING,
    ARQ_MODE_FSM_REQ_IN_FLIGHT,
    ARQ_MODE_FSM_ACK_PENDING
} arq_mode_fsm_t;

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
    uint64_t next_role_tx_at;
    uint64_t remote_busy_until;
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
    uint64_t pending_ack_set_ms;
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
    bool disconnect_after_flush;
    bool disconnect_after_flush_to_no_client;

    size_t app_tx_len;
    uint8_t app_tx_queue[DATA_TX_BUFFER_SIZE];

    int gear;
    int max_gear;
    int success_streak;
    int failure_streak;
    float snr_ema;
    bool payload_start_pending;
    int startup_acks_left;
    time_t startup_deadline;
    int payload_mode;
    int control_mode;
    arq_turn_t turn_role;
    bool peer_backlog_nonzero;
    time_t last_peer_payload_rx;
    bool pending_flow_hint;
    bool flow_hint_value;
    int last_flow_hint_sent;
    bool pending_turn_req;
    bool turn_req_in_flight;
    int turn_req_retries_left;
    time_t turn_req_deadline;
    bool pending_turn_ack;
    bool turn_promote_after_ack;
    bool turn_ack_deferred;
    bool pending_mode_req;
    bool pending_mode_ack;
    uint8_t pending_mode;
    bool mode_req_in_flight;
    uint8_t mode_req_mode;
    int mode_req_retries_left;
    time_t mode_req_deadline;
    int mode_candidate_mode;
    int mode_candidate_hits;
    arq_mode_fsm_t mode_fsm;
    bool mode_apply_pending;
    uint8_t mode_apply_mode;
    arq_action_t action_queue[ARQ_ACTION_QUEUE_CAPACITY];
    size_t action_head;
    size_t action_tail;
    size_t action_count;
    size_t pending_payload_actions;
} arq_ctx_t;

static arq_ctx_t arq_ctx;
static pthread_cond_t arq_action_cond = PTHREAD_COND_INITIALIZER;
static bool arq_fsm_lock_ready = false;
static arq_channel_bus_t arq_bus;
static pthread_t arq_cmd_bridge_tid;
static pthread_t arq_payload_bridge_tid;
static bool arq_cmd_bridge_started = false;
static bool arq_payload_bridge_started = false;

#define ARQ_EVENT_QUEUE_CAPACITY 128

typedef enum {
    ARQ_EVENT_FSM = 1,
    ARQ_EVENT_RX_FRAME = 2,
    ARQ_EVENT_RX_CONNECT_FRAME = 3,
    ARQ_EVENT_LINK_METRICS = 4,
    ARQ_EVENT_APP_TX = 5,
    ARQ_EVENT_ACTIVE_MODE = 6
} arq_event_type_t;

typedef struct {
    int fsm_event;
} arq_event_fsm_payload_t;

typedef struct {
    size_t frame_size;
    uint8_t frame[INT_BUFFER_SIZE];
} arq_event_rx_frame_payload_t;

typedef struct {
    int sync;
    float snr;
    int rx_status;
    bool frame_decoded;
} arq_event_link_metrics_payload_t;

typedef struct arq_event_app_tx_result_t {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
    int queued;
} arq_event_app_tx_result_t;

typedef struct {
    size_t data_len;
    uint8_t data[INT_BUFFER_SIZE];
    arq_event_app_tx_result_t *result;
} arq_event_app_tx_payload_t;

typedef struct {
    int mode;
    size_t frame_size;
} arq_event_active_mode_payload_t;

typedef struct {
    arq_event_type_t type;
    union {
        arq_event_fsm_payload_t fsm;
        arq_event_rx_frame_payload_t rx_frame;
        arq_event_link_metrics_payload_t link_metrics;
        arq_event_app_tx_payload_t app_tx;
        arq_event_active_mode_payload_t active_mode;
    } u;
} arq_event_t;

typedef struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool running;
    bool started;
    arq_event_t queue[ARQ_EVENT_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
} arq_event_loop_t;

static arq_event_loop_t arq_event_loop;

enum {
    ARQ_SUBTYPE_CALL = 1,
    ARQ_SUBTYPE_ACCEPT = 2,
    ARQ_SUBTYPE_ACK = 3,
    ARQ_SUBTYPE_DISCONNECT = 4,
    ARQ_SUBTYPE_DATA = 5,
    ARQ_SUBTYPE_KEEPALIVE = 6,
    ARQ_SUBTYPE_KEEPALIVE_ACK = 7,
    ARQ_SUBTYPE_MODE_REQ = 8,
    ARQ_SUBTYPE_MODE_ACK = 9,
    ARQ_SUBTYPE_TURN_REQ = 10,
    ARQ_SUBTYPE_TURN_ACK = 11,
    ARQ_SUBTYPE_FLOW_HINT = 12
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

#define ARQ_CONTROL_FRAME_SIZE 14
#define ARQ_CONNECT_SESSION_IDX 1
#define ARQ_CONNECT_PAYLOAD_IDX 2
#define ARQ_CONNECT_META_SIZE 2 /* HEADER + connect meta byte */
#define ARQ_CONNECT_MAX_ENCODED (ARQ_CONTROL_FRAME_SIZE - ARQ_CONNECT_META_SIZE)
#define ARQ_ARITH_BUFFER_SIZE 4096
#define ARQ_CONNECT_SESSION_MASK 0x7F
#define ARQ_CONNECT_ACCEPT_FLAG 0x80

#define ARQ_CALL_RETRY_SLOTS 4
#define ARQ_ACCEPT_RETRY_SLOTS 3
#define ARQ_DATA_RETRY_SLOTS 6
#define ARQ_CONNECT_GRACE_SLOTS 2
#define ARQ_CHANNEL_GUARD_MS 200
#define ARQ_ACK_REPLY_EXTRA_GUARD_MS 300
#define ARQ_ACK_GUARD_S 1
#define ARQ_CONNECT_BUSY_EXT_S 2
#define ARQ_DISCONNECT_RETRY_SLOTS 2
#define ARQ_KEEPALIVE_INTERVAL_S 20
#define ARQ_KEEPALIVE_MISS_LIMIT 5
#define ARQ_SNR_HYST_DB 1.0f
#define ARQ_TURN_REQ_RETRIES 2
#define ARQ_MODE_REQ_RETRIES 2
#define ARQ_MODE_SWITCH_HYST_COUNT 1
#define ARQ_BACKLOG_MIN_DATAC3 56
#define ARQ_BACKLOG_MIN_DATAC1 126
#define ARQ_BACKLOG_MIN_BIDIR_MODE_UPGRADE 48 /* > DATAC4 payload capacity (47 bytes) */
#define ARQ_PEER_PAYLOAD_HOLD_S 15
/* Re-enable negotiated payload upgrades once ACK path is stabilized. */
#define ARQ_ENABLE_MODE_UPGRADE 1
#define ARQ_EVENT_LOOP_MIN_TIMEOUT_MS 5
#define ARQ_EVENT_LOOP_TX_BUSY_TIMEOUT_MS 20
#define ARQ_EVENT_LOOP_SPIN_LOG_ITER_S 200
#define ARQ_STARTUP_ACKS_REQUIRED 2
#define ARQ_STARTUP_MAX_S 15

static void state_no_connected_client(int event);
static void state_idle(int event);
static void state_listen(int event);
static void state_calling_wait_accept(int event);
static void state_connected(int event);
static void state_connected_iss(int event);
static void state_connected_irs(int event);
static void state_turn_negotiating(int event);
static void state_disconnecting(int event);
static int arq_event_loop_start(void);
static void arq_event_loop_stop(void);
static void arq_event_loop_enqueue_fsm(int event);
static void arq_event_loop_enqueue_frame(const uint8_t *data, size_t frame_size);
static void arq_event_loop_enqueue_connect_frame(const uint8_t *data, size_t frame_size);
static void arq_event_loop_enqueue_metrics(int sync, float snr, int rx_status, bool frame_decoded);
static void arq_event_loop_enqueue_app_tx(const uint8_t *data, size_t data_len, arq_event_app_tx_result_t *result);
static void arq_event_loop_enqueue_active_mode(int mode, size_t frame_size);
static bool arq_event_loop_enqueue_internal(const arq_event_t *event_item);
static void arq_process_event(const arq_event_t *event_item);
static void *arq_event_loop_worker(void *arg);
static int arq_event_loop_timeout_ms(void);
static bool arq_handle_incoming_connect_frame_locked(const uint8_t *data, size_t frame_size);
static void arq_handle_incoming_frame_locked(const uint8_t *data, size_t frame_size);
static void arq_update_link_metrics_locked(int sync, float snr, int rx_status, bool frame_decoded, time_t now);
static void arq_set_active_modem_mode_locked(int mode, size_t frame_size);
static int arq_queue_app_data_locked(const uint8_t *data, size_t len);
static void arq_event_app_tx_result_complete(arq_event_app_tx_result_t *result, int queued);
static void arq_action_queue_clear_locked(void);
static bool arq_action_queue_push_locked(const arq_action_t *action);
static bool arq_action_queue_pop_locked(arq_action_t *action);
static size_t frame_size_for_payload_mode(int mode);
static bool is_connected_state_locked(void);
static int preferred_rx_mode_locked(time_t now);
static int preferred_tx_mode_locked(time_t now);
static void *arq_cmd_bridge_worker(void *arg);
static void *arq_payload_bridge_worker(void *arg);
static void arq_handle_tcp_cmd_msg(const arq_cmd_msg_t *cmd);

static uint64_t arq_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static uint64_t arq_realtime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static inline void arq_lock(void)
{
    pthread_mutex_lock(&arq_fsm.lock);
}

static inline void arq_unlock(void)
{
    pthread_mutex_unlock(&arq_fsm.lock);
}

static int arq_event_loop_start(void)
{
    if (arq_event_loop.started)
        return 0;

    memset(&arq_event_loop, 0, sizeof(arq_event_loop));
    if (pthread_mutex_init(&arq_event_loop.lock, NULL) != 0)
        return -1;
    if (pthread_cond_init(&arq_event_loop.cond, NULL) != 0)
    {
        pthread_mutex_destroy(&arq_event_loop.lock);
        return -1;
    }

    arq_event_loop.running = true;
    arq_event_loop.started = true;
    if (pthread_create(&arq_event_loop.thread, NULL, arq_event_loop_worker, NULL) != 0)
    {
        arq_event_loop.running = false;
        arq_event_loop.started = false;
        pthread_cond_destroy(&arq_event_loop.cond);
        pthread_mutex_destroy(&arq_event_loop.lock);
        return -1;
    }
    return 0;
}

static void arq_event_loop_stop(void)
{
    if (!arq_event_loop.started)
        return;

    pthread_mutex_lock(&arq_event_loop.lock);
    arq_event_loop.running = false;
    pthread_cond_broadcast(&arq_event_loop.cond);
    pthread_mutex_unlock(&arq_event_loop.lock);

    pthread_join(arq_event_loop.thread, NULL);
    pthread_cond_destroy(&arq_event_loop.cond);
    pthread_mutex_destroy(&arq_event_loop.lock);
    memset(&arq_event_loop, 0, sizeof(arq_event_loop));
}

static bool arq_event_loop_enqueue_internal(const arq_event_t *event_item)
{
    bool queued = false;

    if (!event_item)
        return false;

    pthread_mutex_lock(&arq_event_loop.lock);
    if (arq_event_loop.started &&
        arq_event_loop.running &&
        arq_event_loop.count < ARQ_EVENT_QUEUE_CAPACITY)
    {
        arq_event_loop.queue[arq_event_loop.tail] = *event_item;
        arq_event_loop.tail = (arq_event_loop.tail + 1) % ARQ_EVENT_QUEUE_CAPACITY;
        arq_event_loop.count++;
        pthread_cond_signal(&arq_event_loop.cond);
        queued = true;
    }
    pthread_mutex_unlock(&arq_event_loop.lock);

    return queued;
}

static void arq_action_queue_clear_locked(void)
{
    arq_ctx.action_head = 0;
    arq_ctx.action_tail = 0;
    arq_ctx.action_count = 0;
    arq_ctx.pending_payload_actions = 0;
    pthread_cond_broadcast(&arq_action_cond);
}

static bool arq_action_queue_push_locked(const arq_action_t *action)
{
    if (!action)
        return false;

    if (arq_ctx.action_count >= ARQ_ACTION_QUEUE_CAPACITY)
        return false;

    arq_ctx.action_queue[arq_ctx.action_tail] = *action;
    arq_ctx.action_tail = (arq_ctx.action_tail + 1) % ARQ_ACTION_QUEUE_CAPACITY;
    arq_ctx.action_count++;
    if (action->type == ARQ_ACTION_TX_PAYLOAD)
        arq_ctx.pending_payload_actions++;
    pthread_cond_signal(&arq_action_cond);
    return true;
}

static bool arq_action_queue_pop_locked(arq_action_t *action)
{
    size_t pick_idx;
    bool picked_priority = false;

    if (!action || arq_ctx.action_count == 0)
        return false;

    pick_idx = arq_ctx.action_head;
    for (size_t i = 0; i < arq_ctx.action_count; i++)
    {
        arq_action_type_t t = arq_ctx.action_queue[pick_idx].type;
        if (t == ARQ_ACTION_TX_CONTROL || t == ARQ_ACTION_MODE_SWITCH)
        {
            picked_priority = true;
            break;
        }
        pick_idx = (pick_idx + 1) % ARQ_ACTION_QUEUE_CAPACITY;
    }

    if (picked_priority && pick_idx != arq_ctx.action_head)
    {
        arq_action_t tmp = arq_ctx.action_queue[arq_ctx.action_head];
        arq_ctx.action_queue[arq_ctx.action_head] = arq_ctx.action_queue[pick_idx];
        arq_ctx.action_queue[pick_idx] = tmp;
    }

    *action = arq_ctx.action_queue[arq_ctx.action_head];
    arq_ctx.action_head = (arq_ctx.action_head + 1) % ARQ_ACTION_QUEUE_CAPACITY;
    arq_ctx.action_count--;
    if (action->type == ARQ_ACTION_TX_PAYLOAD && arq_ctx.pending_payload_actions > 0)
        arq_ctx.pending_payload_actions--;
    return true;
}

static void arq_event_app_tx_result_complete(arq_event_app_tx_result_t *result, int queued)
{
    if (!result)
        return;

    pthread_mutex_lock(&result->lock);
    result->queued = queued;
    result->done = true;
    pthread_cond_signal(&result->cond);
    pthread_mutex_unlock(&result->lock);
}

static void arq_process_event(const arq_event_t *event_item)
{
    if (!event_item)
        return;

    switch (event_item->type)
    {
    case ARQ_EVENT_FSM:
        fsm_dispatch(&arq_fsm, event_item->u.fsm.fsm_event);
        return;

    case ARQ_EVENT_RX_FRAME:
        arq_lock();
        if (arq_ctx.initialized)
            arq_handle_incoming_frame_locked(event_item->u.rx_frame.frame, event_item->u.rx_frame.frame_size);
        arq_unlock();
        return;

    case ARQ_EVENT_RX_CONNECT_FRAME:
        arq_lock();
        if (arq_ctx.initialized)
            arq_handle_incoming_connect_frame_locked(event_item->u.rx_frame.frame,
                                                     event_item->u.rx_frame.frame_size);
        arq_unlock();
        return;

    case ARQ_EVENT_LINK_METRICS:
        arq_lock();
        if (arq_ctx.initialized)
            arq_update_link_metrics_locked(event_item->u.link_metrics.sync,
                                           event_item->u.link_metrics.snr,
                                           event_item->u.link_metrics.rx_status,
                                           event_item->u.link_metrics.frame_decoded,
                                           time(NULL));
        arq_unlock();
        return;

    case ARQ_EVENT_APP_TX:
    {
        int queued = 0;
        arq_lock();
        queued = arq_queue_app_data_locked(event_item->u.app_tx.data, event_item->u.app_tx.data_len);
        arq_unlock();
        arq_event_app_tx_result_complete(event_item->u.app_tx.result, queued);
        return;
    }

    case ARQ_EVENT_ACTIVE_MODE:
        arq_lock();
        arq_set_active_modem_mode_locked(event_item->u.active_mode.mode,
                                         event_item->u.active_mode.frame_size);
        arq_unlock();
        return;

    default:
        return;
    }
}

static void arq_event_loop_enqueue_fsm(int event)
{
    arq_event_t event_item;

    if (event < 0)
        return;

    memset(&event_item, 0, sizeof(event_item));
    event_item.type = ARQ_EVENT_FSM;
    event_item.u.fsm.fsm_event = event;
    if (!arq_event_loop_enqueue_internal(&event_item))
        arq_process_event(&event_item);
}

static void arq_event_loop_enqueue_frame(const uint8_t *data, size_t frame_size)
{
    arq_event_t event_item;

    if (!data || frame_size == 0 || frame_size > INT_BUFFER_SIZE)
        return;

    memset(&event_item, 0, sizeof(event_item));
    event_item.type = ARQ_EVENT_RX_FRAME;
    event_item.u.rx_frame.frame_size = frame_size;
    memcpy(event_item.u.rx_frame.frame, data, frame_size);
    if (!arq_event_loop_enqueue_internal(&event_item))
        arq_process_event(&event_item);
}

static void arq_event_loop_enqueue_connect_frame(const uint8_t *data, size_t frame_size)
{
    arq_event_t event_item;

    if (!data || frame_size == 0 || frame_size > INT_BUFFER_SIZE)
        return;

    memset(&event_item, 0, sizeof(event_item));
    event_item.type = ARQ_EVENT_RX_CONNECT_FRAME;
    event_item.u.rx_frame.frame_size = frame_size;
    memcpy(event_item.u.rx_frame.frame, data, frame_size);
    if (!arq_event_loop_enqueue_internal(&event_item))
        arq_process_event(&event_item);
}

static void arq_event_loop_enqueue_metrics(int sync, float snr, int rx_status, bool frame_decoded)
{
    arq_event_t event_item;

    memset(&event_item, 0, sizeof(event_item));
    event_item.type = ARQ_EVENT_LINK_METRICS;
    event_item.u.link_metrics.sync = sync;
    event_item.u.link_metrics.snr = snr;
    event_item.u.link_metrics.rx_status = rx_status;
    event_item.u.link_metrics.frame_decoded = frame_decoded;
    if (!arq_event_loop_enqueue_internal(&event_item))
        arq_process_event(&event_item);
}

static void arq_event_loop_enqueue_app_tx(const uint8_t *data, size_t data_len, arq_event_app_tx_result_t *result)
{
    arq_event_t event_item;

    if (!data || data_len == 0 || data_len > INT_BUFFER_SIZE)
    {
        arq_event_app_tx_result_complete(result, 0);
        return;
    }

    memset(&event_item, 0, sizeof(event_item));
    event_item.type = ARQ_EVENT_APP_TX;
    event_item.u.app_tx.data_len = data_len;
    event_item.u.app_tx.result = result;
    memcpy(event_item.u.app_tx.data, data, data_len);
    if (!arq_event_loop_enqueue_internal(&event_item))
        arq_process_event(&event_item);
}

static void arq_event_loop_enqueue_active_mode(int mode, size_t frame_size)
{
    arq_event_t event_item;

    if (frame_size == 0 || frame_size > INT_BUFFER_SIZE)
        return;

    memset(&event_item, 0, sizeof(event_item));
    event_item.type = ARQ_EVENT_ACTIVE_MODE;
    event_item.u.active_mode.mode = mode;
    event_item.u.active_mode.frame_size = frame_size;
    if (!arq_event_loop_enqueue_internal(&event_item))
        arq_process_event(&event_item);
}

static void arq_consider_deadline_s(time_t deadline, uint64_t *next_deadline_ms)
{
    uint64_t deadline_ms;

    if (!next_deadline_ms || deadline <= 0)
        return;
    deadline_ms = (uint64_t)deadline * 1000ULL;
    if (*next_deadline_ms == 0 || deadline_ms < *next_deadline_ms)
        *next_deadline_ms = deadline_ms;
}

static void arq_consider_deadline_ms(uint64_t deadline_ms, uint64_t *next_deadline_ms)
{
    if (!next_deadline_ms || deadline_ms == 0)
        return;
    if (*next_deadline_ms == 0 || deadline_ms < *next_deadline_ms)
        *next_deadline_ms = deadline_ms;
}

static bool has_outbound_payload_pending_locked(void)
{
    return arq_ctx.app_tx_len > 0 ||
           arq_ctx.waiting_ack ||
           arq_ctx.outstanding_frame_len > 0 ||
           arq_ctx.pending_payload_actions > 0;
}

static bool keepalive_quiescent_locked(void)
{
    return !arq_ctx.payload_start_pending &&
           !arq_ctx.waiting_ack &&
           arq_ctx.app_tx_len == 0 &&
           !arq_ctx.pending_ack &&
           !arq_ctx.pending_accept &&
           !arq_ctx.pending_call &&
           !arq_ctx.pending_disconnect &&
           !arq_ctx.pending_keepalive_ack &&
           !arq_ctx.pending_turn_req &&
           !arq_ctx.turn_req_in_flight &&
           !arq_ctx.pending_turn_ack &&
           !arq_ctx.pending_flow_hint &&
           !arq_ctx.peer_backlog_nonzero &&
           arq_ctx.mode_fsm == ARQ_MODE_FSM_IDLE;
}

static bool has_immediate_control_tx_work_locked(void)
{
    if (arq_ctx.disconnect_after_flush &&
        !has_outbound_payload_pending_locked())
    {
        return true;
    }

    if (arq_ctx.pending_disconnect ||
        arq_ctx.pending_call ||
        arq_ctx.pending_accept ||
        arq_ctx.pending_ack ||
        arq_ctx.pending_keepalive_ack ||
        arq_ctx.pending_turn_ack)
    {
        return true;
    }

    if (arq_ctx.pending_flow_hint &&
        !arq_ctx.waiting_ack &&
        !arq_ctx.payload_start_pending)
    {
        return true;
    }

    if (arq_ctx.pending_turn_req &&
        !arq_ctx.turn_req_in_flight &&
        !arq_ctx.payload_start_pending)
    {
        return true;
    }

    if (arq_ctx.pending_keepalive &&
        !arq_ctx.payload_start_pending &&
        keepalive_quiescent_locked())
        return true;

    if (!arq_ctx.waiting_ack &&
        (arq_ctx.mode_fsm == ARQ_MODE_FSM_ACK_PENDING ||
         arq_ctx.mode_fsm == ARQ_MODE_FSM_REQ_PENDING))
    {
        return true;
    }

    return false;
}

static bool has_immediate_iss_payload_tx_work_locked(void)
{
    if (!is_connected_state_locked())
        return false;
    if (arq_ctx.turn_role != ARQ_TURN_ISS)
        return false;
    if (arq_ctx.waiting_ack)
        return false;
    if (arq_ctx.turn_ack_deferred)
        return false;
    if (arq_ctx.pending_turn_req || arq_ctx.turn_req_in_flight)
        return false;
    if (arq_ctx.app_tx_len > 0)
        return true;

    return arq_ctx.app_tx_len == 0 &&
           !arq_ctx.payload_start_pending &&
           arq_ctx.peer_backlog_nonzero &&
           arq_ctx.mode_fsm == ARQ_MODE_FSM_IDLE;
}

static bool has_immediate_tx_work_locked(void)
{
    if (arq_ctx.role == ARQ_ROLE_NONE)
        return false;

    return has_immediate_control_tx_work_locked() ||
           has_immediate_iss_payload_tx_work_locked();
}

static int arq_event_loop_timeout_ms(void)
{
    uint64_t now_ms;
    uint64_t next_deadline_ms = 0;
    bool tx_work_immediate = false;
    bool local_tx_active = false;

    if (!arq_fsm_lock_ready)
        return 1;

    now_ms = arq_realtime_ms();
    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return -1;
    }
    local_tx_active = arq_conn.TRX == TX;

    if (arq_fsm.current == state_calling_wait_accept)
        arq_consider_deadline_s(arq_ctx.connect_deadline, &next_deadline_ms);

    if (arq_fsm.current == state_disconnecting)
        arq_consider_deadline_s(arq_ctx.disconnect_deadline, &next_deadline_ms);

    if (arq_ctx.role != ARQ_ROLE_NONE)
    {
        tx_work_immediate = has_immediate_tx_work_locked();

        if (arq_ctx.next_role_tx_at > 0)
        {
            if (arq_ctx.next_role_tx_at > now_ms || tx_work_immediate)
                arq_consider_deadline_ms(arq_ctx.next_role_tx_at, &next_deadline_ms);
        }
        else if (tx_work_immediate)
            arq_consider_deadline_ms(now_ms, &next_deadline_ms);
    }

    if (is_connected_state_locked())
    {
        if (arq_ctx.payload_start_pending && arq_ctx.startup_deadline > 0)
            arq_consider_deadline_s(arq_ctx.startup_deadline, &next_deadline_ms);

        if (arq_ctx.waiting_ack && arq_ctx.ack_deadline > 0)
        {
            uint64_t ack_deadline_ms = (uint64_t)arq_ctx.ack_deadline * 1000ULL;
            if (arq_ctx.next_role_tx_at > ack_deadline_ms)
                ack_deadline_ms = arq_ctx.next_role_tx_at;
            arq_consider_deadline_ms(ack_deadline_ms, &next_deadline_ms);
        }
        if (arq_ctx.turn_req_in_flight && arq_ctx.turn_req_deadline > 0)
        {
            uint64_t turn_deadline_ms = (uint64_t)arq_ctx.turn_req_deadline * 1000ULL;
            if (arq_ctx.next_role_tx_at > turn_deadline_ms)
                turn_deadline_ms = arq_ctx.next_role_tx_at;
            arq_consider_deadline_ms(turn_deadline_ms, &next_deadline_ms);
        }
        if (!arq_ctx.waiting_ack &&
            !arq_ctx.payload_start_pending &&
            arq_ctx.mode_fsm == ARQ_MODE_FSM_REQ_IN_FLIGHT &&
            arq_ctx.mode_req_deadline > 0)
        {
            uint64_t mode_deadline_ms = (uint64_t)arq_ctx.mode_req_deadline * 1000ULL;
            if (arq_ctx.next_role_tx_at > mode_deadline_ms)
                mode_deadline_ms = arq_ctx.next_role_tx_at;
            arq_consider_deadline_ms(mode_deadline_ms, &next_deadline_ms);
        }

        if (arq_ctx.keepalive_waiting)
        {
            arq_consider_deadline_s(arq_ctx.keepalive_deadline, &next_deadline_ms);
        }
        else if (arq_ctx.role == ARQ_ROLE_CALLER &&
                 arq_ctx.keepalive_interval_s > 0 &&
                 !arq_ctx.pending_keepalive &&
                 keepalive_quiescent_locked())
        {
            time_t last_link_activity = arq_ctx.last_keepalive_tx;
            if (arq_ctx.last_keepalive_rx > last_link_activity)
                last_link_activity = arq_ctx.last_keepalive_rx;
            if (arq_ctx.last_phy_activity > last_link_activity)
                last_link_activity = arq_ctx.last_phy_activity;
            arq_consider_deadline_s(last_link_activity + arq_ctx.keepalive_interval_s, &next_deadline_ms);
        }
    }

    arq_unlock();

    if (next_deadline_ms == 0)
        return -1;
    if (next_deadline_ms <= now_ms)
    {
        if (local_tx_active)
            return ARQ_EVENT_LOOP_TX_BUSY_TIMEOUT_MS;
        return ARQ_EVENT_LOOP_MIN_TIMEOUT_MS;
    }
    if ((next_deadline_ms - now_ms) > (uint64_t)INT_MAX)
        return INT_MAX;
    return (int)(next_deadline_ms - now_ms);
}

static void *arq_event_loop_worker(void *arg)
{
    uint64_t loop_iter_count = 0;
    uint64_t loop_window_start_ms = arq_monotonic_ms();

    (void)arg;

    for (;;)
    {
        arq_event_t event_item;
        int timeout_ms = arq_event_loop_timeout_ms();
        bool have_event = false;
        bool should_exit = false;
        size_t queue_depth = 0;
        uint64_t now_ms;

        pthread_mutex_lock(&arq_event_loop.lock);
        if (arq_event_loop.running && arq_event_loop.count == 0)
        {
            if (timeout_ms < 0)
            {
                (void)pthread_cond_wait(&arq_event_loop.cond, &arq_event_loop.lock);
            }
            else if (timeout_ms > 0)
            {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += timeout_ms / 1000;
                ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L)
                {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }
                (void)pthread_cond_timedwait(&arq_event_loop.cond, &arq_event_loop.lock, &ts);
            }
        }

        if (arq_event_loop.count > 0)
        {
            event_item = arq_event_loop.queue[arq_event_loop.head];
            arq_event_loop.head = (arq_event_loop.head + 1) % ARQ_EVENT_QUEUE_CAPACITY;
            arq_event_loop.count--;
            have_event = true;
        }
        else if (!arq_event_loop.running)
        {
            should_exit = true;
        }
        queue_depth = arq_event_loop.count;
        pthread_mutex_unlock(&arq_event_loop.lock);

        if (should_exit)
            break;

        if (have_event)
            arq_process_event(&event_item);
        if (have_event || timeout_ms >= 0)
            arq_tick_1hz();

        loop_iter_count++;
        now_ms = arq_monotonic_ms();
        if (now_ms - loop_window_start_ms >= 1000ULL)
        {
            if (loop_iter_count >= ARQ_EVENT_LOOP_SPIN_LOG_ITER_S)
            {
                HLOGD("arq", "Event loop wakeups/s=%llu timeout=%d queue=%zu",
                      (unsigned long long)loop_iter_count,
                      timeout_ms,
                      queue_depth);
            }
            loop_iter_count = 0;
            loop_window_start_ms = now_ms;
        }
    }

    return NULL;
}

static inline uint8_t connect_meta_build(uint8_t session_id, bool is_accept)
{
    return (uint8_t)(session_id & ARQ_CONNECT_SESSION_MASK) |
           (is_accept ? ARQ_CONNECT_ACCEPT_FLAG : 0);
}

static inline uint8_t connect_meta_session(uint8_t meta)
{
    return (uint8_t)(meta & ARQ_CONNECT_SESSION_MASK);
}

static inline bool connect_meta_is_accept(uint8_t meta)
{
    return (meta & ARQ_CONNECT_ACCEPT_FLAG) != 0;
}

static fsm_state idle_or_listen_state_locked(void)
{
    return arq_conn.listen ? state_listen : state_idle;
}

static bool is_connected_state_locked(void)
{
    return arq_fsm.current == state_connected ||
           arq_fsm.current == state_connected_iss ||
           arq_fsm.current == state_connected_irs ||
           arq_fsm.current == state_turn_negotiating;
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

static int ack_timeout_s_for_mode(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        return 12;
    case FREEDV_MODE_DATAC3:
        return 10;
    case FREEDV_MODE_DATAC4:
        return 9;
    default:
        return 6;
    }
}

static int connect_response_wait_s(void)
{
    return mode_slot_len_s(FREEDV_MODE_DATAC13) + ARQ_CONNECT_BUSY_EXT_S;
}

static uint64_t control_slot_ms(int mode)
{
    return (uint64_t)mode_slot_len_s(mode) * 1000ULL;
}

static int peer_payload_hold_s_locked(void)
{
    int hold = ARQ_PEER_PAYLOAD_HOLD_S;
    int mode_hold = arq_ctx.ack_timeout_s + arq_ctx.slot_len_s;

    if (mode_hold > hold)
        hold = mode_hold;
    return hold;
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

static const char *mode_fsm_name(arq_mode_fsm_t state)
{
    switch (state)
    {
    case ARQ_MODE_FSM_REQ_PENDING:
        return "REQ_PENDING";
    case ARQ_MODE_FSM_REQ_IN_FLIGHT:
        return "REQ_IN_FLIGHT";
    case ARQ_MODE_FSM_ACK_PENDING:
        return "ACK_PENDING";
    case ARQ_MODE_FSM_IDLE:
    default:
        return "IDLE";
    }
}

static void mode_fsm_set_locked(arq_mode_fsm_t next, const char *reason)
{
    if (arq_ctx.mode_fsm == next)
        return;

    HLOGD("arq", "Mode FSM %s -> %s (%s)",
          mode_fsm_name(arq_ctx.mode_fsm),
          mode_fsm_name(next),
          reason ? reason : "event");
    arq_ctx.mode_fsm = next;
}

static bool mode_fsm_busy_locked(void)
{
    return arq_ctx.mode_fsm != ARQ_MODE_FSM_IDLE;
}

static void mode_fsm_reset_locked(const char *reason)
{
    arq_ctx.pending_mode_req = false;
    arq_ctx.pending_mode_ack = false;
    arq_ctx.pending_mode = 0;
    arq_ctx.mode_req_in_flight = false;
    arq_ctx.mode_req_mode = 0;
    arq_ctx.mode_req_retries_left = 0;
    arq_ctx.mode_req_deadline = 0;
    mode_fsm_set_locked(ARQ_MODE_FSM_IDLE, reason);
}

static void mode_fsm_queue_req_locked(uint8_t mode, const char *reason)
{
    arq_ctx.pending_mode = mode;
    arq_ctx.pending_mode_req = true;
    arq_ctx.pending_mode_ack = false;
    arq_ctx.mode_req_in_flight = false;
    mode_fsm_set_locked(ARQ_MODE_FSM_REQ_PENDING, reason);
}

static void mode_fsm_queue_ack_locked(uint8_t mode, const char *reason)
{
    arq_ctx.pending_mode = mode;
    arq_ctx.pending_mode_ack = true;
    arq_ctx.pending_mode_req = false;
    arq_ctx.mode_req_in_flight = false;
    arq_ctx.mode_req_retries_left = 0;
    arq_ctx.mode_req_deadline = 0;
    mode_fsm_set_locked(ARQ_MODE_FSM_ACK_PENDING, reason);
}

static void update_connected_state_from_turn_locked(void)
{
    if (arq_fsm.current == state_disconnecting)
        return;

    if (arq_ctx.turn_role == ARQ_TURN_ISS)
        arq_fsm.current = state_connected_iss;
    else
        arq_fsm.current = state_connected_irs;
}

static void become_iss_locked(const char *reason)
{
    time_t now = time(NULL);

    arq_ctx.turn_role = ARQ_TURN_ISS;
    arq_ctx.payload_mode = FREEDV_MODE_DATAC4;
    arq_ctx.payload_start_pending = true;
    arq_ctx.startup_acks_left = ARQ_STARTUP_ACKS_REQUIRED;
    arq_ctx.startup_deadline = now + ARQ_STARTUP_MAX_S;
    mode_fsm_reset_locked("turn iss");
    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;
    update_connected_state_from_turn_locked();
    HLOGD("arq", "Turn -> ISS (%s)", reason ? reason : "role change");
}

static void become_irs_locked(const char *reason)
{
    arq_ctx.turn_role = ARQ_TURN_IRS;
    arq_ctx.waiting_ack = false;
    arq_ctx.outstanding_frame_len = 0;
    arq_ctx.outstanding_app_len = 0;
    mode_fsm_reset_locked("turn irs");
    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;
    update_connected_state_from_turn_locked();
    HLOGD("arq", "Turn -> IRS (%s)", reason ? reason : "role change");
}

static int payload_mode_from_snr(float snr)
{
    if (snr >= 5.0f)
        return FREEDV_MODE_DATAC1;
    if (snr >= 0.0f)
        return FREEDV_MODE_DATAC3;
    return FREEDV_MODE_DATAC4;
}

static bool is_payload_mode(int mode)
{
    return mode == FREEDV_MODE_DATAC1 ||
           mode == FREEDV_MODE_DATAC3 ||
           mode == FREEDV_MODE_DATAC4;
}

static int payload_mode_rank(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        return 2;
    case FREEDV_MODE_DATAC3:
        return 1;
    case FREEDV_MODE_DATAC4:
    default:
        return 0;
    }
}

static void apply_payload_mode_locked(int new_mode, const char *reason)
{
    arq_action_t action;

    if (!is_payload_mode(new_mode))
        return;
    if (arq_ctx.payload_mode == new_mode)
        return;

    arq_ctx.payload_mode = new_mode;
    arq_ctx.slot_len_s = mode_slot_len_s(new_mode);
    arq_ctx.tx_period_s = arq_ctx.slot_len_s;
    arq_ctx.ack_timeout_s = ack_timeout_s_for_mode(new_mode);
    HLOGD("arq", "Payload mode -> %s (%s)",
          mode_name(new_mode),
          reason ? reason : "negotiated");

    action.type = ARQ_ACTION_MODE_SWITCH;
    action.mode = new_mode;
    action.frame_size = frame_size_for_payload_mode(new_mode);
    (void)arq_action_queue_push_locked(&action);
}

static void request_payload_mode_locked(int new_mode, const char *reason)
{
    if (!is_payload_mode(new_mode))
        return;

    if (arq_ctx.waiting_ack && arq_ctx.outstanding_frame_len > 0)
    {
        arq_ctx.mode_apply_pending = true;
        arq_ctx.mode_apply_mode = (uint8_t)new_mode;
        HLOGD("arq", "Payload mode defer -> %s (waiting ack)", mode_name(new_mode));
        return;
    }

    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;
    apply_payload_mode_locked(new_mode, reason);
}

static void apply_deferred_payload_mode_locked(void)
{
    int mode;

    if (!arq_ctx.mode_apply_pending)
        return;
    if (arq_ctx.waiting_ack && arq_ctx.outstanding_frame_len > 0)
        return;

    mode = arq_ctx.mode_apply_mode;
    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;
    apply_payload_mode_locked(mode, "deferred");
}

static int desired_payload_mode_locked(void)
{
    int current = is_payload_mode(arq_ctx.payload_mode) ? arq_ctx.payload_mode : FREEDV_MODE_DATAC4;
    int desired;
    int current_rank;
    int desired_rank;
    size_t effective_backlog = arq_ctx.app_tx_len;

    if (arq_ctx.snr_ema == 0.0f)
        return current;

    desired = payload_mode_from_snr(arq_ctx.snr_ema);
    current_rank = payload_mode_rank(current);
    desired_rank = payload_mode_rank(desired);

    if (desired_rank > current_rank)
    {
        if (arq_ctx.peer_backlog_nonzero &&
            effective_backlog < ARQ_BACKLOG_MIN_BIDIR_MODE_UPGRADE)
            return current;
        if (desired == FREEDV_MODE_DATAC1 && effective_backlog < ARQ_BACKLOG_MIN_DATAC1)
            return current;
        if (desired == FREEDV_MODE_DATAC3 && effective_backlog < ARQ_BACKLOG_MIN_DATAC3)
            return current;
    }

    return desired;
}

static void update_payload_mode_locked(void)
{
    if (!ARQ_ENABLE_MODE_UPGRADE)
        return;
    if (arq_ctx.payload_start_pending)
        return;
    if (arq_ctx.waiting_ack)
        return;
    if (mode_fsm_busy_locked())
        return;
    if (!is_connected_state_locked())
        return;
    if (!is_payload_mode(arq_ctx.payload_mode))
        return;
    if (arq_ctx.turn_role != ARQ_TURN_ISS || arq_ctx.app_tx_len == 0)
        return;

    int desired = desired_payload_mode_locked();
    if (desired == arq_ctx.payload_mode)
    {
        arq_ctx.mode_candidate_mode = 0;
        arq_ctx.mode_candidate_hits = 0;
        return;
    }

    if (arq_ctx.mode_candidate_mode != desired)
    {
        arq_ctx.mode_candidate_mode = desired;
        arq_ctx.mode_candidate_hits = 1;
        if (arq_ctx.mode_candidate_hits < ARQ_MODE_SWITCH_HYST_COUNT)
            return;
    }
    else
    {
        arq_ctx.mode_candidate_hits++;
        if (arq_ctx.mode_candidate_hits < ARQ_MODE_SWITCH_HYST_COUNT)
            return;
    }

    mode_fsm_queue_req_locked((uint8_t)desired, "snr/backlog");
    arq_ctx.mode_candidate_hits = 0;
    HLOGD("arq", "Mode req -> %s (snr=%.1f backlog=%zu)",
          mode_name(desired), arq_ctx.snr_ema, arq_ctx.app_tx_len);
}

static void schedule_flow_hint_locked(void)
{
    bool backlog_nonzero = arq_ctx.app_tx_len > 0;

    if (arq_ctx.payload_start_pending)
        return;
    if (arq_ctx.last_flow_hint_sent >= 0 &&
        ((arq_ctx.last_flow_hint_sent != 0) == backlog_nonzero))
        return;

    arq_ctx.pending_flow_hint = true;
    arq_ctx.flow_hint_value = backlog_nonzero;
}

static int max_gear_for_frame_size(size_t frame_size)
{
    if (frame_size >= 510)
        return 2;
    if (frame_size >= 126)
        return 1;
    return 0;
}

static int payload_mode_for_frame_size(size_t frame_size)
{
    if (frame_size >= 510)
        return FREEDV_MODE_DATAC1;
    if (frame_size >= 126)
        return FREEDV_MODE_DATAC3;
    if (frame_size >= 56)
        return FREEDV_MODE_DATAC4;
    return 0;
}

static size_t frame_size_for_payload_mode(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        return 510;
    case FREEDV_MODE_DATAC3:
        return 126;
    case FREEDV_MODE_DATAC4:
        return 56;
    case FREEDV_MODE_DATAC13:
        return ARQ_CONTROL_FRAME_SIZE;
    default:
        return 0;
    }
}

static size_t chunk_size_for_gear_locked(void)
{
    size_t cap = arq_conn.frame_size - ARQ_PAYLOAD_OFFSET;
    return cap;
}

static size_t control_frame_size_locked(void)
{
    return ARQ_CONTROL_FRAME_SIZE; /* DATAC13 payload bytes per modem frame */
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
    HLOGD("arq", "Gear up -> %d", arq_ctx.gear);
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
    HLOGD("arq", "Gear down -> %d", arq_ctx.gear);
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
    arq_ctx.pending_keepalive = false;
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
    arq_ctx.next_role_tx_at =
        arq_realtime_ms() + ((uint64_t)compute_inter_frame_interval_locked(now, with_jitter) * 1000ULL);
}

static void schedule_immediate_control_tx_with_guard_locked(time_t now,
                                                            const char *reason,
                                                            uint64_t extra_guard_ms)
{
    bool adjusted = false;
    uint64_t now_ms = arq_realtime_ms();
    uint64_t earliest_tx = now_ms + ARQ_CHANNEL_GUARD_MS + extra_guard_ms;

    if (arq_ctx.remote_busy_until > earliest_tx)
        earliest_tx = arq_ctx.remote_busy_until;

    if (arq_ctx.next_role_tx_at != earliest_tx)
    {
        arq_ctx.next_role_tx_at = earliest_tx;
        adjusted = true;
    }

    if (adjusted)
        HLOGD("arq", "Immediate control reply (%s) at +%llums",
              reason ? reason : "rx",
              (unsigned long long)(arq_ctx.next_role_tx_at - now_ms));
}

static void schedule_immediate_control_tx_locked(time_t now, const char *reason)
{
    schedule_immediate_control_tx_with_guard_locked(now, reason, 0);
}

static bool defer_tx_if_busy_locked(time_t now)
{
    uint64_t now_ms = arq_realtime_ms();
    (void)now;

    if (now_ms >= arq_ctx.remote_busy_until)
        return false;

    if (arq_ctx.next_role_tx_at < arq_ctx.remote_busy_until)
        arq_ctx.next_role_tx_at = arq_ctx.remote_busy_until;
    return true;
}

static int queue_frame_locked(const uint8_t *frame, size_t frame_size, bool control_plane)
{
    arq_action_t action;
    cbuf_handle_t tx_buffer = control_plane ? data_tx_buffer_arq_control : data_tx_buffer_arq;
    int written = write_buffer(tx_buffer, (uint8_t *)frame, frame_size);

    if (written < 0)
        return written;

    action.type = control_plane ? ARQ_ACTION_TX_CONTROL : ARQ_ACTION_TX_PAYLOAD;
    action.mode = control_plane ? arq_ctx.control_mode : arq_ctx.payload_mode;
    action.frame_size = frame_size;
    (void)arq_action_queue_push_locked(&action);

    return written;
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

    if (frame_size != ARQ_CONTROL_FRAME_SIZE)
        return -1;
    if (subtype != ARQ_SUBTYPE_CALL && subtype != ARQ_SUBTYPE_ACCEPT)
        return -1;

    encoded_len = encode_connect_callsign_payload(msg, encoded, sizeof(encoded));
    if (encoded_len <= 0)
        return -1;

    memset(frame, 0, frame_size);
    frame[ARQ_CONNECT_SESSION_IDX] = connect_meta_build(session_id, subtype == ARQ_SUBTYPE_ACCEPT);
    memcpy(frame + ARQ_CONNECT_PAYLOAD_IDX, encoded, (size_t)encoded_len);
    write_frame_header(frame, PACKET_TYPE_ARQ_DATA, frame_size);
    return 0;
}

static void split_call_connect_payload(char *decoded, char *dst, char *src)
{
    /* Supports truncation: dst may be present even when "|src" is missing. */
    char *sep = strchr(decoded, '|');

    if (sep)
    {
        *sep = 0;
        sep++;
        strncpy(src, sep, CALLSIGN_MAX_SIZE - 1);
        src[CALLSIGN_MAX_SIZE - 1] = 0;
    }

    snprintf(dst, CALLSIGN_MAX_SIZE, "%.*s", CALLSIGN_MAX_SIZE - 1, decoded);
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

static int send_mode_change_locked(uint8_t subtype, uint8_t mode)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t payload[1];
    size_t frame_size = control_frame_size_locked();

    if (subtype != ARQ_SUBTYPE_MODE_REQ && subtype != ARQ_SUBTYPE_MODE_ACK)
        return -1;
    if (!is_payload_mode((int)mode))
        return -1;

    payload[0] = mode;
    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL,
                           subtype,
                           0,
                           0,
                           payload,
                           1,
                           frame,
                           frame_size) < 0)
        return -1;
    return queue_frame_locked(frame, frame_size, true);
}

static int send_turn_control_locked(uint8_t subtype, uint8_t value)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t payload[1];
    size_t frame_size = control_frame_size_locked();

    if (subtype != ARQ_SUBTYPE_TURN_REQ &&
        subtype != ARQ_SUBTYPE_TURN_ACK &&
        subtype != ARQ_SUBTYPE_FLOW_HINT)
        return -1;

    payload[0] = value;
    if (build_frame_locked(PACKET_TYPE_ARQ_CONTROL,
                           subtype,
                           0,
                           0,
                           payload,
                           1,
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
    arq_action_queue_clear_locked();

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
    arq_ctx.pending_ack_set_ms = 0;
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
    arq_ctx.disconnect_after_flush = false;
    arq_ctx.disconnect_after_flush_to_no_client = false;
    arq_ctx.app_tx_len = 0;
    arq_ctx.gear = 0;
    arq_ctx.success_streak = 0;
    arq_ctx.failure_streak = 0;
    arq_ctx.snr_ema = 0.0f;
    arq_ctx.payload_start_pending = false;
    arq_ctx.startup_acks_left = 0;
    arq_ctx.startup_deadline = 0;
    if (is_payload_mode(arq_conn.mode))
        arq_ctx.payload_mode = arq_conn.mode;
    else
        arq_ctx.payload_mode = FREEDV_MODE_DATAC4;
    arq_ctx.control_mode = FREEDV_MODE_DATAC13;
    arq_ctx.turn_role = ARQ_TURN_NONE;
    arq_ctx.peer_backlog_nonzero = false;
    arq_ctx.last_peer_payload_rx = 0;
    arq_ctx.pending_flow_hint = false;
    arq_ctx.flow_hint_value = false;
    arq_ctx.last_flow_hint_sent = -1;
    arq_ctx.pending_turn_req = false;
    arq_ctx.turn_req_in_flight = false;
    arq_ctx.turn_req_retries_left = 0;
    arq_ctx.turn_req_deadline = 0;
    arq_ctx.pending_turn_ack = false;
    arq_ctx.turn_promote_after_ack = false;
    arq_ctx.turn_ack_deferred = false;
    arq_ctx.pending_mode_req = false;
    arq_ctx.pending_mode_ack = false;
    arq_ctx.pending_mode = 0;
    arq_ctx.mode_req_in_flight = false;
    arq_ctx.mode_req_mode = 0;
    arq_ctx.mode_req_retries_left = 0;
    arq_ctx.mode_req_deadline = 0;
    arq_ctx.mode_candidate_mode = 0;
    arq_ctx.mode_candidate_hits = 0;
    arq_ctx.mode_fsm = ARQ_MODE_FSM_IDLE;
    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;

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
    HLOGI("arq", "Disconnect finalized");
    notify_disconnected_locked();
    arq_fsm.current = next_state;
}

static void start_disconnect_locked(bool to_no_client)
{
    time_t now = time(NULL);
    bool effective_to_no_client = to_no_client || arq_ctx.disconnect_after_flush_to_no_client;

    arq_ctx.disconnect_in_progress = true;
    arq_ctx.disconnect_to_no_client = effective_to_no_client;
    arq_ctx.disconnect_retries_left = ARQ_DISCONNECT_RETRY_SLOTS + 1;
    arq_ctx.pending_disconnect = true;
    arq_ctx.disconnect_deadline = now + (arq_ctx.slot_len_s * 2) + 2;
    arq_ctx.disconnect_after_flush = false;
    arq_ctx.disconnect_after_flush_to_no_client = false;
    arq_ctx.next_role_tx_at = arq_realtime_ms();
    arq_fsm.current = state_disconnecting;
    HLOGI("arq", "Disconnect start (to_no_client=%d)", effective_to_no_client ? 1 : 0);
}

static void request_disconnect_locked(bool to_no_client, const char *reason)
{
    if (arq_fsm.current == state_disconnecting)
    {
        if (to_no_client)
            arq_ctx.disconnect_to_no_client = true;
        return;
    }

    if (has_outbound_payload_pending_locked())
    {
        arq_ctx.disconnect_after_flush = true;
        if (to_no_client)
            arq_ctx.disconnect_after_flush_to_no_client = true;
        HLOGI("arq", "Disconnect deferred (%s): backlog=%zu waiting_ack=%d actions=%zu",
              reason ? reason : "request",
              arq_ctx.app_tx_len,
              arq_ctx.waiting_ack ? 1 : 0,
              arq_ctx.pending_payload_actions);
        return;
    }

    start_disconnect_locked(to_no_client);
}

static void enter_connected_locked(void)
{
    time_t now = time(NULL);
    arq_ctx.payload_mode = FREEDV_MODE_DATAC4;
    arq_ctx.call_retries_left = 0;
    arq_ctx.accept_retries_left = 0;
    arq_ctx.pending_call = false;
    arq_ctx.pending_accept = false;
    arq_ctx.pending_keepalive = false;
    arq_ctx.pending_keepalive_ack = false;
    arq_ctx.keepalive_waiting = false;
    arq_ctx.keepalive_misses = 0;
    mode_fsm_reset_locked("new call");
    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;
    arq_ctx.last_keepalive_rx = now;
    arq_ctx.last_keepalive_tx = now;
    arq_ctx.last_phy_activity = now;
    arq_ctx.disconnect_in_progress = false;
    arq_ctx.disconnect_retries_left = 0;
    arq_ctx.disconnect_deadline = 0;
    arq_ctx.disconnect_after_flush = false;
    arq_ctx.disconnect_after_flush_to_no_client = false;
    arq_ctx.connect_deadline = 0;
    arq_ctx.success_streak = 0;
    arq_ctx.failure_streak = 0;
    arq_ctx.payload_start_pending = true;
    arq_ctx.startup_acks_left = ARQ_STARTUP_ACKS_REQUIRED;
    arq_ctx.startup_deadline = now + ARQ_STARTUP_MAX_S;
    arq_ctx.pending_mode_req = false;
    arq_ctx.pending_mode_ack = false;
    arq_ctx.pending_mode = 0;
    arq_ctx.mode_req_in_flight = false;
    arq_ctx.mode_req_mode = 0;
    arq_ctx.mode_req_retries_left = 0;
    arq_ctx.mode_req_deadline = 0;
    arq_ctx.mode_candidate_mode = 0;
    arq_ctx.mode_candidate_hits = 0;
    arq_ctx.mode_fsm = ARQ_MODE_FSM_IDLE;
    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;
    arq_ctx.pending_turn_req = false;
    arq_ctx.turn_req_in_flight = false;
    arq_ctx.turn_req_retries_left = 0;
    arq_ctx.turn_req_deadline = 0;
    arq_ctx.pending_turn_ack = false;
    arq_ctx.turn_promote_after_ack = false;
    arq_ctx.turn_ack_deferred = false;
    arq_ctx.pending_flow_hint = false;
    arq_ctx.last_flow_hint_sent = -1;
    arq_ctx.peer_backlog_nonzero = (arq_ctx.role == ARQ_ROLE_CALLEE);
    arq_ctx.last_peer_payload_rx = 0;
    arq_ctx.turn_role = (arq_ctx.role == ARQ_ROLE_CALLER) ? ARQ_TURN_ISS : ARQ_TURN_IRS;
    arq_ctx.gear = initial_gear_locked();
    update_connected_state_from_turn_locked();
    if (arq_ctx.turn_role == ARQ_TURN_ISS)
        schedule_flow_hint_locked();
    tnc_send_connected();
}

static void start_outgoing_call_locked(void)
{
    time_t now = time(NULL);
    int response_wait_s = connect_response_wait_s();
    int retry_interval_s = response_wait_s + (2 * mode_slot_len_s(FREEDV_MODE_DATAC13));
    arq_ctx.role = ARQ_ROLE_CALLER;
    arq_ctx.session_id = (uint8_t)((arq_ctx.session_id + 1) & ARQ_CONNECT_SESSION_MASK);
    if (arq_ctx.session_id == 0)
        arq_ctx.session_id = 1;
    arq_ctx.tx_seq = 0;
    arq_ctx.rx_expected_seq = 0;
    arq_ctx.waiting_ack = false;
    arq_ctx.outstanding_frame_len = 0;
    arq_ctx.outstanding_app_len = 0;
    arq_ctx.call_retries_left = arq_ctx.max_call_retries + 1;
    arq_ctx.pending_call = true;
    arq_ctx.turn_role = ARQ_TURN_NONE;
    arq_ctx.pending_accept = false;
    arq_ctx.pending_ack = false;
    arq_ctx.pending_ack_set_ms = 0;
    arq_ctx.turn_ack_deferred = false;
    arq_ctx.pending_disconnect = false;
    arq_ctx.pending_keepalive = false;
    arq_ctx.pending_keepalive_ack = false;
    arq_ctx.keepalive_waiting = false;
    arq_ctx.keepalive_misses = 0;
    mode_fsm_reset_locked("new call");
    arq_ctx.mode_apply_pending = false;
    arq_ctx.mode_apply_mode = 0;
    arq_ctx.last_peer_payload_rx = 0;
    arq_ctx.last_keepalive_rx = now;
    arq_ctx.last_keepalive_tx = now;
    arq_ctx.payload_start_pending = false;
    arq_ctx.startup_acks_left = 0;
    arq_ctx.startup_deadline = 0;
    arq_ctx.disconnect_in_progress = false;
    arq_ctx.disconnect_to_no_client = false;
    arq_ctx.disconnect_retries_left = 0;
    arq_ctx.disconnect_deadline = 0;
    arq_ctx.disconnect_after_flush = false;
    arq_ctx.disconnect_after_flush_to_no_client = false;
    arq_ctx.next_role_tx_at = arq_realtime_ms();
    arq_ctx.remote_busy_until = 0;
    arq_ctx.connect_deadline = now + (retry_interval_s * (arq_ctx.max_call_retries + 1)) + ARQ_CONNECT_GRACE_SLOTS;
    arq_fsm.current = state_calling_wait_accept;
}

static void queue_next_data_frame_locked(void)
{
    uint8_t frame[INT_BUFFER_SIZE];
    size_t chunk;

    if (!is_connected_state_locked())
        return;
    if (arq_ctx.turn_role != ARQ_TURN_ISS)
        return;
    if (arq_ctx.waiting_ack)
        return;
    if (arq_ctx.pending_turn_req || arq_ctx.turn_req_in_flight)
        return;
    if (arq_ctx.app_tx_len == 0)
        return;
    if (arq_conn.mode != arq_ctx.payload_mode)
        return;
    if (arq_conn.frame_size <= ARQ_PAYLOAD_OFFSET)
        return;

    if (arq_ctx.payload_start_pending)
    {
        arq_ctx.payload_mode = FREEDV_MODE_DATAC4;
    }

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
    arq_ctx.ack_deadline = time(NULL) + arq_ctx.ack_timeout_s + ARQ_ACK_GUARD_S;
    arq_ctx.tx_seq++;
}

static bool do_slot_tx_locked(time_t now)
{
    uint64_t now_ms = arq_realtime_ms();

    if (arq_ctx.role == ARQ_ROLE_NONE)
        return false;
    if (arq_conn.TRX == TX)
        return false;
    if (now_ms < arq_ctx.next_role_tx_at)
        return false;
    if (defer_tx_if_busy_locked(now))
        return false;
    apply_deferred_payload_mode_locked();

    if (arq_ctx.turn_ack_deferred &&
        !arq_ctx.waiting_ack &&
        arq_ctx.outstanding_frame_len == 0)
    {
        arq_ctx.turn_ack_deferred = false;
        if (arq_ctx.peer_backlog_nonzero)
            become_irs_locked("turn ack deferred");
        else
            update_connected_state_from_turn_locked();
    }

    if (arq_ctx.disconnect_after_flush &&
        !arq_ctx.pending_disconnect &&
        !has_outbound_payload_pending_locked())
    {
        bool to_no_client = arq_ctx.disconnect_after_flush_to_no_client;
        start_disconnect_locked(to_no_client);
    }

    if (arq_ctx.pending_disconnect)
    {
        if (arq_ctx.disconnect_retries_left <= 0)
        {
            arq_ctx.pending_disconnect = false;
            return false;
        }
        send_disconnect_locked();
        HLOGD("arq", "Disconnect tx retry=%d", arq_ctx.disconnect_retries_left);
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
        arq_ctx.next_role_tx_at = now_ms +
                                  (2 * control_slot_ms(FREEDV_MODE_DATAC13)) +
                                  ((uint64_t)connect_response_wait_s() * 1000ULL);
        return true;
    }

    if (arq_ctx.pending_accept)
    {
        send_accept_locked();
        arq_ctx.pending_accept = false;
        arq_ctx.accept_retries_left = 0;
        if (!is_connected_state_locked())
            enter_connected_locked();
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_ctx.pending_ack)
    {
        uint64_t ack_wait_ms = 0;
        uint64_t now_ms = arq_monotonic_ms();
        if (arq_ctx.pending_ack_set_ms > 0 && now_ms >= arq_ctx.pending_ack_set_ms)
            ack_wait_ms = now_ms - arq_ctx.pending_ack_set_ms;
        send_ack_locked(arq_ctx.pending_ack_seq);
        HLOGD("arq", "ACK tx seq=%u wait=%llums",
              arq_ctx.pending_ack_seq,
              (unsigned long long)ack_wait_ms);
        arq_ctx.pending_ack = false;
        arq_ctx.pending_ack_set_ms = 0;
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_ctx.pending_keepalive_ack)
    {
        send_keepalive_locked(ARQ_SUBTYPE_KEEPALIVE_ACK);
        HLOGD("arq", "Keepalive ACK tx");
        arq_ctx.pending_keepalive_ack = false;
        arq_ctx.last_keepalive_tx = now;
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (arq_ctx.pending_flow_hint && !arq_ctx.waiting_ack && !arq_ctx.payload_start_pending)
    {
        if (send_turn_control_locked(ARQ_SUBTYPE_FLOW_HINT, arq_ctx.flow_hint_value ? 1 : 0) == 0)
        {
            arq_ctx.last_flow_hint_sent = arq_ctx.flow_hint_value ? 1 : 0;
            arq_ctx.pending_flow_hint = false;
            schedule_next_tx_locked(now, false);
            return true;
        }
    }

    if (arq_ctx.pending_turn_ack)
    {
        uint8_t has_data = arq_ctx.app_tx_len > 0 ? 1 : 0;
        if (send_turn_control_locked(ARQ_SUBTYPE_TURN_ACK, has_data) == 0)
        {
            arq_ctx.pending_turn_ack = false;
            if (arq_ctx.turn_promote_after_ack && has_data)
                become_iss_locked("turn ack");
            arq_ctx.turn_promote_after_ack = false;
            update_connected_state_from_turn_locked();
            schedule_next_tx_locked(now, false);
            return true;
        }
    }

    if (arq_ctx.pending_turn_req && !arq_ctx.turn_req_in_flight && !arq_ctx.payload_start_pending)
    {
        if (send_turn_control_locked(ARQ_SUBTYPE_TURN_REQ, 1) == 0)
        {
            arq_ctx.pending_turn_req = false;
            arq_ctx.turn_req_in_flight = true;
            arq_ctx.turn_req_retries_left = ARQ_TURN_REQ_RETRIES;
            arq_ctx.turn_req_deadline = now + arq_ctx.ack_timeout_s;
            arq_fsm.current = state_turn_negotiating;
            schedule_next_tx_locked(now, false);
            return true;
        }
    }

    if (arq_ctx.turn_req_in_flight && now >= arq_ctx.turn_req_deadline)
    {
        if (arq_ctx.turn_req_retries_left > 0 &&
            send_turn_control_locked(ARQ_SUBTYPE_TURN_REQ, 1) == 0)
        {
            arq_ctx.turn_req_retries_left--;
            arq_ctx.turn_req_deadline = now + arq_ctx.ack_timeout_s;
            schedule_next_tx_locked(now, true);
            return true;
        }
        arq_ctx.turn_req_in_flight = false;
        update_connected_state_from_turn_locked();
    }

    if (!arq_ctx.waiting_ack &&
        arq_ctx.mode_fsm == ARQ_MODE_FSM_ACK_PENDING)
    {
        if (send_mode_change_locked(ARQ_SUBTYPE_MODE_ACK, arq_ctx.pending_mode) == 0)
        {
            request_payload_mode_locked(arq_ctx.pending_mode, "mode ack tx");
            mode_fsm_reset_locked("ack tx");
            schedule_next_tx_locked(now, false);
            return true;
        }
    }

    if (!arq_ctx.waiting_ack &&
        arq_ctx.mode_fsm == ARQ_MODE_FSM_REQ_PENDING)
    {
        if (send_mode_change_locked(ARQ_SUBTYPE_MODE_REQ, arq_ctx.pending_mode) == 0)
        {
            arq_ctx.pending_mode_req = false;
            arq_ctx.mode_req_in_flight = true;
            arq_ctx.mode_req_mode = arq_ctx.pending_mode;
            arq_ctx.mode_req_retries_left = ARQ_MODE_REQ_RETRIES;
            arq_ctx.mode_req_deadline = now + arq_ctx.ack_timeout_s;
            mode_fsm_set_locked(ARQ_MODE_FSM_REQ_IN_FLIGHT, "req tx");
            schedule_next_tx_locked(now, false);
            return true;
        }
    }

    if (!arq_ctx.waiting_ack &&
        !arq_ctx.payload_start_pending &&
        arq_ctx.mode_fsm == ARQ_MODE_FSM_REQ_IN_FLIGHT &&
        now >= arq_ctx.mode_req_deadline)
    {
        if (arq_ctx.mode_req_retries_left > 0 &&
            send_mode_change_locked(ARQ_SUBTYPE_MODE_REQ, arq_ctx.mode_req_mode) == 0)
        {
            arq_ctx.mode_req_retries_left--;
            arq_ctx.mode_req_deadline = now + arq_ctx.ack_timeout_s;
            schedule_next_tx_locked(now, true);
            return true;
        }
        mode_fsm_reset_locked("req timeout");
    }

    if (arq_ctx.pending_keepalive && !arq_ctx.payload_start_pending)
    {
        if (!keepalive_quiescent_locked())
        {
            arq_ctx.pending_keepalive = false;
            return false;
        }
        send_keepalive_locked(ARQ_SUBTYPE_KEEPALIVE);
        HLOGD("arq", "Keepalive tx");
        arq_ctx.pending_keepalive = false;
        arq_ctx.keepalive_waiting = true;
        arq_ctx.keepalive_deadline = now + arq_ctx.keepalive_interval_s + ARQ_ACK_GUARD_S;
        arq_ctx.last_keepalive_tx = now;
        schedule_next_tx_locked(now, false);
        return true;
    }

    if (is_connected_state_locked())
    {
        if (arq_ctx.turn_role == ARQ_TURN_ISS &&
            arq_ctx.waiting_ack &&
            now >= arq_ctx.ack_deadline)
        {
            if (arq_ctx.data_retries_left > 0 && arq_ctx.outstanding_frame_len > 0)
            {
                int retry_mode = payload_mode_for_frame_size(arq_ctx.outstanding_frame_len);
                if (is_payload_mode(retry_mode) && retry_mode != arq_ctx.payload_mode)
                    apply_payload_mode_locked(retry_mode, "retry realign");
                queue_frame_locked(arq_ctx.outstanding_frame, arq_ctx.outstanding_frame_len, false);
                arq_ctx.data_retries_left--;
                arq_ctx.ack_deadline = now + arq_ctx.ack_timeout_s + ARQ_ACK_GUARD_S;
                mark_failure_locked();
                HLOGW("arq", "Data retry seq=%u left=%d frame=%zu active=%zu",
                      arq_ctx.outstanding_seq,
                      arq_ctx.data_retries_left,
                      arq_ctx.outstanding_frame_len,
                      arq_conn.frame_size);
                schedule_next_tx_locked(now, true);
                return true;
            }

            HLOGW("arq", "Data timeout disconnect seq=%u retries=%d frame=%zu active=%zu waiting=%d",
                  arq_ctx.outstanding_seq,
                  arq_ctx.data_retries_left,
                  arq_ctx.outstanding_frame_len,
                  arq_conn.frame_size,
                  arq_ctx.waiting_ack ? 1 : 0);
            start_disconnect_locked(false);
            return true;
        }

        if (arq_ctx.turn_role == ARQ_TURN_ISS &&
            !arq_ctx.waiting_ack &&
            !arq_ctx.pending_turn_req &&
            !arq_ctx.turn_req_in_flight &&
            arq_ctx.app_tx_len > 0)
        {
            queue_next_data_frame_locked();
            if (arq_ctx.waiting_ack)
            {
                schedule_next_tx_locked(now, false);
                return true;
            }
        }

        if (arq_ctx.turn_role == ARQ_TURN_ISS &&
            !arq_ctx.waiting_ack &&
            arq_ctx.app_tx_len == 0 &&
            arq_ctx.peer_backlog_nonzero &&
            !arq_ctx.payload_start_pending &&
            !arq_ctx.pending_turn_req &&
            !arq_ctx.turn_req_in_flight &&
            !mode_fsm_busy_locked())
        {
            arq_ctx.pending_turn_req = true;
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
        request_disconnect_locked(false, "link request");
        break;
    case EV_CLIENT_DISCONNECT:
        request_disconnect_locked(true, "client disconnect");
        break;
    default:
        break;
    }
}

static void state_connected_common(int event)
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
        request_disconnect_locked(false, "link request");
        break;
    case EV_CLIENT_DISCONNECT:
        request_disconnect_locked(true, "client disconnect");
        break;
    default:
        break;
    }
}

static void state_connected(int event)
{
    state_connected_common(event);
}

static void state_connected_iss(int event)
{
    state_connected_common(event);
}

static void state_connected_irs(int event)
{
    state_connected_common(event);
}

static void state_turn_negotiating(int event)
{
    state_connected_common(event);
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
        HLOGE("arq", "Init failed: unsupported frame size %zu", frame_size);
        return EXIT_FAILURE;
    }

    arq_fsm_lock_ready = false;
    arq_cmd_bridge_started = false;
    arq_payload_bridge_started = false;
    memset(&arq_conn, 0, sizeof(arq_conn));
    memset(&arq_ctx, 0, sizeof(arq_ctx));
    init_model();
    if (arq_channel_bus_init(&arq_bus) < 0)
    {
        HLOGE("arq", "Init failed: channel bus init");
        return EXIT_FAILURE;
    }

    arq_conn.frame_size = frame_size;
    arq_conn.mode = mode;
    arq_conn.call_burst_size = 1;

    arq_ctx.initialized = true;
    if (mode == FREEDV_MODE_DATAC1 || mode == FREEDV_MODE_DATAC3 || mode == FREEDV_MODE_DATAC4)
        arq_ctx.payload_mode = mode;
    else
        arq_ctx.payload_mode = FREEDV_MODE_DATAC4;
    arq_ctx.control_mode = FREEDV_MODE_DATAC13;

    arq_ctx.slot_len_s = mode_slot_len_s(arq_ctx.payload_mode);
    arq_ctx.tx_period_s = arq_ctx.slot_len_s;
    arq_ctx.max_call_retries = ARQ_CALL_RETRY_SLOTS;
    arq_ctx.max_accept_retries = ARQ_ACCEPT_RETRY_SLOTS;
    arq_ctx.max_data_retries = ARQ_DATA_RETRY_SLOTS;
    arq_ctx.ack_timeout_s = ack_timeout_s_for_mode(arq_ctx.payload_mode);
    arq_ctx.connect_timeout_s =
        (arq_ctx.tx_period_s * (arq_ctx.max_call_retries + 2)) +
        ARQ_CONNECT_GRACE_SLOTS;
    arq_ctx.max_gear = max_gear_for_frame_size(frame_size);
    arq_ctx.payload_start_pending = false;
    arq_ctx.keepalive_interval_s = ARQ_KEEPALIVE_INTERVAL_S;
    arq_ctx.keepalive_miss_limit = ARQ_KEEPALIVE_MISS_LIMIT;

    fsm_init(&arq_fsm, state_no_connected_client);
    arq_fsm_lock_ready = true;
    if (arq_event_loop_start() != 0)
    {
        arq_fsm_lock_ready = false;
        fsm_destroy(&arq_fsm);
        arq_ctx.initialized = false;
        arq_channel_bus_dispose(&arq_bus);
        return EXIT_FAILURE;
    }

    if (pthread_create(&arq_cmd_bridge_tid, NULL, arq_cmd_bridge_worker, NULL) != 0)
    {
        arq_event_loop_stop();
        arq_fsm_lock_ready = false;
        fsm_destroy(&arq_fsm);
        arq_ctx.initialized = false;
        arq_channel_bus_dispose(&arq_bus);
        return EXIT_FAILURE;
    }
    arq_cmd_bridge_started = true;

    if (pthread_create(&arq_payload_bridge_tid, NULL, arq_payload_bridge_worker, NULL) != 0)
    {
        arq_channel_bus_close(&arq_bus);
        pthread_join(arq_cmd_bridge_tid, NULL);
        arq_cmd_bridge_started = false;
        arq_event_loop_stop();
        arq_fsm_lock_ready = false;
        fsm_destroy(&arq_fsm);
        arq_ctx.initialized = false;
        arq_channel_bus_dispose(&arq_bus);
        return EXIT_FAILURE;
    }
    arq_payload_bridge_started = true;
    return EXIT_SUCCESS;
}

void arq_shutdown(void)
{
    if (!arq_fsm_lock_ready)
    {
        arq_channel_bus_close(&arq_bus);
        if (arq_cmd_bridge_started)
        {
            pthread_join(arq_cmd_bridge_tid, NULL);
            arq_cmd_bridge_started = false;
        }
        if (arq_payload_bridge_started)
        {
            pthread_join(arq_payload_bridge_tid, NULL);
            arq_payload_bridge_started = false;
        }
        arq_channel_bus_dispose(&arq_bus);
        return;
    }

    arq_channel_bus_close(&arq_bus);
    if (arq_cmd_bridge_started)
    {
        pthread_join(arq_cmd_bridge_tid, NULL);
        arq_cmd_bridge_started = false;
    }
    if (arq_payload_bridge_started)
    {
        pthread_join(arq_payload_bridge_tid, NULL);
        arq_payload_bridge_started = false;
    }

    if (arq_ctx.initialized)
    {
        arq_event_loop_stop();

        arq_lock();
        arq_action_queue_clear_locked();
        arq_ctx.initialized = false;
        arq_unlock();
    }
    arq_fsm_lock_ready = false;
    fsm_destroy(&arq_fsm);
    arq_channel_bus_dispose(&arq_bus);
}

bool arq_is_link_connected(void)
{
    bool connected;

    if (!arq_fsm_lock_ready)
        return false;

    arq_lock();
    connected = arq_ctx.initialized && is_connected_state_locked();
    arq_unlock();
    return connected;
}

void arq_post_event(int event)
{
    if (!arq_fsm_lock_ready)
        return;

    arq_event_loop_enqueue_fsm(event);
}

void arq_tick_1hz(void)
{
    time_t now = time(NULL);

    if (!arq_fsm_lock_ready)
        return;

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

    if (is_connected_state_locked())
    {
        time_t last_link_activity = arq_ctx.last_keepalive_tx;
        if (arq_ctx.last_keepalive_rx > last_link_activity)
            last_link_activity = arq_ctx.last_keepalive_rx;
        if (arq_ctx.last_phy_activity > last_link_activity)
            last_link_activity = arq_ctx.last_phy_activity;

        bool link_idle =
            !arq_ctx.pending_keepalive &&
            keepalive_quiescent_locked();

        if (arq_ctx.payload_start_pending &&
            arq_ctx.startup_deadline > 0 &&
            now >= arq_ctx.startup_deadline)
        {
            bool startup_traffic_active =
                arq_ctx.waiting_ack ||
                arq_ctx.pending_ack ||
                arq_ctx.pending_turn_ack ||
                (arq_ctx.turn_role == ARQ_TURN_IRS && arq_ctx.peer_backlog_nonzero) ||
                (arq_ctx.turn_role == ARQ_TURN_ISS && arq_ctx.app_tx_len > 0);

            if (startup_traffic_active)
            {
                arq_ctx.startup_deadline = now + ARQ_STARTUP_MAX_S;
                HLOGD("arq", "Startup gate extend (traffic)");
            }
            else
            {
                arq_ctx.payload_start_pending = false;
                arq_ctx.startup_acks_left = 0;
                arq_ctx.startup_deadline = 0;
                HLOGD("arq", "Startup gate end (timeout)");
                schedule_flow_hint_locked();
            }
        }

        if (arq_ctx.keepalive_waiting && now >= arq_ctx.keepalive_deadline)
        {
            arq_ctx.keepalive_waiting = false;
            arq_ctx.keepalive_misses++;
            HLOGW("arq", "Keepalive miss=%d", arq_ctx.keepalive_misses);
            if (arq_ctx.keepalive_misses >= arq_ctx.keepalive_miss_limit)
            {
                HLOGW("arq", "Keepalive timeout disconnect misses=%d limit=%d",
                      arq_ctx.keepalive_misses,
                      arq_ctx.keepalive_miss_limit);
                start_disconnect_locked(false);
            }
        }

        if (link_idle &&
            !arq_ctx.payload_start_pending &&
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

static int arq_queue_app_data_locked(const uint8_t *data, size_t len)
{
    size_t free_space;

    if (!data || len == 0 || !arq_ctx.initialized || !is_connected_state_locked())
        return 0;

    free_space = DATA_TX_BUFFER_SIZE - arq_ctx.app_tx_len;
    if (len > free_space)
        len = free_space;
    if (len == 0)
        return 0;

    memcpy(arq_ctx.app_tx_queue + arq_ctx.app_tx_len, data, len);
    arq_ctx.app_tx_len += len;
    schedule_flow_hint_locked();

    if (len > (size_t)INT_MAX)
        return INT_MAX;

    return (int)len;
}

int arq_queue_data(const uint8_t *data, size_t len)
{
    size_t total_queued = 0;

    if (!arq_fsm_lock_ready || !data || len == 0)
        return 0;

    if (!arq_event_loop.started)
    {
        int queued;

        arq_lock();
        queued = arq_queue_app_data_locked(data, len);
        arq_unlock();
        return queued;
    }

    while (total_queued < len)
    {
        arq_event_app_tx_result_t result;
        size_t chunk = len - total_queued;
        int queued = 0;
        bool sync_ok = false;

        if (chunk > INT_BUFFER_SIZE)
            chunk = INT_BUFFER_SIZE;

        memset(&result, 0, sizeof(result));
        if (pthread_mutex_init(&result.lock, NULL) == 0)
        {
            if (pthread_cond_init(&result.cond, NULL) == 0)
                sync_ok = true;
            else
                pthread_mutex_destroy(&result.lock);
        }

        if (!sync_ok)
        {
            arq_lock();
            queued = arq_queue_app_data_locked(data + total_queued, chunk);
            arq_unlock();
        }
        else
        {
            arq_event_loop_enqueue_app_tx(data + total_queued, chunk, &result);

            pthread_mutex_lock(&result.lock);
            while (!result.done)
                pthread_cond_wait(&result.cond, &result.lock);
            queued = result.queued;
            pthread_mutex_unlock(&result.lock);

            pthread_cond_destroy(&result.cond);
            pthread_mutex_destroy(&result.lock);
        }

        if (queued <= 0)
            break;

        if ((size_t)queued > chunk)
            queued = (int)chunk;

        total_queued += (size_t)queued;
        if ((size_t)queued < chunk)
            break;
    }

    if (total_queued > (size_t)INT_MAX)
        return INT_MAX;

    return (int)total_queued;
}

static void arq_handle_tcp_cmd_msg(const arq_cmd_msg_t *cmd)
{
    if (!cmd || !arq_fsm_lock_ready)
        return;

    switch (cmd->type)
    {
    case ARQ_CMD_CLIENT_CONNECT:
        arq_event_loop_enqueue_fsm(EV_CLIENT_CONNECT);
        break;
    case ARQ_CMD_CLIENT_DISCONNECT:
        arq_event_loop_enqueue_fsm(EV_CLIENT_DISCONNECT);
        break;
    case ARQ_CMD_LISTEN_ON:
        arq_event_loop_enqueue_fsm(EV_START_LISTEN);
        break;
    case ARQ_CMD_LISTEN_OFF:
        arq_event_loop_enqueue_fsm(EV_STOP_LISTEN);
        break;
    case ARQ_CMD_CONNECT:
        arq_lock();
        snprintf(arq_conn.src_addr, sizeof(arq_conn.src_addr), "%s", cmd->arg0);
        snprintf(arq_conn.dst_addr, sizeof(arq_conn.dst_addr), "%s", cmd->arg1);
        arq_unlock();
        arq_event_loop_enqueue_fsm(EV_LINK_CALL_REMOTE);
        break;
    case ARQ_CMD_DISCONNECT:
        arq_event_loop_enqueue_fsm(EV_LINK_DISCONNECT);
        break;
    case ARQ_CMD_SET_CALLSIGN:
        arq_lock();
        snprintf(arq_conn.my_call_sign, sizeof(arq_conn.my_call_sign), "%s", cmd->arg0);
        arq_unlock();
        break;
    case ARQ_CMD_SET_PUBLIC:
        arq_lock();
        arq_conn.encryption = cmd->flag ? false : true;
        arq_unlock();
        break;
    case ARQ_CMD_SET_BANDWIDTH:
        arq_lock();
        arq_conn.bw = cmd->value;
        arq_unlock();
        break;
    case ARQ_CMD_NONE:
    default:
        break;
    }
}

static void *arq_cmd_bridge_worker(void *arg)
{
    arq_cmd_msg_t msg;

    (void)arg;
    while (arq_channel_bus_recv_cmd(&arq_bus, &msg) == 0)
        arq_handle_tcp_cmd_msg(&msg);

    return NULL;
}

static void *arq_payload_bridge_worker(void *arg)
{
    arq_bytes_msg_t payload;

    (void)arg;
    while (arq_channel_bus_recv_payload(&arq_bus, &payload) == 0)
    {
        if (payload.len > 0 && payload.len <= INT_BUFFER_SIZE)
            arq_event_loop_enqueue_app_tx(payload.data, payload.len, NULL);
    }

    return NULL;
}

int arq_submit_tcp_cmd(const arq_cmd_msg_t *cmd)
{
    if (!cmd || !arq_fsm_lock_ready)
        return -1;

    return arq_channel_bus_try_send_cmd(&arq_bus, cmd);
}

int arq_submit_tcp_payload(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > INT_BUFFER_SIZE || !arq_fsm_lock_ready)
        return -1;

    return arq_channel_bus_try_send_payload(&arq_bus, data, len);
}

int arq_get_tx_backlog_bytes(void)
{
    size_t pending = 0;

    if (!arq_fsm_lock_ready)
        return 0;

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

    if (!arq_fsm_lock_ready)
        return 0;

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

    if (!arq_fsm_lock_ready)
        return FREEDV_MODE_DATAC4;

    arq_lock();
    mode = is_payload_mode(arq_ctx.payload_mode) ? arq_ctx.payload_mode : FREEDV_MODE_DATAC4;
    arq_unlock();
    return mode;
}

int arq_get_control_mode(void)
{
    int mode;

    if (!arq_fsm_lock_ready)
        return FREEDV_MODE_DATAC13;

    arq_lock();
    mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    arq_unlock();
    return mode;
}

static int preferred_rx_mode_locked(time_t now)
{
    int mode = is_payload_mode(arq_ctx.payload_mode) ? arq_ctx.payload_mode : FREEDV_MODE_DATAC4;
    bool control_phase;

    if (!arq_ctx.initialized)
        return mode;

    if (arq_ctx.turn_role == ARQ_TURN_IRS &&
        arq_ctx.peer_backlog_nonzero &&
        !arq_ctx.payload_start_pending &&
        arq_ctx.mode_fsm == ARQ_MODE_FSM_IDLE &&
        arq_ctx.last_peer_payload_rx > 0 &&
        (now - arq_ctx.last_peer_payload_rx) > peer_payload_hold_s_locked())
    {
        arq_ctx.peer_backlog_nonzero = false;
        HLOGD("arq", "Peer backlog timeout -> control");
    }

    control_phase =
        arq_fsm.current == state_listen ||
        arq_fsm.current == state_calling_wait_accept ||
        arq_fsm.current == state_turn_negotiating ||
        arq_ctx.pending_call ||
        arq_ctx.pending_accept ||
        arq_ctx.pending_ack ||
        arq_ctx.pending_keepalive ||
        arq_ctx.pending_keepalive_ack ||
        arq_ctx.pending_turn_req ||
        arq_ctx.turn_req_in_flight ||
        arq_ctx.pending_turn_ack ||
        arq_ctx.pending_flow_hint ||
        arq_ctx.mode_fsm != ARQ_MODE_FSM_IDLE;

    if (is_connected_state_locked())
    {
        if (control_phase)
        {
            mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
        }
        else if (arq_ctx.turn_role == ARQ_TURN_ISS)
        {
            if (!arq_ctx.waiting_ack && arq_ctx.app_tx_len > 0)
                mode = is_payload_mode(arq_ctx.payload_mode) ? arq_ctx.payload_mode : FREEDV_MODE_DATAC4;
            else
                mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
        }
        else if (arq_ctx.turn_role == ARQ_TURN_IRS &&
                 (arq_ctx.payload_start_pending || arq_ctx.peer_backlog_nonzero))
        {
            mode = is_payload_mode(arq_ctx.payload_mode) ? arq_ctx.payload_mode : FREEDV_MODE_DATAC4;
        }
        else
        {
            mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
        }
    }
    else if (control_phase)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    return mode;
}

static int preferred_tx_mode_locked(time_t now)
{
    int mode = is_payload_mode(arq_ctx.payload_mode) ? arq_ctx.payload_mode : arq_conn.mode;
    bool must_control_tx;

    if (!arq_ctx.initialized)
        return mode;

    must_control_tx =
        arq_fsm.current == state_listen ||
        arq_fsm.current == state_calling_wait_accept ||
        arq_fsm.current == state_turn_negotiating ||
        arq_ctx.pending_call ||
        arq_ctx.pending_accept ||
        arq_ctx.pending_ack ||
        arq_ctx.pending_keepalive ||
        arq_ctx.pending_keepalive_ack ||
        arq_ctx.pending_turn_req ||
        arq_ctx.turn_req_in_flight ||
        arq_ctx.pending_turn_ack ||
        arq_ctx.pending_flow_hint ||
        arq_ctx.mode_fsm != ARQ_MODE_FSM_IDLE;

    if (is_connected_state_locked())
    {
        if (arq_ctx.turn_role == ARQ_TURN_ISS && !must_control_tx)
        {
            if (arq_ctx.waiting_ack && arq_ctx.pending_payload_actions > 0)
                mode = arq_ctx.payload_mode ? arq_ctx.payload_mode : arq_conn.mode;
            else if (arq_ctx.waiting_ack && now < arq_ctx.ack_deadline)
                mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
            else if (arq_ctx.waiting_ack || arq_ctx.app_tx_len > 0)
                mode = arq_ctx.payload_mode ? arq_ctx.payload_mode : arq_conn.mode;
            else
                mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
        }
        else
        {
            mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
        }
    }
    else if (must_control_tx)
    {
        mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    }
    return mode;
}

int arq_get_preferred_rx_mode(void)
{
    int mode;
    time_t now = time(NULL);

    if (!arq_fsm_lock_ready)
        return FREEDV_MODE_DATAC4;

    arq_lock();
    mode = preferred_rx_mode_locked(now);
    arq_unlock();
    return mode;
}

int arq_get_preferred_tx_mode(void)
{
    int mode;
    time_t now = time(NULL);

    if (!arq_fsm_lock_ready)
        return FREEDV_MODE_DATAC4;

    arq_lock();
    mode = preferred_tx_mode_locked(now);
    arq_unlock();
    return mode;
}

bool arq_v2_get_runtime_snapshot(arq_runtime_snapshot_t *snapshot)
{
    size_t pending = 0;
    int gear = 0;
    time_t now = time(NULL);

    if (!snapshot)
        return false;
    if (!arq_fsm_lock_ready)
        return false;

    memset(snapshot, 0, sizeof(*snapshot));

    arq_lock();
    snapshot->initialized = arq_ctx.initialized;
    snapshot->connected = arq_ctx.initialized && is_connected_state_locked();
    snapshot->trx = arq_conn.TRX;

    if (arq_ctx.initialized)
    {
        pending = arq_ctx.app_tx_len;
        gear = arq_ctx.gear;
    }

    if (pending > (size_t)INT_MAX)
        snapshot->tx_backlog_bytes = INT_MAX;
    else
        snapshot->tx_backlog_bytes = (int)pending;

    if (gear < 0)
        gear = 0;
    snapshot->speed_level = gear;
    snapshot->payload_mode = is_payload_mode(arq_ctx.payload_mode) ? arq_ctx.payload_mode : FREEDV_MODE_DATAC4;
    snapshot->control_mode = arq_ctx.control_mode ? arq_ctx.control_mode : FREEDV_MODE_DATAC13;
    snapshot->preferred_rx_mode = preferred_rx_mode_locked(now);
    snapshot->preferred_tx_mode = preferred_tx_mode_locked(now);
    arq_unlock();

    return snapshot->initialized;
}

static void arq_set_active_modem_mode_locked(int mode, size_t frame_size)
{
    arq_conn.mode = mode;
    arq_conn.frame_size = frame_size;
    if (mode == FREEDV_MODE_DATAC1 ||
        mode == FREEDV_MODE_DATAC3 ||
        mode == FREEDV_MODE_DATAC4)
    {
        arq_ctx.max_gear = max_gear_for_frame_size(frame_size);
        if (arq_ctx.gear > arq_ctx.max_gear)
            arq_ctx.gear = arq_ctx.max_gear;
    }
}

void arq_set_active_modem_mode(int mode, size_t frame_size)
{
    if (!arq_fsm_lock_ready || frame_size == 0 || frame_size > INT_BUFFER_SIZE)
        return;

    if (arq_event_loop.started)
    {
        arq_event_loop_enqueue_active_mode(mode, frame_size);
        return;
    }

    arq_lock();
    arq_set_active_modem_mode_locked(mode, frame_size);
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
        if (arq_fsm.current != state_listen && !is_connected_state_locked())
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
        schedule_immediate_control_tx_locked(now, "call");
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
        if (!is_connected_state_locked() || !arq_ctx.waiting_ack)
        {
            HLOGD("arq", "ACK drop: not waiting (connected=%d waiting=%d)",
                  is_connected_state_locked() ? 1 : 0,
                  arq_ctx.waiting_ack ? 1 : 0);
            return;
        }
        if (session_id != arq_ctx.session_id)
        {
            HLOGD("arq", "ACK drop: session mismatch got=%u expect=%u",
                  session_id, arq_ctx.session_id);
            return;
        }
        if (ack != arq_ctx.outstanding_seq)
        {
            HLOGD("arq", "ACK drop: seq mismatch got=%u expect=%u",
                  ack, arq_ctx.outstanding_seq);
            return;
        }

        HLOGD("arq", "ACK rx seq=%u", ack);
        arq_ctx.waiting_ack = false;
        apply_deferred_payload_mode_locked();
        if (arq_ctx.payload_start_pending)
        {
            if (arq_ctx.startup_acks_left > 0)
                arq_ctx.startup_acks_left--;
            if (arq_ctx.startup_acks_left <= 0)
            {
                arq_ctx.payload_start_pending = false;
                arq_ctx.startup_deadline = 0;
                HLOGD("arq", "Startup gate end (ack streak)");
                schedule_flow_hint_locked();
            }
        }
        if (arq_ctx.outstanding_app_len <= arq_ctx.app_tx_len)
        {
            memmove(arq_ctx.app_tx_queue,
                    arq_ctx.app_tx_queue + arq_ctx.outstanding_app_len,
                    arq_ctx.app_tx_len - arq_ctx.outstanding_app_len);
            arq_ctx.app_tx_len -= arq_ctx.outstanding_app_len;
        }
        arq_ctx.outstanding_app_len = 0;
        arq_ctx.outstanding_frame_len = 0;
        if (arq_ctx.payload_start_pending &&
            arq_ctx.turn_role == ARQ_TURN_ISS &&
            arq_ctx.app_tx_len == 0 &&
            !arq_ctx.waiting_ack)
        {
            arq_ctx.payload_start_pending = false;
            arq_ctx.startup_acks_left = 0;
            arq_ctx.startup_deadline = 0;
            HLOGD("arq", "Startup gate end (iss drained)");
            schedule_flow_hint_locked();
        }
        if (arq_ctx.turn_ack_deferred)
        {
            arq_ctx.turn_ack_deferred = false;
            if (arq_ctx.peer_backlog_nonzero)
                become_irs_locked("turn ack deferred");
            else
                update_connected_state_from_turn_locked();
        }
        mark_link_activity_locked(now);
        mark_success_locked();
        schedule_flow_hint_locked();
        update_payload_mode_locked();
        return;

    case ARQ_SUBTYPE_MODE_REQ:
        if (!is_connected_state_locked())
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (payload_len < 1 || !is_payload_mode(payload[0]))
            return;
        if (arq_ctx.turn_role == ARQ_TURN_IRS)
        {
            arq_ctx.peer_backlog_nonzero = true;
            arq_ctx.last_peer_payload_rx = now;
        }
        mode_fsm_queue_ack_locked(payload[0], "peer req");
        schedule_immediate_control_tx_locked(now, "mode req");
        arq_ctx.mode_candidate_hits = 0;
        mark_link_activity_locked(now);
        return;

    case ARQ_SUBTYPE_MODE_ACK:
        if (!is_connected_state_locked())
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (arq_ctx.mode_fsm != ARQ_MODE_FSM_REQ_IN_FLIGHT)
            return;
        if (payload_len < 1 || !is_payload_mode(payload[0]))
            return;
        if (payload[0] != arq_ctx.mode_req_mode)
            return;
        mode_fsm_reset_locked("peer ack");
        arq_ctx.mode_candidate_hits = 0;
        request_payload_mode_locked(payload[0], "peer ack");
        mark_link_activity_locked(now);
        return;

    case ARQ_SUBTYPE_TURN_REQ:
        if (!is_connected_state_locked())
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (arq_ctx.turn_role == ARQ_TURN_IRS)
        {
            arq_ctx.peer_backlog_nonzero = false;
            arq_ctx.last_peer_payload_rx = 0;
            arq_ctx.pending_turn_ack = true;
            arq_ctx.turn_promote_after_ack = arq_ctx.app_tx_len > 0;
            arq_fsm.current = state_turn_negotiating;
            schedule_immediate_control_tx_locked(now, "turn req");
        }
        mark_link_activity_locked(now);
        return;

    case ARQ_SUBTYPE_TURN_ACK:
        if (!is_connected_state_locked())
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (!arq_ctx.turn_req_in_flight)
            return;
        arq_ctx.turn_req_in_flight = false;
        arq_ctx.turn_req_retries_left = 0;
        arq_ctx.peer_backlog_nonzero = payload_len > 0 && payload[0] != 0;
        arq_ctx.last_peer_payload_rx = arq_ctx.peer_backlog_nonzero ? now : 0;
        if (arq_ctx.waiting_ack || arq_ctx.outstanding_frame_len > 0)
        {
            arq_ctx.turn_ack_deferred = true;
            HLOGD("arq", "Turn ACK defer role switch (waiting local ACK)");
            mark_link_activity_locked(now);
            return;
        }
        arq_ctx.turn_ack_deferred = false;
        if (arq_ctx.peer_backlog_nonzero)
            become_irs_locked("turn ack");
        else
            update_connected_state_from_turn_locked();
        mark_link_activity_locked(now);
        return;

    case ARQ_SUBTYPE_FLOW_HINT:
        if (!is_connected_state_locked())
            return;
        if (session_id != arq_ctx.session_id)
            return;
        if (payload_len > 0)
        {
            arq_ctx.peer_backlog_nonzero = payload[0] != 0;
            arq_ctx.last_peer_payload_rx = arq_ctx.peer_backlog_nonzero ? now : 0;
        }
        mark_link_activity_locked(now);
        return;

    case ARQ_SUBTYPE_DISCONNECT:
        if (session_id != arq_ctx.session_id)
            return;
        if (is_connected_state_locked() ||
            arq_fsm.current == state_calling_wait_accept ||
            arq_fsm.current == state_disconnecting)
            finalize_disconnect_locked();
        return;

    case ARQ_SUBTYPE_KEEPALIVE:
        if (!is_connected_state_locked())
            return;
        if (session_id != arq_ctx.session_id)
            return;
        mark_link_activity_locked(now);
        arq_ctx.pending_keepalive_ack = true;
        schedule_immediate_control_tx_locked(now, "keepalive");
        return;

    case ARQ_SUBTYPE_KEEPALIVE_ACK:
        if (!is_connected_state_locked())
            return;
        if (session_id != arq_ctx.session_id)
            return;
        mark_link_activity_locked(now);
        HLOGD("arq", "Keepalive ACK rx");
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

    if (!is_connected_state_locked())
        return;
    if (session_id != arq_ctx.session_id)
        return;

    if (seq == arq_ctx.rx_expected_seq)
    {
        mark_link_activity_locked(now);
        arq_ctx.peer_backlog_nonzero = true;
        arq_ctx.last_peer_payload_rx = now;
        write_buffer(data_rx_buffer_arq, (uint8_t *)payload, payload_len);
        arq_ctx.rx_expected_seq++;
        if (arq_ctx.payload_start_pending)
        {
            if (arq_ctx.startup_acks_left > 0)
                arq_ctx.startup_acks_left--;
            if (arq_ctx.startup_acks_left <= 0)
            {
                arq_ctx.payload_start_pending = false;
                arq_ctx.startup_deadline = 0;
                HLOGD("arq", "Startup gate end (data streak)");
                schedule_flow_hint_locked();
            }
        }
        arq_ctx.pending_ack = true;
        arq_ctx.pending_ack_seq = seq;
        arq_ctx.pending_ack_set_ms = arq_monotonic_ms();
        schedule_immediate_control_tx_with_guard_locked(now, "data ack",
                                                        ARQ_ACK_REPLY_EXTRA_GUARD_MS);
        mark_success_locked();
        schedule_flow_hint_locked();
        update_payload_mode_locked();
        return;
    }

    if ((uint8_t)(arq_ctx.rx_expected_seq - 1) == seq)
    {
        mark_link_activity_locked(now);
        arq_ctx.peer_backlog_nonzero = true;
        arq_ctx.last_peer_payload_rx = now;
        arq_ctx.pending_ack = true;
        arq_ctx.pending_ack_seq = seq;
        arq_ctx.pending_ack_set_ms = arq_monotonic_ms();
        schedule_immediate_control_tx_with_guard_locked(now, "dup ack",
                                                        ARQ_ACK_REPLY_EXTRA_GUARD_MS);
    }
}

static bool arq_handle_incoming_connect_frame_locked(const uint8_t *data, size_t frame_size)
{
    uint8_t meta;
    uint8_t session_id;
    uint8_t subtype;
    char decoded[(CALLSIGN_MAX_SIZE * 2) + 2] = {0};
    char dst[CALLSIGN_MAX_SIZE] = {0};
    char src[CALLSIGN_MAX_SIZE] = {0};
    time_t now;

    if (!data || frame_size != ARQ_CONTROL_FRAME_SIZE)
        return false;

    meta = data[ARQ_CONNECT_SESSION_IDX];
    subtype = connect_meta_is_accept(meta) ? ARQ_SUBTYPE_ACCEPT : ARQ_SUBTYPE_CALL;
    session_id = connect_meta_session(meta);
    if (!decode_connect_callsign_payload(data + ARQ_CONNECT_PAYLOAD_IDX, decoded))
        return false;

    now = time(NULL);
    if (subtype == ARQ_SUBTYPE_CALL)
    {
        split_call_connect_payload(decoded, dst, src);

        if (dst[0] != 0 &&
            arq_conn.my_call_sign[0] != 0 &&
            strncasecmp(arq_conn.my_call_sign, dst, strnlen(dst, CALLSIGN_MAX_SIZE - 1)) == 0 &&
            (arq_fsm.current == state_listen || is_connected_state_locked()))
        {
            strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
            arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
            if (src[0] != 0)
            {
                strncpy(arq_conn.dst_addr, src, CALLSIGN_MAX_SIZE - 1);
                arq_conn.dst_addr[CALLSIGN_MAX_SIZE - 1] = 0;
            }

            arq_ctx.role = ARQ_ROLE_CALLEE;
            arq_ctx.session_id = session_id;
            arq_ctx.tx_seq = 0;
            arq_ctx.rx_expected_seq = 0;
            arq_ctx.waiting_ack = false;
            arq_ctx.outstanding_frame_len = 0;
            arq_ctx.outstanding_app_len = 0;
            arq_ctx.pending_accept = true;
            arq_ctx.accept_retries_left = 1;
            arq_ctx.turn_role = ARQ_TURN_NONE;
            arq_ctx.pending_turn_req = false;
            arq_ctx.turn_req_in_flight = false;
            arq_ctx.pending_turn_ack = false;
            arq_ctx.turn_promote_after_ack = false;
            arq_ctx.turn_ack_deferred = false;
            arq_ctx.peer_backlog_nonzero = false;
            arq_ctx.last_peer_payload_rx = 0;
            schedule_immediate_control_tx_locked(now, "connect call");
        }
        return true;
    }

    if (arq_fsm.current == state_calling_wait_accept &&
        arq_ctx.role == ARQ_ROLE_CALLER &&
        session_id == connect_meta_session(arq_ctx.session_id) &&
        decoded[0] != 0 &&
        strncasecmp(arq_conn.dst_addr, decoded, strnlen(decoded, CALLSIGN_MAX_SIZE - 1)) == 0)
    {
        enter_connected_locked();
        mark_link_activity_locked(now);
        mark_success_locked();
    }
    return true;
}

bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size)
{
    if (!data || frame_size != ARQ_CONTROL_FRAME_SIZE)
        return false;
    if (!arq_fsm_lock_ready)
        return false;

    if (!decode_connect_callsign_payload(data + ARQ_CONNECT_PAYLOAD_IDX,
                                         (char[(CALLSIGN_MAX_SIZE * 2) + 2]){0}))
        return false;

    if (arq_event_loop.started)
    {
        arq_event_loop_enqueue_connect_frame(data, frame_size);
        return true;
    }

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return true;
    }
    bool handled = arq_handle_incoming_connect_frame_locked(data, frame_size);
    arq_unlock();
    return handled;
}

static void arq_handle_incoming_frame_locked(const uint8_t *data, size_t frame_size)
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

    if (packet_type == PACKET_TYPE_ARQ_CONTROL)
        handle_control_frame_locked(subtype, session_id, ack, payload, payload_len);
    else if (packet_type == PACKET_TYPE_ARQ_DATA && subtype == ARQ_SUBTYPE_DATA)
        handle_data_frame_locked(session_id, seq, payload, payload_len);
}

void arq_handle_incoming_frame(uint8_t *data, size_t frame_size)
{
    if (!data || frame_size < HEADER_SIZE)
        return;
    if (!arq_fsm_lock_ready)
        return;

    if (arq_event_loop.started)
    {
        arq_event_loop_enqueue_frame(data, frame_size);
        return;
    }

    arq_lock();
    if (arq_ctx.initialized)
        arq_handle_incoming_frame_locked(data, frame_size);
    arq_unlock();
}

static void arq_update_link_metrics_locked(int sync, float snr, int rx_status, bool frame_decoded, time_t now)
{
    uint64_t now_ms = arq_realtime_ms();

    if (frame_decoded && snr > -20.0f && snr < 30.0f)
    {
        if (arq_ctx.snr_ema == 0.0f)
            arq_ctx.snr_ema = snr;
        else
            arq_ctx.snr_ema = (0.8f * arq_ctx.snr_ema) + (0.2f * snr);
    }

    if (!frame_decoded && (rx_status & 0x4))
        mark_failure_locked();

    if (sync || frame_decoded)
    {
        arq_ctx.last_phy_activity = now;
        uint64_t busy_until = now_ms + ARQ_CHANNEL_GUARD_MS;
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
}

void arq_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded)
{
    if (!arq_fsm_lock_ready)
        return;

    if (arq_event_loop.started)
    {
        arq_event_loop_enqueue_metrics(sync, snr, rx_status, frame_decoded);
        return;
    }

    time_t now = time(NULL);
    arq_lock();
    if (arq_ctx.initialized)
        arq_update_link_metrics_locked(sync, snr, rx_status, frame_decoded, now);
    arq_unlock();
}

bool arq_v2_try_dequeue_action(arq_action_t *action)
{
    bool ok;

    if (!arq_fsm_lock_ready || !action)
        return false;

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return false;
    }

    ok = arq_action_queue_pop_locked(action);
    arq_unlock();
    return ok;
}

bool arq_v2_wait_dequeue_action(arq_action_t *action, int timeout_ms)
{
    int rc = 0;
    bool ok = false;

    if (!arq_fsm_lock_ready || !action)
        return false;

    arq_lock();
    if (!arq_ctx.initialized)
    {
        arq_unlock();
        return false;
    }

    if (timeout_ms <= 0)
    {
        ok = arq_action_queue_pop_locked(action);
        arq_unlock();
        return ok;
    }

    if (arq_ctx.action_count == 0)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        while (arq_ctx.initialized && arq_ctx.action_count == 0 && rc == 0)
            rc = pthread_cond_timedwait(&arq_action_cond, &arq_fsm.lock, &ts);
    }

    if (arq_ctx.initialized && rc != ETIMEDOUT)
        ok = arq_action_queue_pop_locked(action);
    arq_unlock();
    return ok;
}

void clear_connection_data(void)
{
    if (!arq_fsm_lock_ready)
        return;

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
    if (!arq_fsm_lock_ready)
        return;

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
    if (!arq_fsm_lock_ready)
        return;

    arq_lock();
    if (arq_ctx.initialized && arq_conn.my_call_sign[0] != 0 && arq_conn.dst_addr[0] != 0)
    {
        arq_ctx.role = ARQ_ROLE_CALLEE;
        arq_ctx.pending_accept = true;
        arq_ctx.accept_retries_left = 1;
        arq_ctx.next_role_tx_at = arq_realtime_ms() + ARQ_CHANNEL_GUARD_MS;
    }
    arq_unlock();
}

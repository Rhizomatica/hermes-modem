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
#include <time.h>

#include "arq.h"
#include "framer.h"
#include "defines_modem.h"
#include "ring_buffer_posix.h"
#include "tcp_interfaces.h"

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_rx_buffer_arq;
extern bool shutdown_;

arq_info arq_conn;
fsm_handle arq_fsm;

enum {
    ARQ_CTRL_CALL = 1,
    ARQ_CTRL_ACCEPT = 2,
    ARQ_CTRL_DISCONNECT = 3
};

#define ARQ_CONNECT_TIMEOUT_S 15
#define ARQ_CALL_RETRY_TICKS 12
#define ARQ_CTRL_META_SIZE 3
#define ARQ_CTRL_PAYLOAD_OFFSET (HEADER_SIZE + ARQ_CTRL_META_SIZE)
#define ARQ_DATA_LEN_SIZE 2
#define ARQ_DATA_PAYLOAD_OFFSET (HEADER_SIZE + ARQ_DATA_LEN_SIZE)
#define ARQ_ACCEPT_RETRY_TICKS 4

static time_t connect_deadline;
static bool arq_initialized = false;
static int call_retry_ticks = 0;
static int accept_retry_ticks = 0;

static void state_no_connected_client(int event);
static void state_link_connected(int event);
void state_listen(int event);
void state_idle(int event);
void state_connecting_caller(int event);
void state_connecting_callee(int event);

static void clear_connect_deadline(void)
{
    connect_deadline = 0;
}

static void clear_accept_retries(void)
{
    accept_retry_ticks = 0;
}

static void clear_call_retries(void)
{
    call_retry_ticks = 0;
}

static void arm_call_retries(void)
{
    call_retry_ticks = ARQ_CALL_RETRY_TICKS;
}

static void arm_accept_retries(void)
{
    accept_retry_ticks = ARQ_ACCEPT_RETRY_TICKS;
}

static void set_connect_deadline(void)
{
    connect_deadline = time(NULL) + ARQ_CONNECT_TIMEOUT_S;
}

static bool arq_state_is(fsm_state state)
{
    bool is_state = false;

    if (!arq_initialized)
        return false;

    pthread_mutex_lock(&arq_fsm.lock);
    is_state = (arq_fsm.current == state);
    pthread_mutex_unlock(&arq_fsm.lock);

    return is_state;
}

static void go_to_idle_or_listen(void)
{
    arq_fsm.current = arq_conn.listen ? state_listen : state_idle;
}

static int queue_control_frame(uint8_t opcode, const char *src, const char *dst, int repeat)
{
    if (arq_conn.frame_size < ARQ_CTRL_PAYLOAD_OFFSET || repeat <= 0)
        return -1;

    size_t src_len = strnlen(src, CALLSIGN_MAX_SIZE - 1);
    size_t dst_len = strnlen(dst, CALLSIGN_MAX_SIZE - 1);
    size_t payload_len = ARQ_CTRL_PAYLOAD_OFFSET + src_len + dst_len;

    if (payload_len > arq_conn.frame_size)
        return -1;

    uint8_t *frame = (uint8_t *)malloc(arq_conn.frame_size);
    if (!frame)
        return -1;

    memset(frame, 0, arq_conn.frame_size);
    frame[1] = opcode;
    frame[2] = (uint8_t)src_len;
    frame[3] = (uint8_t)dst_len;
    memcpy(frame + ARQ_CTRL_PAYLOAD_OFFSET, src, src_len);
    memcpy(frame + ARQ_CTRL_PAYLOAD_OFFSET + src_len, dst, dst_len);
    write_frame_header(frame, PACKET_TYPE_ARQ_CONTROL, arq_conn.frame_size);

    for (int i = 0; i < repeat; i++)
    {
        if (write_buffer(data_tx_buffer_arq, frame, arq_conn.frame_size) < 0)
        {
            free(frame);
            return -1;
        }
    }

    free(frame);
    return 0;
}

static void queue_disconnect_control(void)
{
    if (arq_conn.my_call_sign[0] == 0 || arq_conn.dst_addr[0] == 0)
        return;

    queue_control_frame(ARQ_CTRL_DISCONNECT, arq_conn.my_call_sign, arq_conn.dst_addr, 1);
}

void call_remote()
{
    if (arq_conn.src_addr[0] == 0 && arq_conn.my_call_sign[0] != 0)
    {
        strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
        arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
    }

    if (arq_conn.src_addr[0] == 0 || arq_conn.dst_addr[0] == 0)
        return;

    int repeat = arq_conn.call_burst_size > 0 ? arq_conn.call_burst_size : 1;
    queue_control_frame(ARQ_CTRL_CALL, arq_conn.src_addr, arq_conn.dst_addr, repeat);
}

void callee_accept_connection()
{
    if (arq_conn.my_call_sign[0] == 0 || arq_conn.dst_addr[0] == 0)
        return;

    queue_control_frame(ARQ_CTRL_ACCEPT, arq_conn.my_call_sign, arq_conn.dst_addr, 1);
}

void clear_connection_data()
{
    size_t frame_size = arq_conn.frame_size;
    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_rx_buffer_arq);
    reset_arq_info(&arq_conn);
    arq_conn.frame_size = frame_size;
    clear_connect_deadline();
    clear_call_retries();
    clear_accept_retries();
}

void reset_arq_info(arq_info *arq_conn_i)
{
    arq_conn_i->TRX = RX;
    arq_conn_i->bw = 0;
    arq_conn_i->encryption = false;
    arq_conn_i->call_burst_size = CALL_BURST_SIZE;
    arq_conn_i->listen = false;
    arq_conn_i->my_call_sign[0] = 0;
    arq_conn_i->src_addr[0] = 0;
    arq_conn_i->dst_addr[0] = 0;
}

static void state_no_connected_client(int event)
{
    switch (event)
    {
    case EV_CLIENT_CONNECT:
        clear_connection_data();
        arq_fsm.current = state_idle;
        break;
    default:
        break;
    }
}

void state_idle(int event)
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
        call_remote();
        set_connect_deadline();
        arm_call_retries();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

void state_listen(int event)
{
    switch (event)
    {
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        arq_fsm.current = state_idle;
        break;
    case EV_LINK_CALL_REMOTE:
        call_remote();
        set_connect_deadline();
        arm_call_retries();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    case EV_LINK_INCOMING_CALL:
        callee_accept_connection();
        set_connect_deadline();
        arq_fsm.current = state_connecting_callee;
        break;
    default:
        break;
    }
}

void state_connecting_caller(int event)
{
    switch (event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_ESTABLISHED:
        clear_connect_deadline();
        clear_call_retries();
        clear_accept_retries();
        tnc_send_connected();
        arq_fsm.current = state_link_connected;
        break;
    case EV_LINK_ESTABLISHMENT_TIMEOUT:
        clear_connect_deadline();
        clear_call_retries();
        clear_accept_retries();
        tnc_send_disconnected();
        go_to_idle_or_listen();
        break;
    case EV_LINK_DISCONNECT:
        clear_connect_deadline();
        clear_call_retries();
        clear_accept_retries();
        queue_disconnect_control();
        tnc_send_disconnected();
        go_to_idle_or_listen();
        break;
    case EV_CLIENT_DISCONNECT:
        clear_call_retries();
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

void state_connecting_callee(int event)
{
    switch (event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_ESTABLISHED:
        clear_connect_deadline();
        clear_call_retries();
        tnc_send_connected();
        arq_fsm.current = state_link_connected;
        break;
    case EV_LINK_ESTABLISHMENT_TIMEOUT:
        clear_connect_deadline();
        clear_call_retries();
        clear_accept_retries();
        tnc_send_disconnected();
        go_to_idle_or_listen();
        break;
    case EV_LINK_DISCONNECT:
        clear_connect_deadline();
        clear_call_retries();
        clear_accept_retries();
        queue_disconnect_control();
        tnc_send_disconnected();
        go_to_idle_or_listen();
        break;
    case EV_CLIENT_DISCONNECT:
        clear_call_retries();
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

static void state_link_connected(int event)
{
    switch (event)
    {
    case EV_LINK_DISCONNECT:
        clear_call_retries();
        clear_accept_retries();
        queue_disconnect_control();
        tnc_send_disconnected();
        go_to_idle_or_listen();
        break;
    case EV_CLIENT_DISCONNECT:
        clear_call_retries();
        queue_disconnect_control();
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        break;
    }
}

int arq_init(size_t frame_size)
{
    if (frame_size <= ARQ_DATA_PAYLOAD_OFFSET)
        return EXIT_FAILURE;

    reset_arq_info(&arq_conn);
    arq_conn.frame_size = frame_size;
    clear_connect_deadline();
    clear_call_retries();
    clear_accept_retries();

    fsm_init(&arq_fsm, state_no_connected_client);
    arq_initialized = true;
    return EXIT_SUCCESS;
}

void arq_shutdown()
{
    if (!arq_initialized)
        return;

    clear_call_retries();
    clear_accept_retries();
    arq_initialized = false;
    fsm_destroy(&arq_fsm);
}

bool arq_is_link_connected(void)
{
    return arq_state_is(state_link_connected);
}

void arq_tick_1hz(void)
{
    if (!arq_initialized)
        return;

    if (connect_deadline == 0)
        goto maybe_retry_call;

    time_t now = time(NULL);
    if (now < connect_deadline)
        goto maybe_retry_call;

    if (arq_state_is(state_connecting_caller) || arq_state_is(state_connecting_callee))
        fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHMENT_TIMEOUT);

maybe_retry_call:
    if (call_retry_ticks > 0 && arq_state_is(state_connecting_caller))
    {
        call_remote();
        call_retry_ticks--;
    }

    if (accept_retry_ticks > 0 &&
        (arq_state_is(state_connecting_callee) || arq_state_is(state_link_connected)))
    {
        callee_accept_connection();
        accept_retry_ticks--;
    }
}

int arq_queue_data(const uint8_t *data, size_t len)
{
    if (!arq_initialized || !data || len == 0 || !arq_is_link_connected())
        return 0;

    if (arq_conn.frame_size <= ARQ_DATA_PAYLOAD_OFFSET)
        return -1;

    size_t max_chunk = arq_conn.frame_size - ARQ_DATA_PAYLOAD_OFFSET;
    uint8_t *frame = (uint8_t *)malloc(arq_conn.frame_size);
    if (!frame)
        return -1;

    size_t off = 0;
    while (off < len)
    {
        size_t chunk = len - off;
        if (chunk > max_chunk)
            chunk = max_chunk;

        memset(frame, 0, arq_conn.frame_size);
        frame[1] = (uint8_t)((chunk >> 8) & 0xff);
        frame[2] = (uint8_t)(chunk & 0xff);
        memcpy(frame + ARQ_DATA_PAYLOAD_OFFSET, data + off, chunk);
        write_frame_header(frame, PACKET_TYPE_ARQ_DATA, arq_conn.frame_size);

        if (write_buffer(data_tx_buffer_arq, frame, arq_conn.frame_size) < 0)
        {
            free(frame);
            return -1;
        }
        off += chunk;
    }

    free(frame);
    return (int)off;
}

static int decode_control_frame(const uint8_t *data,
                                size_t frame_size,
                                uint8_t *opcode,
                                char *src,
                                char *dst)
{
    if (frame_size < ARQ_CTRL_PAYLOAD_OFFSET)
        return -1;

    size_t src_len = data[2];
    size_t dst_len = data[3];
    size_t total_len = ARQ_CTRL_PAYLOAD_OFFSET + src_len + dst_len;

    if (src_len >= CALLSIGN_MAX_SIZE || dst_len >= CALLSIGN_MAX_SIZE || total_len > frame_size)
        return -1;

    *opcode = data[1];
    memcpy(src, data + ARQ_CTRL_PAYLOAD_OFFSET, src_len);
    src[src_len] = 0;
    memcpy(dst, data + ARQ_CTRL_PAYLOAD_OFFSET + src_len, dst_len);
    dst[dst_len] = 0;

    return 0;
}

static void handle_control_frame(uint8_t *data, size_t frame_size)
{
    uint8_t opcode = 0;
    char src[CALLSIGN_MAX_SIZE] = {0};
    char dst[CALLSIGN_MAX_SIZE] = {0};

    if (decode_control_frame(data, frame_size, &opcode, src, dst) < 0)
        return;

    switch (opcode)
    {
    case ARQ_CTRL_CALL:
        if (arq_conn.my_call_sign[0] == 0 ||
            strncmp(dst, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1) != 0)
        {
            return;
        }

        if (arq_state_is(state_link_connected) || arq_state_is(state_connecting_callee))
        {
            if (strncmp(src, arq_conn.dst_addr, CALLSIGN_MAX_SIZE - 1) == 0)
            {
                callee_accept_connection();
                arm_accept_retries();
            }
            return;
        }

        if (!arq_state_is(state_listen))
            return;

        strncpy(arq_conn.src_addr, arq_conn.my_call_sign, CALLSIGN_MAX_SIZE - 1);
        arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
        strncpy(arq_conn.dst_addr, src, CALLSIGN_MAX_SIZE - 1);
        arq_conn.dst_addr[CALLSIGN_MAX_SIZE - 1] = 0;

        fsm_dispatch(&arq_fsm, EV_LINK_INCOMING_CALL);
        fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHED);
        arm_accept_retries();
        return;

    case ARQ_CTRL_ACCEPT:
        if (!arq_state_is(state_connecting_caller))
            return;

        if (strncmp(src, arq_conn.dst_addr, CALLSIGN_MAX_SIZE - 1) != 0)
            return;

        fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHED);
        return;

    case ARQ_CTRL_DISCONNECT:
        if (arq_state_is(state_link_connected) ||
            arq_state_is(state_connecting_caller) ||
            arq_state_is(state_connecting_callee))
        {
            fsm_dispatch(&arq_fsm, EV_LINK_DISCONNECT);
        }
        return;

    default:
        return;
    }
}

void arq_handle_incoming_frame(uint8_t *data, size_t frame_size)
{
    if (!arq_initialized || !data || frame_size < HEADER_SIZE)
        return;

    uint8_t packet_type = (data[0] >> 6) & 0x3;

    if (packet_type == PACKET_TYPE_ARQ_CONTROL)
    {
        handle_control_frame(data, frame_size);
        return;
    }

    if (packet_type != PACKET_TYPE_ARQ_DATA || !arq_is_link_connected())
        return;

    if (frame_size <= ARQ_DATA_PAYLOAD_OFFSET)
        return;

    size_t payload_len = ((size_t)data[1] << 8) | data[2];
    size_t payload_cap = frame_size - ARQ_DATA_PAYLOAD_OFFSET;
    if (payload_len == 0 || payload_len > payload_cap)
        return;

    write_buffer(data_rx_buffer_arq, data + ARQ_DATA_PAYLOAD_OFFSET, payload_len);
    clear_accept_retries();
}

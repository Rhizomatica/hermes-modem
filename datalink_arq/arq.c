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

#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "arq.h"
#include "fsm.h"
#include "../audioio/audioio.h"
#include "../data_interfaces/net.h"
#include "../common/defines_modem.h"
#include "../common/crc6.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../modem/framer.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;
extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_rx_buffer_arq;
extern bool shutdown_;

// ARQ connection state
arq_info arq_conn;
fsm_handle arq_fsm;
static pthread_t tid[2];

static void arq_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ARQ] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static const char *packet_type_to_str(uint8_t type)
{
    switch (type)
    {
    case PACKET_ARQ_CONTROL: return "CONTROL";
    case PACKET_ARQ_DATA:    return "DATA";
    case PACKET_ARQ_ACK:     return "ACK";
    case PACKET_ARQ_NACK:    return "NACK";
    default:                 return "UNKNOWN";
    }
}

static const char *fsm_state_to_str(fsm_state state)
{
    if (state == state_no_connected_client) return "no_client";
    if (state == state_idle) return "idle";
    if (state == state_listen) return "listen";
    if (state == state_connecting_caller) return "connecting_caller";
    if (state == state_connecting_callee) return "connecting_callee";
    if (state == state_link_connected) return "link_connected";
    return "unknown";
}

static inline const char *current_state_name(void)
{
    return fsm_state_to_str(arq_fsm.current);
}

static void arq_enqueue_frame(const uint8_t *src, size_t len)
{
    size_t frame_size = arq_conn.frame_size;
    if (frame_size > INT_BUFFER_SIZE)
        frame_size = INT_BUFFER_SIZE;
    uint8_t padded[INT_BUFFER_SIZE];
    memset(padded, 0, frame_size);
    if (len > frame_size)
        len = frame_size;
    memcpy(padded, src, len);
    uint8_t header = padded[0] & 0xC0;
    uint8_t crc = (uint8_t)crc6_0X6F(1, padded + HEADER_SIZE, frame_size - HEADER_SIZE);
    padded[0] = header | (crc & 0x3F);
    write_buffer(data_tx_buffer_arq, padded, frame_size);
}

/* FSM States */

void state_no_connected_client(int event)
{
    arq_log("state=no_connected_client event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_CLIENT_CONNECT:
        clear_connection_data();
        arq_fsm.current = state_idle;
        break;
    default:
        printf("Event: %d ignored in state_no_connected_client().\n", event);
    }
    return;
}

void state_link_connected(int event)
{
    arq_log("state=link_connected event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_CLIENT_DISCONNECT:
        arq_fsm.current = state_no_connected_client;
        break;

    case EV_LINK_DISCONNECT:
        arq_fsm.current = (arq_conn.listen == true)? state_listen : state_idle;
        break;
    default:
        printf("Event: %d ignored in state_link_connected().\n", event);
    }
    return;
}

void state_listen(int event)
{
    arq_log("state=listen event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_START_LISTEN:
        printf("EV_START_LISTEN ignored in state_listen() - already listening.\n");
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        arq_fsm.current = state_idle;
        break;
    case EV_LINK_CALL_REMOTE:
        call_remote();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_LINK_DISCONNECT:
        printf("EV_LINK_DISCONNECT ignored in state_listen() - not connected.\n");
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    case EV_LINK_INCOMING_CALL:
        callee_accept_connection();
        arq_fsm.current = state_connecting_callee;
        break;
    default:
        printf("Event: %d ignored in state_listen().\n", event);
    }

    return;
}

void state_idle(int event)
{
    arq_log("state=idle event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        arq_fsm.current = state_listen;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        printf("EV_STOP_LISTEN ignored in state_idle() - already stopped.\n");
        break;
    case EV_LINK_CALL_REMOTE:
        call_remote();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_LINK_DISCONNECT:
        printf("EV_LINK_DISCONNECT ignored in state_idle() - not connected.\n");
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        printf("Event: %d ignored from state_idle\n", event);
    }

    return;
}

void state_connecting_caller(int event)
{
    arq_log("state=connecting_caller event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_CALL_REMOTE:
        printf("EV_LINK_CALL_REMOTE ignored in state_connecting_caller() - already connecting.\n");
        break;
    case EV_LINK_DISCONNECT:
        arq_conn.retry_count = 0;
        arq_fsm.current = (arq_conn.listen == true)? state_listen : state_idle;
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    case EV_LINK_ESTABLISHED:
        tnc_send_connected();
        arq_conn.retry_count = 0;
        arq_fsm.current = state_link_connected;
        break;
    case EV_LINK_ESTABLISHMENT_TIMEOUT:
        if (arq_conn.retry_count < MAX_RETRIES)
        {
            arq_conn.retry_count++;
            call_remote(); // Retry
        }
        else
        {
            printf("Connection failed after %d retries.\n", MAX_RETRIES);
            arq_fsm.current = (arq_conn.listen == true)? state_listen : state_idle;
        }
        break;
    default:
        printf("Event: %d ignored from state_connecting_caller\n", event);
    }

    return;
}

void state_connecting_callee(int event)
{
    arq_log("state=connecting_callee event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_CALL_REMOTE:
        printf("EV_LINK_CALL_REMOTE ignored in state_connecting_callee() - already connecting.\n");
        break;
    case EV_LINK_DISCONNECT:
        arq_conn.retry_count = 0;
        arq_fsm.current = (arq_conn.listen == true)? state_listen : state_idle;
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    case EV_LINK_ESTABLISHMENT_TIMEOUT:
        arq_fsm.current = (arq_conn.listen == true)? state_listen : state_idle;
        break;
    case EV_LINK_ESTABLISHED:
        tnc_send_connected();
        arq_conn.retry_count = 0;
        arq_fsm.current = state_link_connected;
        break;
    default:
        printf("Event: %d ignored from state_connecting_callee\n", event);
    }

    return;
}

/* ---- END OF FSM Definitions ---- */

// Get current timestamp in milliseconds
uint32_t get_timestamp_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Check CRC6 of a frame
bool check_crc(uint8_t *data, size_t frame_size)
{
    if (frame_size < HEADER_SIZE)
        return false;

    uint16_t crc = (uint16_t)(data[0] & 0x3f);
    uint16_t calculated_crc = crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE);

    return (crc == calculated_crc);
}

// Parse ARQ frame and extract components
int parse_arq_frame(uint8_t *data, size_t data_len, uint8_t *packet_type, uint8_t *subtype,
                    uint8_t *sequence, uint8_t **payload, size_t *payload_len)
{
    if (data_len < HEADER_SIZE)
        return -1;

    *packet_type = (data[0] >> 6) & 0x3;

    if (!check_crc(data, data_len))
        return -1;

    *payload = data + HEADER_SIZE;
    *payload_len = data_len - HEADER_SIZE;

    // Extract sequence number and subtype for data/control packets
    if (*packet_type == PACKET_ARQ_DATA || *packet_type == PACKET_ARQ_CONTROL)
    {
        if (*payload_len < 1)
            return -1;
        *sequence = (*payload)[0] & 0x7F; // 7-bit sequence number
        if (*packet_type == PACKET_ARQ_CONTROL && *payload_len >= 2)
        {
            *subtype = (*payload)[1];
        }
    }
    else if (*packet_type == PACKET_ARQ_ACK || *packet_type == PACKET_ARQ_NACK)
    {
        if (*payload_len < 1)
            return -1;
        *sequence = (*payload)[0] & 0x7F;
    }

    return 0;
}

// Create a control frame
int create_control_frame(uint8_t *buffer, size_t buffer_size, uint8_t subtype, const char *callsign_data)
{
    if (buffer_size < HEADER_SIZE + 2)
        return -1;

    memset(buffer, 0, buffer_size);
    buffer[0] = (PACKET_ARQ_CONTROL << 6) & 0xff;

    // Encode callsign data using arithmetic encoding
    uint8_t encoded_callsign[INT_BUFFER_SIZE];
    int enc_len = arithmetic_encode(callsign_data, encoded_callsign);

    size_t available_payload = buffer_size - HEADER_SIZE - 2; // -2 for sequence and subtype
    if (enc_len > (int)available_payload)
    {
        arq_log("control_frame truncated payload (%zu bytes requested, %d available)", available_payload, enc_len);
        enc_len = available_payload;
    }

    // Sequence number (not used for control, set to 0)
    buffer[HEADER_SIZE] = 0;
    buffer[HEADER_SIZE + 1] = subtype;
    memcpy(buffer + HEADER_SIZE + 2, encoded_callsign, enc_len);

    size_t frame_size = HEADER_SIZE + 2 + enc_len;
    buffer[0] |= (uint8_t)crc6_0X6F(1, buffer + HEADER_SIZE, frame_size - HEADER_SIZE);

    arq_log("control_frame subtype=%u payload=%d bytes", subtype, (int)enc_len);
    return (int)frame_size;
}

// Create a data frame
int create_data_frame(uint8_t *buffer, size_t buffer_size, uint8_t *payload, size_t payload_len, uint8_t sequence)
{
    size_t frame_size = HEADER_SIZE + 1 + payload_len; // +1 for sequence number

    if (buffer_size < frame_size)
        return -1;

    memset(buffer, 0, frame_size);
    buffer[0] = (PACKET_ARQ_DATA << 6) & 0xff;
    buffer[HEADER_SIZE] = sequence & 0x7F;
    memcpy(buffer + HEADER_SIZE + 1, payload, payload_len);

    buffer[0] |= (uint8_t)crc6_0X6F(1, buffer + HEADER_SIZE, frame_size - HEADER_SIZE);

    return frame_size;
}

// Create an ACK frame
int create_ack_frame(uint8_t *buffer, size_t buffer_size, uint8_t sequence)
{
    size_t frame_size = HEADER_SIZE + 1;

    if (buffer_size < frame_size)
        return -1;

    memset(buffer, 0, frame_size);
    buffer[0] = (PACKET_ARQ_ACK << 6) & 0xff;
    buffer[HEADER_SIZE] = sequence & 0x7F;

    buffer[0] |= (uint8_t)crc6_0X6F(1, buffer + HEADER_SIZE, frame_size - HEADER_SIZE);

    return frame_size;
}

// Create a NACK frame
int create_nack_frame(uint8_t *buffer, size_t buffer_size, uint8_t sequence)
{
    size_t frame_size = HEADER_SIZE + 1;

    if (buffer_size < frame_size)
        return -1;

    memset(buffer, 0, frame_size);
    buffer[0] = (PACKET_ARQ_NACK << 6) & 0xff;
    buffer[HEADER_SIZE] = sequence & 0x7F;

    buffer[0] |= (uint8_t)crc6_0X6F(1, buffer + HEADER_SIZE, frame_size - HEADER_SIZE);

    return frame_size;
}

int check_for_incoming_connection(uint8_t *data, size_t data_len)
{
    char callsigns[CALLSIGN_MAX_SIZE * 2];
    char dst_callsign[CALLSIGN_MAX_SIZE] = { 0 };
    char src_callsign[CALLSIGN_MAX_SIZE] = { 0 };

    uint8_t packet_type, subtype, sequence;
    uint8_t *payload;
    size_t payload_len;

    if (parse_arq_frame(data, data_len, &packet_type, &subtype, &sequence, &payload, &payload_len) < 0)
    {
        printf("Failed to parse ARQ frame.\n");
        return -1;
    }

    if (packet_type != PACKET_ARQ_CONTROL || subtype != CONTROL_CALL_REQUEST)
    {
        return 1; // Not a connection request
    }

    if (arithmetic_decode(payload + 2, payload_len - 2, callsigns) < 0)
    {
      arq_log("incoming connection truncated callsigns");
        return -1;
    }

      arq_log("incoming connection decoded callsigns: %s", callsigns);

    char *needle;
    if ((needle = strstr(callsigns, "|")))
    {
        int i = 0;
        while (callsigns[i] != '|' && i < CALLSIGN_MAX_SIZE - 1)
        {
            dst_callsign[i] = callsigns[i];
            i++;
        }
        dst_callsign[i] = 0;

        i = 0;
        needle++;
        while (needle[i] != 0 && i < CALLSIGN_MAX_SIZE - 1)
        {
            src_callsign[i] = needle[i];
            i++;
        }
        src_callsign[i] = 0;
    }
    else
    {
        strncpy(dst_callsign, callsigns, CALLSIGN_MAX_SIZE - 1);
        dst_callsign[CALLSIGN_MAX_SIZE - 1] = 0;
    }

    if (!strncmp(dst_callsign, arq_conn.my_call_sign, strlen(dst_callsign)))
    {
        strncpy(arq_conn.src_addr, src_callsign, CALLSIGN_MAX_SIZE - 1);
        arq_conn.src_addr[CALLSIGN_MAX_SIZE - 1] = 0;
        fsm_dispatch(&arq_fsm, EV_LINK_INCOMING_CALL);
        return 0;
    }
    else
    {
          arq_log("incoming connection ignored: dst=%s my=%s", dst_callsign, arq_conn.my_call_sign);
        return 1;
    }
}

int check_for_connection_acceptance_caller(uint8_t *data, size_t data_len)
{
    char callsign[CALLSIGN_MAX_SIZE];

    uint8_t packet_type, subtype = 0, sequence;
    uint8_t *payload;
    size_t payload_len;

    if (parse_arq_frame(data, data_len, &packet_type, &subtype, &sequence, &payload, &payload_len) < 0)
    {
        printf("Failed to parse ARQ frame.\n");
        return -1;
    }

    if (packet_type != PACKET_ARQ_CONTROL || subtype != CONTROL_CALL_RESPONSE)
    {
        return 1; // Not a connection response
    }

    if (arithmetic_decode(payload + 2, payload_len - 2, callsign) < 0)
    {
        printf("Truncated callsign.\n");
        return -1;
    }

      arq_log("connection response decoded callsign: %s", callsign);

    if (!strncmp(callsign, arq_conn.dst_addr, strlen(callsign)))
    {
        fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHED);
        return 0;
    }
    else
    {
          arq_log("connection response mismatch dst=%s expected=%s", callsign, arq_conn.dst_addr);
        return 1;
    }
}

void callee_accept_connection()
{
    uint8_t data[INT_BUFFER_SIZE];
    char callsign[CALLSIGN_MAX_SIZE];

    sprintf(callsign, "%s", arq_conn.my_call_sign);
    int frame_size = create_control_frame(data, sizeof(data), CONTROL_CALL_RESPONSE, callsign);

    if (frame_size > 0)
    {
        arq_enqueue_frame(data, (size_t)frame_size);
        arq_log("sent connection acceptance (callee)");
    }
}

void call_remote()
{
    uint8_t data[INT_BUFFER_SIZE];
    char joint_callsigns[CALLSIGN_MAX_SIZE * 2];

    arq_log("Calling remote dst=%s my=%s", arq_conn.dst_addr, arq_conn.my_call_sign);

    sprintf(joint_callsigns, "%s|%s", arq_conn.dst_addr, arq_conn.my_call_sign);

    int frame_size = create_control_frame(data, sizeof(data), CONTROL_CALL_REQUEST, joint_callsigns);

    if (frame_size > 0)
    {
        // Send multiple times for reliability during call establishment
        for (int i = 0; i < arq_conn.call_burst_size; i++)
        {
            arq_enqueue_frame(data, (size_t)frame_size);
        }
        arq_log("queued connection request burst=%d frame_size=%d", arq_conn.call_burst_size, frame_size);
        arq_conn.last_tx_time = get_timestamp_ms();
    }
}

// Process received ARQ frame
int process_arq_frame(uint8_t *data, size_t data_len)
{
    uint8_t packet_type, subtype, sequence;
    uint8_t *payload;
    size_t payload_len;

    if (parse_arq_frame(data, data_len, &packet_type, &subtype, &sequence, &payload, &payload_len) < 0)
    {
        return -1; // Invalid frame
    }

    arq_log("rx frame type=%s sequence=%u subtype=%u state=%s", packet_type_to_str(packet_type), sequence, subtype, current_state_name());

    switch (packet_type)
    {
    case PACKET_ARQ_CONTROL:
        if (arq_fsm.current == state_listen)
        {
            check_for_incoming_connection(data, data_len);
        }
        else if (arq_fsm.current == state_connecting_caller)
        {
            check_for_connection_acceptance_caller(data, data_len);
        }
        break;

    case PACKET_ARQ_DATA:
        if (arq_fsm.current == state_link_connected)
        {
            // Check sequence number
            if (sequence == arq_conn.rx_sequence)
            {
                // Correct sequence - send ACK and forward data
                uint8_t ack_frame[INT_BUFFER_SIZE];
                int ack_size = create_ack_frame(ack_frame, sizeof(ack_frame), sequence);
                if (ack_size > 0)
                {
                    arq_log("sending ACK for seq=%u", sequence);
                    write_buffer(data_tx_buffer_arq, ack_frame, (size_t)ack_size);
                }

                // Forward payload to TCP client (skip sequence byte)
                if (payload_len > 1)
                {
                    write_buffer(data_rx_buffer_arq, payload + 1, payload_len - 1);
                    arq_log("delivered DATA seq=%u payload=%zu bytes", sequence, payload_len - 1);
                }

                arq_conn.rx_sequence = (arq_conn.rx_sequence + 1) & 0x7F;
            }
            else
            {
                // Wrong sequence - send NACK
                uint8_t nack_frame[INT_BUFFER_SIZE];
                int nack_size = create_nack_frame(nack_frame, sizeof(nack_frame), arq_conn.rx_sequence);
                if (nack_size > 0)
                {
                    arq_log("sending NACK expected=%u got=%u", arq_conn.rx_sequence, sequence);
                    write_buffer(data_tx_buffer_arq, nack_frame, (size_t)nack_size);
                }
            }
        }
        break;

    case PACKET_ARQ_ACK:
        if (arq_fsm.current == state_link_connected && arq_conn.waiting_for_ack)
        {
            if (sequence == arq_conn.last_acked_seq)
            {
                arq_conn.waiting_for_ack = false;
                arq_log("received ACK seq=%u", sequence);
                arq_conn.tx_sequence = (arq_conn.tx_sequence + 1) & 0x7F;
                arq_conn.retry_count = 0;
            }
        }
        break;

    case PACKET_ARQ_NACK:
        if (arq_fsm.current == state_link_connected && arq_conn.waiting_for_ack)
        {
            // Retransmit last frame
            arq_conn.retry_count++;
            if (arq_conn.retry_count < MAX_RETRIES)
            {
                // TODO: Retransmit last frame from buffer
                    arq_log("received NACK, retry_count=%d", arq_conn.retry_count);
                    arq_conn.waiting_for_ack = false;
            }
        }
        break;
    }

    return 0;
}

void clear_connection_data()
{
    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_rx_buffer_arq);
    reset_arq_info(&arq_conn);
}

void reset_arq_info(arq_info *arq_conn_i)
{
    arq_conn_i->TRX = RX;
    arq_conn_i->bw = 0; // 0 = auto
    arq_conn_i->encryption = false;
    arq_conn_i->listen = false;
    arq_conn_i->my_call_sign[0] = 0;
    arq_conn_i->src_addr[0] = 0;
    arq_conn_i->dst_addr[0] = 0;
    arq_conn_i->tx_sequence = 0;
    arq_conn_i->rx_sequence = 0;
    arq_conn_i->last_acked_seq = 0;
    arq_conn_i->waiting_for_ack = false;
    arq_conn_i->retry_count = 0;
}

int arq_init(generic_modem_t *modem)
{
    if (!modem || !modem->freedv)
    {
        fprintf(stderr, "ARQ init: Invalid modem reference\n");
        return EXIT_FAILURE;
    }

    arq_conn.modem = modem;
    arq_conn.call_burst_size = CALL_BURST_SIZE;

    // Get frame size from modem
    size_t bits_per_frame = freedv_get_bits_per_modem_frame(modem->freedv);
    arq_conn.frame_size = bits_per_frame / 8;
    arq_conn.payload_size = arq_conn.frame_size - HEADER_SIZE;

    arq_log("initialized frame_size=%zu payload_size=%zu", arq_conn.frame_size, arq_conn.payload_size);

    reset_arq_info(&arq_conn);
    init_model(); // Initialize arithmetic encoder
    fsm_init(&arq_fsm, state_no_connected_client);

    // Create DSP threads
    pthread_create(&tid[0], NULL, dsp_thread_tx, NULL);
    pthread_create(&tid[1], NULL, dsp_thread_rx, NULL);

    return EXIT_SUCCESS;
}

void arq_set_frame_size(size_t frame_size)
{
    arq_conn.frame_size = frame_size;
    arq_conn.payload_size = frame_size - HEADER_SIZE;
}

void arq_shutdown()
{
    shutdown_ = true;
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

void *dsp_thread_tx(void *conn)
{
    uint8_t data[INT_BUFFER_SIZE];
    uint8_t frame_buffer[INT_BUFFER_SIZE];
    size_t frame_size = arq_conn.frame_size;

    while(!shutdown_)
    {
        // Check if we have enough data to send
        size_t queued = size_buffer(data_tx_buffer_arq);
        if (queued < frame_size ||
            arq_fsm.current == state_idle ||
            arq_fsm.current == state_no_connected_client)
        {
            static uint32_t idle_counter = 0;
            if ((idle_counter++ % 40) == 0)
                arq_log("tx_thread waiting state=%s queued=%zu frame=%zu", current_state_name(), queued, frame_size);
            msleep(50);
            continue;
        }
        // Only transmit when connected or connecting
        if (arq_fsm.current == state_link_connected)
        {
            // Check for data from TCP client
            size_t available = size_buffer(data_tx_buffer_arq);
            if (available >= frame_size)
            {
                read_buffer(data_tx_buffer_arq, data, frame_size);
                uint8_t frame_type = (data[0] >> 6) & 0x3;
                arq_log("tx_thread dequeued %s frame seq=%u state=%s", packet_type_to_str(frame_type),
                        data[HEADER_SIZE] & 0x7F, current_state_name());

                // Create ARQ data frame with sequence number
                size_t payload_len = frame_size - HEADER_SIZE - 1; // -1 for sequence
                int arq_frame_size = create_data_frame(frame_buffer, sizeof(frame_buffer),
                                                       data + HEADER_SIZE, payload_len,
                                                       arq_conn.tx_sequence);

                if (arq_frame_size > 0)
                {
                    // Send via modem (will be handled by modem tx_thread)
                    // For now, we write back to buffer for modem to pick up
                    write_buffer(data_tx_buffer_arq, frame_buffer, arq_frame_size);
                    arq_conn.last_tx_time = get_timestamp_ms();
                    arq_conn.waiting_for_ack = true;
                    arq_conn.last_acked_seq = arq_conn.tx_sequence;
                }
            }
        }
        else if (arq_fsm.current == state_connecting_caller || arq_fsm.current == state_connecting_callee)
        {
            // Connection establishment frames are already in buffer from call_remote/callee_accept_connection
            // Check for timeout
            uint32_t elapsed = get_timestamp_ms() - arq_conn.last_tx_time;
            if (elapsed > CONNECTION_TIMEOUT_MS)
            {
                fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHMENT_TIMEOUT);
            }
        }

        msleep(50);
    }

    return NULL;
}

void *dsp_thread_rx(void *conn)
{
    uint8_t data[INT_BUFFER_SIZE];
    size_t frame_size = arq_conn.frame_size;

    while(!shutdown_)
    {
        // Check for received frames from modem
        if (size_buffer(data_rx_buffer_arq) >= frame_size)
        {
            read_buffer(data_rx_buffer_arq, data, frame_size);

            // Process ARQ frame
            process_arq_frame(data, frame_size);
        }
        else
        {
            msleep(50);
        }
    }

    return NULL;
}

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
#include <unistd.h>

#include "arq.h"
#include "fsm.h"
#include "../audioio/audioio.h"
#include "../data_interfaces/net.h"
#include "../common/defines_modem.h"
#include "../common/crc6.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../modem/framer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <sched.h>

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;
extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_rx_buffer_arq;
extern cbuf_handle_t arq_payload_tx_buffer;
extern volatile sig_atomic_t shutdown_;

// ARQ connection state
arq_info arq_conn;
fsm_handle arq_fsm;
static pthread_t tid[2];

// Protects shared ARQ runtime state (arq_conn fields and tx_window)
static pthread_mutex_t arq_state_mutex = PTHREAD_MUTEX_INITIALIZER;

// Single-frame TX window: keep a copy of the last-sent DATA frame until ACKed
typedef struct {
    uint8_t frame[INT_BUFFER_SIZE];
    size_t  frame_len;
    uint8_t seq;
    uint32_t last_tx_ms;
    int     retry_count;
    bool    valid;
} tx_window_t;

static tx_window_t tx_window = { .valid = false };

// How many frames to process per DSP tick to bound work
#define MAX_FRAMES_PER_TICK 4

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
    fsm_state cur = fsm_get_current(&arq_fsm);
    return fsm_state_to_str(cur);
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

// Get current timestamp in milliseconds
uint64_t get_timestamp_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
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

    uint8_t *encoded_callsign = malloc(arq_conn.payload_size);
    if (!encoded_callsign) return -1;
    // Compresses callsign_data to save space
    int enc_len = arithmetic_encode(callsign_data, encoded_callsign);
    if (enc_len < 0) {
        free(encoded_callsign);
        return -1; // Encoding failed
    }

    size_t available_payload = buffer_size - HEADER_SIZE - 2; // -2 for sequence and subtype
    if (enc_len > (int)available_payload)
    {
        printf("[ARQ] control_frame truncated payload (%zu bytes requested, %d available)", available_payload, enc_len);
        enc_len = available_payload;
    }

    // Sequence number (not used for control, set to 0)
    buffer[HEADER_SIZE] = 0;
    buffer[HEADER_SIZE + 1] = subtype;
    memcpy(buffer + HEADER_SIZE + 2, encoded_callsign, enc_len);

    size_t frame_size = HEADER_SIZE + 2 + enc_len;
    buffer[0] |= (uint8_t)crc6_0X6F(1, buffer + HEADER_SIZE, frame_size - HEADER_SIZE);

    printf("[ARQ] control_frame subtype=%u payload=%d bytes", subtype, (int)enc_len);
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
        printf("[ARQ] incoming connection truncated callsigns");
        return -1;
    }

        printf("[ARQ] incoming connection decoded callsigns: %s", callsigns);

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
        printf("[ARQ] incoming connection ignored: dst=%s my=%s", dst_callsign, arq_conn.my_call_sign);
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

    printf("[ARQ] connection response decoded callsign: %s", callsign);

    if (!strncmp(callsign, arq_conn.dst_addr, strlen(callsign)))
    {
        fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHED);
        return 0;
    }
    else
    {
        printf("[ARQ] connection response mismatch dst=%s expected=%s", callsign, arq_conn.dst_addr);
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
        printf("[ARQ] sent connection acceptance (callee)");
    }
}

void call_remote()
{
    uint8_t data[INT_BUFFER_SIZE];
    char joint_callsigns[CALLSIGN_MAX_SIZE * 2];

    printf("[ARQ] Calling remote dst=%s my=%s", arq_conn.dst_addr, arq_conn.my_call_sign);

    sprintf(joint_callsigns, "%s|%s", arq_conn.dst_addr, arq_conn.my_call_sign);

    int frame_size = create_control_frame(data, sizeof(data), CONTROL_CALL_REQUEST, joint_callsigns);

    if (frame_size > 0)
    {
        // Send multiple times for reliability during call establishment
        for (int i = 0; i < arq_conn.call_burst_size; i++)
        {
            arq_enqueue_frame(data, (size_t)frame_size);
        }
        printf("[ARQ] queued connection request burst=%d frame_size=%d", arq_conn.call_burst_size, frame_size);
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
    fsm_state cur = fsm_get_current(&arq_fsm);

    printf("[ARQ] rx frame type=%s sequence=%u subtype=%u state=%s", packet_type_to_str(packet_type), sequence, subtype, current_state_name());

    switch (packet_type)
    {
    case PACKET_ARQ_CONTROL:
        if (cur == state_listen)
        {
            check_for_incoming_connection(data, data_len);
        }
        else if (cur == state_connecting_caller)
        {
            check_for_connection_acceptance_caller(data, data_len);
        }
        break;

    case PACKET_ARQ_DATA:
        if (cur == state_link_connected)
        {
            // Check sequence number
            if (sequence == arq_conn.rx_sequence)
            {
                // Correct sequence - send ACK and forward data
                uint8_t ack_frame[INT_BUFFER_SIZE];
                int ack_size = create_ack_frame(ack_frame, sizeof(ack_frame), sequence);
                if (ack_size > 0)
                {
                    printf("[ARQ] sending ACK for seq=%u", sequence);
                    write_buffer(data_tx_buffer_arq, ack_frame, (size_t)ack_size);
                }

                // Forward payload to TCP client (skip sequence byte)
                if (payload_len > 1)
                {
                    write_buffer(data_rx_buffer_arq, payload + 1, payload_len - 1);
                    printf("[ARQ] delivered DATA seq=%u payload=%zu bytes", sequence, payload_len - 1);
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
                    printf("[ARQ] sending NACK expected=%u got=%u", arq_conn.rx_sequence, sequence);
                    write_buffer(data_tx_buffer_arq, nack_frame, (size_t)nack_size);
                }
            }
        }
        break;

    case PACKET_ARQ_ACK:
        // ACK: if it matches tx_window, free it and advance sequence
        pthread_mutex_lock(&arq_state_mutex);
        if (tx_window.valid && sequence == tx_window.seq)
        {
            tx_window.valid = false;
            arq_conn.waiting_for_ack = false;
            arq_conn.tx_sequence = (arq_conn.tx_sequence + 1) & 0x7F;
            arq_conn.retry_count = 0;
            printf("[ARQ] received ACK seq=%u - freed tx_window", sequence);
        }
        pthread_mutex_unlock(&arq_state_mutex);
        break;

    case PACKET_ARQ_NACK:
        // NACK: immediate retransmit the tx_window if present
        pthread_mutex_lock(&arq_state_mutex);
        if (tx_window.valid)
        {
            tx_window.retry_count++;
            if (tx_window.retry_count <= MAX_RETRIES)
            {
                printf("[ARQ] received NACK for seq=%u, retransmit (retry=%d)", tx_window.seq, tx_window.retry_count);
                arq_enqueue_frame(tx_window.frame, tx_window.frame_len);
                tx_window.last_tx_ms = get_timestamp_ms();
            }
            else
            {
                printf("[ARQ] NACK exceeded retries for seq=%u, dropping", tx_window.seq);
                tx_window.valid = false;
                arq_conn.waiting_for_ack = false;
            }
        }
        pthread_mutex_unlock(&arq_state_mutex);
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

    printf("[ARQ] initialized frame_size=%zu payload_size=%zu\n", arq_conn.frame_size, arq_conn.payload_size);

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
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

void *dsp_thread_tx(void *conn)
{
    uint8_t payload[INT_BUFFER_SIZE];

    while (!shutdown_)
    {
        // Snapshot FSM state safely
        fsm_state fsm_snapshot = fsm_get_current(&arq_fsm);

        if (fsm_snapshot == state_link_connected)
        {
            // If we don't have a frame in the TX window, build one from payload buffer
            pthread_mutex_lock(&arq_state_mutex);
            if (!tx_window.valid)
            {
                size_t available = size_buffer(arq_payload_tx_buffer);
                if (available >= arq_conn.payload_size)
                {
                    size_t to_read = arq_conn.payload_size;
                    read_buffer(arq_payload_tx_buffer, payload, to_read);

                    int arq_frame_size = create_data_frame(tx_window.frame, sizeof(tx_window.frame),
                                                            payload, to_read,
                                                            arq_conn.tx_sequence);
                    if (arq_frame_size > 0)
                    {
                        tx_window.frame_len = (size_t)arq_frame_size;
                        tx_window.seq = arq_conn.tx_sequence;
                        tx_window.last_tx_ms = get_timestamp_ms();
                        tx_window.retry_count = 0;
                        tx_window.valid = true;

                        // Enqueue framed data to modem TX buffer (separated)
                        arq_enqueue_frame(tx_window.frame, tx_window.frame_len);
                        arq_conn.waiting_for_ack = true;
                        arq_conn.last_tx_time = tx_window.last_tx_ms;
                        printf("[ARQ] queued DATA seq=%u payload=%zu\n", tx_window.seq, to_read);
                    }
                }
            }
            else
            {
                // Check retransmit timeout for the pending window
                uint64_t now = get_timestamp_ms();
                if ((now - tx_window.last_tx_ms) > ACK_TIMEOUT_MS)
                {
                    tx_window.retry_count++;
                    if (tx_window.retry_count <= MAX_RETRIES)
                    {
                        printf("[ARQ] timeout retransmit seq=%u retry=%d\n", tx_window.seq, tx_window.retry_count);
                        arq_enqueue_frame(tx_window.frame, tx_window.frame_len);
                        tx_window.last_tx_ms = now;
                    }
                    else
                    {
                        printf("[ARQ] retransmit exhausted seq=%u, dropping\n", tx_window.seq);
                        tx_window.valid = false;
                        arq_conn.waiting_for_ack = false;
                        arq_conn.retry_count = 0;
                    }
                }
            }
            pthread_mutex_unlock(&arq_state_mutex);
        }
        else if (fsm_snapshot == state_connecting_caller || fsm_snapshot == state_connecting_callee)
        {
            // Connection establishment frames are already queued by call_remote/callee_accept_connection
            pthread_mutex_lock(&arq_state_mutex);
            uint64_t elapsed = get_timestamp_ms() - arq_conn.last_tx_time;
            pthread_mutex_unlock(&arq_state_mutex);
            if (elapsed > CONNECTION_TIMEOUT_MS)
            {
                fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHMENT_TIMEOUT);
            }
        }

        // Bound work per tick and yield to scheduler (no blocking waits)
        for (int i = 0; i < MAX_FRAMES_PER_TICK && !shutdown_; i++)
            ;

        sched_yield();
        usleep(1000); // short pause to avoid busy spin while remaining non-blocking
    }

    return NULL;
}

void *dsp_thread_rx(void *conn)
{
    uint8_t data[INT_BUFFER_SIZE];
    const size_t frame_size = arq_conn.frame_size;

    while (!shutdown_)
    {
        // Process up to a bounded number of frames per tick to avoid long stalls
        int processed = 0;
        while (processed < MAX_FRAMES_PER_TICK && size_buffer(data_rx_buffer_arq) >= frame_size)
        {
            read_buffer(data_rx_buffer_arq, data, frame_size);
            process_arq_frame(data, frame_size);
            processed++;
        }

        // Yield to scheduler; do not block
        sched_yield();
        usleep(1000);
    }

    return NULL;
}

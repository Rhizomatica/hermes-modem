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

#ifndef ARQ_H_
#define ARQ_H_

#include <stdint.h>
#include <stdbool.h>
#include "modem.h"
#include "fsm.h"

#define CALLSIGN_MAX_SIZE 16

#define RX 0
#define TX 1

#define HEADER_SIZE 1

// Packet types (2 bits in header)
#define PACKET_ARQ_CONTROL 0x00
#define PACKET_ARQ_DATA 0x01
#define PACKET_ARQ_ACK 0x02
#define PACKET_ARQ_NACK 0x03

// Control packet subtypes (in payload)
#define CONTROL_CALL_REQUEST 0x01
#define CONTROL_CALL_RESPONSE 0x02
#define CONTROL_DISCONNECT 0x03

#define CALL_BURST_SIZE 3 // Number of frames to send during call establishment
#define MAX_RETRIES 3     // Maximum retransmission attempts
#define ACK_TIMEOUT_MS 2000 // Timeout for ACK reception in milliseconds
#define CONNECTION_TIMEOUT_MS 10000 // Timeout for connection establishment

// ARQ connection information
typedef struct
{
    int TRX; // RX (0) or TX (1)
    char my_call_sign[CALLSIGN_MAX_SIZE];
    char src_addr[CALLSIGN_MAX_SIZE], dst_addr[CALLSIGN_MAX_SIZE];
    bool encryption;
    int call_burst_size;
    bool listen;
    int bw; // in Hz

    // ARQ protocol state
    uint8_t tx_sequence;      // Next sequence number to send
    uint8_t rx_sequence;       // Expected next sequence number
    uint8_t last_acked_seq;    // Last acknowledged sequence number
    bool waiting_for_ack;      // True if waiting for ACK
    uint32_t last_tx_time;     // Timestamp of last transmission (ms)
    int retry_count;           // Current retry count

    // Frame size (obtained from modem)
    size_t frame_size;
    size_t payload_size;       // frame_size - HEADER_SIZE

    // Modem reference (for getting frame size)
    generic_modem_t *modem;
} arq_info;

// FSM states (forward declarations)
void state_listen(int event);
void state_idle(int event);
void state_connecting_caller(int event);
void state_connecting_callee(int event);
void state_link_connected(int event);
void state_no_connected_client(int event);

// ARQ core functions
int arq_init(generic_modem_t *modem);
void arq_shutdown();
void arq_set_frame_size(size_t frame_size);

// DSP threads
void *dsp_thread_tx(void *conn);
void *dsp_thread_rx(void *conn);

// Auxiliary functions
void clear_connection_data();
void reset_arq_info(arq_info *arq_conn_i);
void call_remote();
void callee_accept_connection();
int check_for_incoming_connection(uint8_t *data, size_t data_len);
int check_for_connection_acceptance_caller(uint8_t *data, size_t data_len);
int process_arq_frame(uint8_t *data, size_t data_len);
bool check_crc(uint8_t *data, size_t frame_size);
uint32_t get_timestamp_ms();

// ARQ protocol functions
int create_control_frame(uint8_t *buffer, size_t buffer_size, uint8_t subtype, const char *callsign_data);
int create_data_frame(uint8_t *buffer, size_t buffer_size, uint8_t *payload, size_t payload_len, uint8_t sequence);
int create_ack_frame(uint8_t *buffer, size_t buffer_size, uint8_t sequence);
int create_nack_frame(uint8_t *buffer, size_t buffer_size, uint8_t sequence);
int parse_arq_frame(uint8_t *data, size_t data_len, uint8_t *packet_type, uint8_t *subtype, uint8_t *sequence, uint8_t **payload, size_t *payload_len);

// Arithmetic encoding functions (from arith.c)
void init_model();
int arithmetic_encode(const char* msg, uint8_t* output);
int arithmetic_decode(uint8_t* input, int max_len, char* output);

// External references
extern arq_info arq_conn;
extern fsm_handle arq_fsm;

#endif // ARQ_H_

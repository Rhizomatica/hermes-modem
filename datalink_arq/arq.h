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

#define CALLSIGN_MAX_SIZE 16 

#define RX 0
#define TX 1

#define HEADER_SIZE 1

#define PACKET_ARQ_CONTROL 0x00
#define PACKET_ARQ_DATA 0x01
#define PACKET_BROADCAST_CONTROL 0x02
#define PACKET_BROADCAST_PAYLOAD 0x03

#define CALL_BURST_SIZE 3 // 3 frames

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsm.h"

typedef struct
{
    int TRX; // RX (0) or TX (1)
    char my_call_sign[CALLSIGN_MAX_SIZE];
    char src_addr[CALLSIGN_MAX_SIZE], dst_addr[CALLSIGN_MAX_SIZE];
    bool encryption;
    int call_burst_size;
    bool listen;
    int bw; // in Hz
    size_t frame_size;
} arq_info;

extern arq_info arq_conn;
extern fsm_handle arq_fsm;

// ARQ core functions
int arq_init(size_t frame_size);
void arq_shutdown();
void arq_tick_1hz(void);
bool arq_is_link_connected(void);
int arq_queue_data(const uint8_t *data, size_t len);
void arq_handle_incoming_frame(uint8_t *data, size_t frame_size);

// auxiliary functions
void clear_connection_data();
void reset_arq_info(arq_info *arq_conn);
void call_remote();
void callee_accept_connection();

#endif

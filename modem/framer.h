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

#include <stdint.h>
#include <stddef.h>

#ifndef FRAMER_H
#define FRAMER_H

#define PACKET_TYPE_ARQ_CONTROL       0x00  /* ACK, DISCONNECT, TURN_REQ, etc.    */
#define PACKET_TYPE_ARQ_DATA          0x01  /* data payload frames                */
#define PACKET_TYPE_ARQ_CALL          0x02  /* CALL/ACCEPT setup (compact layout) */
#define PACKET_TYPE_BROADCAST_CONTROL 0x03  /* (was 0x02 in v2)                   */
#define PACKET_TYPE_BROADCAST_DATA    0x04  /* (was 0x03 in v2)                   */

#define PACKET_TYPE_BITS   3    /* bits [7:5] of framer byte */
#define PACKET_TYPE_SHIFT  5
#define PACKET_TYPE_MASK   0x07
#define CRC_BITS           5    /* bits [4:0] of framer byte */
#define CRC_MASK           0x1f

#define HEADER_SIZE 1 // Size of the Hermes header
#define BROADCAST_CONFIG_PACKET_SIZE 9 // hermes-broadcast RaptorQ config packet

// Parse the frame header, validate CRC
// Returns packet type or negative if CRC error
int8_t parse_frame_header(uint8_t *data_frame, uint32_t frame_size);
void write_frame_header(uint8_t *data, int packet_type, size_t frame_size);




#endif // FRAMER_H

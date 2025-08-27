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

#ifndef FRAMER_H
#define FRAMER_H

#define PACKET_TYPE_ARQ_CONTROL 0x00
#define PACKET_TYPE_ARQ_DATA 0x01
#define PACKET_TYPE_BROADCAST_CONTROL 0x02
#define PACKET_TYPE_BROADCAST_DATA 0x03

#define HEADER_SIZE 1 // Size of the Hermes header

// Parse the frame header, validate CRC
// Returns packet type or negative if CRC error
int8_t parse_frame_header(uint8_t *data_frame, uint32_t frame_size);




#endif // FRAMER_H

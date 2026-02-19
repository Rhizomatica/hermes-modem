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

#include "crc6.h"
#include "framer.h"

/*
 * Framer byte layout (v3):
 *   bits [7:5] = packet_type  (3 bits, PACKET_TYPE_* values)
 *   bits [4:0] = CRC5         (polynomial x^5+x^4+x^2+1 = 0x15, init=1)
 */

int8_t parse_frame_header(uint8_t *data_frame, uint32_t frame_size)
{
    if (frame_size < 2)
        return -1;

    uint8_t packet_type = (data_frame[0] >> PACKET_TYPE_SHIFT) & PACKET_TYPE_MASK;
    uint8_t stored_crc  = data_frame[0] & CRC_MASK;
    uint8_t calc_crc    = crc5_0X15(1, data_frame + HEADER_SIZE, (int)(frame_size - HEADER_SIZE));

    if (stored_crc != calc_crc)
    {
        printf("Packet received has CRC5 error!\n");
        return -1;
    }

    return (int8_t)packet_type;
}

void write_frame_header(uint8_t *data, int packet_type, size_t frame_size)
{
    uint8_t crc = crc5_0X15(1, data + HEADER_SIZE, (int)(frame_size - HEADER_SIZE));
    data[0] = (uint8_t)(((packet_type & PACKET_TYPE_MASK) << PACKET_TYPE_SHIFT) | (crc & CRC_MASK));
}

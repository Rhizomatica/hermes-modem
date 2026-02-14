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

// Parse the frame header and validate CRC
int8_t parse_frame_header(uint8_t *data_frame, uint32_t frame_size)
{
    if (frame_size < 2)
        return -1;

    uint8_t packet_type = (data_frame[0] >> 6) & 0x3;

    uint16_t crc6_local = data_frame[0] & 0x3f;
    uint16_t crc6_full = crc6_0X6F(1, data_frame + HEADER_SIZE, frame_size - HEADER_SIZE);

    if (crc6_local != crc6_full)
    {
        if (packet_type == PACKET_TYPE_BROADCAST_CONTROL &&
            frame_size >= BROADCAST_CONFIG_PACKET_SIZE)
        {
            uint16_t crc6_legacy_cfg = crc6_0X6F(1, data_frame + HEADER_SIZE, BROADCAST_CONFIG_PACKET_SIZE - HEADER_SIZE);
            if (crc6_local == crc6_legacy_cfg)
                return packet_type;
        }
        printf("Packet received has CRC6 error!\n");
        return -1;
    }

    return packet_type;
}

// Writes the frame header in place, so data must have enough space
// for the header in the beginning of the buffer
void write_frame_header(uint8_t *data, int packet_type, size_t frame_size)
{
    size_t crc_len = frame_size - HEADER_SIZE;
    if (packet_type == PACKET_TYPE_BROADCAST_CONTROL &&
        frame_size >= BROADCAST_CONFIG_PACKET_SIZE)
    {
        crc_len = BROADCAST_CONFIG_PACKET_SIZE - HEADER_SIZE;
    }

    // set payload packet type
    data[0] = (packet_type << 6) & 0xff;
    data[0] |= crc6_0X6F(1, data + HEADER_SIZE, crc_len);
}

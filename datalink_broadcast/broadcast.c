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
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "modem.h"
#include "arq.h"
#include "defines_modem.h"
#include "ring_buffer_posix.h"
#include "tcp_interfaces.h"

extern bool shutdown_; // global shutdown flag
extern arq_info arq_conn; // ARQ connection info

static const uint32_t hermes_broadcast_frame_size[7] = { 510, 126, 14, 54, 14, 3, 30 };

// Main function to handle broadcast operations
void broadcast_run(generic_modem_t *g_modem)
{
    if (!g_modem)
    {
        printf("Broadcast system: invalid modem context.\n");
        return;
    }

    printf("Starting broadcast system...\n");

    if (g_modem->mode >= 0 && g_modem->mode <= 6)
    {
        uint32_t expected_frame_size = hermes_broadcast_frame_size[g_modem->mode];
        if (g_modem->payload_bytes_per_modem_frame != expected_frame_size)
        {
            printf("WARNING: Broadcast frame mismatch (mode %d): modem payload=%zu, hermes-broadcast expects=%u\n",
                   g_modem->mode, g_modem->payload_bytes_per_modem_frame, expected_frame_size);
        }
        else
        {
            printf("Broadcast frame alignment OK (mode %d): %u bytes.\n",
                   g_modem->mode, expected_frame_size);
        }
    }
    else
    {
        printf("WARNING: Broadcast mode %d is outside hermes-broadcast supported range (0..6).\n",
               g_modem->mode);
    }
}

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
static const int freedv_to_hermes_mode_map[7] = {
    FREEDV_MODE_DATAC1,
    FREEDV_MODE_DATAC3,
    FREEDV_MODE_DATAC0,
    FREEDV_MODE_DATAC4,
    FREEDV_MODE_DATAC13,
    FREEDV_MODE_DATAC14,
    FREEDV_MODE_FSK_LDPC
};

// Main function to handle broadcast operations
void broadcast_run(generic_modem_t *g_modem)
{
    if (!g_modem)
    {
        printf("Broadcast system: invalid modem context.\n");
        return;
    }

    printf("Starting broadcast system...\n");

    int hermes_mode = -1;
    for (int i = 0; i < 7; i++)
    {
        if (g_modem->mode == freedv_to_hermes_mode_map[i])
        {
            hermes_mode = i;
            break;
        }
    }

    if (hermes_mode >= 0)
    {
        uint32_t expected_frame_size = hermes_broadcast_frame_size[hermes_mode];
        if (g_modem->payload_bytes_per_modem_frame != expected_frame_size)
        {
            printf("WARNING: Broadcast frame mismatch (FreeDV mode %d, hermes mode %d): modem payload=%zu, hermes-broadcast expects=%u\n",
                   g_modem->mode, hermes_mode, g_modem->payload_bytes_per_modem_frame, expected_frame_size);
        }
        else
        {
            printf("Broadcast frame alignment OK (FreeDV mode %d, hermes mode %d): %u bytes.\n",
                   g_modem->mode, hermes_mode, expected_frame_size);
        }
    }
    else
    {
        printf("WARNING: FreeDV mode %d is not supported by hermes-broadcast mode mapping.\n",
               g_modem->mode);
    }
}

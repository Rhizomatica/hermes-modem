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

extern cbuf_handle_t data_tx_buffer;
extern cbuf_handle_t data_rx_buffer;

extern bool shutdown_; // global shutdown flag
extern arq_info arq_conn; // ARQ connection info

// Function to handle TX logic
void *broadcast_tx_thread(void *g_modem)
{
    // TODO: Add support for other modems
    struct freedv *freedv = ((generic_modem_t *)g_modem)->freedv;
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    uint8_t *data = (uint8_t *)malloc(bytes_per_modem_frame);

    if (!data)
    {
        fprintf(stderr, "Failed to allocate memory for TX data.\n");
        return NULL;
    }

    while (!shutdown_)
    {
        if (size_buffer(data_tx_buffer) >= bytes_per_modem_frame)
        {
            if (arq_conn.TRX == RX)
            {
                // Switch to TX mode
                ptt_on();
                printf("Switching to TX mode.\n");
            }
            // Read data from TX buffer
            read_buffer(data_tx_buffer, data, bytes_per_modem_frame);

            // Transmit the data
            send_modulated_data(g_modem, data, 1);
            usleep(300000); // 300ms sleep
            ptt_off();
        }
    }

    free(data);
    return NULL;
}

// Function to handle RX logic
void *broadcast_rx_thread(void *g_modem)
{
    // TODO: Add support for other modems
    struct freedv *freedv = ((generic_modem_t *)g_modem)->freedv;
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    uint8_t *data = (uint8_t *)malloc(bytes_per_modem_frame);
    size_t nbytes_out = 0;

    if (!data)
    {
        fprintf(stderr, "Failed to allocate memory for RX data.\n");
        return NULL;
    }

    while (!shutdown_)
    {
        if (arq_conn.TRX == RX)
        {
            // Receive data
            receive_modulated_data(g_modem, data, &nbytes_out);
            if (nbytes_out > 0)
            {
                write_buffer(data_rx_buffer, data, nbytes_out);
                printf("Received %zu bytes of data.\n", nbytes_out);
            }
        }
    }

    free(data);
    return NULL;
}

// Main function to handle broadcast operations
void broadcast_run(generic_modem_t *g_modem)
{
    printf("Starting broadcast system...\n");

    pthread_t tx_thread, rx_thread;

    // Create TX and RX threads
    pthread_create(&tx_thread, NULL, broadcast_tx_thread, (void *)g_modem);
    pthread_create(&rx_thread, NULL, broadcast_rx_thread, (void *)g_modem);

    // Wait for threads to finish
    pthread_join(tx_thread, NULL);
    pthread_join(rx_thread, NULL);


    printf("Broadcast system stopped.\n");
}

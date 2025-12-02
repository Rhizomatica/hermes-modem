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

#include "modem.h"
#include "defines_modem.h"
#include "ring_buffer_posix.h"
#include "kiss.h"
#include "../modem/framer.h"
#include "../common/crc6.h"

extern bool shutdown_;
extern cbuf_handle_t data_tx_buffer_broadcast;
extern cbuf_handle_t data_rx_buffer_broadcast;

// Broadcast state
typedef struct {
    generic_modem_t *modem;
    size_t frame_size;
    size_t payload_size;
    bool active;
    pthread_t tx_tid;
    pthread_t rx_tid;
} broadcast_state_t;

static broadcast_state_t broadcast_state = {0};
static cbuf_handle_t broadcast_tcp_buffer = NULL;

void broadcast_enqueue_tcp_data(uint8_t *data, size_t len)
{
    if (!broadcast_tcp_buffer || !data || len == 0)
        return;

    // Drop oldest data if buffer is full
    size_t free_space = circular_buf_free_size(broadcast_tcp_buffer);
    if (free_space < len)
    {
        size_t to_drop = len - free_space;
        uint8_t tmp[INT_BUFFER_SIZE];
        while (to_drop > 0)
        {
            size_t chunk = to_drop > sizeof(tmp) ? sizeof(tmp) : to_drop;
            read_buffer(broadcast_tcp_buffer, tmp, chunk);
            to_drop -= chunk;
        }
    }

    write_buffer(broadcast_tcp_buffer, data, len);
}

// Broadcast TX thread - processes KISS frames from TCP and sends via modem
// Similar to ARQ dsp_thread_tx
static void *broadcast_tx_thread(void *arg)
{
    uint8_t raw_data[INT_BUFFER_SIZE];
    uint8_t frame_buffer[INT_BUFFER_SIZE];
    size_t frame_size = broadcast_state.frame_size;
    size_t payload_size = broadcast_state.payload_size;
    const size_t len_field_size = 2; // little-endian uint16 length prefix
    if (payload_size <= len_field_size)
    {
        fprintf(stderr, "Broadcast TX: payload too small (%zu bytes)\n", payload_size);
        return NULL;
    }
    size_t payload_capacity = payload_size - len_field_size;
    
    // KISS frame parsing state
    static bool in_kiss_frame = false;
    static bool kiss_escape = false;
    static uint8_t kiss_cmd = CMD_UNKNOWN;
    static int kiss_frame_pos = 0;
    static uint8_t kiss_frame_data[MAX_PAYLOAD];

      while (!shutdown_ && broadcast_state.active)
    {
        // Read raw data from TCP (KISS frames)
          if (!broadcast_tcp_buffer)
          {
              msleep(50);
              continue;
          }
          size_t available = size_buffer(broadcast_tcp_buffer);
        if (available == 0)
        {
            msleep(50);
            continue;
        }

        // Read available data
          size_t read_len = read_buffer_all(broadcast_tcp_buffer, raw_data);
        if (read_len == 0)
        {
            msleep(50);
            continue;
        }

        // Process KISS frames byte by byte
        for (size_t i = 0; i < read_len; i++)
        {
            uint8_t byte = raw_data[i];

            if (byte == FEND)
            {
                if (in_kiss_frame && kiss_cmd == CMD_DATA && kiss_frame_pos > 0)
                {
                    // Complete KISS frame received
                    // Fragment payload across multiple modem frames if needed
                    size_t payload_to_send = kiss_frame_pos;
                    size_t payload_per_frame = payload_capacity;
                    size_t offset = 0;
                    
                    while (offset < payload_to_send)
                    {
                        size_t chunk_size = (payload_to_send - offset > payload_per_frame) ? 
                                           payload_per_frame : (payload_to_send - offset);
                        
                        memset(frame_buffer, 0, frame_size);
                        
                          // Length prefix (little endian)
                          frame_buffer[HEADER_SIZE] = (uint8_t)(chunk_size & 0xff);
                          frame_buffer[HEADER_SIZE + 1] = (uint8_t)((chunk_size >> 8) & 0xff);
                          
                          // Copy chunk of KISS payload immediately after length
                          memcpy(frame_buffer + HEADER_SIZE + len_field_size,
                                 kiss_frame_data + offset, chunk_size);
                          
                          // Populate header (packet type + CRC6 over entire payload area)
                          write_frame_header(frame_buffer, PACKET_TYPE_BROADCAST_DATA, frame_size);
                        
                        // Write frame to buffer for modem to pick up
                          write_buffer(data_tx_buffer_broadcast, frame_buffer, frame_size);
                          printf("Broadcast TX: queued frame chunk=%zu/%zu header=0x%02x\n",
                                 chunk_size, payload_per_frame, frame_buffer[0]);
                        
                        offset += chunk_size;
                    }
                    
                    printf("Broadcast TX: Processed %d bytes KISS frame (%zu frames)\n", 
                           kiss_frame_pos, (payload_to_send + payload_per_frame - 1) / payload_per_frame);
                    
                    // Reset KISS frame state
                    kiss_frame_pos = 0;
                    in_kiss_frame = false;
                    kiss_escape = false;
                }
                else
                {
                    // Start of new KISS frame
                    in_kiss_frame = true;
                    kiss_cmd = CMD_UNKNOWN;
                    kiss_frame_pos = 0;
                    kiss_escape = false;
                }
            }
            else if (in_kiss_frame)
            {
                if (kiss_frame_pos == 0 && kiss_cmd == CMD_UNKNOWN)
                {
                    // Extract command byte
                    kiss_cmd = byte & 0x0F;
                    if (kiss_cmd != CMD_DATA)
                    {
                        in_kiss_frame = false; // Ignore non-data frames
                    }
                }
                else if (kiss_cmd == CMD_DATA)
                {
                    if (byte == FESC)
                    {
                        kiss_escape = true;
                    }
                    else if (kiss_escape)
                    {
                        if (byte == TFEND)
                            kiss_frame_data[kiss_frame_pos++] = FEND;
                        else if (byte == TFESC)
                            kiss_frame_data[kiss_frame_pos++] = FESC;
                        kiss_escape = false;
                        
                        if (kiss_frame_pos >= MAX_PAYLOAD)
                        {
                            fprintf(stderr, "KISS frame too large, resetting\n");
                            kiss_frame_pos = 0;
                            in_kiss_frame = false;
                        }
                    }
                    else
                    {
                        if (kiss_frame_pos < MAX_PAYLOAD)
                        {
                            kiss_frame_data[kiss_frame_pos++] = byte;
                        }
                        else
                        {
                            fprintf(stderr, "KISS frame exceeded MAX_PAYLOAD, resetting\n");
                            kiss_frame_pos = 0;
                            in_kiss_frame = false;
                        }
                    }
                }
            }
        }

        msleep(10);
    }

    return NULL;
}

// Broadcast RX thread - processes frames from modem and sends KISS to TCP
// Similar to ARQ dsp_thread_rx
static void *broadcast_rx_thread(void *arg)
{
    uint8_t frame_buffer[INT_BUFFER_SIZE];
    uint8_t kiss_buffer[INT_BUFFER_SIZE];
    size_t frame_size = broadcast_state.frame_size;
    const size_t len_field_size = 2;

    while (!shutdown_ && broadcast_state.active)
    {
        // Check for received frames from modem (already parsed by modem rx_thread)
        if (size_buffer(data_rx_buffer_broadcast) >= frame_size)
        {
            read_buffer(data_rx_buffer_broadcast, frame_buffer, frame_size);
            
            // Parse frame header to verify it's a broadcast frame
            int8_t packet_type = parse_frame_header(frame_buffer, frame_size);
            
            if (packet_type == PACKET_TYPE_BROADCAST_DATA || packet_type == PACKET_TYPE_BROADCAST_CONTROL)
            {
                // Extract payload (skip header)
                size_t payload_area = frame_size - HEADER_SIZE;
                if (payload_area <= len_field_size)
                    continue;
                uint16_t reported_len = frame_buffer[HEADER_SIZE] |
                                        (frame_buffer[HEADER_SIZE + 1] << 8);
                size_t max_payload = payload_area - len_field_size;
                size_t payload_len = reported_len > max_payload ? max_payload : reported_len;
                uint8_t *payload = frame_buffer + HEADER_SIZE + len_field_size;
                
                // Wrap payload in KISS frame (only the meaningful bytes)
                int kiss_len = kiss_write_frame(payload, payload_len, kiss_buffer);
                
                if (kiss_len > 0)
                {
                    // Write KISS frame to buffer (TCP send_thread will send it)
                    write_buffer(data_rx_buffer_broadcast, kiss_buffer, kiss_len);
                    printf("Broadcast RX: Received %zu bytes, sent %d bytes KISS frame\n", 
                           payload_len, kiss_len);
                }
            }
        }
        else
        {
            msleep(50);
        }
    }

    return NULL;
}

// Main function to handle broadcast operations
void broadcast_run(generic_modem_t *g_modem)
{
    if (!g_modem || !g_modem->freedv)
    {
        fprintf(stderr, "Broadcast: Invalid modem reference\n");
        return;
    }

    printf("Starting broadcast system...\n");

    // Initialize broadcast state
    broadcast_state.modem = g_modem;
    size_t bits_per_frame = freedv_get_bits_per_modem_frame(g_modem->freedv);
    broadcast_state.frame_size = bits_per_frame / 8;
    broadcast_state.payload_size = broadcast_state.frame_size - HEADER_SIZE;
    uint8_t *tcp_storage = (uint8_t *)malloc(DATA_TX_BUFFER_SIZE);
    if (!tcp_storage)
    {
        fprintf(stderr, "Broadcast: failed to allocate TCP buffer\n");
        return;
    }
    broadcast_tcp_buffer = circular_buf_init(tcp_storage, DATA_TX_BUFFER_SIZE);
    
    // Debug: verify frame size matches modem expectations
    printf("Broadcast: bits_per_frame=%zu, frame_size=%zu, payload_size=%zu\n",
           bits_per_frame, broadcast_state.frame_size, broadcast_state.payload_size);
    broadcast_state.active = true;

    printf("Broadcast initialized: frame_size=%zu bytes, payload_size=%zu bytes\n",
           broadcast_state.frame_size, broadcast_state.payload_size);

    // Create broadcast threads (same pattern as ARQ)
    pthread_create(&broadcast_state.tx_tid, NULL, broadcast_tx_thread, NULL);
    pthread_create(&broadcast_state.rx_tid, NULL, broadcast_rx_thread, NULL);

    printf("Broadcast threads started. System running...\n");
    
    // Return immediately - threads run in background (like arq_init)
    // Main program will handle shutdown via broadcast_shutdown()
}

// Shutdown broadcast system and join threads
void broadcast_shutdown()
{
    if (!broadcast_state.active)
        return;
    
    broadcast_state.active = false;
    
    // Wait for threads to finish
    if (broadcast_state.tx_tid != 0)
    {
        pthread_join(broadcast_state.tx_tid, NULL);
    }
    if (broadcast_state.rx_tid != 0)
    {
        pthread_join(broadcast_state.rx_tid, NULL);
    }
    
    printf("Broadcast system stopped.\n");

    if (broadcast_tcp_buffer)
    {
        free(broadcast_tcp_buffer->buffer);
        circular_buf_free(broadcast_tcp_buffer);
        broadcast_tcp_buffer = NULL;
    }
}

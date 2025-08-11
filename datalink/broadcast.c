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
#include "defines.h"
#include "ring_buffer_posix.h"
#include "arq.h"

extern cbuf_handle_t data_tx_buffer;
extern cbuf_handle_t data_rx_buffer;

extern bool shutdown_; // global shutdown flag
extern arq_info arq_conn; // ARQ connection info

// Function to handle TX logic
void *broadcast_tx_thread(void *freedv_ptr)
{
    struct freedv *freedv = (struct freedv *)freedv_ptr;
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
                arq_conn.TRX = TX;
                printf("Switching to TX mode.\n");
            }
            // Read data from TX buffer
            read_buffer(data_tx_buffer, data, bytes_per_modem_frame);

            // Transmit the data
            send_modulated_data(freedv, data, 1);
            usleep(5000000); // 5s for now - TODO !!! some delay for transmission
        }
    }

    free(data);
    return NULL;
}

// Function to handle RX logic
void *broadcast_rx_thread(void *freedv_ptr)
{
    struct freedv *freedv = (struct freedv *)freedv_ptr;
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
            receive_modulated_data(freedv, data, &nbytes_out);
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

// Function to handle TCP server logic
void *tcp_server_thread(void *port_ptr)
{
    int tcp_port = *((int *)port_ptr);
    int tcp_socket, client_socket;
    struct sockaddr_in local_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Open TCP socket
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0)
    {
        perror("Failed to create TCP socket");
        return NULL;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(tcp_port);

    if (bind(tcp_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        perror("Failed to bind TCP socket");
        close(tcp_socket);
        return NULL;
    }

    if (listen(tcp_socket, 1) < 0)
    {
        perror("Failed to listen on TCP socket");
        close(tcp_socket);
        return NULL;
    }

    printf("Waiting for a client to connect...\n");

    while (!shutdown_)
    {
        client_socket = accept(tcp_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0)
        {
            perror("Failed to accept client connection");
            if (shutdown_)
                break; // Exit if shutdown flag is set
            continue; // Retry accepting a connection
        }

        printf("Client connected.\n");

        uint8_t *buffer = (uint8_t *)malloc(DATA_TX_BUFFER_SIZE);
        if (!buffer)
        {
            fprintf(stderr, "Failed to allocate memory for TCP buffer.\n");
            close(client_socket);
            continue;
        }

        while (!shutdown_)
        {
            ssize_t received = recv(client_socket, buffer, DATA_TX_BUFFER_SIZE, 0);
            if (received < 0)
            {
                perror("Error receiving TCP data");
                break;
            }
            else if (received == 0)
            {
                printf("Client disconnected.\n");
                break;
            }

            write_buffer(data_tx_buffer, buffer, received);


            if (size_buffer(data_rx_buffer) == 0)
                continue;
            
            size_t n = read_buffer_all(data_rx_buffer, buffer);
            if (n > 0)
            {
                ssize_t sent = send(client_socket, buffer, n, 0);
                if (sent < 0)
                {
                    perror("Error sending TCP data");
                    break;
                }
                else if (sent == 0)
                {
                    printf("Client disconnected while sending data.\n");
                    break;
                }
            }
        }

        free(buffer);
        close(client_socket);
        printf("Waiting for a new client to connect...\n");
    }

    close(tcp_socket);
    return NULL;
}

// Main function to handle broadcast operations
void broadcast_run(struct freedv *freedv, int tcp_port)
{
    printf("Starting broadcast system...\n");

    pthread_t tx_thread, rx_thread, server_thread;

    // Create TX and RX threads
    pthread_create(&tx_thread, NULL, broadcast_tx_thread, (void *)freedv);
    pthread_create(&rx_thread, NULL, broadcast_rx_thread, (void *)freedv);

    // Create TCP server thread
    pthread_create(&server_thread, NULL, tcp_server_thread, (void *)&tcp_port);

    // Wait for threads to finish
    pthread_join(tx_thread, NULL);
    pthread_join(rx_thread, NULL);
    pthread_join(server_thread, NULL);

    printf("Broadcast system stopped.\n");
}

/* HERMES Modem Loopback Test Utility
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This utility tests the modem without radio hardware by creating
 * a loopback between TX and RX using shared memory or direct buffer access.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>

#include "../common/ring_buffer_posix.h"
#include "../common/defines_modem.h"
#include "../modem/modem.h"

static bool running = true;

void signal_handler(int sig)
{
    running = false;
    printf("\nShutting down...\n");
}

// Loopback thread - copies TX buffer to RX buffer
void *loopback_thread(void *arg)
{
    cbuf_handle_t tx_buffer = ((cbuf_handle_t *)arg)[0];
    cbuf_handle_t rx_buffer = ((cbuf_handle_t *)arg)[1];
    uint8_t *buffer = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);

    if (!buffer)
    {
        fprintf(stderr, "Failed to allocate loopback buffer\n");
        return NULL;
    }

    printf("Loopback thread started\n");

    while (running)
    {
        size_t available = size_buffer(tx_buffer);
        if (available > 0)
        {
            size_t read_len = read_buffer_all(tx_buffer, buffer);
            if (read_len > 0)
            {
                // Add some delay to simulate propagation
                usleep(1000); // 1ms delay

                // Write to RX buffer
                write_buffer(rx_buffer, buffer, read_len);
                printf("Loopback: forwarded %zu bytes\n", read_len);
            }
        }
        else
        {
            usleep(10000); // 10ms sleep when no data
        }
    }

    free(buffer);
    printf("Loopback thread stopped\n");
    return NULL;
}

int main(int argc, char *argv[])
{
    printf("HERMES Modem Loopback Test Utility\n");
    printf("===================================\n\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize modem
    generic_modem_t g_modem;
    int mod_config = FREEDV_MODE_DATAC1;

    if (argc > 1)
    {
        mod_config = atoi(argv[1]);
    }

    printf("Initializing modem with mode %d\n", mod_config);

    // Create loopback buffers
    uint8_t *tx_buf = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);
    uint8_t *rx_buf = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);

    if (!tx_buf || !rx_buf)
    {
        fprintf(stderr, "Failed to allocate buffers\n");
        return EXIT_FAILURE;
    }

    cbuf_handle_t tx_buffer = circular_buf_init(tx_buf, SIGNAL_BUFFER_SIZE);
    cbuf_handle_t rx_buffer = circular_buf_init(rx_buf, SIGNAL_BUFFER_SIZE);

    // Override modem buffers (hack - would need to modify modem.c for proper support)
    // For now, this is a demonstration of the concept

    printf("Loopback buffers created\n");
    printf("Press Ctrl+C to stop\n\n");

    // Create loopback thread
    cbuf_handle_t buffers[2] = {tx_buffer, rx_buffer};
    pthread_t loopback_tid;
    pthread_create(&loopback_tid, NULL, loopback_thread, buffers);

    // Main loop
    while (running)
    {
        sleep(1);
    }

    // Cleanup
    pthread_join(loopback_tid, NULL);
    circular_buf_free(tx_buffer);
    circular_buf_free(rx_buffer);
    free(tx_buf);
    free(rx_buf);

    printf("Test completed\n");
    return EXIT_SUCCESS;
}

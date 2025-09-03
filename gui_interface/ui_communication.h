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

// UDP JSON sender + receiver interface (POSIX).
// Threaded receiver, variadic JSON key/value sender.

#ifndef UDP_JSON_H
#define UDP_JSON_H

#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

// ---- Direction type ----
typedef enum {
    DIR_RX,
    DIR_TX
} modem_direction_t;

// ---- Status message ----
typedef struct {
    int bitrate;
    double snr;
    char user_callsign[32];
    char dest_callsign[32];
    int sync;                 // bool
    modem_direction_t dir;
    int client_tcp_connected; // bool
} modem_status_t;

// ---- Message types ----
typedef enum {
    MSG_UNKNOWN,
    MSG_STATUS,
    MSG_CONFIG
} message_type_t;

// ---- Unified message ----
typedef struct {
    message_type_t type;

    // Status
    modem_status_t status;

    // Config
    char soundcard[64];   // string or index
    int broadcast_port;
    int arq_base_port;
    char aes_key[128];
    int encryption_enabled; // bool
} modem_message_t;

// ---- TX handle ----
typedef struct {
    int sock;
    struct sockaddr_in dest;
} udp_tx_t;

// ---- RX thread args ----
typedef struct {
    uint16_t listen_port;
} rx_args_t;

// ---- API ----
int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port);
void udp_tx_close(udp_tx_t *tx);
int udp_tx_send_json_pairs(udp_tx_t *tx, ...);

// Helpers for specific messages
int udp_tx_send_status(udp_tx_t *tx,
                       int bitrate, double snr,
                       const char *user_callsign,
                       const char *dest_callsign,
                       int sync, modem_direction_t dir,
                       int client_tcp_connected);

int udp_tx_send_config(udp_tx_t *tx,
                       const char *soundcard,
                       int broadcast_port,
                       int arq_base_port,
                       const char *aes_key,
                       int encryption_enabled);

// RX thread
void *rx_thread_main(void *arg);

#endif

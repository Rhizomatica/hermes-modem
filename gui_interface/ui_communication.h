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
// Author: Rafael Diniz

#ifndef UDP_JSON_H
#define UDP_JSON_H

#include <stdint.h>

// ------------------------- Transmitter --------------------------

typedef struct {
    int                 sock;   // UDP socket
    struct sockaddr_in  dst;    // Destination address
} udp_tx_t;

typedef enum {
    DIR_RX,
    DIR_TX
} modem_direction_t;

typedef struct {
    int bitrate;              // bits per second
    double snr;               // dB
    char user_callsign[32];
    char dest_callsign[32];
    int sync;                 // boolean (0/1)
    modem_direction_t dir;    // RX or TX
    int client_tcp_connected; // boolean (0/1)
} modem_status_t;

// Initialize UDP transmitter to given IPv4 string and port.
// Returns 0 on success, -1 on error.
int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port);

// Close transmitter socket.
void udp_tx_close(udp_tx_t *tx);

// Send a JSON object built from NULL-terminated key/value pairs.
// Example:
//   udp_tx_send_json_pairs(&tx,
//                          "type", "greeting",
//                          "msg", "hello", NULL);
//
// Returns 0 on success, -1 on error.
int udp_tx_send_json_pairs(udp_tx_t *tx, ...);

// ------------------------- Receiver -----------------------------

typedef struct {
    uint16_t listen_port;   // UDP port to listen on
} rx_args_t;

// Receiver thread function. Blocks forever on recvfrom()
// and prints any received JSON string to stdout.
void *rx_thread_main(void *arg);

#endif // UDP_JSON_H

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

#ifndef TCP_INTERFACES_H_
#define TCP_INTERFACES_H_

#define DEFAULT_ARQ_PORT 8300
#define DEFAULT_BROADCAST_PORT 8100

#define TCP_BLOCK_SIZE 128

int interfaces_init(int arq_tcp_base_port, int broadcast_tcp_port);
void interfaces_shutdown();

// ARQ TCP/IP server threads
void *server_worker_thread_ctl(void *port);
void *server_worker_thread_data(void *port);
void *data_worker_thread_tx(void *conn);
void *data_worker_thread_rx(void *conn);
void *control_worker_thread_tx(void *conn);
void *control_worker_thread_rx(void *conn);

// BROADCAST TCP/IP server threads
void *send_thread(void *client_socket_ptr);
void *recv_thread(void *client_socket_ptr);
void *tcp_server_thread(void *port_ptr);

// TNC / radio functions
void ptt_on();
void ptt_off();
void tnc_send_connected();
void tnc_send_disconnected();
char *get_timestamp();

#endif // TCP_INTERFACES_H_

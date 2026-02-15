/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 */

#ifndef ARQ_V2_H_
#define ARQ_V2_H_

#include "arq.h"

int arq_v2_init(size_t frame_size, int mode);
void arq_v2_shutdown(void);
void arq_v2_tick_1hz(void);
void arq_v2_post_event(int event);
bool arq_v2_is_link_connected(void);
int arq_v2_queue_data(const uint8_t *data, size_t len);
int arq_v2_get_tx_backlog_bytes(void);
int arq_v2_get_speed_level(void);
int arq_v2_get_payload_mode(void);
int arq_v2_get_control_mode(void);
int arq_v2_get_preferred_rx_mode(void);
int arq_v2_get_preferred_tx_mode(void);
void arq_v2_set_active_modem_mode(int mode, size_t frame_size);
bool arq_v2_handle_incoming_connect_frame(uint8_t *data, size_t frame_size);
void arq_v2_handle_incoming_frame(uint8_t *data, size_t frame_size);
void arq_v2_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded);
bool arq_v2_try_dequeue_action(arq_action_t *action);
bool arq_v2_wait_dequeue_action(arq_action_t *action, int timeout_ms);
void arq_v2_clear_connection_data(void);
void arq_v2_reset_arq_info(arq_info *arq_conn_i);
void arq_v2_call_remote(void);
void arq_v2_callee_accept_connection(void);

#endif

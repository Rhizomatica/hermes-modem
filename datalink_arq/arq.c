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

#include "arq.h"
#include "arq_v2.h"

int arq_init(size_t frame_size, int mode)
{
    return arq_v2_init(frame_size, mode);
}

void arq_shutdown(void)
{
    arq_v2_shutdown();
}

void arq_tick_1hz(void)
{
    arq_v2_tick_1hz();
}

void arq_post_event(int event)
{
    arq_v2_post_event(event);
}

bool arq_is_link_connected(void)
{
    return arq_v2_is_link_connected();
}

int arq_queue_data(const uint8_t *data, size_t len)
{
    return arq_v2_queue_data(data, len);
}

int arq_get_tx_backlog_bytes(void)
{
    return arq_v2_get_tx_backlog_bytes();
}

int arq_get_speed_level(void)
{
    return arq_v2_get_speed_level();
}

int arq_get_payload_mode(void)
{
    return arq_v2_get_payload_mode();
}

int arq_get_control_mode(void)
{
    return arq_v2_get_control_mode();
}

int arq_get_preferred_rx_mode(void)
{
    return arq_v2_get_preferred_rx_mode();
}

int arq_get_preferred_tx_mode(void)
{
    return arq_v2_get_preferred_tx_mode();
}

void arq_set_active_modem_mode(int mode, size_t frame_size)
{
    arq_v2_set_active_modem_mode(mode, frame_size);
}

bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size)
{
    return arq_v2_handle_incoming_connect_frame(data, frame_size);
}

void arq_handle_incoming_frame(uint8_t *data, size_t frame_size)
{
    arq_v2_handle_incoming_frame(data, frame_size);
}

void arq_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded)
{
    arq_v2_update_link_metrics(sync, snr, rx_status, frame_decoded);
}

bool arq_try_dequeue_action(arq_action_t *action)
{
    return arq_v2_try_dequeue_action(action);
}

bool arq_wait_dequeue_action(arq_action_t *action, int timeout_ms)
{
    return arq_v2_wait_dequeue_action(action, timeout_ms);
}

void clear_connection_data(void)
{
    arq_v2_clear_connection_data();
}

void reset_arq_info(arq_info *arq_conn_i)
{
    arq_v2_reset_arq_info(arq_conn_i);
}

void call_remote(void)
{
    arq_v2_call_remote();
}

void callee_accept_connection(void)
{
    arq_v2_callee_accept_connection();
}

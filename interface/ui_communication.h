/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_COMMUNICATION_H_
#define UI_COMMUNICATION_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIR_RX = 0,
    DIR_TX = 1
} modem_direction_t;

typedef struct {
    int bitrate;
    double snr;
    char user_callsign[32];
    char dest_callsign[32];
    int sync;
    modem_direction_t dir;
    int client_tcp_connected;
    long bytes_transmitted;
    long bytes_received;
} modem_status_t;

typedef enum {
    MSG_UNKNOWN,
    MSG_STATUS,
    MSG_CONFIG,
    MSG_SOUNDCARD_LIST,
    MSG_RADIO_LIST
} message_type_t;

typedef struct {
    message_type_t type;

    modem_status_t status;

    struct {
        char soundcard[64];
        int broadcast_port;
        int arq_base_port;
        char aes_key[128];
        int encryption_enabled;
    } config;

    struct {
        char selected[64];
        char list[512];
    } soundcard_list;

    struct {
        char selected[64];
        char list[512];
    } radio_list;
} modem_message_t;

int ui_comm_init(const char *tx_ip, uint16_t tx_port, uint16_t rx_port);
void ui_comm_shutdown(void);
bool ui_comm_is_enabled(void);
void ui_comm_set_logging(bool enabled);

int ui_comm_send_status(const modem_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* UI_COMMUNICATION_H_ */

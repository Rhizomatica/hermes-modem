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

// json communication between UI and backend
// POSIX UDP JSON sender + blocking receiver, each on its own thread.
// No third-party libs; includes a minimal JSON string escaper.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <time.h>

#include "ui_communication.h"

// ---------------- TX ----------------

int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port) {
    tx->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx->sock < 0) {
        perror("socket");
        return -1;
    }
    memset(&tx->dest, 0, sizeof(tx->dest));
    tx->dest.sin_family = AF_INET;
    tx->dest.sin_port = htons(port);
    if (inet_aton(ip, &tx->dest.sin_addr) == 0) {
        perror("inet_aton");
        close(tx->sock);
        tx->sock = -1;
        return -1;
    }
    return 0;
}

void udp_tx_close(udp_tx_t *tx) {
    if (tx->sock >= 0) close(tx->sock);
    tx->sock = -1;
}

int udp_tx_send_json_pairs(udp_tx_t *tx, ...) {
    char buf[512];
    char tmp[128];
    buf[0] = '\0';

    strcat(buf, "{");
    va_list ap;
    va_start(ap, tx);
    const char *key;
    int first = 1;
    while ((key = va_arg(ap, const char *)) != NULL) {
        const char *val = va_arg(ap, const char *);
        if (!first) strcat(buf, ",");
        snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\"", key, val);
        strcat(buf, tmp);
        first = 0;
    }
    va_end(ap);
    strcat(buf, "}");

    ssize_t sent = sendto(tx->sock, buf, strlen(buf), 0,
                  (struct sockaddr *)&tx->dest, sizeof(tx->dest));

    printf("[tx] Sent %zu bytes: %s\n", sent, buf);

    return sent;
}

int udp_tx_send_status(udp_tx_t *tx,
                       int bitrate, double snr,
                       const char *user_callsign,
                       const char *dest_callsign,
                       int sync, modem_direction_t dir,
                       int client_tcp_connected) {
    char br[32], snrbuf[32];
    snprintf(br, sizeof(br), "%d", bitrate);
    snprintf(snrbuf, sizeof(snrbuf), "%.1f", snr);

    return udp_tx_send_json_pairs(tx,
        "type", "status",
        "bitrate", br,
        "snr", snrbuf,
        "user_callsign", user_callsign,
        "dest_callsign", dest_callsign,
        "sync", sync ? "true" : "false",
        "direction", dir == DIR_TX ? "tx" : "rx",
        "client_tcp_connected", client_tcp_connected ? "true" : "false",
        NULL);
}

int udp_tx_send_config(udp_tx_t *tx,
                       const char *soundcard,
                       int broadcast_port,
                       int arq_base_port,
                       const char *aes_key,
                       int encryption_enabled) {
    char bp[32], ap[32];
    snprintf(bp, sizeof(bp), "%d", broadcast_port);
    snprintf(ap, sizeof(ap), "%d", arq_base_port);

    return udp_tx_send_json_pairs(tx,
        "type", "config",
        "soundcard", soundcard,
        "broadcast_port", bp,
        "arq_base_port", ap,
        "aes_key", aes_key,
        "encryption", encryption_enabled ? "true" : "false",
        NULL);
}

// ---------------- RX ----------------

static void parse_json(const char *json,
                       char keys[][64], char vals[][128], int *count) {
    *count = 0;
    const char *p = json;
    while ((p = strchr(p, '"')) && *count < 32) {
        const char *q = strchr(p + 1, '"');
        if (!q) break;
        int klen = q - (p + 1);
        strncpy(keys[*count], p + 1, klen);
        keys[*count][klen] = '\0';
        p = strchr(q + 1, '"');
        if (!p) break;
        q = strchr(p + 1, '"');
        if (!q) break;
        int vlen = q - (p + 1);
        strncpy(vals[*count], p + 1, vlen);
        vals[*count][vlen] = '\0';
        (*count)++;
        p = q + 1;
    }
}

static void fill_modem_message(modem_message_t *msg,
                               char keys[][64], char vals[][128], int pairs) {
    memset(msg, 0, sizeof(*msg));
    msg->type = MSG_UNKNOWN;

    for (int i = 0; i < pairs; i++) {
        if (strcmp(keys[i], "type") == 0) {
            if (strcmp(vals[i], "status") == 0) msg->type = MSG_STATUS;
            else if (strcmp(vals[i], "config") == 0) msg->type = MSG_CONFIG;
        }
    }

    for (int i = 0; i < pairs; i++) {
        if (msg->type == MSG_STATUS) {
            if (strcmp(keys[i], "bitrate") == 0)
                msg->status.bitrate = atoi(vals[i]);
            else if (strcmp(keys[i], "snr") == 0)
                msg->status.snr = atof(vals[i]);
            else if (strcmp(keys[i], "user_callsign") == 0)
                strncpy(msg->status.user_callsign, vals[i],
                        sizeof msg->status.user_callsign - 1);
            else if (strcmp(keys[i], "dest_callsign") == 0)
                strncpy(msg->status.dest_callsign, vals[i],
                        sizeof msg->status.dest_callsign - 1);
            else if (strcmp(keys[i], "sync") == 0)
                msg->status.sync = (strcmp(vals[i], "true") == 0);
            else if (strcmp(keys[i], "direction") == 0)
                msg->status.dir = (strcmp(vals[i], "tx") == 0) ? DIR_TX : DIR_RX;
            else if (strcmp(keys[i], "client_tcp_connected") == 0)
                msg->status.client_tcp_connected = (strcmp(vals[i], "true") == 0);
        } else if (msg->type == MSG_CONFIG) {
            if (strcmp(keys[i], "soundcard") == 0)
                strncpy(msg->soundcard, vals[i], sizeof msg->soundcard - 1);
            else if (strcmp(keys[i], "broadcast_port") == 0)
                msg->broadcast_port = atoi(vals[i]);
            else if (strcmp(keys[i], "arq_base_port") == 0)
                msg->arq_base_port = atoi(vals[i]);
            else if (strcmp(keys[i], "aes_key") == 0)
                strncpy(msg->aes_key, vals[i], sizeof msg->aes_key - 1);
            else if (strcmp(keys[i], "encryption") == 0)
                msg->encryption_enabled = (strcmp(vals[i], "true") == 0);
        }
    }
}

void *rx_thread_main(void *arg) {
    rx_args_t *rxa = (rx_args_t *)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket(rx)");
        return NULL;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(rxa->listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return NULL;
    }

    char buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &srclen);
        if (n <= 0) continue;
        buf[n] = '\0';

        char keys[32][64], vals[32][128];
        int pairs = 0;
        parse_json(buf, keys, vals, &pairs);

        modem_message_t msg;
        fill_modem_message(&msg, keys, vals, pairs);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        printf("[rx] From %s:%u\n", ip, ntohs(src.sin_port));

        if (msg.type == MSG_STATUS) {
            printf("   STATUS:\n");
            printf("      bitrate: %d bps\n", msg.status.bitrate);
            printf("      snr: %.1f dB\n", msg.status.snr);
            printf("      user_callsign: %s\n", msg.status.user_callsign);
            printf("      dest_callsign: %s\n", msg.status.dest_callsign);
            printf("      sync: %s\n", msg.status.sync ? "true" : "false");
            printf("      direction: %s\n", msg.status.dir == DIR_TX ? "tx" : "rx");
            printf("      client_tcp_connected: %s\n",
                   msg.status.client_tcp_connected ? "true" : "false");
        } else if (msg.type == MSG_CONFIG) {
            printf("   CONFIG:\n");
            printf("      soundcard: %s\n", msg.soundcard);
            printf("      broadcast_port: %d\n", msg.broadcast_port);
            printf("      arq_base_port: %d (ports %d and %d)\n",
                   msg.arq_base_port,
                   msg.arq_base_port,
                   msg.arq_base_port + 1);
            printf("      aes_key: %s\n", msg.aes_key);
            printf("      encryption: %s\n", msg.encryption_enabled ? "true" : "false");
        } else {
            printf("   Unknown message type, raw: %s\n", buf);
        }
    }
    close(sock);
    return NULL;
}

// ---------------- MAIN TEST ----------------

#ifdef TEST_MAIN
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <tx_ip> <tx_port> <rx_port>\n", argv[0]);
        return 1;
    }

    const char *tx_ip = argv[1];
    int tx_port = atoi(argv[2]);
    int rx_port = atoi(argv[3]);

    pthread_t rx_thread;
    rx_args_t rxa = { .listen_port = (uint16_t)rx_port };
    pthread_create(&rx_thread, NULL, rx_thread_main, &rxa);
    pthread_detach(rx_thread);

    udp_tx_t tx;
    if (udp_tx_init(&tx, tx_ip, (uint16_t)tx_port) != 0) {
        fprintf(stderr, "TX init failed\n");
        return 1;
    }

    srand((unsigned)time(NULL));
    int counter = 0;

    while (1) {
        // Send status
        int bitrate = (rand() % 2) ? 1200 : 2400;
        double snr = 15.0 + (rand() % 100) / 10.0;
        int sync = rand() % 2;
        modem_direction_t dir = (counter % 2) ? DIR_TX : DIR_RX;
        int client = rand() % 2;

        udp_tx_send_status(&tx, bitrate, snr, "K1ABC", "N0XYZ", sync, dir, client);

        // Occasionally send a config
        if (counter % 5 == 0) {
            udp_tx_send_config(&tx, "hw:1,0", 8500, 8600,
                               "ABCDEF1234567890", rand() % 2);
        }

        counter++;
        usleep(500 * 1000); // 500 ms
    }

    udp_tx_close(&tx);
    return 0;
}
#endif

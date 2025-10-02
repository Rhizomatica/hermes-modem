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

// To run test main(), compile with -DTEST_MAIN
// Eg. how to run. One side listens on port 5006, other side sends to
// port 5005. Both sides can send and receive.
// ./ui_communication 127.0.0.1 5005 5006
// ./ui_communication 127.0.0.1 5006 5005

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
int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port)
{
    tx->sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (tx->sock < 0)
    {
        perror("socket");
        return -1;
    }

    memset(&tx->dest, 0, sizeof(tx->dest));
    tx->dest.sin_family = AF_INET;
    tx->dest.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &tx->dest.sin_addr) <= 0)
    {
        perror("inet_pton");
        close(tx->sock);
        tx->sock = -1;
        return -1;
    }

    return 0;
}

int udp_tx_send_json_pairs(udp_tx_t *tx, ...)
{
    char buf[1500];
    char tmp[512];
    buf[0] = '\0';

    strcat(buf, "{");
    va_list ap;
    va_start(ap, tx);
    const char *key;
    int first = 1;
    while ((key = va_arg(ap, const char *)) != NULL)
    {
        const char *val = va_arg(ap, const char *);
        if (!first) strcat(buf, ",");

        // Don't quote arrays, objects, numbers, booleans, null
        if (val[0] == '[' || val[0] == '{' ||
            strcmp(val, "true") == 0 || strcmp(val, "false") == 0 ||
            strcmp(val, "null") == 0 || strspn(val, "0123456789.-") == strlen(val)) {
            snprintf(tmp, sizeof(tmp), "\"%s\":%s", key, val);
        } else {
            snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\"", key, val);
        }

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
                       int client_tcp_connected,
                       long bytes_transmitted, long bytes_received)
{
    char br[32], snrbuf[32], tx_bytes[32], rx_bytes[32];
    snprintf(br, sizeof(br), "%d", bitrate);
    snprintf(snrbuf, sizeof(snrbuf), "%.1f", snr);
    snprintf(tx_bytes, sizeof(tx_bytes), "%ld", bytes_transmitted);
    snprintf(rx_bytes, sizeof(rx_bytes), "%ld", bytes_received);

    return udp_tx_send_json_pairs(tx,
        "type", "status",
        "bitrate", br,
        "snr", snrbuf,
        "user_callsign", user_callsign,
        "dest_callsign", dest_callsign,
        "sync", sync ? "true" : "false",
        "direction", dir == DIR_TX ? "tx" : "rx",
        "client_tcp_connected", client_tcp_connected ? "true" : "false",
        "bytes_transmitted", tx_bytes,
        "bytes_received", rx_bytes,
        NULL);
}

int udp_tx_send_soundcard_list(udp_tx_t *tx,
                               const char *selected_soundcard,
                               const char *soundcards[], int count) {
    char buf[1500]; // max mtu size
    snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < count; i++) {
        strcat(buf, "\"");
        strcat(buf, soundcards[i]);
        strcat(buf, "\"");
        if (i < count - 1) strcat(buf, ",");
    }
    strcat(buf, "]");

    return udp_tx_send_json_pairs(tx,
        "type", "soundcard_list",
        "selected", selected_soundcard,
        "list", buf,
        NULL);
}

int udp_tx_send_radio_list(udp_tx_t *tx,
                           const char *selected_radio,
                           const char *radios[], int count) {
    char buf[1500];
    snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < count; i++) {
        strcat(buf, "\"");
        strcat(buf, radios[i]);
        strcat(buf, "\"");
        if (i < count - 1) strcat(buf, ",");
    }
    strcat(buf, "]");

    return udp_tx_send_json_pairs(tx,
        "type", "radio_list",
        "selected", selected_radio,
        "list", buf,
        NULL);
}

// ---------------- RX ----------------
static void parse_json(const char *json, char keys[][64], char vals[][1024], int *count)
{
    *count = 0;
    const char *p = json;
    while ((p = strchr(p, '"')) && *count < 32)
    {
        // ---- parse key ----
        const char *q = strchr(p + 1, '"');
        if (!q)
            break;
        int klen = q - (p + 1);
        strncpy(keys[*count], p + 1, klen);
        keys[*count][klen] = '\0';

        // ---- find colon ----
        p = strchr(q + 1, ':');
        if (!p)
            break;
        p++;

        // ---- skip whitespace ----
        while (*p == ' ' || *p == '\t')
            p++;

        // ---- parse value ----
        if (*p == '"')
        {
            // string value
            p++;
            q = strchr(p, '"');
            if (!q) break;
            int vlen = q - p;
            strncpy(vals[*count], p, vlen);
            vals[*count][vlen] = '\0';
            p = q + 1;
        }
        else if (*p == '[')
        {
            // array value
            int depth = 1;
            const char *start = p;
            p++;
            while (*p && depth > 0)
            {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }

            if (depth != 0)
                break; // malformed

            int vlen = p - start;
            if (vlen >= (int)sizeof(vals[*count]))
                vlen = sizeof(vals[*count]) - 1;

            strncpy(vals[*count], start, vlen);
            vals[*count][vlen] = '\0';
        }
        else
        {
            // bare value (true, false, null, number)
            const char *start = p;
            while (*p && *p != ',' && *p != '}') p++;
            int vlen = p - start;
            strncpy(vals[*count], start, vlen);
            vals[*count][vlen] = '\0';
        }

        (*count)++;

        // ---- skip to next key ----
        while (*p && *p != '"')
        {
            if (*p == '}') return;
            p++;
        }
    }
}

static void fill_modem_message(modem_message_t *msg, char keys[][64], char vals[][1024], int pairs)
{
    memset(msg, 0, sizeof(*msg));
    msg->type = MSG_UNKNOWN;

    for (int i = 0; i < pairs; i++)
    {
        if (strcmp(keys[i], "type") == 0)
        {
            if (strcmp(vals[i], "status") == 0) msg->type = MSG_STATUS;
            else if (strcmp(vals[i], "config") == 0) msg->type = MSG_CONFIG;
            else if (strcmp(vals[i], "soundcard_list") == 0) msg->type = MSG_SOUNDCARD_LIST;
            else if (strcmp(vals[i], "radio_list") == 0) msg->type = MSG_RADIO_LIST;
        }
    }

    for (int i = 0; i < pairs; i++)
    {
        switch (msg->type) {
        case MSG_UNKNOWN:
            // No specific fields to parse
            break;
        case MSG_STATUS:
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
            else if (strcmp(keys[i], "bytes_transmitted") == 0)
                msg->status.bytes_transmitted = atol(vals[i]);
            else if (strcmp(keys[i], "bytes_received") == 0)
                msg->status.bytes_received = atol(vals[i]);
                    break;
        case MSG_SOUNDCARD_LIST:
            if (msg->type == MSG_SOUNDCARD_LIST)
            {
                if (strcmp(keys[i], "selected") == 0)
                    strncpy(msg->soundcard_list.selected, vals[i], sizeof msg->soundcard_list.selected - 1);
                else
                    if (strcmp(keys[i], "list") == 0)
                        strncpy(msg->soundcard_list.list, vals[i], sizeof msg->soundcard_list.list - 1);
            }
            break;
        case MSG_RADIO_LIST:
            if (strcmp(keys[i], "selected") == 0)
                strncpy(msg->radio_list.selected, vals[i], sizeof msg->radio_list.selected - 1);
            else
                if (strcmp(keys[i], "list") == 0)
                    strncpy(msg->radio_list.list, vals[i], sizeof msg->radio_list.list - 1);
            break;
        default:
            printf("   Unknown message type, raw: %s\n", vals[i]);
        }
    }
}


void *rx_thread_main(void *arg)
{
    rx_args_t *rxa = (rx_args_t *)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
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

    char buf[1500];
    while (1)
    {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &srclen);
        if (n <= 0) continue;
        buf[n] = '\0';

        printf("[rx] Received %d bytes: %s\n", n, buf);
        char keys[32][64], vals[32][1024];
        int pairs = 0;
        parse_json(buf, keys, vals, &pairs);

        modem_message_t msg;
        fill_modem_message(&msg, keys, vals, pairs);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        printf("[rx] From %s:%u\n", ip, ntohs(src.sin_port));

        switch (msg.type) {
        case MSG_STATUS:
            printf("   STATUS:\n");
            printf("      bitrate: %d bps\n", msg.status.bitrate);
            printf("      snr: %.1f dB\n", msg.status.snr);
            printf("      user_callsign: %s\n", msg.status.user_callsign);
            printf("      dest_callsign: %s\n", msg.status.dest_callsign);
            printf("      sync: %s\n", msg.status.sync ? "true" : "false");
            printf("      direction: %s\n", msg.status.dir == DIR_TX ? "tx" : "rx");
            printf("      client_tcp_connected: %s\n",
                   msg.status.client_tcp_connected ? "true" : "false");
            printf("      bytes_transmitted: %ld\n", msg.status.bytes_transmitted);
            printf("      bytes_received: %ld\n", msg.status.bytes_received);
            break;

        case MSG_SOUNDCARD_LIST:
            printf("   SOUNDCARD LIST:\n");
            printf("      selected: %s\n", msg.soundcard_list.selected);
            printf("      list: %s\n", msg.soundcard_list.list);
            break;

        case MSG_RADIO_LIST:
            printf("   RADIO LIST:\n");
            printf("      selected: %s\n", msg.radio_list.selected);
            printf("      list: %s\n", msg.radio_list.list);
            break;

        default:
            printf("   Unknown message type, raw: %s\n", buf);
            break;
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
        double snr = 5.0 - (rand() % 100) / 10.0;
        int sync = rand() % 2;
        modem_direction_t dir = (counter % 2) ? DIR_TX : DIR_RX;
        int client = rand() % 2;
        long bytes_transmitted = rand() % 100000;
        long bytes_received = rand() % 100000;

        udp_tx_send_status(&tx, bitrate, snr, "K1ABC", "N0XYZ", sync, dir, client,
                           bytes_transmitted, bytes_received);

        // Occasionally send a soundcard list
        if (counter % 3 == 0) {
            const char *soundcards[] = { "hw:0,0", "hw:1,0", "hw:2,0" };
            udp_tx_send_soundcard_list(&tx, "hw:1,0", soundcards, 3);
        }

        // Occasionally send a radio list
        if (counter % 5 == 0) {
            const char *radios[] = { "Radio A", "Radio B", "Radio C" };
            udp_tx_send_radio_list(&tx, "Radio B", radios, 3);
        }

        counter++;
        usleep(500 * 1000); // 500 ms
    }

    udp_tx_close(&tx);
    return 0;
}
#endif

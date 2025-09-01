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

#define _POSIX_C_SOURCE 200112L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include "ui_communication.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// ---------------------------- JSON utils ---------------------------------

// Escape a C string for JSON string value: writes into 'out' with max 'outsz' bytes.
// Returns number of bytes written (excluding terminating NUL), or -1 on overflow.
static int json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)in[i];
        const char *rep = NULL;
        char buf[7] = {0}; // for \u00XX if needed

        switch (c) {
            case '\"': rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if (c < 0x20) {
                    // Control chars -> \u00XX
                    snprintf(buf, sizeof(buf), "\\u%04X", c);
                    rep = buf;
                } else {
                    // Normal char
                    if (o + 1 >= outsz) return -1;
                    out[o++] = (char)c;
                    continue;
                }
        }
        size_t len = strlen(rep);
        if (o + len >= outsz) return -1;
        memcpy(out + o, rep, len);
        o += len;
    }

    if (o >= outsz) return -1;
    out[o] = '\0';
    return (int)o;
}

// Build a simple JSON object from key/value C strings into 'out'.
// Pairs are passed as: key1, value1, key2, value2, ..., NULL.
// Returns length, or -1 on error/overflow.
static int json_build_pairs(char *out, size_t outsz, ...) {
    if (outsz == 0) return -1;
    out[0] = '\0';

    va_list ap;
    va_start(ap, outsz);

    size_t o = 0;
    if (o + 1 >= outsz) { va_end(ap); return -1; }
    out[o++] = '{';

    bool first = true;
    for (;;) {
        const char *k = va_arg(ap, const char *);
        if (!k) break; // terminator

        const char *v = va_arg(ap, const char *);
        if (!v) { va_end(ap); return -1; } // malformed call

        if (!first) {
            if (o + 1 >= outsz) { va_end(ap); return -1; }
            out[o++] = ',';
        }
        first = false;

        // Write "key":"value"
        char ke[512], ve[1024];
        if (json_escape(k, ke, sizeof(ke)) < 0 ||
            json_escape(v, ve, sizeof(ve)) < 0) {
            va_end(ap);
            return -1;
        }

        int n = snprintf(out + o, outsz - o, "\"%s\":\"%s\"", ke, ve);
        if (n < 0 || (size_t)n >= outsz - o) { va_end(ap); return -1; }
        o += (size_t)n;
    }

    if (o + 2 > outsz) { va_end(ap); return -1; }
    out[o++] = '}';
    out[o] = '\0';

    va_end(ap);
    return (int)o;
}

// ---------------------------- UDP TX -------------------------------------

// Initialize a UDP transmitter socket to the given IPv4 and port.
// Returns 0 on success, -1 on error.
int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port) {
    if (!tx || !ip) return -1;
    memset(tx, 0, sizeof(*tx));

    tx->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx->sock < 0) {
        perror("socket");
        return -1;
    }

    memset(&tx->dst, 0, sizeof(tx->dst));
    tx->dst.sin_family = AF_INET;
    tx->dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &tx->dst.sin_addr) != 1) {
        perror("inet_pton");
        close(tx->sock);
        tx->sock = -1;
        return -1;
    }

    return 0;
}

void udp_tx_close(udp_tx_t *tx) {
    if (tx && tx->sock >= 0) {
        close(tx->sock);
        tx->sock = -1;
    }
}

// Send a JSON object built from NULL-terminated key/value pairs.
// Call like: udp_tx_send_json_pairs(&tx, "type","greeting","msg","hello", NULL);
int udp_tx_send_json_pairs(udp_tx_t *tx, ...) {
    if (!tx || tx->sock < 0) return -1;

    char payload[2048];
    va_list ap;
    va_start(ap, tx);

    // Re-run the variadic list for builder (since we can't pass va_list twice easily,
    // we pull args into an array first).
    const char *kv[64]; // up to 32 pairs
    int count = 0;
    for (;;) {
        const char *k = va_arg(ap, const char *);
        if (!k) break;
        const char *v = va_arg(ap, const char *);
        if (!v) { va_end(ap); return -1; }
        if (count + 2 >= (int)ARRAY_SIZE(kv)) { va_end(ap); return -1; }
        kv[count++] = k;
        kv[count++] = v;
    }
    va_end(ap);

    // Rebuild a new va_list from the array to call json_build_pairs()
    // We can't truly rebuild va_list, so we just stitch the JSON manually here.
    // (Simpler: call json_build_pairs() directly with a small helper.)
    // We'll manually append pairs:
    size_t o = 0;
    payload[o++] = '{';
    for (int i = 0; i < count; i += 2) {
        if (i > 0) payload[o++] = ',';
        char ke[512], ve[1024];
        if (json_escape(kv[i], ke, sizeof(ke)) < 0) return -1;
        if (json_escape(kv[i+1], ve, sizeof(ve)) < 0) return -1;
        int n = snprintf(payload + o, sizeof(payload) - o, "\"%s\":\"%s\"", ke, ve);
        if (n < 0 || (size_t)n >= sizeof(payload) - o) return -1;
        o += (size_t)n;
    }
    if (o + 2 > sizeof(payload)) return -1;
    payload[o++] = '}';
    payload[o] = '\0';

    ssize_t sent = sendto(tx->sock, payload, o, 0,
                          (struct sockaddr *)&tx->dst, sizeof(tx->dst));
    if (sent < 0) {
        perror("sendto");
        return -1;
    }
    return 0;
}

// ---------------------------- UDP RX -------------------------------------


// --- helper: parse simple {"key":"value"} JSON ---
static int parse_json_pairs(const char *json,
                            char keys[][64], char vals[][128],
                            int max_pairs) {
    int count = 0;
    const char *p = json;

    while (*p && count < max_pairs) {
        while (*p && *p != '\"') p++;
        if (!*p) break;
        p++;
        const char *kstart = p;
        while (*p && *p != '\"') p++;
        if (!*p) break;
        int klen = p - kstart;
        if (klen >= 64) klen = 63;
        strncpy(keys[count], kstart, klen);
        keys[count][klen] = '\0';
        p++;

        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;

        while (*p && *p != '\"') p++;
        if (!*p) break;
        p++;
        const char *vstart = p;
        while (*p && *p != '\"') p++;
        if (!*p) break;
        int vlen = p - vstart;
        if (vlen >= 128) vlen = 127;
        strncpy(vals[count], vstart, vlen);
        vals[count][vlen] = '\0';
        p++;

        count++;
        while (*p && *p != ',' && *p != '}') p++;
        if (*p == ',') p++;
    }
    return count;
}

// --- helper: fill modem_status_t from parsed pairs ---
static void fill_modem_status(modem_status_t *st,
                              char keys[][64], char vals[][128],
                              int pairs) {
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < pairs; i++) {
        if (strcmp(keys[i], "bitrate") == 0) {
            st->bitrate = atoi(vals[i]);
        } else if (strcmp(keys[i], "snr") == 0) {
            st->snr = atof(vals[i]);
        } else if (strcmp(keys[i], "user_callsign") == 0) {
            strncpy(st->user_callsign, vals[i], sizeof(st->user_callsign) - 1);
        } else if (strcmp(keys[i], "dest_callsign") == 0) {
            strncpy(st->dest_callsign, vals[i], sizeof(st->dest_callsign) - 1);
        } else if (strcmp(keys[i], "sync") == 0) {
            st->sync = (strcmp(vals[i], "true") == 0) ? 1 : 0;
        } else if (strcmp(keys[i], "direction") == 0) {
            st->dir = (strcmp(vals[i], "tx") == 0) ? DIR_TX : DIR_RX;
        } else if (strcmp(keys[i], "client_tcp_connected") == 0) {
            st->client_tcp_connected = (strcmp(vals[i], "true") == 0) ? 1 : 0;
        }
    }
}

void *rx_thread_main(void *arg) {
    rx_args_t *cfg = (rx_args_t *)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[rx] socket");
        return NULL;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(cfg->listen_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("[rx] bind");
        close(sock);
        return NULL;
    }

    printf("[rx] Listening on UDP port %u (blocking)...\n", cfg->listen_port);

    for (;;) {
        char buf[4096];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &srclen);
        if (n <= 0) continue;
        buf[n] = '\0';

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));

        char keys[16][64];
        char vals[16][128];
        int pairs = parse_json_pairs(buf, keys, vals, 16);

        modem_status_t st;
        fill_modem_status(&st, keys, vals, pairs);

        printf("[rx] From %s:%u\n", ip, ntohs(src.sin_port));
        printf("   bitrate: %d bps\n", st.bitrate);
        printf("   snr: %.1f dB\n", st.snr);
        printf("   user_callsign: %s\n", st.user_callsign);
        printf("   dest_callsign: %s\n", st.dest_callsign);
        printf("   sync: %s\n", st.sync ? "true" : "false");
        printf("   direction: %s\n", (st.dir == DIR_TX) ? "tx" : "rx");
        printf("   client_tcp_connected: %s\n\n",
               st.client_tcp_connected ? "true" : "false");
        fflush(stdout);
    }
}


// ---------------------------- Demo main ----------------------------------
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s <tx_ip> <tx_port> <rx_port>\n"
                "Example: %s 127.0.0.1 9999 10000\n",
                argv[0], argv[0]);
        return 1;
    }

    const char *tx_ip = argv[1];
    int tx_port = atoi(argv[2]);
    int rx_port = atoi(argv[3]);

    // 1) Start receiver thread on rx_port
    pthread_t rx_thread;
    rx_args_t rxa = { .listen_port = (uint16_t)rx_port };

    if (pthread_create(&rx_thread, NULL, rx_thread_main, &rxa) != 0) {
        perror("pthread_create(rx)");
        return 1;
    }
    pthread_detach(rx_thread);

    // 2) Prepare transmitter
    udp_tx_t tx;
    if (udp_tx_init(&tx, tx_ip, (uint16_t)tx_port) != 0) {
        fprintf(stderr, "Failed to init transmitter\n");
        return 1;
    }

    srand((unsigned)time(NULL));

    // 3) Loop with status messages
    const char *callsign_user = "K1ABC";
    const char *callsign_dest = "N0XYZ";

    int counter = 0;
    while (1) {
        int bitrate = (rand() % 2) ? 1200 : 2400;
        double snr = 15.0 + (rand() % 100) / 10.0;
        const char *sync = (rand() % 2) ? "true" : "false";
        const char *direction = (counter % 2) ? "tx" : "rx";
        const char *client_tcp = (rand() % 3 == 0) ? "true" : "false";

        char snr_buf[32], br_buf[32];
        snprintf(snr_buf, sizeof(snr_buf), "%.1f", snr);
        snprintf(br_buf, sizeof(br_buf), "%d", bitrate);

        udp_tx_send_json_pairs(&tx,
                               "type", "status",
                               "bitrate", br_buf,
                               "snr", snr_buf,
                               "user_callsign", callsign_user,
                               "dest_callsign", callsign_dest,
                               "sync", sync,
                               "direction", direction,
                               "client_tcp_connected", client_tcp,
                               NULL);

        counter++;
        usleep(1000 * 500); // 500 ms
    }

    udp_tx_close(&tx);
    return 0;
}

#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ui_communication.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

typedef struct {
    int sock;
    struct sockaddr_in dst;
} udp_tx_t;

typedef struct {
    uint16_t listen_port;
} rx_args_t;

typedef struct {
    udp_tx_t tx;
    pthread_mutex_t tx_lock;
    bool tx_ready;
    bool logging;

    pthread_t rx_thread;
    int rx_sock;
    uint16_t rx_port;
    bool rx_running;
} ui_comm_context_t;

static ui_comm_context_t g_ctx = {
    .tx = { .sock = -1 },
    .tx_lock = PTHREAD_MUTEX_INITIALIZER,
    .tx_ready = false,
    .logging = false,
    .rx_thread = 0,
    .rx_sock = -1,
    .rx_port = 0,
    .rx_running = false
};

static void log_debug(const char *fmt, ...)
{
    if (!g_ctx.logging) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)in[i];
        const char *rep = NULL;
        char buf[7] = {0};

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
                    snprintf(buf, sizeof(buf), "\\u%04X", c);
                    rep = buf;
                } else {
                    if (o + 1 >= outsz)
                        return -1;
                    out[o++] = (char)c;
                    continue;
                }
        }

        size_t len = strlen(rep);
        if (o + len >= outsz)
            return -1;
        memcpy(out + o, rep, len);
        o += len;
    }

    if (o >= outsz)
        return -1;

    out[o] = '\0';
    return (int)o;
}

static int parse_json_pairs(const char *json,
                            char keys[][64], char vals[][128],
                            int max_pairs)
{
    int count = 0;
    const char *p = json;

    while (*p && count < max_pairs) {
        while (*p && *p != '\"')
            p++;
        if (!*p)
            break;
        p++;
        const char *kstart = p;
        while (*p && *p != '\"')
            p++;
        if (!*p)
            break;
        int klen = p - kstart;
        if (klen >= 64)
            klen = 63;
        strncpy(keys[count], kstart, klen);
        keys[count][klen] = '\0';
        p++;

        while (*p && *p != ':')
            p++;
        if (!*p)
            break;
        p++;

        while (*p && *p != '\"')
            p++;
        if (!*p)
            break;
        p++;
        const char *vstart = p;
        while (*p && *p != '\"')
            p++;
        if (!*p)
            break;
        int vlen = p - vstart;
        if (vlen >= 128)
            vlen = 127;
        strncpy(vals[count], vstart, vlen);
        vals[count][vlen] = '\0';
        p++;

        count++;
        while (*p && *p != ',' && *p != '}')
            p++;
        if (*p == ',')
            p++;
    }

    return count;
}

static void fill_modem_status(modem_status_t *st,
                              char keys[][64],
                              char vals[][128],
                              int pairs)
{
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < pairs; i++) {
        if (strcmp(keys[i], "bitrate") == 0) {
            st->bitrate = atoi(vals[i]);
        } else if (strcmp(keys[i], "snr") == 0) {
            st->snr = atof(vals[i]);
        } else if (strcmp(keys[i], "user_callsign") == 0) {
            snprintf(st->user_callsign, sizeof(st->user_callsign), "%.*s",
                     (int)sizeof(st->user_callsign) - 1, vals[i]);
        } else if (strcmp(keys[i], "dest_callsign") == 0) {
            snprintf(st->dest_callsign, sizeof(st->dest_callsign), "%.*s",
                     (int)sizeof(st->dest_callsign) - 1, vals[i]);
        } else if (strcmp(keys[i], "sync") == 0) {
            st->sync = (strcmp(vals[i], "true") == 0) ? 1 : 0;
        } else if (strcmp(keys[i], "direction") == 0) {
            st->dir = (strcmp(vals[i], "tx") == 0) ? DIR_TX : DIR_RX;
        } else if (strcmp(keys[i], "client_tcp_connected") == 0) {
            st->client_tcp_connected = (strcmp(vals[i], "true") == 0) ? 1 : 0;
        }
    }
}

static int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port)
{
    if (!tx || !ip)
        return -1;

    memset(tx, 0, sizeof(*tx));
    tx->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx->sock < 0) {
        perror("socket");
        return -1;
    }

    memset(&tx->dst, 0, sizeof(tx->dst));
    tx->dst.sin_family = AF_INET;
    tx->dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &tx->dst.sin_addr) != 1) {
        perror("inet_pton");
        close(tx->sock);
        tx->sock = -1;
        return -1;
    }

    return 0;
}

static void udp_tx_close(udp_tx_t *tx)
{
    if (tx && tx->sock >= 0) {
        close(tx->sock);
        tx->sock = -1;
    }
}

static int udp_tx_send_json_pairs(udp_tx_t *tx, ...)
{
    if (!tx || tx->sock < 0)
        return -1;

    char payload[2048];
    va_list ap;
    va_start(ap, tx);

    const char *kv[64];
    int count = 0;
    while (1) {
        const char *k = va_arg(ap, const char *);
        if (!k)
            break;
        const char *v = va_arg(ap, const char *);
        if (!v) {
            va_end(ap);
            return -1;
        }
        if (count + 2 >= (int)ARRAY_SIZE(kv)) {
            va_end(ap);
            return -1;
        }
        kv[count++] = k;
        kv[count++] = v;
    }
    va_end(ap);

    size_t o = 0;
    payload[o++] = '{';
    for (int i = 0; i < count; i += 2) {
        if (i > 0)
            payload[o++] = ',';
        char ke[256], ve[512];
        if (json_escape(kv[i], ke, sizeof(ke)) < 0)
            return -1;
        if (json_escape(kv[i + 1], ve, sizeof(ve)) < 0)
            return -1;
        int n = snprintf(payload + o, sizeof(payload) - o,
                         "\"%s\":\"%s\"", ke, ve);
        if (n < 0 || (size_t)n >= sizeof(payload) - o)
            return -1;
        o += (size_t)n;
    }

    if (o + 2 > sizeof(payload))
        return -1;

    payload[o++] = '}';
    payload[o] = '\0';

    ssize_t sent = sendto(tx->sock, payload, o, 0,
                          (struct sockaddr *)&tx->dst,
                          sizeof(tx->dst));
    if (sent < 0) {
        perror("sendto");
        return -1;
    }

    log_debug("[ui] Sent %zd bytes: %s\n", sent, payload);
    return 0;
}

static void *rx_thread_main(void *arg)
{
    rx_args_t *cfg = (rx_args_t *)arg;
    uint16_t listen_port = cfg ? cfg->listen_port : 0;
    free(cfg);

    if (listen_port == 0) {
        return NULL;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ui][rx] socket");
        return NULL;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(listen_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[ui][rx] bind");
        close(sock);
        return NULL;
    }

    g_ctx.rx_sock = sock;
    g_ctx.rx_running = true;

    log_debug("[ui][rx] Listening on UDP port %u\n", listen_port);

    while (g_ctx.rx_running) {
        char buf[2048];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &srclen);
        if (n <= 0) {
            if (!g_ctx.rx_running)
                break;
            continue;
        }

        buf[n] = '\0';
        char keys[16][64];
        char vals[16][128];
        int pairs = parse_json_pairs(buf, keys, vals, 16);

        modem_status_t status;
        fill_modem_status(&status, keys, vals, pairs);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));

        log_debug("[ui][rx] From %s:%u -> bitrate=%d snr=%.1f dir=%s\n",
                  ip,
                  ntohs(src.sin_port),
                  status.bitrate,
                  status.snr,
                  status.dir == DIR_TX ? "tx" : "rx");
    }

    close(sock);
    g_ctx.rx_sock = -1;
    g_ctx.rx_running = false;
    return NULL;
}

static void ui_comm_stop_rx_thread(void)
{
    if (!g_ctx.rx_running)
        return;

    g_ctx.rx_running = false;
    if (g_ctx.rx_sock >= 0) {
        shutdown(g_ctx.rx_sock, SHUT_RDWR);
        close(g_ctx.rx_sock);
        g_ctx.rx_sock = -1;
    }

    if (g_ctx.rx_thread) {
        pthread_join(g_ctx.rx_thread, NULL);
        g_ctx.rx_thread = 0;
    }
}

int ui_comm_init(const char *tx_ip, uint16_t tx_port, uint16_t rx_port)
{
    if (!tx_ip)
        tx_ip = "127.0.0.1";

    if (tx_port == 0)
        tx_port = 9999;

    if (udp_tx_init(&g_ctx.tx, tx_ip, tx_port) != 0) {
        fprintf(stderr, "[ui] Failed to initialize UDP transmitter\n");
        return -1;
    }

    g_ctx.tx_ready = true;

    if (rx_port != 0) {
        g_ctx.rx_port = rx_port;
        rx_args_t *args = (rx_args_t *)malloc(sizeof(*args));
        if (!args) {
            perror("[ui] malloc");
            udp_tx_close(&g_ctx.tx);
            g_ctx.tx_ready = false;
            return -1;
        }
        args->listen_port = rx_port;

        if (pthread_create(&g_ctx.rx_thread, NULL, rx_thread_main, args) != 0) {
            perror("[ui] pthread_create");
            free(args);
            udp_tx_close(&g_ctx.tx);
            g_ctx.tx_ready = false;
            return -1;
        }
    }

    return 0;
}

void ui_comm_shutdown(void)
{
    ui_comm_stop_rx_thread();

    if (g_ctx.tx_ready) {
        udp_tx_close(&g_ctx.tx);
        g_ctx.tx_ready = false;
    }
}

bool ui_comm_is_enabled(void)
{
    return g_ctx.tx_ready;
}

void ui_comm_set_logging(bool enabled)
{
    g_ctx.logging = enabled;
}

int ui_comm_send_status(const modem_status_t *status)
{
    if (!status || !g_ctx.tx_ready)
        return -1;

    char bitrate_buf[32];
    char snr_buf[32];
    char tx_bytes_buf[32];
    char rx_bytes_buf[32];

    snprintf(bitrate_buf, sizeof(bitrate_buf), "%d", status->bitrate);
    snprintf(snr_buf, sizeof(snr_buf), "%.1f", status->snr);
    snprintf(tx_bytes_buf, sizeof(tx_bytes_buf), "%ld",
             status->bytes_transmitted);
    snprintf(rx_bytes_buf, sizeof(rx_bytes_buf), "%ld",
             status->bytes_received);

    const char *sync = status->sync ? "true" : "false";
    const char *dir = status->dir == DIR_TX ? "tx" : "rx";
    const char *client = status->client_tcp_connected ? "true" : "false";

    pthread_mutex_lock(&g_ctx.tx_lock);
    int ret = udp_tx_send_json_pairs(
        &g_ctx.tx,
        "type", "status",
        "bitrate", bitrate_buf,
        "snr", snr_buf,
        "user_callsign", status->user_callsign,
        "dest_callsign", status->dest_callsign,
        "sync", sync,
        "direction", dir,
        "client_tcp_connected", client,
        "bytes_transmitted", tx_bytes_buf,
        "bytes_received", rx_bytes_buf,
        NULL);
    pthread_mutex_unlock(&g_ctx.tx_lock);

    return ret;
}

#ifdef UI_COMM_TEST_MAIN
int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <tx_ip> <tx_port> [rx_port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *tx_ip = argv[1];
    uint16_t tx_port = (uint16_t)atoi(argv[2]);
    uint16_t rx_port = (argc == 4) ? (uint16_t)atoi(argv[3]) : 0;

    if (ui_comm_init(tx_ip, tx_port, rx_port) != 0)
        return EXIT_FAILURE;

    ui_comm_set_logging(true);

    srand((unsigned)time(NULL));
    for (int counter = 0;; counter++) {
        modem_status_t status = {
            .bitrate = (counter % 2) ? 1200 : 2400,
            .snr = 10.0 + (rand() % 50) / 10.0,
            .sync = counter % 2,
            .dir = (counter % 2) ? DIR_TX : DIR_RX,
            .client_tcp_connected = (counter % 3 == 0),
            .bytes_transmitted = counter * 128,
            .bytes_received = counter * 256
        };
        strncpy(status.user_callsign, "K1ABC", sizeof(status.user_callsign) - 1);
        strncpy(status.dest_callsign, "N0XYZ", sizeof(status.dest_callsign) - 1);

        ui_comm_send_status(&status);
        usleep(500 * 1000);
    }

    ui_comm_shutdown();
    return EXIT_SUCCESS;
}
#endif

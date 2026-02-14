#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "crc6.h"
#include "kiss.h"

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT 8100
#define CONFIG_PACKET_SIZE 9

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static int create_tcp_socket(const char *ip, int port)
{
    int tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0)
    {
        perror("Failed to create TCP socket");
        return -1;
    }

    struct sockaddr_in modem_addr;
    memset(&modem_addr, 0, sizeof(modem_addr));
    modem_addr.sin_family = AF_INET;
    modem_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &modem_addr.sin_addr) <= 0)
    {
        perror("Invalid modem IP address");
        close(tcp_socket);
        return -1;
    }

    if (connect(tcp_socket, (struct sockaddr *)&modem_addr, sizeof(modem_addr)) < 0)
    {
        perror("Failed to connect to modem");
        close(tcp_socket);
        return -1;
    }

    return tcp_socket;
}

static uint16_t crc_len_for_type(uint8_t packet_type, size_t frame_size)
{
    if (packet_type == 0x02 && frame_size >= CONFIG_PACKET_SIZE)
        return CONFIG_PACKET_SIZE - 1;
    if (frame_size > 0)
        return frame_size - 1;
    return 0;
}

static void print_frame_debug(uint64_t frame_no, const uint8_t *frame, size_t frame_size)
{
    if (frame_size == 0)
    {
        printf("[RX] frame=%llu EMPTY\n", (unsigned long long)frame_no);
        return;
    }

    uint8_t packet_type = (frame[0] >> 6) & 0x3;
    uint8_t crc_local = frame[0] & 0x3f;
    uint16_t crc_len = crc_len_for_type(packet_type, frame_size);
    uint8_t crc_calc = (uint8_t)crc6_0X6F(1, frame + 1, crc_len);
    bool crc_ok = (crc_local == crc_calc);

    printf("[RX] frame=%llu len=%zu type=0x%02x crc(local=0x%02x calc=0x%02x %s) first16=",
           (unsigned long long)frame_no, frame_size, packet_type, crc_local, crc_calc, crc_ok ? "OK" : "BAD");
    for (size_t i = 0; i < frame_size && i < 16; i++)
    {
        printf("%02x ", frame[i]);
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    const char *ip = (argc > 1) ? argv[1] : DEFAULT_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int tcp_socket = create_tcp_socket(ip, port);
    if (tcp_socket < 0)
        return EXIT_FAILURE;

    printf("Connected to %s:%d\n", ip, port);

    uint8_t rx_buf[4096];
    uint8_t frame_buf[MAX_PAYLOAD];
    uint64_t frame_no = 0;
    uint64_t t02 = 0, t03 = 0, bad_crc = 0;

    while (running)
    {
        ssize_t n = recv(tcp_socket, rx_buf, sizeof(rx_buf), 0);
        if (n == 0)
        {
            printf("Server disconnected\n");
            break;
        }
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            perror("recv failed");
            break;
        }

        printf("[RX] raw_bytes=%zd\n", n);
        for (ssize_t i = 0; i < n; i++)
        {
            int frame_len = kiss_read(rx_buf[i], frame_buf);
            if (frame_len <= 0)
                continue;

            frame_no++;
            print_frame_debug(frame_no, frame_buf, (size_t)frame_len);

            uint8_t packet_type = (frame_buf[0] >> 6) & 0x3;
            uint8_t crc_local = frame_buf[0] & 0x3f;
            uint8_t crc_calc = (uint8_t)crc6_0X6F(1, frame_buf + 1, crc_len_for_type(packet_type, (size_t)frame_len));
            if (packet_type == 0x02) t02++;
            if (packet_type == 0x03) t03++;
            if (crc_local != crc_calc) bad_crc++;

            if ((frame_no % 20) == 0)
            {
                printf("[RX-SUM] frames=%llu type02=%llu type03=%llu bad_crc=%llu\n",
                       (unsigned long long)frame_no,
                       (unsigned long long)t02,
                       (unsigned long long)t03,
                       (unsigned long long)bad_crc);
            }
        }
    }

    close(tcp_socket);
    printf("broadcast_diag_rx terminated\n");
    return EXIT_SUCCESS;
}

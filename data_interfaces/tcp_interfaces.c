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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>

#include "tcp_interfaces.h"
#include "ring_buffer_posix.h"
#include "net.h"
#include "arq.h"
#include "fsm.h"
#include "defines_modem.h"
#include "modem.h"
#include "kiss.h"

static pthread_t tid[7];

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_rx_buffer_arq;

extern cbuf_handle_t data_tx_buffer_broadcast;
extern cbuf_handle_t data_rx_buffer_broadcast;

extern bool shutdown_;

extern arq_info arq_conn;
extern fsm_handle arq_fsm;

// For now, we turn on verbosity for debugging purposes
#define DEBUG

static ssize_t send_all(int socket_fd, const uint8_t *buffer, size_t len)
{
    size_t total_sent = 0;

    while (total_sent < len)
    {
        ssize_t sent = send(socket_fd, buffer + total_sent, len - total_sent, 0);
        if (sent <= 0)
        {
            return -1;
        }
        total_sent += (size_t)sent;
    }

    return (ssize_t)total_sent;
}

/********** ARQ TCP ports INTERFACES **********/
void *server_worker_thread_ctl(void *port)
{
    int tcp_base_port = *((int *) port);
    int socket;
    
    while(!shutdown_)
    {
        int ret = tcp_open(tcp_base_port, CTL_TCP_PORT);

        if (ret < 0)
        {
            fprintf(stderr, "Could not open TCP port %d\n", tcp_base_port);
            shutdown_ = true;
        }
        
        socket = listen4connection(CTL_TCP_PORT);

        if (socket < 0)
        {
            status_ctl = NET_RESTART;
            tcp_close(CTL_TCP_PORT);
            continue;
        }

        fsm_dispatch(&arq_fsm, EV_CLIENT_CONNECT);
        
        // TODO: pthread wait here?
        while (status_ctl == NET_CONNECTED)
            sleep(1);

        // inform the data thread
        if (status_data == NET_CONNECTED)
            status_data = NET_RESTART;

        fsm_dispatch(&arq_fsm, EV_CLIENT_DISCONNECT);
        
        tcp_close(CTL_TCP_PORT);
    }

    return NULL;
    
}

void *server_worker_thread_data(void *port)
{
    int tcp_base_port = *((int *) port);
    int socket;

    while(!shutdown_)
    {
        int ret = tcp_open(tcp_base_port+1, DATA_TCP_PORT);

        if (ret < 0)
        {
            fprintf(stderr, "Could not open TCP port %d\n", tcp_base_port+1);
            shutdown_ = true;
        }
        
        socket = listen4connection(DATA_TCP_PORT);

        if (socket < 0)
        {
            status_data = NET_RESTART;
            tcp_close(DATA_TCP_PORT);
            continue;
        }

        // pthread wait here?
        while (status_data == NET_CONNECTED)
            sleep(1);

        tcp_close(DATA_TCP_PORT);
    }
    
    return NULL;
}

// tx to tcp socket the received data from the modem
void *data_worker_thread_tx(void *conn)
{
    uint8_t *buffer = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    
    while(!shutdown_)
    {
        if (status_data != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        size_t n = read_buffer_all(data_rx_buffer_arq, buffer);

        ssize_t i = tcp_write(DATA_TCP_PORT, buffer, n);

        if (i < (ssize_t) n)
            fprintf(stderr, "Problems in data_worker_thread_tx!\n");
    }

    free(buffer);
    
    return NULL;
}

// rx from tcp socket and send to trasmit by the modem
void *data_worker_thread_rx(void *conn)
{
    uint8_t *buffer = (uint8_t *) malloc(TCP_BLOCK_SIZE);

    while(!shutdown_)
    {
        if (status_data != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        int n = tcp_read(DATA_TCP_PORT, buffer, TCP_BLOCK_SIZE);

        write_buffer(data_tx_buffer_arq, buffer, n);
    }

    free(buffer);
    return NULL;
}

void *control_worker_thread_tx(void *conn)
{
    int counter = 0;
    char imalive[] = "IAMALIVE\r";

    while(!shutdown_)
    {
        if (status_ctl != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        // ok ok ... this is not a good way to do this, but whatever, nobody is syncing clock with this info
        if (counter == 60)
        {
            counter = 0;
            tcp_write(CTL_TCP_PORT, (uint8_t *)imalive, strlen(imalive));

        }

        sleep(1);
        counter++;
    }
    
    return NULL;
}

void *control_worker_thread_rx(void *conn)
{
    char *buffer = (char *) malloc(TCP_BLOCK_SIZE+1);
    char temp[16];
    int count = 0;

    memset(buffer, 0, TCP_BLOCK_SIZE+1);

    while(!shutdown_)
    {
        if (status_ctl != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        int n = tcp_read(CTL_TCP_PORT, (uint8_t *)buffer + count, 1);

        if (n < 0)
        {
            count = 0;
            fprintf(stderr, "ERROR ctl socket reading\n");            
            status_ctl = NET_RESTART;
            continue;
        }

        if (buffer[count] != '\r')
        {
            count++;
            continue;
        }

        // we found the '\r'
        buffer[count] = 0;

        if (count >= TCP_BLOCK_SIZE)
        {
            count = 0;
            fprintf(stderr, "ERROR in command parsing\n");
            continue;
        }

        count = 0;
#ifdef DEBUG
        fprintf(stderr,"Command received: %s\n", buffer);  
#endif
        
        // now we parse the commands
        if (!memcmp(buffer, "MYCALL", strlen("MYCALL")))
        {
            sscanf(buffer,"MYCALL %s", arq_conn.my_call_sign);
            goto send_ok;
        }
        
        if (!memcmp(buffer, "LISTEN", strlen("LISTEN")))
        {
            sscanf(buffer,"LISTEN %s", temp);
            if (temp[1] == 'N') // ON
            {
                fsm_dispatch(&arq_fsm, EV_START_LISTEN);
            }
            if (temp[1] == 'F') // OFF
            {
                fsm_dispatch(&arq_fsm, EV_STOP_LISTEN);
            }

            goto send_ok;
        }

        if (!memcmp(buffer, "PUBLIC", strlen("PUBLIC")))
        {
            sscanf(buffer,"PUBLIC %s", temp);
            if (temp[1] == 'N') // ON
                arq_conn.encryption = false;
            if (temp[1] == 'F') // OFF
               arq_conn.encryption = true;
            
            goto send_ok;
        }

        if (!memcmp(buffer, "BW", strlen("BW")))
        {
            sscanf(buffer,"BW%d", &arq_conn.bw);
            goto send_ok;
        }

        if (!memcmp(buffer, "CONNECT", strlen("CONNECT")))
        {
            sscanf(buffer,"CONNECT %s %s", arq_conn.src_addr, arq_conn.dst_addr);
            fsm_dispatch(&arq_fsm, EV_LINK_CALL_REMOTE);
            goto send_ok;
        }

        if (!memcmp(buffer, "DISCONNECT", strlen("DISCONNECT")))
        {   
            fsm_dispatch(&arq_fsm, EV_LINK_DISCONNECT);
            goto send_ok;
        }

        fprintf(stderr, "Unknown command\n");
        tcp_write(CTL_TCP_PORT, (uint8_t *) "WRONG\r", 6);
        continue;
        
    send_ok:
        tcp_write(CTL_TCP_PORT, (uint8_t *) "OK\r", 3);

    }

    free(buffer);

    return NULL;
}


/********** BROADCAST TCP port INTERFACE **********/
void *send_thread(void *client_socket_ptr)
{
    int client_socket = *((int *)client_socket_ptr);
    size_t frame_size = modem_get_payload_bytes_per_frame();
    uint8_t *frame_buffer = NULL;
    uint8_t *kiss_buffer = NULL;

    if (frame_size == 0 || frame_size > MAX_PAYLOAD)
    {
        fprintf(stderr, "Invalid broadcast frame size: %zu\n", frame_size);
        return NULL;
    }

    frame_buffer = (uint8_t *)malloc(frame_size);
    kiss_buffer = (uint8_t *)malloc((frame_size * 2) + 3);

    if (!frame_buffer || !kiss_buffer)
    {
        fprintf(stderr, "Failed to allocate memory for send buffer.\n");
        free(frame_buffer);
        free(kiss_buffer);
        return NULL;
    }

    while (!shutdown_)
    {
        if (read_buffer(data_rx_buffer_broadcast, frame_buffer, frame_size) < 0)
            break;

        int kiss_len = kiss_write_frame(frame_buffer, (int)frame_size, kiss_buffer);
        if (send_all(client_socket, kiss_buffer, (size_t)kiss_len) < 0)
        {
            perror("Error sending KISS broadcast frame");
            break;
        }
    }

    free(frame_buffer);
    free(kiss_buffer);
    return NULL;
}

void *recv_thread(void *client_socket_ptr)
{
    int client_socket = *((int *)client_socket_ptr);
    size_t frame_size = modem_get_payload_bytes_per_frame();
    uint8_t *buffer = (uint8_t *)malloc(DATA_TX_BUFFER_SIZE);
    uint8_t decoded_frame[MAX_PAYLOAD];

    if (frame_size == 0 || frame_size > MAX_PAYLOAD)
    {
        fprintf(stderr, "Invalid broadcast frame size: %zu\n", frame_size);
        return NULL;
    }

    if (!buffer)
    {
        fprintf(stderr, "Failed to allocate memory for recv buffer.\n");
        return NULL;
    }

    while (!shutdown_)
    {
        ssize_t received = recv(client_socket, buffer, DATA_TX_BUFFER_SIZE, 0);
        if (received > 0)
        {
            for (ssize_t i = 0; i < received; i++)
            {
                int frame_len = kiss_read(buffer[i], decoded_frame);
                if (frame_len <= 0)
                    continue;

                if ((size_t)frame_len != frame_size)
                {
                    fprintf(stderr, "Discarding broadcast frame with unexpected size %d (expected %zu)\n",
                            frame_len, frame_size);
                    continue;
                }

                write_buffer(data_tx_buffer_broadcast, decoded_frame, frame_size);
            }
        }
        else if (received == 0)
        {
            printf("Client disconnected.\n");
            break;
        }
        else if (received < 0)
        {
            perror("Error receiving TCP data");
            break;
        }
    }

    free(buffer);
    return NULL;
}


// Function to handle TCP server logic
void *tcp_server_thread(void *port_ptr)
{
    int tcp_port = *((int *)port_ptr);
    int tcp_socket, client_socket;
    struct sockaddr_in local_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Open TCP socket
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0)
    {
        perror("Failed to create TCP socket");
        return NULL;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(tcp_port);

    if (bind(tcp_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        perror("Failed to bind TCP socket");
        close(tcp_socket);
        return NULL;
    }

    if (listen(tcp_socket, 1) < 0)
    {
        perror("Failed to listen on TCP socket");
        close(tcp_socket);
        return NULL;
    }

    printf("Waiting for a client to connect...\n");

    while (!shutdown_)
    {
        client_socket = accept(tcp_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0)
        {
            perror("Failed to accept client connection");
            if (shutdown_)
                break; // Exit if shutdown flag is set
            continue; // Retry accepting a connection
        }

        printf("Client connected.\n");

        pthread_t recv_tid, send_tid;

        // Create threads for receiving and sending data
        pthread_create(&recv_tid, NULL, recv_thread, (void *)&client_socket);
        pthread_create(&send_tid, NULL, send_thread, (void *)&client_socket);

        // Wait for threads to finish
        pthread_join(recv_tid, NULL);
        pthread_cancel(send_tid);
        pthread_join(send_tid, NULL);

        close(client_socket);
        printf("Waiting for a new client to connect...\n");
    }

    close(tcp_socket);
    return NULL;
}

/*********** TNC / radio functions ***********/
char *get_timestamp()
{
    static char buffer[32];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm = localtime(&tv.tv_sec);
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03ld\n", tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec / 1000);
    return buffer;
}

void ptt_on()
{
    char buffer[] = "PTT ON\r";
    arq_conn.TRX = TX;
    tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
    // print timestamp with miliseconds precision
#ifdef DEBUG
    printf("PTT ON %s", get_timestamp());
#endif
}

void ptt_off()
{
    char buffer[] = "PTT OFF\r";
    arq_conn.TRX = RX;
    tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
    // print timestamp with miliseconds precision
#ifdef DEBUG
    printf("PTT OFF %s", get_timestamp());
#endif
}

void tnc_send_connected()
{
    char buffer[128];
    sprintf(buffer, "CONNECTED %s %s %d\r", arq_conn.my_call_sign, arq_conn.dst_addr, 2300);
    ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
    if (i < 0)
        printf("Error sending connected message: %s\n", strerror(errno));
}

void tnc_send_disconnected()
{
    char buffer[128];
    sprintf(buffer, "DISCONNECTED\r");
    ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
    if (i < 0)
        printf("Error sending disconnected message: %s\n", strerror(errno));
}

int interfaces_init(int arq_tcp_base_port, int broadcast_tcp_port)
{

    /*************** ARQ TCP ports *******************/
    status_ctl = NET_NONE;
    status_data = NET_NONE;

    // here is the thread that runs the accept(), each per port, and mantains the
    // state of the connection
    pthread_create(&tid[0], NULL, server_worker_thread_ctl, (void *) &arq_tcp_base_port);
    pthread_create(&tid[1], NULL, server_worker_thread_data, (void *) &arq_tcp_base_port);
    
    // control channel threads
    pthread_create(&tid[2], NULL, control_worker_thread_rx, (void *) NULL);
    pthread_create(&tid[3], NULL, control_worker_thread_tx, (void *) NULL);

    // data channel threads
    pthread_create(&tid[4], NULL, data_worker_thread_tx, (void *) NULL);
    pthread_create(&tid[5], NULL, data_worker_thread_rx, (void *) NULL);


    /*************** BROADCAST TCP ports **************/
    // Create TCP BROADCAST server thread
    pthread_create(&tid[6], NULL, tcp_server_thread, (void *)&broadcast_tcp_port);

    
    return EXIT_SUCCESS;
}

void interfaces_shutdown()
{
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
    pthread_join(tid[2], NULL);
    pthread_join(tid[3], NULL);
    pthread_join(tid[4], NULL);
    pthread_join(tid[5], NULL);
    pthread_join(tid[6], NULL);
}

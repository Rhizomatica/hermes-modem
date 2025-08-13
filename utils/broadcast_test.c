#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

#include "kiss.h"

#define BUFFER_SIZE 8192

int shutdown_ = 0;

int create_tcp_socket(const char *ip, int port)
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

    printf("Connected to modem at %s:%d\n", ip, port);
    return tcp_socket;
}

void *receive_thread(void *socket_ptr)
{
    int tcp_socket = *((int *)socket_ptr);
    char buffer[BUFFER_SIZE];
    char decoded_buffer[BUFFER_SIZE];

    while (1)
    {
        ssize_t received = recv(tcp_socket, buffer, BUFFER_SIZE, 0);
        if (received > 0)
        {
            // KISS framing processing
            for (ssize_t i = 0; i < received; i++)
            {
                int frame_len = kiss_read(buffer[i], (uint8_t *)decoded_buffer);
                if (frame_len > 0)
                {
                    // Successfully read a frame
                    printf("\rReceived %d bytes:\n", frame_len);
                    for (int j = 0; j < frame_len; j++)
                    {
                        printf("%c", (buffer[j]));
                    }
                    
                    printf("\n> ");
                }
            }
            printf("\rReceived %zd bytes: %s\n> ", received, buffer);
        }
        else if (received == 0)
        {
            printf("\nConnection closed by modem.\n");
            break;
        }
        else if (received < 0)
        {
            perror("\nError receiving data");
            break;
        }
    }

    shutdown_ = 1;
    return NULL;
}


int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <IP> <PORT>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int tcp_socket = create_tcp_socket(ip, port);
    if (tcp_socket < 0)
    {
        return EXIT_FAILURE;
    }

    pthread_t recv_tid;
    // Start the receive thread
    pthread_create(&recv_tid, NULL, receive_thread, (void *)&tcp_socket);

    char send_buffer[BUFFER_SIZE];
    char write_buffer[BUFFER_SIZE * 2 + 3]; // Adjusted size for KISS framing
    printf("Enter data to send (type 'exit' to quit):\n");

    while (1)
    {
        // Get user input
        printf("> ");
        fgets(send_buffer, BUFFER_SIZE, stdin);

        if (shutdown_)
        {
            printf("Connection closed. Exiting...\n");
            break;
        }
        
        send_buffer[strcspn(send_buffer, "\n")] = '\0'; // Remove newline character

        int kiss_frame_size = kiss_write_frame((uint8_t *)send_buffer, strlen(send_buffer), (uint8_t *)write_buffer);

        // Exit if user types "exit"
        if (strcmp(send_buffer, "exit") == 0)
        {
            break;
        }

        // Send data to modem
        ssize_t sent = send(tcp_socket, write_buffer, kiss_frame_size, 0);
        if (sent < 0)
        {
            perror("Failed to send data");
            break;
        }
        else
        {
            printf("Sent %d bytes: %s\n", kiss_frame_size, write_buffer);
        }
    }

    // Wait for the receive thread to finish
    pthread_cancel(recv_tid); // Cancel the receive thread
    pthread_join(recv_tid, NULL);

    close(tcp_socket);
    printf("TCP client terminated.\n");
    return EXIT_SUCCESS;
}

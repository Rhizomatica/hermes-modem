/* HERMES ARQ Test Client
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * Simple test client for ARQ protocol testing via TCP
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_ARQ_CTL_PORT 8300
#define DEFAULT_ARQ_DATA_PORT 8301
#define BUFFER_SIZE 4096

int main(int argc, char *argv[])
{
    int ctl_socket, data_socket;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char command[256];
    int port = DEFAULT_ARQ_CTL_PORT;

    if (argc > 1)
    {
        port = atoi(argv[1]);
    }

    printf("HERMES ARQ Test Client\n");
    printf("======================\n\n");
    printf("Connecting to ARQ server on port %d (control) and %d (data)\n", port, port + 1);

    // Connect control socket
    ctl_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ctl_socket < 0)
    {
        perror("Socket creation failed");
        return EXIT_FAILURE;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0)
    {
        perror("Invalid address");
        return EXIT_FAILURE;
    }

    if (connect(ctl_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connection failed");
        return EXIT_FAILURE;
    }

    printf("Connected to control port\n");

    // Connect data socket
    data_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (data_socket < 0)
    {
        perror("Data socket creation failed");
        close(ctl_socket);
        return EXIT_FAILURE;
    }

    server_addr.sin_port = htons(port + 1);
    if (connect(data_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Data connection failed");
        close(ctl_socket);
        close(data_socket);
        return EXIT_FAILURE;
    }

    printf("Connected to data port\n\n");

    printf("Available commands:\n");
    printf("  MYCALL <callsign>     - Set your callsign\n");
    printf("  LISTEN ON/OFF         - Enable/disable listening\n");
    printf("  CONNECT <src> <dst>   - Connect to remote station\n");
    printf("  DISCONNECT            - Disconnect current link\n");
    printf("  PUBLIC ON/OFF         - Enable/disable public mode\n");
    printf("  BW <hz>               - Set bandwidth\n");
    printf("  SEND <message>         - Send data message\n");
    printf("  QUIT                  - Exit\n\n");

    while (1)
    {
        printf("ARQ> ");
        fflush(stdout);

        if (!fgets(command, sizeof(command), stdin))
        {
            break;
        }

        // Remove newline
        command[strcspn(command, "\n")] = 0;

        if (strlen(command) == 0)
        {
            continue;
        }

        if (strncmp(command, "QUIT", 4) == 0)
        {
            break;
        }

        if (strncmp(command, "SEND", 4) == 0)
        {
            // Send data via data socket
            char *message = command + 5;
            if (strlen(message) > 0)
            {
                send(data_socket, message, strlen(message), 0);
                printf("Sent: %s\n", message);
            }
            continue;
        }

        // Send command via control socket
        strcat(command, "\r");
        send(ctl_socket, command, strlen(command), 0);

        // Read response
        int n = recv(ctl_socket, buffer, sizeof(buffer) - 1, 0);
        if (n > 0)
        {
            buffer[n] = 0;
            printf("%s", buffer);
        }
        else if (n == 0)
        {
            printf("Server disconnected\n");
            break;
        }
    }

    close(ctl_socket);
    close(data_socket);
    printf("Disconnected\n");

    return EXIT_SUCCESS;
}

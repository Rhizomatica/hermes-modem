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

#include <sys/time.h>


#include "arq.h"
#include "fsm.h"
#include "../audioio/audioio.h"
#include "net.h"
#include "defines.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

cbuf_handle_t data_tx_buffer;
cbuf_handle_t data_rx_buffer;

extern bool shutdown_;

// ARQ definitions
arq_info arq_conn;

#define DEBUG

static pthread_t tid[8];


// our simple fsm struct
fsm_handle arq_fsm;

/* FSM States */
void state_no_connected_client(int event)
{   
    printf("FSM State: no_connected_client\n");

    switch(event)
    {
    case EV_CLIENT_CONNECT:
        clear_connection_data();
        arq_fsm.current = state_idle;
        break;
    default:
        printf("Event: %d ignored in state_no_connected_client().\n", event);
    }
    return;
}

void state_link_connected(int event)
{
    printf("FSM State: link_connected\n");

    switch(event)
    {
    case EV_CLIENT_DISCONNECT:
        arq_fsm.current = state_no_connected_client;
        break;

    case EV_LINK_DISCONNECT:
        arq_fsm.current = (arq_conn.listen == true)? state_listen : state_idle;
        break;
    default:
        printf("Event: %d ignored in state_disconnected().\n", event);
    }
    return;
}


void state_listen(int event)
{
    printf("FSM State: listen\n");
    
    switch(event)
    {
    case EV_START_LISTEN:
        printf("EV_START_LISTEN ignored in state_listen() - already listening.\n");   
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        arq_fsm.current = state_idle;
        break;
    case EV_LINK_CALL_REMOTE:
        call_remote();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_LINK_DISCONNECT:
        printf("EV_LINK_DISCONNECT ignored in state_listen() - not connected.\n");   
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    case EV_LINK_INCOMING_CALL:
        callee_accept_connection(); //packets created to anwser in case of incoming call with correct callsign
        arq_fsm.current = state_connecting_callee;
        break;
    default:
        printf("Event: %d ignored in state_listen().\n", event);
    }

    return;
}

void state_idle(int event)
{
    printf("FSM State: idle\n");
    
    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        arq_fsm.current = state_listen;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        printf("EV_STOP_LISTEN ignored in state_idle() - already stopped.\n");   
        break;
    case EV_LINK_CALL_REMOTE:
        call_remote();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_LINK_DISCONNECT:
        printf("EV_LINK_DISCONNECT ignored in state_idle() - not connected.\n");   
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    default:
        printf("Event: %d ignored from state_idle\n", event);
    }
    
    return;
}

void state_connecting_caller(int event)
{
    printf("FSM State: connecting_caller\n");
    
    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_CALL_REMOTE:
        printf("EV_LINK_CALL_REMOTE ignored in state_connecting_caller() - already connecting.\n");           
        break;
    case EV_LINK_DISCONNECT:
        // TODO: kill the connection first? Do we need to do something?
        arq_fsm.current = (arq_conn.listen == true)? state_listen : state_idle;
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    case EV_LINK_ESTABLISHED:
        tnc_send_connected();
        arq_fsm.current = state_link_connected;
        break;
    default:
        printf("Event: %d ignored from state_idle\n", event);
    }

    return;
}

void state_connecting_callee(int event)
{
    printf("FSM State: connecting_callee\n");
    
    switch(event)
    {
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_CALL_REMOTE:
        printf("EV_LINK_CALL_REMOTE ignored in state_connecting_caller() - already connecting.\n");           
        break;
    case EV_LINK_DISCONNECT:
        // TODO: kill the connection first? Do we need to do something?
        arq_fsm.current = (arq_conn.listen == true)? state_idle : state_listen;
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_no_connected_client;
        break;
    case EV_LINK_ESTABLISHMENT_TIMEOUT:
        // TODO: do some house cleeping here?
        arq_fsm.current = (arq_conn.listen == true)? state_idle : state_listen;
        break;
    case EV_LINK_ESTABLISHED:
        // TODO: do some house cleeping here?
        tnc_send_connected();
        arq_fsm.current = state_link_connected;
        break;
    default:
        printf("Event: %d ignored from state_idle\n", event);
    }

    return;
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

bool check_crc(uint8_t *data)
{
    // TODO: put the correct frame size here
    int frame_size = 0;

    uint16_t crc = (uint16_t) (data[0] & 0x3f);
    uint16_t calculated_crc = crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE);

    if (crc == calculated_crc)
        return true;

    return false;
    
}

int check_for_incoming_connection(uint8_t *data)
{
    char callsigns[CALLSIGN_MAX_SIZE * 2];
    char dst_callsign[CALLSIGN_MAX_SIZE] = { 0 };
    char src_callsign[CALLSIGN_MAX_SIZE] = { 0 };

    // TODO: put the correct frame size here
    int frame_size = 0;

    uint8_t pack_type = (data[0] >> 6) & 0xff;

    if (check_crc(data) == false)
    {
        printf("Bad crc for packet type %u.\n", pack_type);
        return -1;
    }

    if (pack_type != PACKET_ARQ_CONTROL)
    {
        return 1;
    }

    if (arithmetic_decode(data + HEADER_SIZE, frame_size - HEADER_SIZE, callsigns) < 0)
    {
        printf("Truncated callsigns.\n");
    }
    printf("Decoded callsigns: %s\n", callsigns);

    char *needle;
    if ( (needle = strstr(callsigns,"|")) )
    {
        int i = 0;
        while (callsigns[i] != '|')
        {
            dst_callsign[i] = callsigns[i];
            i++;
        }
        i++;
        dst_callsign[i] = 0;

        i = 0;
        needle++;
        while (callsigns[i] != 0)
        {
            src_callsign[i] = needle[i];
            i++;
        }
        src_callsign[i] = 0;
    }
    else // corner case where only the destination address fits in the frame
    {
        strcpy(dst_callsign, callsigns);
    }    

    if (!strncmp(dst_callsign, arq_conn.my_call_sign, strlen(dst_callsign)))
    {
        fsm_dispatch(&arq_fsm, EV_LINK_INCOMING_CALL);
    }
    else
    {
        // TODO: or we just wait for timeout, or drop a EV_LINK_ESTABLISHMENT_FAILURE
        fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHMENT_TIMEOUT);
        printf("Called call %s sign does not match my callsign %s. Doing nothing.\n", dst_callsign, arq_conn.my_call_sign);
    }
    
    return 0;
}

int check_for_connection_acceptance_caller(uint8_t *data)
{
    char callsign[CALLSIGN_MAX_SIZE];

    // TODO: put the correct frame size here
    int frame_size = 0;

    uint8_t pack_type = (data[0] >> 6) & 0xff;
    
    if (check_crc(data) == false)
    {
        printf("Bad crc for packet type %u.\n", pack_type);
        return -1;
    }
    
    if (pack_type != PACKET_ARQ_CONTROL)
    {
        return 1;
    }
    
    if (arithmetic_decode(data + HEADER_SIZE, frame_size - HEADER_SIZE, callsign) < 0)
    {
        printf("Truncated callsigns.\n");
    }
    printf("Decoded callsign: %s\n", callsign);

    if (!strncmp(callsign, arq_conn.dst_addr, strlen(callsign)))
    {
        fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHED);
    }
    else
    {
        // TODO:
        // fsm_dispatch(&arq_fsm, EV_LINK_ESTABLISHED_FAILURE);
        printf("Responded callsign %s does not match called %s. Doing nothing.\n", callsign, arq_conn.dst_addr);
    }
    return 0;
}

void callee_accept_connection()
{
    // here we just send the callee callsign back
    uint8_t data[INT_BUFFER_SIZE];
    char callsign[CALLSIGN_MAX_SIZE];
    uint8_t encoded_callsign[CALLSIGN_MAX_SIZE];

    // TODO: put the correct frame size here
    int frame_size = 0;

    memset(data, 0, frame_size);
    // 1 byte header, 4 bits packet type, 6 bits crc
    data[0] = (PACKET_ARQ_CONTROL << 6) & 0xff; // set packet type

    // encode the callsign
    sprintf(callsign, "%s", arq_conn.my_call_sign);
    int enc_len = arithmetic_encode(callsign, encoded_callsign);

    if (enc_len > frame_size - HEADER_SIZE)
    {
        printf("Trucating callsigns. This is ok (%d bytes out of %d transmitted)\n", frame_size - HEADER_SIZE, enc_len);
        enc_len = frame_size - HEADER_SIZE;
    }

    memcpy(data + HEADER_SIZE, encoded_callsign, enc_len);

    data[0] |= (uint8_t) crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE);

    write_buffer(data_tx_buffer, data, frame_size);

}

void call_remote()
{
    uint8_t data[INT_BUFFER_SIZE];
    char joint_callsigns[CALLSIGN_MAX_SIZE * 2];
    uint8_t encoded_callsigns[INT_BUFFER_SIZE];

    // TODO: put the correct frame size here
    int frame_size = 0;

    printf("Calling remote %s, frame_size: %d\n", arq_conn.dst_addr, frame_size);
    
    memset(data, 0, frame_size);
    // 1 byte header, 4 bits packet type, 6 bits crc
    data[0] = (PACKET_ARQ_CONTROL << 6) & 0xff; // set packet type

    // encode the destination callsign first, then the source, separated by "|"
    sprintf(joint_callsigns,"%s|%s", arq_conn.dst_addr, arq_conn.src_addr);
    printf("Joint callsigns: %s\n", joint_callsigns);

    int enc_len = arithmetic_encode(joint_callsigns, encoded_callsigns);

    printf("Encoded callsigns: %s, length: %d\n", joint_callsigns, enc_len);
    
    if (enc_len > frame_size - HEADER_SIZE)
    {
        printf("Trucating joint destination + source callsigns. This is ok (%d bytes out of %d transmitted)\n", frame_size - HEADER_SIZE, enc_len);
        enc_len = frame_size - HEADER_SIZE;
    }
    memcpy(data + HEADER_SIZE, encoded_callsigns, enc_len);

    data[0] |= (uint8_t) crc6_0X6F(1, (uint8_t *)data + HEADER_SIZE, frame_size - HEADER_SIZE);

    write_buffer(data_tx_buffer, data, frame_size);
    
    return;
}

/* ---- END OF FSM Definitions ---- */


void clear_connection_data()
{
    clear_buffer(data_tx_buffer);
    clear_buffer(data_rx_buffer);
    reset_arq_info(&arq_conn);
}

void reset_arq_info(arq_info *arq_conn_i)
{
    arq_conn_i->TRX = RX;
    arq_conn_i->bw = 0; // 0 = auto
    arq_conn_i->encryption = false;
    arq_conn_i->listen = false;
    arq_conn_i->my_call_sign[0] = 0;
    arq_conn_i->src_addr[0] = 0;
    arq_conn_i->dst_addr[0] = 0;
}


int arq_init(int tcp_base_port, int initial_mode)
{
    status_ctl = NET_NONE;
    status_data = NET_NONE;

    arq_conn.call_burst_size = CALL_BURST_SIZE;

    uint8_t *buffer_tx = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    uint8_t *buffer_rx = (uint8_t *) malloc(DATA_RX_BUFFER_SIZE);
    data_tx_buffer = circular_buf_init(buffer_tx, DATA_TX_BUFFER_SIZE);
    data_rx_buffer = circular_buf_init(buffer_rx, DATA_RX_BUFFER_SIZE);

    // TODO: init modem here?
    
    reset_arq_info(&arq_conn);

    init_model(); // the arithmetic encoder init function
    fsm_init(&arq_fsm, state_no_connected_client);
    
    // here is the thread that runs the accept(), each per port, and mantains the
    // state of the connection
    pthread_create(&tid[0], NULL, server_worker_thread_ctl, (void *) &tcp_base_port);
    pthread_create(&tid[1], NULL, server_worker_thread_data, (void *) &tcp_base_port);

    // control channel threads
    pthread_create(&tid[2], NULL, control_worker_thread_rx, (void *) NULL);
    pthread_create(&tid[3], NULL, control_worker_thread_tx, (void *) NULL);

    // data channel threads
    pthread_create(&tid[4], NULL, data_worker_thread_tx, (void *) NULL);
    pthread_create(&tid[5], NULL, data_worker_thread_rx, (void *) NULL);

    // dsp threads
    pthread_create(&tid[6], NULL, dsp_thread_tx, (void *) NULL);
    pthread_create(&tid[7], NULL, dsp_thread_rx, (void *) NULL);
    
    return EXIT_SUCCESS;
}


void arq_shutdown()
{
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
    pthread_join(tid[2], NULL);
    pthread_join(tid[3], NULL);
    pthread_join(tid[4], NULL);
    pthread_join(tid[5], NULL);
    pthread_join(tid[6], NULL);
    pthread_join(tid[7], NULL);

    free(data_tx_buffer->buffer);
    free(data_rx_buffer->buffer);
}

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
    ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
    // print timestamp with miliseconds precision
#ifdef DEBUG
    printf("PTT ON %s", get_timestamp());
#endif
}
void ptt_off()
{
    char buffer[] = "PTT OFF\r";
    arq_conn.TRX = RX;
    ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
    // print timestamp with miliseconds precision
#ifdef DEBUG
    printf("PTT OFF %s", get_timestamp());
#endif
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

        if(arq_fsm.current == state_link_connected)
        {
            size_t n = read_buffer_all(data_rx_buffer, buffer);

            ssize_t i = tcp_write(DATA_TCP_PORT, buffer, n);

            if (i < (ssize_t) n)
                fprintf(stderr, "Problems in data_worker_thread_tx!\n");
        }
        else
        {
            msleep(50);
        }
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


        if(arq_fsm.current == state_link_connected)
        {
            int n = tcp_read(DATA_TCP_PORT, buffer, TCP_BLOCK_SIZE);

            write_buffer(data_tx_buffer, buffer, n);
        }
        else
        {
            msleep(50);
        }

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

        if (counter == 60)
        {
            counter = 0;
            ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)imalive, strlen(imalive));

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

void *dsp_thread_tx(void *conn)
{
    static uint32_t spinner_anim = 0; char spinner[] = ".oOo";
    // TODO: put the correct frame size here
    int frame_size = 0;
    uint8_t data[INT_BUFFER_SIZE];

    // TODO: may be we need another function to queue the already prepared packets?
    while(!shutdown_)
    {
        // should we add a header already here, to know the size of each package? (size need to match frame_size
        if ((int) size_buffer(data_tx_buffer) < frame_size ||
            arq_fsm.current == state_idle ||
            arq_fsm.current == state_no_connected_client)
        {
            msleep(50);
            continue;
        }

            
        if(arq_fsm.current != state_idle && arq_fsm.current != state_listen)
        {
            read_buffer(data_tx_buffer, data, frame_size);
        }
        else
        {
            msleep(50);
            continue;
        }
        
        for (int i = 0; i < frame_size; i++)
        {
            printf("%02x ", data[i]);
        }

        
        ptt_on();
        msleep(10); // TODO: tune me!

        // our connection request
        if (arq_fsm.current == state_connecting_caller || arq_fsm.current == state_connecting_callee)
        {
            // tx_transfer(...);
        }

        if (arq_fsm.current == state_link_connected)
        {
            // here we have the data to transmit, so we call the tx_transfer function

            //tx_transfer();        }
        }
        
        // TODO: signal when stream is finished playing via pthread_cond_wait() here
        while (size_buffer(playback_buffer) != 0)
        {
            printf("%c\033[1D", spinner[spinner_anim % 4]); spinner_anim++;
            fflush(stdout);
            msleep(10);
        }

        msleep(40); // TODO: parametrize-me!
        ptt_off();

        printf("%c\033[1D", spinner[spinner_anim % 4]); spinner_anim++;
        fflush(stdout);

    }
    
    return NULL;
}

void *dsp_thread_rx(void *conn)
{
    static uint32_t spinner_anim = 0; char spinner[] = ".oOo";

    while(!shutdown_)
    {
        msleep(50);

    }

    return NULL;
}

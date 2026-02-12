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
#include "defines_modem.h"
#include "tcp_interfaces.h"
#include "../modem/modem.h"

#define FIXED_FRAME_SIZE 512

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_rx_buffer_arq;

extern bool shutdown_;

// ARQ definitions
arq_info arq_conn;


static pthread_t tid[2];


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

bool check_crc(uint8_t *data)
{
    int frame_size = FIXED_FRAME_SIZE;

    uint16_t rx_crc = data[0] & 0x3f;
    uint16_t calc_crc = crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE - 2);

    fprintf(stderr,
        "[RX] frame_size=%d rx_crc=%u calc_crc=%u packet_type=%u\n",
        frame_size,
        rx_crc,
        calc_crc,
        (data[0] >> 6) & 0x3
    );

    if (rx_crc == calc_crc)
        return true;

    return false;
    
}

int check_for_incoming_connection(uint8_t *data)
{
    char callsigns[CALLSIGN_MAX_SIZE * 2];
    char dst_callsign[CALLSIGN_MAX_SIZE] = { 0 };
    char src_callsign[CALLSIGN_MAX_SIZE] = { 0 };

    int frame_size = FIXED_FRAME_SIZE;

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

    int frame_size = FIXED_FRAME_SIZE;

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

    int frame_size = FIXED_FRAME_SIZE;

    /* REAL padding: zero-initialize entire frame before building */
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

    data[0] |= (uint8_t) crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE - 2);

    fprintf(stderr,
        "[ARQ TX CALLEE] bytes being sent to modem = %d (payload = %d + header = 1)\n",
        frame_size, frame_size - 1
    );

    write_buffer(data_tx_buffer_arq, data, frame_size);

}

void call_remote()
{
    uint8_t data[INT_BUFFER_SIZE];
    char joint_callsigns[CALLSIGN_MAX_SIZE * 2];
    uint8_t encoded_callsigns[INT_BUFFER_SIZE];

    int frame_size = FIXED_FRAME_SIZE;

    printf("Calling remote %s, frame_size: %d\n", arq_conn.dst_addr, frame_size);
    
    /* REAL padding: zero-initialize entire frame before building */
    memset(data, 0, frame_size);

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

    data[0] = (PACKET_ARQ_DATA << 6) & 0xff;
    // CRC6 is computed over the payload excluding the final 2 bytes reserved for CRC16.
    data[0] |= crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE - 2);

    uint16_t tx_crc = crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE - 2);
    data[0] |= (uint8_t) tx_crc;

    fprintf(stderr,
        "[ARQ TX CALLER] TX-CRC6=0x%02x, payload_len=%d, frame_size=%d\n",
        tx_crc, enc_len, frame_size
    );

    fprintf(stderr,
        "[ARQ TX CALLER] bytes being sent to modem = %d (payload = %d + header = 1)\n",
        frame_size, frame_size - HEADER_SIZE
    );

    write_buffer(data_tx_buffer_arq, data, frame_size);
    
    return;
}

/* ---- END OF FSM Definitions ---- */


void clear_connection_data()
{
    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_rx_buffer_arq);
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


int arq_init()
{
    arq_conn.call_burst_size = CALL_BURST_SIZE;

    reset_arq_info(&arq_conn);

    init_model(); // the arithmetic encoder init function
    fsm_init(&arq_fsm, state_no_connected_client);
    
    // dsp threads
    pthread_create(&tid[0], NULL, dsp_thread_tx, (void *) NULL);
    pthread_create(&tid[1], NULL, dsp_thread_rx, (void *) NULL);

    return EXIT_SUCCESS;
}


void arq_shutdown()
{
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}


void *dsp_thread_tx(void *conn)
{
    while(!shutdown_)
    {
        msleep(100);
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

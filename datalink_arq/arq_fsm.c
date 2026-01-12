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

#include "fsm.h"
#include "arq.h"
#include "../common/defines_modem.h"
#include "../data_interfaces/tcp_interfaces.h"

/* FSM States */

void state_no_connected_client(int event)
{
    printf("FSM State: no_connected_client\n");

    switch(event)
    {
    case EV_CLIENT_CONNECT:
        clear_connection_data();
        fsm_set_state(&arq_fsm, state_idle);
        break;
    default:
        printf("Event: %d ignored in state_no_connected_client().\n", event);
    }
    return;
}

void state_link_connected(int event)
{
    printf("[ARQ] state=link_connected event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_CLIENT_DISCONNECT:
        fsm_set_state(&arq_fsm, state_no_connected_client);
        break;

    case EV_LINK_DISCONNECT:
        fsm_set_state(&arq_fsm, (arq_conn.listen == true)? state_listen : state_idle);
        break;
    default:
        printf("Event: %d ignored in state_link_connected().\n", event);
    }
    return;
}

void state_listen(int event)
{
    printf("[ARQ] state=listen event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_START_LISTEN:
        printf("EV_START_LISTEN ignored in state_listen() - already listening.\n");
        printf("EV_START_LISTEN ignored in state_listen() - already listening.\n");
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        fsm_set_state(&arq_fsm, state_idle);
        break;
    case EV_LINK_CALL_REMOTE:
        call_remote();
        fsm_set_state(&arq_fsm, state_connecting_caller);
        break;
    case EV_LINK_DISCONNECT:
        printf("EV_LINK_DISCONNECT ignored in state_listen() - not connected.\n");
        printf("EV_LINK_DISCONNECT ignored in state_listen() - not connected.\n");
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        fsm_set_state(&arq_fsm, state_no_connected_client);
        break;
    case EV_LINK_INCOMING_CALL:
        callee_accept_connection(); //packets created to anwser in case of incoming call with correct callsign
        fsm_set_state(&arq_fsm, state_connecting_callee);
        break;
    default:
        printf("Event: %d ignored in state_listen().\n", event);
    }

    return;
}

void state_idle(int event)
{
    printf("[ARQ] state=idle event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        fsm_set_state(&arq_fsm, state_listen);
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        printf("EV_STOP_LISTEN ignored in state_idle() - already stopped.\n");
        printf("EV_STOP_LISTEN ignored in state_idle() - already stopped.\n");
        break;
    case EV_LINK_CALL_REMOTE:
        call_remote();
        fsm_set_state(&arq_fsm, state_connecting_caller);
        break;
    case EV_LINK_DISCONNECT:
        printf("EV_LINK_DISCONNECT ignored in state_idle() - not connected.\n");
        printf("EV_LINK_DISCONNECT ignored in state_idle() - not connected.\n");
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        fsm_set_state(&arq_fsm, state_no_connected_client);
        break;
    default:
        printf("Event: %d ignored from state_idle\n", event);
    }

    return;
}

void state_connecting_caller(int event)
{
    printf("[ARQ] state=connecting_caller event=%s", fsm_event_names[event]);

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
        printf("EV_LINK_CALL_REMOTE ignored in state_connecting_caller() - already connecting.\n");
        break;
    case EV_LINK_DISCONNECT:
        // TODO: kill the connection first? Do we need to do something more?
        arq_conn.retry_count = 0;
        fsm_set_state(&arq_fsm, (arq_conn.listen == true)? state_listen : state_idle);
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        fsm_set_state(&arq_fsm, state_no_connected_client);
        break;
    case EV_LINK_ESTABLISHED:
        tnc_send_connected();
        arq_conn.retry_count = 0;
        fsm_set_state(&arq_fsm, state_link_connected);
        break;
    case EV_LINK_ESTABLISHMENT_TIMEOUT:
        if (arq_conn.retry_count < MAX_RETRIES)
        {
            arq_conn.retry_count++;
            call_remote(); // Retry
        }
        else
        {
            printf("Connection failed after %d retries.\n", MAX_RETRIES);
            fsm_set_state(&arq_fsm, (arq_conn.listen == true)? state_listen : state_idle);
        }
        break;
    default:
        printf("Event: %d ignored from state_connecting_caller\n", event);
    }

    return;
}

void state_connecting_callee(int event)
{
    printf("[ARQ] state=connecting_callee event=%s", fsm_event_names[event]);

    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_LINK_CALL_REMOTE:
        printf("EV_LINK_CALL_REMOTE ignored in state_connecting_callee() - already connecting.\n");
        break;
    case EV_LINK_DISCONNECT:
        arq_conn.retry_count = 0;
        fsm_set_state(&arq_fsm, (arq_conn.listen == true)? state_listen : state_idle);
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        fsm_set_state(&arq_fsm, state_no_connected_client);
        break;
    case EV_LINK_ESTABLISHMENT_TIMEOUT:
        fsm_set_state(&arq_fsm, (arq_conn.listen == true)? state_listen : state_idle);
        break;
    case EV_LINK_ESTABLISHED:
        tnc_send_connected();
        arq_conn.retry_count = 0;
        fsm_set_state(&arq_fsm, state_link_connected);
        break;
    default:
        printf("Event: %d ignored from state_connecting_callee\n", event);
    }

    return;
}

/* ---- END OF FSM Definitions ---- */

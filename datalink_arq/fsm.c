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

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include "fsm.h"
#include "hermes_log.h"

const char *fsm_event_names[] = {
    "EV_CLIENT_CONNECT",
    "EV_CLIENT_DISCONNECT",
    "EV_START_LISTEN",
    "EV_STOP_LISTEN",
    "EV_LINK_CALL_REMOTE",
    "EV_LINK_INCOMING_CALL",
    "EV_LINK_DISCONNECT",
    "EV_LINK_ESTABLISHMENT_TIMEOUT",
    "EV_LINK_ESTABLISHED"
};

// Initialize the FSM
void fsm_init(fsm_handle* fsm, fsm_state initial_state)
{
    HLOGI("fsm", "Initializing FSM");
    
    if (!fsm)
        return;

    fsm->current = initial_state;
    pthread_mutex_init(&fsm->lock, NULL);
}

// Dispatch an event (thread-safe)
void fsm_dispatch(fsm_handle* fsm, int event)
{
    const char *event_name = NULL;

    if (!fsm)
        return;

    if (event < EV_CLIENT_CONNECT || event > EV_LINK_ESTABLISHED)
    {
        HLOGW("fsm", "Dropping invalid event id %d", event);
        return;
    }
    event_name = fsm_event_names[event];
    HLOGI("fsm", "Dispatching event %s", event_name);
    
    pthread_mutex_lock(&fsm->lock);
    if (fsm->current)
        fsm->current(event);  // Execute current state

    pthread_mutex_unlock(&fsm->lock);
}

// Clean up resources
void fsm_destroy(fsm_handle* fsm)
{
    if (!fsm)
        return;

    HLOGI("fsm", "Destroying FSM");
    
    pthread_mutex_destroy(&fsm->lock);
    fsm->current = NULL;
}

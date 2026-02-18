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

#ifndef FSM_H
#define FSM_H

#include <pthread.h>

/* ---- FSM Definitions ---- */

/* FSM Events */
// TNC TCP client event
#define EV_CLIENT_CONNECT 0
#define EV_CLIENT_DISCONNECT 1

#define EV_START_LISTEN 2
#define EV_STOP_LISTEN 3

#define EV_LINK_CALL_REMOTE 4
#define EV_LINK_INCOMING_CALL 5
#define EV_LINK_DISCONNECT 6

#define EV_LINK_ESTABLISHMENT_TIMEOUT 7

#define EV_LINK_ESTABLISHED 8


#ifdef __cplusplus
extern "C" {
#endif
    
extern const char* fsm_event_names[];

#ifdef __cplusplus
}
#endif

/** @brief State-handler function signature used by FSM dispatcher. */
typedef void (*fsm_state)(int event);

/** @brief Thread-safe finite-state machine wrapper. */
typedef struct {
    fsm_state current;
    pthread_mutex_t lock;
} fsm_handle;

/**
 * @brief Initialize FSM instance.
 * @param fsm FSM handle.
 * @param initial_state Initial state callback.
 */
void fsm_init(fsm_handle* fsm, fsm_state initial_state);

/**
 * @brief Dispatch an event into current FSM state.
 * @param fsm FSM handle.
 * @param event Event identifier.
 */
void fsm_dispatch(fsm_handle* fsm, int event);

/**
 * @brief Destroy FSM synchronization resources.
 * @param fsm FSM handle.
 */
void fsm_destroy(fsm_handle* fsm);

#endif // FSM_H

/* HERMES Modem — ARQ Modem interface: action queue and PTT callbacks (Phase 1 stub)
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_modem.h"

#include "../modem/freedv/freedv_api.h"

/* ======================================================================
 * Action queue stubs
 * TODO Phase 5: implement using chan.h or a semaphore-guarded ring buffer
 * ====================================================================== */

int arq_modem_queue_init(size_t capacity)
{
    /* TODO Phase 5 */
    (void)capacity;
    return 0;
}

void arq_modem_queue_shutdown(void)
{
    /* TODO Phase 5 */
}

int arq_modem_enqueue(const arq_action_t *action)
{
    /* TODO Phase 5 */
    (void)action;
    return -1;
}

bool arq_modem_dequeue(arq_action_t *action, int timeout_ms)
{
    /* TODO Phase 5 */
    (void)action;
    (void)timeout_ms;
    return false;
}

/* ======================================================================
 * PTT notification stubs
 * TODO Phase 5: post ARQ_EV_TX_STARTED / ARQ_EV_TX_COMPLETE to event queue
 * ====================================================================== */

void arq_modem_ptt_on(int mode, size_t frame_size)
{
    /* TODO Phase 5 */
    (void)mode;
    (void)frame_size;
}

void arq_modem_ptt_off(void)
{
    /* TODO Phase 5 */
}

/* ======================================================================
 * Mode selection stubs
 * TODO Phase 5: replicate mode-ladder logic from old arq.c
 * ====================================================================== */

int arq_modem_preferred_rx_mode(const arq_session_t *sess)
{
    /* TODO Phase 5 */
    (void)sess;
    return FREEDV_MODE_DATAC13;
}

int arq_modem_preferred_tx_mode(const arq_session_t *sess)
{
    /* TODO Phase 5 */
    (void)sess;
    return FREEDV_MODE_DATAC13;
}

/* HERMES Modem — ARQ Timing: instrumentation and telemetry (Phase 1 stub)
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_timing.h"

#include <string.h>

#include "../common/hermes_log.h"
#include "arq_protocol.h"

#define LOG_COMP "arq-timing"

void arq_timing_init(arq_timing_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void arq_timing_record_tx_queue(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes)
{
    /* TODO Phase 3: record timestamp, emit HLOGT */
    (void)ctx;
    (void)seq;
    (void)mode;
    (void)backlog_bytes;
}

void arq_timing_record_tx_start(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes)
{
    /* TODO Phase 3: record timestamp, emit HLOGT */
    (void)ctx;
    (void)seq;
    (void)mode;
    (void)backlog_bytes;
}

void arq_timing_record_tx_end(arq_timing_ctx_t *ctx, int seq)
{
    /* TODO Phase 3: record timestamp, compute duration, emit HLOGT */
    (void)ctx;
    (void)seq;
}

void arq_timing_record_ack_rx(arq_timing_ctx_t *ctx, int seq,
                               uint8_t ack_delay_raw, int peer_snr_x10)
{
    /* TODO Phase 3: compute RTT, emit HLOGT */
    (void)ctx;
    (void)seq;
    (void)ack_delay_raw;
    (void)peer_snr_x10;
}

void arq_timing_record_data_rx(arq_timing_ctx_t *ctx, int seq,
                                int bytes, int snr_x10)
{
    /* TODO Phase 3: record timestamp, update counters, emit HLOGT */
    (void)ctx;
    (void)seq;
    (void)bytes;
    (void)snr_x10;
}

void arq_timing_record_ack_tx(arq_timing_ctx_t *ctx, int seq)
{
    /* TODO Phase 3: record ack_tx_start_ms, emit HLOGT */
    (void)ctx;
    (void)seq;
}

void arq_timing_record_retry(arq_timing_ctx_t *ctx, int seq,
                              int attempt, const char *reason)
{
    /* TODO Phase 3: update counters, emit HLOGT */
    (void)ctx;
    (void)seq;
    (void)attempt;
    (void)reason;
}

void arq_timing_record_turn(arq_timing_ctx_t *ctx, bool to_iss,
                             const char *reason)
{
    /* TODO Phase 3: emit HLOGT */
    (void)ctx;
    (void)to_iss;
    (void)reason;
}

void arq_timing_record_connect(arq_timing_ctx_t *ctx, int mode)
{
    /* TODO Phase 3: emit HLOGT connect event */
    (void)ctx;
    (void)mode;
}

void arq_timing_record_disconnect(arq_timing_ctx_t *ctx, const char *reason)
{
    /* TODO Phase 3: emit HLOGT session summary */
    (void)ctx;
    (void)reason;
}

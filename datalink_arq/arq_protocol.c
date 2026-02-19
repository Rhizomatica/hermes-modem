/* HERMES Modem — ARQ Protocol: mode timing table and frame codec
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_protocol.h"

#include <string.h>

/* Include FreeDV mode constants */
#include "../modem/freedv/freedv_api.h"

/* ======================================================================
 * Mode timing table
 *
 * Empirical values from OTA testing on NVIS paths.
 * ack_timeout_s is the maximum time from PTT-ON to ACK receipt.
 * Derivation:
 *   DATAC13: TX ~2.5s + 0.4s guard + ~2.5s ACK return ≈ 5.4s → 6s
 *   DATAC4:  TX ~5.7s + 0.4s guard + ~2.5s ACK return ≈ 8.6s → 9s
 *   DATAC3:  TX ~4.0s + 0.4s guard + ~2.5s ACK return ≈ 6.9s → 8s
 *   DATAC1:  TX ~6.5s + 0.4s guard + ~2.5s ACK return ≈ 9.4s → 11s
 * retry_interval_s = ack_timeout_s + ARQ_ACK_GUARD_S (1s)
 * ====================================================================== */

const arq_mode_timing_t arq_mode_table[] = {
    /*  freedv_mode           frame_dur  tx_period  ack_timeout  retry_interval  payload_bytes */
    {  FREEDV_MODE_DATAC13,   2.5f,      1.0f,      6.0f,        7.0f,           14 },
    {  FREEDV_MODE_DATAC4,    5.7f,      1.0f,      9.0f,        10.0f,          54 },
    {  FREEDV_MODE_DATAC3,    4.0f,      1.0f,      8.0f,        9.0f,           126 },
    {  FREEDV_MODE_DATAC1,    6.5f,      1.0f,      11.0f,       12.0f,          510 },
};

const int arq_mode_table_count =
    (int)(sizeof(arq_mode_table) / sizeof(arq_mode_table[0]));

/* ======================================================================
 * Mode timing lookup
 * ====================================================================== */

const arq_mode_timing_t *arq_protocol_mode_timing(int freedv_mode)
{
    for (int i = 0; i < arq_mode_table_count; i++)
    {
        if (arq_mode_table[i].freedv_mode == freedv_mode)
            return &arq_mode_table[i];
    }
    return NULL;
}

/* ======================================================================
 * Frame header codec
 * TODO Phase 2: implement encode/decode
 * ====================================================================== */

int arq_protocol_encode_hdr(uint8_t *buf, size_t buf_len, const arq_frame_hdr_t *hdr)
{
    if (!buf || !hdr || buf_len < ARQ_FRAME_HDR_SIZE)
        return -1;

    /* byte 0 (framer: CRC6 + packet_type) is set by write_frame_header() */
    buf[ARQ_HDR_SUBTYPE_IDX]  = hdr->subtype;
    buf[ARQ_HDR_FLAGS_IDX]    = hdr->flags;
    buf[ARQ_HDR_SESSION_IDX]  = hdr->session_id;
    buf[ARQ_HDR_SEQ_IDX]      = hdr->tx_seq;
    buf[ARQ_HDR_ACK_IDX]      = hdr->rx_ack_seq;
    buf[ARQ_HDR_SNR_IDX]      = hdr->snr_raw;
    buf[ARQ_HDR_DELAY_IDX]    = hdr->ack_delay_raw;
    return 0;
}

int arq_protocol_decode_hdr(const uint8_t *buf, size_t buf_len, arq_frame_hdr_t *hdr)
{
    if (!buf || !hdr || buf_len < ARQ_FRAME_HDR_SIZE)
        return -1;

    hdr->packet_type   = buf[0];      /* framer byte — packet_type is in bits [7:6] */
    hdr->subtype       = buf[ARQ_HDR_SUBTYPE_IDX];
    hdr->flags         = buf[ARQ_HDR_FLAGS_IDX];
    hdr->session_id    = buf[ARQ_HDR_SESSION_IDX];
    hdr->tx_seq        = buf[ARQ_HDR_SEQ_IDX];
    hdr->rx_ack_seq    = buf[ARQ_HDR_ACK_IDX];
    hdr->snr_raw       = buf[ARQ_HDR_SNR_IDX];
    hdr->ack_delay_raw = buf[ARQ_HDR_DELAY_IDX];
    return 0;
}

/* ======================================================================
 * SNR codec
 *
 * Encoding: snr_raw = clamp(round(snr_db) + 128, 1, 255); 0 = unknown.
 * Decoding: snr_db  = (float)(snr_raw - 128).
 * ====================================================================== */

uint8_t arq_protocol_encode_snr(float snr_db)
{
    int v = (int)(snr_db + 0.5f) + 128;
    if (v < 1)   v = 1;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

float arq_protocol_decode_snr(uint8_t snr_raw)
{
    if (snr_raw == 0)
        return 0.0f;  /* unknown */
    return (float)((int)snr_raw - 128);
}

/* ======================================================================
 * ACK delay codec
 *
 * Wire value is in 10ms units (uint16_t big-endian).
 * 0 = unknown.
 * ====================================================================== */

uint8_t arq_protocol_encode_ack_delay(uint32_t delay_ms)
{
    uint32_t units = delay_ms / 10;
    if (units == 0 && delay_ms > 0)
        units = 1;  /* round up from sub-10ms */
    if (units > 0xff)
        units = 0xff;
    return (uint8_t)units;
}

uint32_t arq_protocol_decode_ack_delay(uint8_t raw)
{
    return (uint32_t)raw * 10;
}

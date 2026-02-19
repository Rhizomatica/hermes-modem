/* HERMES Modem — ARQ Protocol: mode timing table and frame codec
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_protocol.h"

#include <string.h>
#include <stdio.h>

/* Include FreeDV mode constants */
#include "../modem/freedv/freedv_api.h"

/* Framer layer (write_frame_header, PACKET_TYPE_* constants) */
#include "../modem/framer.h"

/* Arithmetic encoder for callsign compression */
extern void init_model(void);
extern int  arithmetic_encode(const char *msg, uint8_t *output);
extern int  arithmetic_decode(uint8_t *input, int max_len, char *output);

/* CALLSIGN_MAX_SIZE from arq.h */
#include "arq.h"

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

    hdr->packet_type   = (buf[0] >> PACKET_TYPE_SHIFT) & PACKET_TYPE_MASK;
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

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

/* Write a standard 8-byte control frame header; return frame size or -1. */
static int build_ctrl(uint8_t *buf, size_t buf_len,
                      uint8_t subtype, uint8_t session_id,
                      uint8_t tx_seq, uint8_t rx_ack_seq,
                      uint8_t flags, uint8_t snr_raw, uint8_t ack_delay_raw,
                      const uint8_t *payload, size_t payload_len)
{
    size_t total = ARQ_FRAME_HDR_SIZE + payload_len;
    if (buf_len < total)
        return -1;

    memset(buf, 0, total);
    /* byte 0 set by write_frame_header below */
    buf[ARQ_HDR_SUBTYPE_IDX]  = subtype;
    buf[ARQ_HDR_FLAGS_IDX]    = flags;
    buf[ARQ_HDR_SESSION_IDX]  = session_id;
    buf[ARQ_HDR_SEQ_IDX]      = tx_seq;
    buf[ARQ_HDR_ACK_IDX]      = rx_ack_seq;
    buf[ARQ_HDR_SNR_IDX]      = snr_raw;
    buf[ARQ_HDR_DELAY_IDX]    = ack_delay_raw;
    if (payload && payload_len > 0)
        memcpy(buf + ARQ_FRAME_HDR_SIZE, payload, payload_len);
    write_frame_header(buf, PACKET_TYPE_ARQ_CONTROL, total);
    return (int)total;
}

/* ======================================================================
 * Frame builder implementations
 * ====================================================================== */

int arq_protocol_build_ack(uint8_t *buf, size_t buf_len,
                            uint8_t session_id, uint8_t rx_ack_seq,
                            uint8_t flags, uint8_t snr_raw,
                            uint8_t ack_delay_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_ACK, session_id,
                      0, rx_ack_seq, flags, snr_raw, ack_delay_raw,
                      NULL, 0);
}

int arq_protocol_build_disconnect(uint8_t *buf, size_t buf_len,
                                   uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_DISCONNECT, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_keepalive(uint8_t *buf, size_t buf_len,
                                  uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_KEEPALIVE, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_keepalive_ack(uint8_t *buf, size_t buf_len,
                                      uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_KEEPALIVE_ACK, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_turn_req(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t rx_ack_seq,
                                 uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_TURN_REQ, session_id,
                      0, rx_ack_seq, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_turn_ack(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_TURN_ACK, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_mode_req(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw,
                                 int freedv_mode)
{
    uint8_t payload[1] = { (uint8_t)freedv_mode };
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_MODE_REQ, session_id,
                      0, 0, 0, snr_raw, 0,
                      payload, 1);
}

int arq_protocol_build_mode_ack(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw,
                                 int freedv_mode)
{
    uint8_t payload[1] = { (uint8_t)freedv_mode };
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_MODE_ACK, session_id,
                      0, 0, 0, snr_raw, 0,
                      payload, 1);
}

int arq_protocol_build_data(uint8_t *buf, size_t buf_len,
                             uint8_t session_id, uint8_t tx_seq,
                             uint8_t rx_ack_seq, uint8_t flags,
                             uint8_t snr_raw,
                             const uint8_t *payload, size_t payload_len)
{
    size_t total = ARQ_FRAME_HDR_SIZE + payload_len;
    if (buf_len < total || !payload || payload_len == 0)
        return -1;

    memset(buf, 0, ARQ_FRAME_HDR_SIZE);
    buf[ARQ_HDR_SUBTYPE_IDX]  = ARQ_SUBTYPE_DATA;
    buf[ARQ_HDR_FLAGS_IDX]    = flags;
    buf[ARQ_HDR_SESSION_IDX]  = session_id;
    buf[ARQ_HDR_SEQ_IDX]      = tx_seq;
    buf[ARQ_HDR_ACK_IDX]      = rx_ack_seq;
    buf[ARQ_HDR_SNR_IDX]      = snr_raw;
    buf[ARQ_HDR_DELAY_IDX]    = 0;
    memcpy(buf + ARQ_FRAME_HDR_SIZE, payload, payload_len);
    write_frame_header(buf, PACKET_TYPE_ARQ_DATA, total);
    return (int)total;
}

/* ======================================================================
 * CALL/ACCEPT frame builders and parsers
 * ====================================================================== */

/* Encode "DST|SRC" as arithmetic-compressed callsign payload. */
static int encode_callsign_payload(const char *src, const char *dst,
                                   uint8_t *out, size_t out_cap)
{
    char msg[(CALLSIGN_MAX_SIZE * 2) + 2];
    uint8_t tmp[4096];
    int n, enc_len;

    n = snprintf(msg, sizeof(msg), "%s|%s", dst, src);
    if (n <= 0 || n >= (int)sizeof(msg))
        return -1;

    /* normalise to uppercase */
    for (int i = 0; i < n; i++)
        if (msg[i] >= 'a' && msg[i] <= 'z')
            msg[i] = (char)(msg[i] - ('a' - 'A'));

    init_model();
    enc_len = arithmetic_encode(msg, tmp);
    if (enc_len <= 0)
        return -1;
    if ((size_t)enc_len > out_cap)
        enc_len = (int)out_cap;
    memcpy(out, tmp, (size_t)enc_len);
    return enc_len;
}

/* Decode compressed callsign payload into src/dst strings. */
static int decode_callsign_payload(const uint8_t *in, size_t in_len,
                                   char *src_out, char *dst_out)
{
    char decoded[(CALLSIGN_MAX_SIZE * 2) + 2];
    char *sep;

    init_model();
    if (arithmetic_decode((uint8_t *)in, (int)in_len, decoded) < 0)
        return -1;
    if (decoded[0] == 0)
        return -1;

    sep = strchr(decoded, '|');
    if (sep)
    {
        size_t dst_len = (size_t)(sep - decoded);
        if (dst_len >= CALLSIGN_MAX_SIZE) dst_len = CALLSIGN_MAX_SIZE - 1;
        memcpy(dst_out, decoded, dst_len);
        dst_out[dst_len] = 0;
        strncpy(src_out, sep + 1, CALLSIGN_MAX_SIZE - 1);
        src_out[CALLSIGN_MAX_SIZE - 1] = 0;
    }
    else
    {
        /* Only dst present (truncated) */
        strncpy(dst_out, decoded, CALLSIGN_MAX_SIZE - 1);
        dst_out[CALLSIGN_MAX_SIZE - 1] = 0;
        src_out[0] = 0;
    }
    return 0;
}

static int build_call_accept(uint8_t *buf, size_t buf_len, bool is_accept,
                              uint8_t session_id,
                              const char *src, const char *dst)
{
    uint8_t encoded[ARQ_CONNECT_MAX_ENCODED];
    int enc_len;

    if (!buf || !src || !dst || buf_len < ARQ_CONTROL_FRAME_SIZE)
        return -1;

    enc_len = encode_callsign_payload(src, dst, encoded, sizeof(encoded));
    if (enc_len <= 0)
        return -1;

    memset(buf, 0, ARQ_CONTROL_FRAME_SIZE);
    buf[ARQ_CONNECT_SESSION_IDX] =
        (uint8_t)((session_id & ARQ_CONNECT_SESSION_MASK) |
                  (is_accept ? ARQ_CONNECT_ACCEPT_FLAG : 0));
    memcpy(buf + ARQ_CONNECT_PAYLOAD_IDX, encoded, (size_t)enc_len);
    write_frame_header(buf, PACKET_TYPE_ARQ_CALL, ARQ_CONTROL_FRAME_SIZE);
    return ARQ_CONTROL_FRAME_SIZE;
}

int arq_protocol_build_call(uint8_t *buf, size_t buf_len,
                             uint8_t session_id,
                             const char *src, const char *dst)
{
    return build_call_accept(buf, buf_len, false, session_id, src, dst);
}

int arq_protocol_build_accept(uint8_t *buf, size_t buf_len,
                               uint8_t session_id,
                               const char *src, const char *dst)
{
    return build_call_accept(buf, buf_len, true, session_id, src, dst);
}

static int parse_call_accept(const uint8_t *buf, size_t buf_len,
                              uint8_t *session_id_out,
                              char *src_out, char *dst_out)
{
    if (!buf || buf_len < ARQ_CONTROL_FRAME_SIZE ||
        !session_id_out || !src_out || !dst_out)
        return -1;

    *session_id_out = buf[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_SESSION_MASK;
    return decode_callsign_payload(buf + ARQ_CONNECT_PAYLOAD_IDX,
                                   ARQ_CONNECT_MAX_ENCODED,
                                   src_out, dst_out);
}

int arq_protocol_parse_call(const uint8_t *buf, size_t buf_len,
                             uint8_t *session_id_out,
                             char *src_out, char *dst_out)
{
    return parse_call_accept(buf, buf_len, session_id_out, src_out, dst_out);
}

int arq_protocol_parse_accept(const uint8_t *buf, size_t buf_len,
                               uint8_t *session_id_out,
                               char *src_out, char *dst_out)
{
    return parse_call_accept(buf, buf_len, session_id_out, src_out, dst_out);
}

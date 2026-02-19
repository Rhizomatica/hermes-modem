/* HERMES Modem — ARQ Protocol: wire format, mode timing, codec API
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_PROTOCOL_H_
#define ARQ_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * Protocol version (informational — not carried in wire frames)
 * ====================================================================== */

#define ARQ_PROTO_VERSION  3   /* v3: 8-byte header, no proto_ver field on wire */

/* ======================================================================
 * Frame header layout (v3, 8 bytes total)
 *
 * Proto_ver field removed — both sides always run the same binary.
 * ack_delay reduced to 1 byte (10ms units, max 2.55s — covers all real delays).
 * HAS_SNR bit removed — snr_raw==0 already signals "unknown".
 *
 *  Byte 0: framer byte — set/validated by write_frame_header()/parse_frame_header()
 *            bits [7:6] = packet_type (PACKET_TYPE_ARQ_CONTROL or PACKET_TYPE_ARQ_DATA)
 *            bits [5:0] = CRC6 of bytes [1..frame_size-1]
 *  Byte 1: subtype      — arq_subtype_t
 *  Byte 2: flags        — bit7=TURN_REQ, bit6=HAS_DATA, bits[5:0]=spare
 *  Byte 3: session_id   — random byte chosen by caller at connect time
 *  Byte 4: tx_seq       — sender's frame sequence number
 *  Byte 5: rx_ack_seq   — last sequence number received from peer
 *  Byte 6: snr_raw      — local RX SNR feedback to peer; 0=unknown
 *                         encoded as uint8_t: (int)round(snr_dB) + 128, clamped 1-255
 *  Byte 7: ack_delay    — IRS→ISS: time from data_rx to ack_tx, in 10ms units; 0=unknown
 *                         ISS computes: OTA_RTT = (ack_rx_ms - data_tx_start_ms) - ack_delay×10
 *
 * CONNECT frames (CALL/ACCEPT) use a separate compact layout — see below.
 * They are identified by: DATAC13 mode + PACKET_TYPE_ARQ_DATA from framer byte.
 *
 * Payload bytes (DATA frames only) follow immediately after byte 7.
 * ====================================================================== */

#define ARQ_HDR_SUBTYPE_IDX   1
#define ARQ_HDR_FLAGS_IDX     2
#define ARQ_HDR_SESSION_IDX   3
#define ARQ_HDR_SEQ_IDX       4
#define ARQ_HDR_ACK_IDX       5
#define ARQ_HDR_SNR_IDX       6
#define ARQ_HDR_DELAY_IDX     7
#define ARQ_FRAME_HDR_SIZE    8   /* bytes 0-7 inclusive */

/* CONNECT frames (CALL/ACCEPT) compact layout — 14 bytes, DATAC13 only.
 * Uses PACKET_TYPE_ARQ_DATA (framer routes it as data).
 * Distinguished from regular DATA by the fact that these are always sent
 * in DATAC13 mode; the receiver's DATAC13 decoder hands them to ARQ which
 * checks packet_type and frame_size to identify them.
 *
 *  Byte 0: packet_type (PACKET_TYPE_ARQ_DATA, set by write_frame_header)
 *  Byte 1: connect_meta  = (session_id & 0x7F) | (is_accept ? 0x80 : 0x00)
 *  Bytes 2-13: arithmetic-encoded "DST|SRC" callsign string (12 bytes max)
 */
#define ARQ_CONNECT_SESSION_IDX       1
#define ARQ_CONNECT_PAYLOAD_IDX       2
#define ARQ_CONNECT_SESSION_MASK      0x7F
#define ARQ_CONNECT_ACCEPT_FLAG       0x80
#define ARQ_CONTROL_FRAME_SIZE        14
#define ARQ_CONNECT_META_SIZE         2   /* framer byte + connect_meta byte */
#define ARQ_CONNECT_MAX_ENCODED       (ARQ_CONTROL_FRAME_SIZE - ARQ_CONNECT_META_SIZE)

/* ======================================================================
 * Flags byte (byte 2)
 * ====================================================================== */

#define ARQ_FLAG_TURN_REQ  0x80  /* bit 7: sender requests role turn          */
#define ARQ_FLAG_HAS_DATA  0x40  /* bit 6: sender has data queued (IRS→ISS)   */

/* ======================================================================
 * Frame subtypes
 * ====================================================================== */

typedef enum
{
    ARQ_SUBTYPE_CALL          =  1,
    ARQ_SUBTYPE_ACCEPT        =  2,
    ARQ_SUBTYPE_ACK           =  3,
    ARQ_SUBTYPE_DISCONNECT    =  4,
    ARQ_SUBTYPE_DATA          =  5,
    ARQ_SUBTYPE_KEEPALIVE     =  6,
    ARQ_SUBTYPE_KEEPALIVE_ACK =  7,
    ARQ_SUBTYPE_MODE_REQ      =  8,
    ARQ_SUBTYPE_MODE_ACK      =  9,
    ARQ_SUBTYPE_TURN_REQ      = 10,
    ARQ_SUBTYPE_TURN_ACK      = 11,
    /* Subtype 12 (FLOW_HINT) removed in v3 — replaced by HAS_DATA flag */
} arq_subtype_t;

/* ======================================================================
 * Parsed frame header (in-memory representation, not wire layout)
 * ====================================================================== */

typedef struct
{
    uint8_t  packet_type;   /* PACKET_TYPE_ARQ_CONTROL or _DATA   (from framer byte) */
    uint8_t  subtype;       /* arq_subtype_t                                         */
    uint8_t  flags;         /* ARQ_FLAG_* bitmask                                    */
    uint8_t  session_id;
    uint8_t  tx_seq;
    uint8_t  rx_ack_seq;
    uint8_t  snr_raw;       /* 0=unknown; decode via arq_protocol_decode_snr         */
    uint8_t  ack_delay_raw; /* 0=unknown; 10ms units; decode via _decode_ack_delay   */
} arq_frame_hdr_t;

/* ======================================================================
 * Per-mode timing table
 *
 * All times are in seconds (float) measured from the moment PTT goes ON
 * unless noted otherwise.
 *
 * frame_duration_s: empirically measured on-air TX duration.
 * tx_period_s:      expected queue-to-PTT-ON latency (scheduling jitter).
 * ack_timeout_s:    maximum time from PTT-ON until ACK must be received.
 *                   ack_timeout ≥ frame_duration + propagation + ACK return
 *                   First frame deadline: enqueue_time + tx_period_s + ack_timeout_s
 *                   Retry deadline:       tx_start_ms  + ack_timeout_s
 * retry_interval_s: = ack_timeout_s + ARQ_ACK_GUARD_S
 * payload_bytes:    usable data bytes per frame.
 * ====================================================================== */

typedef struct
{
    int   freedv_mode;          /* FREEDV_MODE_* constant                        */
    float frame_duration_s;     /* measured TX duration                          */
    float tx_period_s;          /* queue-to-PTT-ON latency                       */
    float ack_timeout_s;        /* from PTT-ON to ACK deadline                   */
    float retry_interval_s;     /* ack_timeout_s + ACK_GUARD_S                   */
    int   payload_bytes;        /* usable payload per frame                      */
} arq_mode_timing_t;

/* Timing constants shared across modules */
#define ARQ_CHANNEL_GUARD_MS          400   /* channel guard after PTT-OFF (ms)    */
#define ARQ_ACK_GUARD_S               1     /* extra slack added to retry interval */
#define ARQ_CALL_RETRY_SLOTS          4     /* CALL retries before giving up       */
#define ARQ_ACCEPT_RETRY_SLOTS        3     /* ACCEPT retries before returning     */
#define ARQ_DATA_RETRY_SLOTS          10    /* DATA retries before disconnect      */
#define ARQ_DISCONNECT_RETRY_SLOTS    2     /* DISCONNECT frame retries            */
#define ARQ_CONNECT_GRACE_SLOTS       2     /* extra wait slots for ACCEPT         */
#define ARQ_CONNECT_BUSY_EXT_S        2     /* busy-extension guard after CALL     */
#define ARQ_KEEPALIVE_INTERVAL_S      20    /* keepalive TX interval               */
#define ARQ_KEEPALIVE_MISS_LIMIT      5     /* missed keepalives before disconnect */
#define ARQ_TURN_REQ_RETRIES          2
#define ARQ_MODE_REQ_RETRIES          2
#define ARQ_MODE_SWITCH_HYST_COUNT    1
#define ARQ_STARTUP_MAX_S             8     /* DATAC13-only startup window         */
#define ARQ_STARTUP_ACKS_REQUIRED     1
#define ARQ_PEER_PAYLOAD_HOLD_S       15    /* hold peer payload mode after activity */
#define ARQ_SNR_HYST_DB               1.0f
#define ARQ_BACKLOG_MIN_DATAC3        56
#define ARQ_BACKLOG_MIN_DATAC1        126
#define ARQ_BACKLOG_MIN_BIDIR_UPGRADE 48    /* > DATAC4 payload capacity          */

/* Mode table (defined in arq_protocol.c) */
extern const arq_mode_timing_t arq_mode_table[];
extern const int                arq_mode_table_count;

/* ======================================================================
 * Frame codec API
 * ====================================================================== */

/**
 * @brief Encode a parsed header into the first ARQ_FRAME_HDR_SIZE bytes of buf.
 * @return 0 on success, -1 if buf_len < ARQ_FRAME_HDR_SIZE.
 */
int arq_protocol_encode_hdr(uint8_t *buf, size_t buf_len, const arq_frame_hdr_t *hdr);

/**
 * @brief Decode the ARQ header from the first bytes of buf.
 * @return 0 on success, -1 if buf too short.
 */
int arq_protocol_decode_hdr(const uint8_t *buf, size_t buf_len, arq_frame_hdr_t *hdr);

/**
 * @brief Encode a floating-point SNR (dB) into the snr_raw wire byte.
 * @return Encoded byte (0 if snr_db is out of range or unknown).
 */
uint8_t arq_protocol_encode_snr(float snr_db);

/**
 * @brief Decode snr_raw wire byte back to float dB.
 * @return SNR in dB, or 0.0f if snr_raw == 0 (unknown).
 */
float arq_protocol_decode_snr(uint8_t snr_raw);

/**
 * @brief Encode ack_delay_ms to the 8-bit wire value (10ms units, max 2.55s).
 */
uint8_t arq_protocol_encode_ack_delay(uint32_t delay_ms);

/**
 * @brief Decode the 8-bit wire ack_delay to milliseconds.
 */
uint32_t arq_protocol_decode_ack_delay(uint8_t raw);

/**
 * @brief Look up mode timing entry for a FreeDV mode.
 * @return Pointer to timing entry, or NULL if mode is unknown.
 */
const arq_mode_timing_t *arq_protocol_mode_timing(int freedv_mode);

#endif /* ARQ_PROTOCOL_H_ */

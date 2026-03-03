# HERMES ARQ Datalink — Architecture and Protocol Reference

This document covers the ARQ (Automatic Repeat Request) datalink layer in
Mercury v2 (`datalink_arq/`). It replaces the original monolithic `arq.c` with
a table-driven, two-level hierarchical FSM and a clean protocol codec.

---

## Table of Contents

1. [Overview](#overview)
2. [Module Map](#module-map)
3. [Wire Protocol v3](#wire-protocol-v3)
4. [Frame Types and Subtypes](#frame-types-and-subtypes)
5. [Mode Timing Table](#mode-timing-table)
6. [Two-Level FSM](#two-level-fsm)
   - [Level 1 — Connection FSM](#level-1--connection-fsm)
   - [Level 2 — Data-Flow Sub-FSM](#level-2--data-flow-sub-fsm)
7. [Events](#events)
8. [Thread Architecture](#thread-architecture)
9. [Session Roles: ISS and IRS](#session-roles-iss-and-irs)
10. [Turn Mechanism](#turn-mechanism)
11. [Mode Upgrade / Downgrade](#mode-upgrade--downgrade)
12. [Keepalive](#keepalive)
13. [Timing Instrumentation](#timing-instrumentation)
14. [Logging](#logging)
15. [Tuning Constants Reference](#tuning-constants-reference)
16. [Source Files](#source-files)

---

## Overview

Mercury ARQ provides a reliable, half-duplex point-to-point data channel over HF
radio using the FreeDV modem stack.

Key properties:
- **Half-duplex**, explicit turn-taking (ISS/IRS roles).
- **DATAC13** is the *control-only* mode: CALL, ACCEPT, ACK, DISCONNECT,
  TURN_REQ/ACK, KEEPALIVE, MODE_REQ/ACK frames are always 14 bytes on DATAC13.
- **Data frames** start in DATAC4 (54-byte modem frame, 46 user bytes) and may
  upgrade to DATAC3 (126-byte frame, 118 user bytes) or DATAC1 (510-byte frame,
  502 user bytes) based on SNR and TX backlog.
- **VARA-compatible TCP TNC** interface: control on `base_port` (default 8300),
  data on `base_port+1` (8301). This interface is frozen and not modified by the
  ARQ rewrite.
- **Broadcast** runs in parallel on a separate TCP port (default 8100) and is
  completely independent of ARQ.

---

## Module Map

```
datalink_arq/
  arq.c           — Public API, event-loop thread, TCP cmd/payload bridge threads,
                    FSM callbacks, incoming-frame dispatch, session snapshot API
  arq_fsm.c/.h    — Two-level hierarchical FSM: all state transitions, action
                    callbacks, reliability ladder, mode negotiation, keepalive,
                    turn-taking; arq_session_t holds all session state (no statics)
  arq_protocol.c/.h — Wire codec: mode timing table, frame builder/parse API,
                      SNR and ack_delay encoding helpers
  arq_modem.c/.h  — Action queue (FSM -> modem worker), PTT ON/OFF notifications,
                    mode-selection helpers
  arq_channels.c/.h — Channel bus connecting TCP, modem, and ARQ threads
  arq_events.h    — Bus message types (arq_bus_msg_t), TCP command/status enums,
                    and arq_event_t that flows through the internal event queue
  arq_timing.c/.h — Per-frame timing instrumentation; [TMG] log lines
  arq.h           — Public types: arq_info, arq_action_t, arq_runtime_snapshot_t
  fsm.c/.h        — Legacy single-level FSM wrapper kept for link-time compatibility
  arith.c         — Arithmetic codec for callsign compression in CALL/ACCEPT frames
```

---

## Wire Protocol v3

All ARQ frames begin with a **framer byte** managed by `modem/framer.c`:

```
Byte 0: [packet_type(3)] | [CRC5(5)]
         bits [7:5] = packet_type
         bits [4:0] = CRC5 of bytes [1 .. frame_size-1]
```

Packet type values (defined in `modem/framer.h`):

| Value | Constant                        | Used for                               |
|-------|---------------------------------|----------------------------------------|
| 0x0   | `PACKET_TYPE_ARQ_CONTROL`       | ACK, DISCONNECT, KEEPALIVE, TURN, MODE |
| 0x1   | `PACKET_TYPE_ARQ_DATA`          | Data payload frames                    |
| 0x2   | `PACKET_TYPE_ARQ_CALL`          | CALL and ACCEPT compact frames         |
| 0x3   | `PACKET_TYPE_BROADCAST_CONTROL` | Broadcast subsystem (unrelated)        |
| 0x4   | `PACKET_TYPE_BROADCAST_DATA`    | Broadcast subsystem (unrelated)        |

### Standard 8-byte header (ARQ_CONTROL and ARQ_DATA)

```
Byte 0: framer byte  (packet_type | CRC5)
Byte 1: subtype      (arq_subtype_t enum value)
Byte 2: flags        bit7=TURN_REQ  bit6=HAS_DATA  bit5=LEN_HI (DATA frames only)
Byte 3: session_id   random byte chosen by caller at connect time
Byte 4: tx_seq       sender's 8-bit frame sequence number (wraps at 256)
Byte 5: rx_ack_seq   last sequence number received from peer (implicit ACK)
Byte 6: snr_raw      local RX SNR feedback:
                     uint8_t = (int)round(snr_dB) + 128, clamped to [1, 255]
                     0 = unknown
Byte 7: ack_delay    IRS->ISS: time from data_rx to ack_tx, in 10 ms units; 0=unknown
                     ISS computes: OTA_RTT = (ack_rx_ms - tx_start_ms) - ack_delay*10
```

**DATA frames** (`PACKET_TYPE_ARQ_DATA`): payload bytes follow immediately after
byte 7. The payload slot size is the FreeDV mode frame size minus 8 (the header).

#### `payload_valid` / `ARQ_FLAG_LEN_HI` in DATA frames

Byte 7 is **repurposed** in DATA frames — it carries `payload_valid` rather than
`ack_delay`:
- `0` (`ARQ_DATA_LEN_FULL`) = full slot; all user bytes are valid data.
- `1 .. N` = only this many leading bytes carry user data; the rest is padding.
- `ARQ_FLAG_LEN_HI` (bit 5 of flags byte) carries bit 8 of the count, allowing
  values up to 511 — needed because DATAC1 has a 502-byte user-data slot.

This allows partially-filled last frames to be transmitted with a correct CRC5,
since the modem slot is always padded to its full size before framing.

### CALL/ACCEPT compact frame (ARQ_CALL, 14 bytes)

CALL and ACCEPT use a different layout to fit two callsigns in 14 bytes:

```
Byte 0:     framer byte  (PACKET_TYPE_ARQ_CALL | CRC5)
Byte 1:     connect_meta = (session_id & 0x7F) | (is_accept ? 0x80 : 0x00)
Bytes 2-13: arithmetic-encoded "DST|SRC" callsign string (12 bytes max)
```

The callsign pair is compressed with an arithmetic codec (`arith.c`).

### CRC5

Polynomial `0x15` (x^5 + x^4 + x^2 + 1), non-reflected, init=1.
Covers bytes `[1 .. frame_size-1]` (the full frame, excluding byte 0 itself).
False-positive rate: 1/32 ≈ 3.1 %.
Implementation: `crc5_0X15()` in `common/crc6.c`, used by `modem/framer.c`.

---

## Frame Types and Subtypes

| Subtype | Constant                    | Direction       | Mode used  |
|---------|-----------------------------|-----------------|------------|
| 1       | `ARQ_SUBTYPE_CALL`          | Caller -> Callee | DATAC13   |
| 2       | `ARQ_SUBTYPE_ACCEPT`        | Callee -> Caller | DATAC13   |
| 3       | `ARQ_SUBTYPE_ACK`           | IRS -> ISS       | DATAC13   |
| 4       | `ARQ_SUBTYPE_DISCONNECT`    | Either          | DATAC13    |
| 5       | `ARQ_SUBTYPE_DATA`          | ISS -> IRS       | DATAC4/3/1|
| 6       | `ARQ_SUBTYPE_KEEPALIVE`     | Either          | DATAC13    |
| 7       | `ARQ_SUBTYPE_KEEPALIVE_ACK` | Either          | DATAC13    |
| 8       | `ARQ_SUBTYPE_MODE_REQ`      | ISS -> IRS       | DATAC13   |
| 9       | `ARQ_SUBTYPE_MODE_ACK`      | IRS -> ISS       | DATAC13   |
| 10      | `ARQ_SUBTYPE_TURN_REQ`      | IRS -> ISS       | DATAC13   |
| 11      | `ARQ_SUBTYPE_TURN_ACK`      | ISS -> IRS       | DATAC13   |

Note: Subtype 12 (`FLOW_HINT`) was removed in v3; the `ARQ_FLAG_HAS_DATA` bit
(bit 6 of the flags byte) serves the same purpose.

MODE_REQ and MODE_ACK carry the requested `FREEDV_MODE_*` constant as the first
payload byte immediately after the 8-byte header.

---

## Mode Timing Table

Values measured empirically from bench and OTA tests. All times in seconds.
`frame_duration_s` is the measured on-air TX duration (PTT-ON to PTT-OFF).
`ack_timeout_s` is set at PTT-OFF and must cover the worst case where the peer
piggybacks a DATA frame immediately after its ACK:

```
ack_timeout >= ACK_return + inter_frame_gap + piggybacked_DATA_dur + margin
```

Measured bench constants used to derive the table:
- ACK round trip (PTT-OFF to ack_rx, ACK always on DATAC13): ~2848 ms
- IRS inter-frame gap (ACK PTT-OFF to next DATA PTT-ON): ~1035 ms

| Mode    | Frame size (bytes) | Frame duration | TX period | ACK timeout | Retry interval |
|---------|--------------------|----------------|-----------|-------------|----------------|
| DATAC13 | 14                 | 2.50 s         | 1.0 s     | 6.0 s       | 7.0 s          |
| DATAC4  | 54                 | 5.80 s         | 1.0 s     | 12.0 s      | 13.0 s         |
| DATAC3  | 126                | 3.82 s         | 1.0 s     | 9.0 s       | 10.0 s         |
| DATAC1  | 510                | 4.81 s         | 1.0 s     | 11.0 s      | 12.0 s         |

- **Frame size**: total modem frame = 8-byte ARQ header + user bytes
  (DATAC13: 14 total; DATAC4: 46 user; DATAC3: 118 user; DATAC1: 502 user).
- **ACK timeout**: from PTT-OFF to ACK reception deadline.
- **Retry interval**: `ack_timeout + ARQ_ACK_GUARD_S (1 s)`.
- **IRS response guard** (`ARQ_CHANNEL_GUARD_MS = 700 ms`): applied by IRS after
  frame decode before transmitting ACK/TURN_REQ. The OFDM decoder fires ~200 ms
  before sender PTT-OFF, so the effective air gap is ~500 ms — sufficient for
  the ~340 ms radio TX->RX hardware switch with ~160 ms preamble detection margin.
- **ISS post-ACK guard** (`ARQ_ISS_POST_ACK_GUARD_MS = 900 ms`): applied by ISS
  before resuming DATA TX after receiving an ACK. Larger than the IRS guard
  because `ack_rx` fires ~168 ms before IRS PTT-OFF — at 500 ms the effective
  gap was only ~432 ms, causing ~50% ACK loss on DATAC1. At 900 ms the gap is
  ~832 ms, giving 492 ms of clear air before the next ISS DATA preamble.

All timing constants are in `arq_protocol.h`.

---

## Two-Level FSM

The FSM is entirely in `arq_fsm.c`. All state is in `arq_session_t`; there are
no hidden global flags. The FSM is driven by a single event-loop thread via
`arq_fsm_dispatch()`.

### Level 1 — Connection FSM

States (`arq_conn_state_t`, prefix `ARQ_CONN_`):

| State           | Description                                        |
|-----------------|----------------------------------------------------|
| `DISCONNECTED`  | No active session; idle                            |
| `LISTENING`     | Waiting for an incoming CALL frame                 |
| `CALLING`       | Outgoing CALL sent; awaiting ACCEPT                |
| `ACCEPTING`     | ACCEPT sent; awaiting first data or ACK            |
| `CONNECTED`     | Data-flow sub-FSM is active                        |
| `DISCONNECTING` | DISCONNECT frame exchange in progress              |

### Level 2 — Data-Flow Sub-FSM

Active only when L1 is in `CONNECTED`. Manages half-duplex turn-taking,
retransmission, mode negotiation, and keepalive.

States (`arq_dflow_state_t`, prefix `ARQ_DFLOW_`):

| State            | Description                                       |
|------------------|---------------------------------------------------|
| `IDLE_ISS`       | ISS: no pending frame; waiting for TX data        |
| `DATA_TX`        | ISS: frame queued or on air                       |
| `WAIT_ACK`       | ISS: PTT-OFF; waiting for peer ACK                |
| `IDLE_IRS`       | IRS: waiting for peer data frame                  |
| `DATA_RX`        | IRS: data frame decoded; ACK pending              |
| `ACK_TX`         | IRS: ACK frame being transmitted                  |
| `TURN_REQ_TX`    | IRS->ISS: TURN_REQ being transmitted              |
| `TURN_REQ_WAIT`  | IRS->ISS: waiting for TURN_ACK                   |
| `TURN_ACK_TX`    | ISS->IRS: TURN_ACK being transmitted              |
| `MODE_REQ_TX`    | Mode negotiation: MODE_REQ being transmitted      |
| `MODE_REQ_WAIT`  | Mode negotiation: waiting for MODE_ACK            |
| `MODE_ACK_TX`    | Mode negotiation: MODE_ACK being transmitted      |
| `KEEPALIVE_TX`   | KEEPALIVE being transmitted                       |
| `KEEPALIVE_WAIT` | Waiting for KEEPALIVE_ACK                         |

---

## Events

Events are typed `arq_event_t` and dispatched via `arq_fsm_dispatch()`. Event
identifiers use the `arq_event_id_t` enum (full prefix `ARQ_EV_`).

### Application events (from TCP TNC via channel bus)

| Event (`ARQ_EV_*`)  | Triggered by                     |
|---------------------|----------------------------------|
| `APP_LISTEN`        | `LISTEN ON` command              |
| `APP_STOP_LISTEN`   | `LISTEN OFF` command             |
| `APP_CONNECT`       | `CONNECT <dst>` command          |
| `APP_DISCONNECT`    | `DISCONNECT` command             |
| `APP_DATA_READY`    | Data bytes arrived from TCP      |

### Radio RX events (from modem worker)

| Event (`ARQ_EV_*`)  | Triggered by                     |
|---------------------|----------------------------------|
| `RX_CALL`           | CALL frame decoded               |
| `RX_ACCEPT`         | ACCEPT frame decoded             |
| `RX_ACK`            | ACK frame decoded                |
| `RX_DATA`           | DATA frame decoded               |
| `RX_DISCONNECT`     | DISCONNECT frame decoded         |
| `RX_TURN_REQ`       | TURN_REQ frame decoded           |
| `RX_TURN_ACK`       | TURN_ACK frame decoded           |
| `RX_MODE_REQ`       | MODE_REQ frame decoded           |
| `RX_MODE_ACK`       | MODE_ACK frame decoded           |
| `RX_KEEPALIVE`      | KEEPALIVE frame decoded          |
| `RX_KEEPALIVE_ACK`  | KEEPALIVE_ACK frame decoded      |

### Timer events (synthesised by the event loop)

| Event (`ARQ_EV_*`)   | Fired when                                            |
|----------------------|-------------------------------------------------------|
| `TIMER_RETRY`        | Retry deadline expired                                |
| `TIMER_TIMEOUT`      | Session/call timeout expired                          |
| `TIMER_ACK`          | ACK wait deadline expired                             |
| `TIMER_PEER_BACKLOG` | Peer-backlog hold (`ARQ_PEER_PAYLOAD_HOLD_S`) expired |
| `TIMER_KEEPALIVE`    | Keepalive interval (`ARQ_KEEPALIVE_INTERVAL_S`) fired |

The session holds a single `deadline_ms` / `deadline_event` pair; the event loop
calls `arq_fsm_timeout_ms()` to determine the next poll timeout.

### Modem events (from `arq_modem.c`)

| Event (`ARQ_EV_*`) | Triggered by                      |
|--------------------|-----------------------------------|
| `TX_STARTED`       | `arq_modem_ptt_on()` (PTT ON)     |
| `TX_COMPLETE`      | `arq_modem_ptt_off()` (PTT OFF)   |

---

## Thread Architecture

```
TCP client
  |  (commands)
  v
arq_cmd_bridge_worker -------> internal event queue
  |  (arq_channel_bus tcp_cmd)         |
                                        v
arq_payload_bridge_worker -> arq_event_loop_worker
  (tcp_payload -> 64 KiB app      (arq_fsm_dispatch)
   TX ring buffer, posts               |
   APP_DATA_READY)                     v
                               arq_modem action queue
                                        |
                                        v
                                modem TX worker (modem.c)
```

- **`arq_event_loop_worker`** — single FSM dispatch thread. Dequeues `arq_event_t`
  items from a 64-slot ring buffer (`g_evq`), calls `arq_fsm_dispatch()`, then fires
  any deadline event if `sess.deadline_ms` has passed. All FSM logic and `arq_session_t`
  access is confined to this thread; no internal locking is required.
- **`arq_cmd_bridge_worker`** — reads `arq_cmd_msg_t` from the channel bus and
  converts TCP TNC commands (`LISTEN ON`, `CONNECT`, etc.) into FSM events.
- **`arq_payload_bridge_worker`** — reads payload bytes from the channel bus into
  the 64 KiB app TX ring buffer (`g_app_tx_buf`) and posts `ARQ_EV_APP_DATA_READY`.
- **Modem TX worker** (in `modem.c`) — dequeues `arq_action_t` items from the
  action queue (`arq_modem.c`) and drives the FreeDV TX pipeline.

---

## Session Roles: ISS and IRS

- **ISS** (Information Sending Station): the side currently transmitting data.
- **IRS** (Information Receiving Station): the side currently receiving and ACKing.

At connection establishment:
- **Caller** (side that issued `CONNECT`) starts as **ISS** (sends data first).
- **Callee** (side in `LISTEN ON`) starts as **IRS** (receives first).

This matches the VARA protocol convention and the UUCP use case where the
connecting side sends session-initiation data first.

Role is stored in `arq_session_t.role` (`ARQ_ROLE_CALLER` / `ARQ_ROLE_CALLEE`)
and remains fixed for the session lifetime; only the ISS/IRS data-flow assignment
changes on each turn.

---

## Turn Mechanism

Either side may request a role reversal.

**Piggyback turn** (fast path — zero extra frames):
1. IRS has TX data pending.
2. IRS sets `ARQ_FLAG_HAS_DATA` (bit 6) in the ACK frame.
3. ISS receives ACK with `HAS_DATA` set -> transitions immediately to IRS;
   IRS transitions to ISS and begins sending.

**Explicit TURN_REQ / TURN_ACK** (when piggyback is not available):
1. IRS waits `ARQ_TURN_WAIT_AFTER_ACK_MS` (3500 ms) after its last ACK, then
   sends `TURN_REQ` (includes `rx_ack_seq` so ISS can flush pending retries).
2. ISS stops sending, sends `TURN_ACK`.
3. Both sides swap ISS/IRS roles.

`ARQ_PEER_PAYLOAD_HOLD_S` (15 s): after the peer's last data activity, ISS holds
off mode negotiations that would downgrade the modem — the peer may still be
preparing its TX burst.

---

## Mode Upgrade / Downgrade

### Reliability Ladder

Mode selection uses a `speed_level` integer ladder (`arq_session_t.speed_level`):

| Level | Mode    | Total frame size | User bytes/frame |
|-------|---------|------------------|------------------|
| 0     | DATAC4  | 54 bytes         | 46 bytes         |
| 1     | DATAC3  | 126 bytes        | 118 bytes        |
| 2     | DATAC1  | 510 bytes        | 502 bytes        |

> **Note on FreeDV mode integers**: `FREEDV_MODE_DATAC1=10`, `FREEDV_MODE_DATAC3=12`,
> `FREEDV_MODE_DATAC4=18`, `FREEDV_MODE_DATAC13=19`. The integer values do *not* rank
> by throughput. Always use the speed_level, not the raw enum, for comparisons.

**Step up**: after `ARQ_LADDER_UP_SUCCESSES` (4) consecutive clean ACKs (no retry
consumed for that frame), `speed_level` increments by 1.

**Step down**: on any retry, `speed_level` decrements by 1 immediately (one step
at a time — never jumps straight to level 0).

### SNR Gate and Mode Selection

Mode negotiations are triggered by `maybe_upgrade_mode()`, called after each ACK.
The desired mode is computed by `select_best_mode()`:

- Upgrade to **DATAC3** requires all of:
  - `peer_snr >= ARQ_SNR_MIN_DATAC3_DB + ARQ_SNR_HYST_DB` (i.e. >= 0.0 dB)
  - backlog >= `ARQ_BACKLOG_MIN_DATAC3` (56 bytes)
  - `speed_level >= 1`

- Upgrade to **DATAC1** requires all of:
  - `peer_snr >= ARQ_SNR_MIN_DATAC1_DB + ARQ_SNR_HYST_DB` (i.e. >= 4.0 dB)
  - backlog >= `ARQ_BACKLOG_MIN_DATAC1` (126 bytes)
  - `speed_level == 2`

- **Downgrade** is ladder-driven only: when `speed_level` drops below the rank of
  the current `payload_mode`, `select_best_mode()` returns the ladder cap mode.
  SNR is deliberately NOT used to force downgrades — a link just below the SNR
  threshold but with zero retries stays at the current mode.

- No negotiation is triggered when the entire TX backlog fits in a single
  current-mode frame (MODE_REQ/ACK overhead would exceed the benefit).

A **hysteresis counter** (`mode_upgrade_count`, threshold `ARQ_MODE_SWITCH_HYST_COUNT=3`)
requires 3 consecutive observations of a new desired mode before a `MODE_REQ` is sent.

### Negotiation Procedure

1. ISS sends `MODE_REQ` (payload byte = desired `FREEDV_MODE_*` constant) and enters
   `MODE_REQ_TX` / `MODE_REQ_WAIT`.
2. IRS responds with `MODE_ACK` (same payload byte); both sides switch decoder/encoder.
3. If no `MODE_ACK` is received, ISS retries up to `ARQ_MODE_REQ_RETRIES` (2) times
   before abandoning the negotiation for this cycle.

### Startup Window

For `ARQ_STARTUP_MAX_S` (8 s) after session creation, `maybe_upgrade_mode()` is a
no-op — only DATAC13 is used. `ARQ_STARTUP_ACKS_REQUIRED` (1) clean ACK must also
be received before the first ladder step-up is considered.

---

## Keepalive

When the link is idle (no data in either direction):
- Every `ARQ_KEEPALIVE_INTERVAL_S` (20 s), the current ISS sends a `KEEPALIVE` frame.
- The peer responds with `KEEPALIVE_ACK`.
- If `ARQ_KEEPALIVE_MISS_LIMIT` (5) consecutive keepalives receive no reply, the
  session is torn down with a local `DISCONNECT`.

---

## Timing Instrumentation

`arq_timing.c` records timestamps for every significant event in `arq_timing_ctx_t`
and emits `[TMG]` log lines. All timestamps are `uint64_t` monotonic milliseconds
from `hermes_log_init()` (via `hermes_uptime_ms()`).

### Recorded events

| Function                           | Log tag      | What it records                           |
|------------------------------------|--------------|-------------------------------------------|
| `arq_timing_record_tx_queue`       | `tx_queue`   | Frame submitted to action queue           |
| `arq_timing_record_tx_start`       | `tx_start`   | PTT ON (frame now on air)                 |
| `arq_timing_record_tx_end`         | `tx_end`     | PTT OFF + on-air duration                 |
| `arq_timing_record_ack_rx`         | `ack_rx`     | ACK received + OTA RTT + peer SNR         |
| `arq_timing_record_data_rx`        | `data_rx`    | Data frame decoded (IRS side) + local SNR |
| `arq_timing_record_ack_tx`         | `ack_tx`     | ACK TX started (IRS side) + local delay   |
| `arq_timing_record_retry`          | `retry`      | Retry event + attempt number + reason     |
| `arq_timing_record_turn`           | `turn`       | Role change + reason string               |
| `arq_timing_record_connect`        | `connect`    | Session established + initial mode        |
| `arq_timing_record_disconnect`     | `disconnect` | Session ended + cumulative session totals |

### OTA RTT computation

```
OTA_RTT = (ack_rx_ms - tx_start_ms) - ack_delay * 10 ms
```

`ack_delay` (byte 7 of the ACK frame, 10 ms units) is the IRS-reported time
between receiving the data frame and starting its ACK transmission. ISS subtracts
this to get the pure over-the-air round-trip time.

---

## Logging

Mercury uses an asynchronous ring-buffer logger (`common/hermes_log.c`).

### Log levels

| Level  | Tag   | When emitted                                        |
|--------|-------|-----------------------------------------------------|
| DEBUG  | `DBG` | Internal FSM traces (only with `-v` flag)           |
| TIMING | `TMG` | Protocol timing events (always when `-L` file used) |
| INFO   | `INF` | General status (default console level)              |
| WARN   | `WRN` | Retries, degraded conditions                        |
| ERROR  | `ERR` | Fatal / unexpected errors                           |

### CLI flags

| Flag        | Effect                                                               |
|-------------|----------------------------------------------------------------------|
| `-v`        | Set console log level to DEBUG (shows all `[DBG]` and `[TMG]` lines)|
| `-L <path>` | Write TIMING level and above to file at `<path>`                     |
| `-J`        | Combined with `-L`: write file in **JSONL** format for machine parsing|

Example for OTA analysis:

```sh
./mercury -v -L /tmp/mercury-session.log
```

JSONL example (one JSON object per line):

```json
{"ts":"14:42:36.900","uptime_ms":12224,"level":"TMG","component":"arq","msg":"tx_start seq=0 mode=18 backlog=54"}
{"ts":"14:42:39.838","uptime_ms":15162,"level":"TMG","component":"arq","msg":"tx_end seq=0 dur_ms=2938"}
{"ts":"14:42:42.110","uptime_ms":17434,"level":"TMG","component":"arq","msg":"ack_rx seq=0 rtt_ms=2272 ack_delay_ms=410 peer_snr=2.2dB"}
```

---

## Tuning Constants Reference

All constants are in `datalink_arq/arq_protocol.h` unless noted otherwise.

### Guard / timing constants

| Constant                      | Value    | Description                                             |
|-------------------------------|----------|---------------------------------------------------------|
| `ARQ_CHANNEL_GUARD_MS`        | 700 ms   | IRS response guard after frame decode before ACK TX     |
| `ARQ_ISS_POST_ACK_GUARD_MS`   | 900 ms   | ISS guard before resuming DATA TX after receiving ACK   |
| `ARQ_TURN_WAIT_AFTER_ACK_MS`  | 3500 ms  | IRS wait after last ACK before sending TURN_REQ         |
| `ARQ_ACCEPT_RX_WINDOW_MS`     | 9000 ms  | ACCEPTING state RX window after ACCEPT TX               |
| `ARQ_ACK_GUARD_S`             | 1 s      | Extra slack added to retry_interval                     |
| `ARQ_CONNECT_BUSY_EXT_S`      | 2 s      | Busy-extension guard after CALL frame                   |
| `ARQ_PEER_PAYLOAD_HOLD_S`     | 15 s     | Hold peer payload mode after last peer data activity    |
| `ARQ_STARTUP_MAX_S`           | 8 s      | DATAC13-only startup window after connect               |

### Retry limits

| Constant                      | Value | Description                               |
|-------------------------------|-------|-------------------------------------------|
| `ARQ_CALL_RETRY_SLOTS`        | 4     | CALL retries before giving up             |
| `ARQ_ACCEPT_RETRY_SLOTS`      | 4     | ACCEPT retries before returning           |
| `ARQ_DATA_RETRY_SLOTS`        | 10    | DATA retries before disconnect            |
| `ARQ_DISCONNECT_RETRY_SLOTS`  | 2     | DISCONNECT frame retries                  |
| `ARQ_CONNECT_GRACE_SLOTS`     | 2     | Extra wait slots for ACCEPT               |
| `ARQ_TURN_REQ_RETRIES`        | 2     | TURN_REQ retries                          |
| `ARQ_MODE_REQ_RETRIES`        | 2     | MODE_REQ retries before abandoning        |

### Mode selection thresholds

| Constant                      | Value   | Description                                               |
|-------------------------------|---------|-----------------------------------------------------------|
| `ARQ_SNR_HYST_DB`             | 1.0 dB  | Hysteresis added to all SNR upgrade thresholds            |
| `ARQ_SNR_MIN_DATAC4_DB`       | -4.0 dB | Minimum target SNR for DATAC4 (MPP path per codec2 docs)  |
| `ARQ_SNR_MIN_DATAC3_DB`       | -1.0 dB | Min SNR for DATAC3; effective upgrade gate = 0.0 dB       |
| `ARQ_SNR_MIN_DATAC1_DB`       | 3.0 dB  | Min SNR for DATAC1; effective upgrade gate = 4.0 dB       |
| `ARQ_BACKLOG_MIN_DATAC3`      | 56 B    | Minimum TX backlog to consider DATAC3 upgrade             |
| `ARQ_BACKLOG_MIN_DATAC1`      | 126 B   | Minimum TX backlog to consider DATAC1 upgrade             |
| `ARQ_BACKLOG_MIN_BIDIR_UPGRADE`| 48 B   | Bidir upgrade threshold (> DATAC4 payload capacity)       |

### Reliability ladder

| Constant                      | Value | Description                                               |
|-------------------------------|-------|-----------------------------------------------------------|
| `ARQ_LADDER_LEVELS`           | 3     | Number of speed levels (0=DATAC4, 1=DATAC3, 2=DATAC1)    |
| `ARQ_LADDER_UP_SUCCESSES`     | 4     | Consecutive clean ACKs required for one ladder step-up    |
| `ARQ_MODE_SWITCH_HYST_COUNT`  | 3     | Consecutive observations before MODE_REQ is sent          |
| `ARQ_STARTUP_ACKS_REQUIRED`   | 1     | Clean ACKs required before first ladder step-up           |

### Keepalive

| Constant                      | Value | Description                                |
|-------------------------------|-------|--------------------------------------------|
| `ARQ_KEEPALIVE_INTERVAL_S`    | 20 s  | Interval between KEEPALIVE transmissions   |
| `ARQ_KEEPALIVE_MISS_LIMIT`    | 5     | Missed keepalives before session teardown  |

---

## Source Files

| File                               | Role                                                       |
|------------------------------------|------------------------------------------------------------|
| `datalink_arq/arq.c`               | Public API, workers, FSM callbacks, frame dispatch         |
| `datalink_arq/arq.h`               | Public types: `arq_info`, `arq_action_t`, snapshot API     |
| `datalink_arq/arq_fsm.c`           | All FSM logic: transitions, mode ladder, turn, keepalive   |
| `datalink_arq/arq_fsm.h`           | FSM types: states, events, `arq_session_t`, callbacks      |
| `datalink_arq/arq_protocol.c`      | Mode timing table, frame builders/parsers, codec helpers   |
| `datalink_arq/arq_protocol.h`      | Wire constants, timing structs, tuning constants, codec API|
| `datalink_arq/arq_modem.c`         | Action queue, PTT ON/OFF event injection                   |
| `datalink_arq/arq_modem.h`         | Action queue and PTT callback API                          |
| `datalink_arq/arq_channels.c/.h`   | Channel bus init/close and typed send/receive helpers      |
| `datalink_arq/arq_events.h`        | Bus message union, TCP command/status enums, `arq_event_t` |
| `datalink_arq/arq_timing.c`        | Timing recorder implementation                             |
| `datalink_arq/arq_timing.h`        | `arq_timing_ctx_t` and record API                          |
| `datalink_arq/arith.c`             | Arithmetic codec for CALL/ACCEPT callsign compression      |
| `datalink_arq/fsm.c/.h`            | Legacy single-level FSM shim (link-time compatibility only)|
| `modem/framer.c/.h`                | Framer byte: `write_frame_header()`, `parse_frame_header()`|
| `common/hermes_log.c/.h`           | Async ring-buffer logger, `hermes_uptime_ms()`             |

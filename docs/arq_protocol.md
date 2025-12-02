# ARQ Protocol Specification

## Overview

The ARQ (Automatic Repeat Request) protocol provides reliable point-to-point communication with automatic error detection and retransmission.

## Frame Structure

### Header Format

```
Bit:  7  6  5  4  3  2  1  0
     ┌──┬──┬──┬──┬──┬──┬──┬──┐
     │PT│PT│CRC│CRC│CRC│CRC│CRC│CRC│
     └──┴──┴──┴──┴──┴──┴──┴──┘
      │  │  └──────────────┘
      │  │       CRC6
      └──┘
   Packet Type (2 bits)
```

- **Packet Type (PT)**: 2 bits, bits 6-7
  - `00`: Control
  - `01`: Data
  - `10`: ACK
  - `11`: NACK
- **CRC6**: 6 bits, bits 0-5
  - CRC6 polynomial: 0x6F
  - Calculated over payload (header + 1, payload)

### Control Frame Format

```
┌─────────────┬──────────────┬─────────────────────┐
│ Header      │ Sequence (0) │ Subtype            │
│ (1 byte)    │ (1 byte)     │ (1 byte)           │
└─────────────┴──────────────┴─────────────────────┘
│ Encoded Callsign Data (variable)                 │
└──────────────────────────────────────────────────┘
```

**Control Subtypes:**
- `0x01`: CALL_REQUEST - Connection request
- `0x02`: CALL_RESPONSE - Connection acceptance
- `0x03`: DISCONNECT - Disconnection request

**Callsign Encoding:**
- Uses arithmetic encoding
- Format: `DESTINATION|SOURCE` for CALL_REQUEST
- Format: `CALLSIGN` for CALL_RESPONSE

### Data Frame Format

```
┌─────────────┬──────────────┬─────────────────────┐
│ Header      │ Sequence     │ Payload             │
│ (1 byte)    │ (7 bits)     │ (variable)         │
└─────────────┴──────────────┴─────────────────────┘
```

- **Sequence**: 7-bit sequence number (0-127)
- **Payload**: User data

### ACK/NACK Frame Format

```
┌─────────────┬──────────────┐
│ Header      │ Sequence     │
│ (1 byte)    │ (7 bits)     │
└─────────────┴──────────────┘
```

- **Sequence**: Acknowledged sequence number

## Protocol Flow

### Connection Establishment

```
Caller                          Callee
  │                               │
  │── CALL_REQUEST ──────────────>│
  │   (dst|src callsigns)         │
  │                               │── Validate dst callsign
  │                               │
  │<── CALL_RESPONSE ─────────────│
  │   (callee callsign)           │
  │                               │
  │── Validate callsign           │
  │── Connection Established      │
```

**Timeout:** 10 seconds for connection establishment
**Retries:** Up to 3 attempts

### Data Transfer

```
Sender                          Receiver
  │                               │
  │── DATA (seq=N) ─────────────>│
  │                               │── Validate CRC
  │                               │── Check sequence
  │                               │
  │<── ACK (seq=N) ──────────────│  (if correct)
  │                               │
  │── Increment sequence          │
  │── Send next frame             │
```

**On Error:**
```
Sender                          Receiver
  │                               │
  │── DATA (seq=N) ─────────────>│
  │                               │── CRC error or wrong seq
  │                               │
  │<── NACK (seq=expected) ───────│
  │                               │
  │── Retransmit                  │
```

**Timeout:** 2 seconds for ACK
**Max Retries:** 3 attempts

## State Machine

See [ARQ States](arq_states.md) for detailed state transitions.

## Sequence Number Management

- Sequence numbers are 7-bit (0-127)
- Wraps around: 127 → 0
- Receiver maintains expected sequence number
- Out-of-order frames are rejected with NACK

## Error Handling

### CRC Errors

- Invalid CRC → Frame discarded
- No response sent (silent discard)

### Sequence Errors

- Wrong sequence → NACK sent with expected sequence
- Sender retransmits

### Timeouts

- ACK timeout → Retransmit
- Connection timeout → Abort connection

## Implementation Notes

### Frame Size

Frame size is determined dynamically from the modem:
```c
size_t bits_per_frame = freedv_get_bits_per_modem_frame(freedv);
size_t frame_size = bits_per_frame / 8;
size_t payload_size = frame_size - HEADER_SIZE;
```

### Buffer Management

- TX buffer: `data_tx_buffer_arq`
- RX buffer: `data_rx_buffer_arq`
- Circular buffers with configurable size

### Thread Safety

- FSM uses mutex for thread-safe state transitions
- Buffers are thread-safe circular buffers

## Configuration

### Constants

- `CALL_BURST_SIZE`: 3 frames (connection establishment)
- `MAX_RETRIES`: 3 attempts
- `ACK_TIMEOUT_MS`: 2000ms
- `CONNECTION_TIMEOUT_MS`: 10000ms

### Callsign Format

- Maximum length: 15 characters
- Allowed characters: A-Z, 0-9, '-'
- Case-insensitive

## Examples

### Connection Request

```
Header:    0x00 (CONTROL, CRC=...)
Sequence:  0x00
Subtype:   0x01 (CALL_REQUEST)
Payload:   Arithmetic encoded "DEST|SOURCE"
```

### Data Frame

```
Header:    0x40 (DATA, CRC=...)
Sequence:  0x05
Payload:   "Hello World"
```

### ACK Frame

```
Header:    0x80 (ACK, CRC=...)
Sequence:  0x05
```

# HERMES Modem Documentation

Welcome to the HERMES (High-Frequency Emergency and Rural Multimedia Exchange System) modem documentation.

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [ARQ Protocol](#arq-protocol)
4. [Broadcast Protocol](#broadcast-protocol)
5. [Testing](#testing)
6. [API Reference](#api-reference)
7. [Configuration](#configuration)

## Overview

HERMES is a digital modem system designed for HF (High Frequency) radio communications, providing reliable data transmission over long distances. It supports two main modes:

- **ARQ (Automatic Repeat Request)**: Reliable point-to-point communication with error detection and retransmission
- **Broadcast**: One-to-many transmission without acknowledgments

The modem is built on top of FreeDV and supports multiple modulation schemes.

## Architecture

### System Components

```
┌─────────────┐
│ TCP Clients │
└──────┬──────┘
       │
       ▼
┌─────────────────────────────────┐
│   Data Interfaces (TCP)         │
│   - ARQ Control/Data Ports      │
│   - Broadcast Port              │
└──────┬──────────────────────────┘
       │
       ▼
┌─────────────────────────────────┐
│   Datalink Layer                │
│   - ARQ Protocol               │
│   - Broadcast Protocol          │
└──────┬──────────────────────────┘
       │
       ▼
┌─────────────────────────────────┐
│   Modem Layer (FreeDV)          │
│   - Modulation/Demodulation     │
│   - Framer                      │
└──────┬──────────────────────────┘
       │
       ▼
┌─────────────────────────────────┐
│   Audio I/O                     │
│   - ALSA/Pulse/SHM              │
│   - Radio Interface             │
└─────────────────────────────────┘
```

### Buffer Flow

**ARQ Path:**
- TX: TCP → `data_tx_buffer_arq` → Modem → Radio
- RX: Radio → Modem → `data_rx_buffer_arq` → TCP

**Broadcast Path:**
- TX: TCP → `data_tx_buffer_broadcast` → Broadcast Processor → Modem → Radio
- RX: Radio → Modem → Broadcast Processor → `data_rx_buffer_broadcast` → TCP

### Thread Model

The system uses multiple threads for concurrent processing:

1. **Modem Threads:**
   - `tx_thread`: Reads from datalink buffers and modulates data
   - `rx_thread`: Demodulates received signals and writes to datalink buffers

2. **ARQ Threads:**
   - `dsp_thread_tx`: Processes ARQ protocol for transmission
   - `dsp_thread_rx`: Processes ARQ protocol for reception

3. **Broadcast Threads:**
   - `broadcast_tx_thread`: Processes KISS frames for transmission
   - `broadcast_rx_thread`: Processes received frames for TCP

4. **TCP Server Threads:**
   - Control and data port listeners
   - Client connection handlers

## ARQ Protocol

### Overview

ARQ (Automatic Repeat Request) provides reliable point-to-point communication with automatic error detection and retransmission.

### Frame Format

All ARQ frames use the following structure:

```
┌─────────────────────────────────────┐
│ Header (1 byte)                   │
│ ├─ Packet Type (2 bits, bits 6-7) │
│ └─ CRC6 (6 bits, bits 0-5)          │
├─────────────────────────────────────┤
│ Payload (variable)                  │
└─────────────────────────────────────┘
```

### Packet Types

- `0x00` - ARQ Control: Connection establishment and control
- `0x01` - ARQ Data: User data with sequence numbers
- `0x02` - ARQ ACK: Acknowledgment
- `0x03` - ARQ NACK: Negative acknowledgment

### Connection Establishment

1. **Caller** sends `CONTROL_CALL_REQUEST` with destination and source callsigns
2. **Callee** receives request, validates destination callsign
3. **Callee** sends `CONTROL_CALL_RESPONSE` with own callsign
4. **Caller** receives response, validates callsign
5. Connection established, data transfer can begin

### Data Transfer

1. **Sender** creates data frame with sequence number
2. **Sender** transmits frame and waits for ACK
3. **Receiver** validates CRC and sequence number
4. **Receiver** sends ACK (correct) or NACK (error)
5. **Sender** retransmits on NACK or timeout (max 3 retries)

### Sequence Numbers

- 7-bit sequence numbers (0-127)
- Wraps around after 127
- Receiver expects next sequence number
- Out-of-order frames are rejected

### State Machine

The ARQ protocol uses a finite state machine with the following states:

- `no_connected_client`: No TCP client connected
- `idle`: Ready but not listening
- `listen`: Listening for incoming calls
- `connecting_caller`: Initiating connection
- `connecting_callee`: Responding to connection request
- `link_connected`: Connection established, data transfer active

See [ARQ States](arq_states.md) for detailed state transitions.

## Broadcast Protocol

### Overview

Broadcast mode provides one-to-many transmission without acknowledgments. It uses KISS (Keep It Simple, Stupid) framing for compatibility with existing tools.

### Frame Format

Broadcast frames use the standard HERMES frame header:

```
┌─────────────────────────────────────┐
│ Header (1 byte)                    │
│ ├─ Packet Type: BROADCAST_DATA (2) │
│ └─ CRC6 (6 bits)                    │
├─────────────────────────────────────┤
│ KISS Frame (variable)              │
└─────────────────────────────────────┘
```

### KISS Framing

KISS frames use the following format:

```
FEND | CMD | DATA... | FEND
```

- `FEND` (0xC0): Frame delimiter
- `CMD`: Command byte (0x02 for DATA)
- `DATA`: Payload bytes (escaped if needed)
- Escape sequences: `FESC TFEND` for FEND, `FESC TFESC` for FESC

### Operation

1. **TX Path:**
   - TCP client sends KISS frame
   - Broadcast processor unwraps KISS
   - Adds HERMES header with CRC
   - Sends to modem

2. **RX Path:**
   - Modem receives frame
   - Broadcast processor validates CRC
   - Extracts payload
   - Wraps in KISS frame
   - Sends to TCP client

## Testing

### Testing Without Radio Hardware

The modem supports testing without radio hardware using:

1. **Shared Memory (SHM) Interface:**
   ```bash
   ./mercury -x shm -s 0
   ```
   This mode uses shared memory buffers that can be accessed by test utilities.

2. **Loopback Test:**
   See [Testing Guide](testing.md) for details on using the loopback test utility.

### ARQ Testing

Use the ARQ test client:

```bash
cd utils
make arq_test_client
./arq_test_client [port]
```

Example session:
```
ARQ> MYCALL TEST1
OK
ARQ> LISTEN ON
OK
ARQ> CONNECT TEST1 TEST2
OK
ARQ> SEND Hello World
```

### Broadcast Testing

Use the existing broadcast test utility:

```bash
cd utils
make broadcast_test
./broadcast_test [host] [port]
```

## API Reference

### ARQ API

See [ARQ API](arq_api.md) for detailed function documentation.

### Broadcast API

See [Broadcast API](broadcast_api.md) for detailed function documentation.

### Modem API

See [Modem API](modem_api.md) for detailed function documentation.

## Configuration

### Command Line Options

```
-s [mode]          Modulation mode (0-6)
-i [device]        Input audio device
-o [device]        Output audio device
-x [system]        Audio system (alsa/pulse/shm)
-p [port]          ARQ TCP base port (default: 8300)
-b [port]          Broadcast TCP port (default: 8100)
-v                 Verbose mode
-l                 List modulation modes
-z                 List sound cards
-h                 Help
```

### Audio Systems

- `alsa`: Advanced Linux Sound Architecture (default on Linux)
- `pulse`: PulseAudio
- `shm`: Shared Memory (for testing)
- `dsound`: DirectSound (Windows)
- `wasapi`: Windows Audio Session API

### Modulation Modes

- `0`: DATAC1
- `1`: DATAC3
- `2`: DATAC0
- `3`: DATAC4
- `4`: DATAC13
- `5`: DATAC14
- `6`: FSK_LDPC

Use `-l` to see detailed information about each mode.

## Further Reading

- [ARQ Protocol Details](arq_protocol.md)
- [Broadcast Protocol Details](broadcast_protocol.md)
- [Testing Guide](testing.md)
- [Troubleshooting](troubleshooting.md)

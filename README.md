# HERMES modem

## Introduction

This is the Rhizomatica's HERMES (High-Frequency Emergency and Rural Multimedia Exchange System) modem. Currently based
on David Rowe's FreeDV modem, while support for other modems, as Mercury, will come next.

## What this software does

- **ARQ data link for P2P sessions** with connect/accept handshake, ACK/retry logic, keepalive, and controlled disconnect.
- **Adaptive payload "gear-shifting"** (DATAC4/DATAC3/DATAC1) driven by link quality and backlog, with DATAC13 used for control signaling.
- **Broadcast data mode** in parallel to ARQ, with dedicated broadcast framing and TCP ingress port.
- **VARA-style TCP TNC interface** with separate control and data sockets (base port and base+1), including commands/status like `MYCALL`, `LISTEN`, `CONNECT`, `BUFFER`, `SN`, and `BITRATE`.
- **Audio modem operation over multiple backends** (`alsa`, `pulse`, `dsound`, `wasapi`, `shm`) with split RX/TX modem orchestration.

```
Usage modes: 
./mercury -s [mode_index] -i [device] -o [device] -x [sound_system] -p [arq_tcp_base_port] -b [broadcast_tcp_port]
./mercury [-h -l -z]

Options:
 -c [cpu_nr]                Run on CPU [cpu_nr]. Use -1 to disable CPU selection, which is the default.
 -s [mode_index]            Selects modem mode by index shown in "-l" output. Default is 1 (DATAC3).
 -i [device]                Radio Capture device id (eg: "plughw:0,0").
 -o [device]                Radio Playback device id (eg: "plughw:0,0").
 -x [sound_system]          Sets the sound system or IO API to use: alsa, pulse, dsound, wasapi or shm. Default is alsa on Linux and dsound on Windows.
 -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 8300.
 -b [broadcast_tcp_port]    Sets the broadcast TCP port. Default is 8100.
 -l                         Lists all modulator/coding modes.
 -z                         Lists all available sound cards.
 -v                         Verbose mode. Prints more information during execution.
 -t                         Test TX mode.
 -r                         Test RX mode.
 -h                         Prints this help.
```

## Compilation

Edit config.mk with your C compiler and appropriate flags (defaults should be fine for most) and type:

```
make
```

## Logging and collision tracing

- Default run (`./mercury`): logger runs at **INFO** level with timestamps (`[INF]/[WRN]/[ERR]`).
- Verbose run (`./mercury -v`): logger runs at **DEBUG** level and includes all detailed ARQ/modem traces (`[DBG]`).
- TX state transitions are logged with timestamps at INFO level as:
  - `TX enabled (PTT ON)`
  - `TX disabled (PTT OFF)`

## Authors

- Rafael Diniz (Rhizomatica, ARQ, Broadcast, TCP interface, etc)
- David Rowe (FreeDV - physical layer)

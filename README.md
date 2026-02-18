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
./mercury -m [mode_index] -i [device] -o [device] -x [sound_system] -p [arq_tcp_base_port] -b [broadcast_tcp_port] -f [freedv_verbosity] -k [rx_input_channel]
./mercury [-h -l -z]

Options:
 -c [cpu_nr]                Run on CPU [cpu_nr]. Use -1 to disable CPU selection, which is the default.
 -m [mode_index]            Startup payload mode index shown in "-l" output. Sets broadcast/test mode. Default is 1 (DATAC3).
 -s [mode_index]            Legacy alias for -m.
 -f [freedv_verbosity]      FreeDV modem verbosity level (0..3). Default is 0.
 -k [rx_input_channel]      Capture input channel: left, right, or stereo. Default is left.
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

Mode behavior notes:
- `-m` / `-s` affects **broadcast** and **test** modes only.
- During an active ARQ link, control frames use DATAC13 and ARQ payload starts in DATAC4 (then may adapt to DATAC3/DATAC1).
- `FSK_LDPC` is currently **experimental** (mainly for lab/test usage), may have longer decode/sync latency depending on setup, and is not recommended for production links yet.

## Compilation

Edit config.mk with your C compiler and appropriate flags (defaults should be fine for most) and type:

```
make
```

## API documentation (Doxygen)

Online HTML docs: https://rhizomatica.github.io/hermes-modem/

If you have `doxygen` installed, you can generate HTML documentation for the ARQ subsystem:

```
make doxygen
```

Output will be generated in `docs/html/` (open `docs/html/index.html` in a browser). To remove generated docs:

```
make doxygen-clean
```

## Logging and collision tracing

- Default run (`./mercury`): logger runs at **INFO** level with timestamps (`[INF]/[WRN]/[ERR]`).
- Verbose run (`./mercury -v`): logger runs at **DEBUG** level and includes all detailed ARQ/modem traces (`[DBG]`).
- TX state transitions are logged with timestamps at INFO level as:
  - `TX enabled (PTT ON)`
  - `TX disabled (PTT OFF)`

## Physical Layer

The HERMES modem uses the FreeDV modulator developed by David Rowe.

## Author

- Rafael Diniz (Rhizomatica, ARQ, Broadcast, TCP interface, etc)

## LICENSE

Please check LICENSE and LICENSE-freedv.

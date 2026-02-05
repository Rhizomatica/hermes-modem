# HERMES modem

## Introduction

This is the Rhizomatica's HERMES (High-Frequency Emergency and Rural Multimedia Exchange System) modem. Currently based
on David Rowe's FreeDV modem, while support for other modems, as Mercury, will come next.

```
Usage modes: 
./modem -s [modulation_config] -i [device] -o [device] -x [sound_system] -p [arq_tcp_base_port] -b [broadcast_tcp_port]
./modem [-h -l -z]

Options:
 -c [cpu_nr]                Run on CPU [cpu_br]. Use -1 to disable CPU selection, which is the default.
 -s [modulation_config]     Sets modulation configuration for broadcasting. Modes: 0 to 6. Use "-l" for listing all available modulations. Default is 0 (DATAC1)
 -i [device]                Radio Capture device id (eg: "plughw:0,0").
 -o [device]                Radio Playback device id (eg: "plughw:0,0").
 -x [sound_system]          Sets the sound system or IO API to use: alsa, pulse, dsound, wasapi or shm. Default is alsa on Linux and dsound on Windows.
 -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 7002.
 -b [broadcast_tcp_port]    Sets the broadcast TCP port. Default is 7004.
 -l                         Lists all modulator/coding modes.
 -z                         Lists all available sound cards.
 -v                         Verbose mode. Prints more information during execution.
 -h                         Prints this help.
```

## Compilation

Edit config.mk with your C compiler and appropriate flags (defaults should be fine for most) and type:

```
make
```

## Authors

- Rafael Diniz (Rhizomatica)
- David Rowe

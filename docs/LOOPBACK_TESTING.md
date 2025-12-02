# Loopback Testing Guide

This guide explains how to test HERMES modem without radio hardware using ALSA or PulseAudio loopback.

## Overview

Loopback testing allows you to:
- Test ARQ protocol end-to-end
- Test Broadcast protocol
- Verify modem functionality
- Debug issues without radio hardware

## ALSA Loopback Setup

### 1. Load ALSA Loopback Module

```bash
sudo modprobe snd-aloop
```

To make it permanent, add to `/etc/modules`:
```
snd-aloop
```

### 2. Verify Loopback Device

```bash
aplay -l
```

Look for output like:
```
card 1: Loopback [Loopback], device 0: Loopback PCM [Loopback PCM]
```

### 3. Use Loopback in Mercury

```bash
./mercury -x alsa -i plughw:1,0 -o plughw:1,0 -s 0 -p 8300
```

**Note:** Device number may vary. Check `aplay -l` for correct device.

### 4. Automated Setup

Use the provided script:
```bash
./scripts/setup_alsa_loopback.sh
```

## PulseAudio Loopback Setup

### 1. Create Loopback

Use the helper script so you get consistent PulseAudio settings:

```bash
./scripts/setup_pulse_loopback.sh
```

### 2. Use PulseAudio in Mercury

```bash
./mercury -x pulse -s 0 -p 8300
```

**Note:** PulseAudio doesn't require explicit device names.

### 3. Automated Setup

Already covered above — rerun `./scripts/setup_pulse_loopback.sh` whenever you need to recreate the loopback (for example after a reboot).

### 4. Remove Loopback

```bash
pactl list modules short | grep module-loopback
pactl unload-module <module_id>
```

## Running Tests

### ARQ Loopback Test

```bash
./scripts/test_arq_loopback.sh [port] [modulation] [device]
```

Example:
```bash
./scripts/test_arq_loopback.sh 8300 0 plughw:1,0
```

This will:
1. Setup ALSA loopback (if needed)
2. Start mercury with loopback
3. Run ARQ connection tests
4. Clean up automatically

### Broadcast Loopback Test

```bash
./scripts/test_broadcast_loopback.sh [port] [modulation] [device]
```

Example:
```bash
./scripts/test_broadcast_loopback.sh 8100 0 plughw:1,0
```

### Full Loopback Test

Test both ARQ and Broadcast:
```bash
./scripts/test_full_loopback.sh [modulation] [device]
```

Example:
```bash
./scripts/test_full_loopback.sh 0 plughw:1,0
```

## Manual Testing

### ARQ Manual Test

1. **Terminal 1 - Start Modem:**
   ```bash
   ./mercury -x alsa -i plughw:1,0 -o plughw:1,0 -s 0 -p 8300
   ```

2. **Terminal 2 - Connect Client:**
   ```bash
   ./utils/arq_test_client 8300
   ```

3. **In ARQ Client:**
   ```
   ARQ> MYCALL TEST1
   ARQ> LISTEN ON
   ```

### Broadcast Manual Test

1. **Terminal 1 - Start Modem:**
   ```bash
   ./mercury -x alsa -i plughw:1,0 -o plughw:1,0 -s 0 -b 8100
   ```

2. **Terminal 2 - Send Data:**
   ```bash
   echo "Test message" | ./utils/broadcast_test localhost 8100
   ```

## Troubleshooting

### ALSA Loopback Not Found

**Problem:** `aplay -l` doesn't show loopback device

**Solution:**
```bash
# Load module
sudo modprobe snd-aloop

# Verify
lsmod | grep snd_aloop

# Check devices
aplay -l
```

### Device Busy

**Problem:** Device already in use

**Solution:**
```bash
# Find processes using audio
lsof /dev/snd/*

# Kill mercury if needed
pkill mercury
```

### No Audio Output

**Problem:** No sound/activity

**Solution:**
1. Check device permissions: `ls -l /dev/snd/*`
2. Test with `aplay`: `aplay -D plughw:1,0 /usr/share/sounds/alsa/Front_Left.wav`
3. Check modem logs for errors
4. If ALSA still refuses to pass samples, switch to the PulseAudio loopback (`./scripts/setup_pulse_loopback.sh` + `./mercury -x pulse ...`) which avoids ALSA’s two-subdevice limit.

### PulseAudio Issues

**Problem:** PulseAudio not working

**Solution:**
```bash
# Restart PulseAudio
pulseaudio -k
pulseaudio --start

# Check status
pulseaudio --check -v
```

## Performance Considerations

### Latency

- ALSA loopback: Low latency, direct hardware access
- PulseAudio loopback: Higher latency, but more flexible

### CPU Usage

Loopback testing uses CPU for:
- Audio processing
- Modem encoding/decoding
- Protocol handling

Monitor with: `top` or `htop`

## Advanced Testing

### Two Modem Instances

Test communication between two modems:

1. **Terminal 1 - Modem A:**
   ```bash
   ./mercury -x alsa -i plughw:1,0 -o plughw:1,1 -s 0 -p 8300
   ```

2. **Terminal 2 - Modem B:**
   ```bash
   ./mercury -x alsa -i plughw:1,1 -o plughw:1,0 -s 0 -p 8310
   ```

**Note:** Requires careful device mapping to avoid feedback loops.

### Network Testing

Test TCP interfaces without audio:
```bash
# Start modem with SHM (no audio)
./mercury -x shm -s 0 -p 8300

# Connect clients
./utils/arq_test_client 8300
```

### Capturing SNR on the Bench

1. Run mercury with PulseAudio (or ALSA if your hardware supports it) and capture the log output:
   ```bash
   ./mercury -x pulse -s 0 -p 8400 -b 8500 -U off > /tmp/mercury_loop.log 2>&1
   ```
2. Feed traffic through the loopback, e.g. `printf 'Test loopback message\nexit\n' | ./utils/broadcast_test 127.0.0.1 8500`.
3. Inspect the log for lines like `Received N frames, SNR: X.XX dB, sync: 1`. Those values are the real-time FreeDV estimates; in a software loopback they should cluster near 0 dB.

## Best Practices

1. **Always cleanup:** Scripts handle this automatically
2. **Check logs:** Review `/tmp/mercury_*.log` for issues
3. **Start simple:** Use modulation mode 0 (DATAC1) first
4. **Monitor resources:** Watch CPU/memory usage
5. **Test incrementally:** Test ARQ then Broadcast separately

## See Also

- [Testing Guide](testing.md)
- [Troubleshooting Guide](troubleshooting.md)
- [ARQ Protocol](arq_protocol.md)

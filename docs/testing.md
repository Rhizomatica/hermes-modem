# Testing Guide

## Overview

This guide covers testing the HERMES modem without radio hardware and with various test scenarios.

## Testing Without Radio

### ARQ/KISS Logic Bench (no audio devices)

When you only need to validate the control-plane logic, build the standalone bench:

```bash
make arq_kiss_bench
./tests/arq_kiss_bench
```

This utility instantiates the ARQ finite-state machine and KISS framer entirely in-process.  
It exercises:

- Call setup (`PU2UIT` ↔ `PU2GNU`) without any loopback or audio plumbing
- Data delivery and ACK generation through the ARQ buffers
- KISS encode/decode escapes to guarantee TCP payload integrity

The bench returns a non-zero exit code if any step fails, which makes it suitable for CI pipelines (`make bench-tests` will build and run it).

### Shared Memory (SHM) Interface

The SHM interface allows testing without physical radio hardware by using shared memory buffers.

**Start modem in SHM mode:**
```bash
./mercury -x shm -s 0
```

This creates shared memory buffers that can be accessed by test utilities or other processes.

### Loopback Test

The loopback test utility creates a direct connection between TX and RX buffers, simulating a perfect radio link.

**Build:**
```bash
cd utils
make loopback_test
```

**Run:**
```bash
./loopback_test [modulation_mode]
```

**Note:** The loopback test is a demonstration utility. For full testing, use the SHM interface with separate test processes.

## ARQ Testing

### Test Client

The ARQ test client provides an interactive interface for testing ARQ connections.

**Build:**
```bash
cd utils
make arq_test_client
```

**Run:**
```bash
./arq_test_client [port]
```

Default port is 8300 (control) and 8301 (data).

### Test Scenario 1: Basic Connection

**Terminal 1 (Station A - Listening):**
```bash
./mercury -x shm -s 0 -p 8300
```

**Terminal 2 (Station A - Control):**
```bash
./utils/arq_test_client 8300
ARQ> MYCALL STATIONA
OK
ARQ> LISTEN ON
OK
```

**Terminal 3 (Station B - Control):**
```bash
./utils/arq_test_client 8300
ARQ> MYCALL STATIONB
OK
ARQ> CONNECT STATIONB STATIONA
OK
```

**Terminal 2 should show:**
```
CONNECTED
```

### Test Scenario 2: Data Transfer

After connection is established:

**Terminal 3:**
```bash
ARQ> SEND Hello from Station B
```

**Terminal 2 should receive the message via data port.**

### Test Scenario 3: Connection Failure

**Terminal 3:**
```bash
ARQ> CONNECT STATIONB INVALID
```

Should timeout after 10 seconds with connection failure.

## Broadcast Testing

### Test Client

Use the existing broadcast test utility:

**Build:**
```bash
cd utils
make broadcast_test
```

**Run:**
```bash
./utils/broadcast_test localhost 8100
```

### Test Scenario: Broadcast Transmission

**Terminal 1 (Broadcaster):**
```bash
./mercury -x shm -s 0 -b 8100
```

**Terminal 2 (Receiver):**
```bash
./utils/broadcast_test localhost 8100
```

Send data from Terminal 2, it should be broadcast via the modem.

## Integration Testing

### End-to-End Test

1. Start two modem instances with SHM interface
2. Create loopback between their SHM buffers
3. Connect ARQ clients to both
4. Establish connection
5. Transfer data
6. Verify data integrity

### Performance Testing

**Throughput Test:**
- Measure data transfer rate
- Test with different modulation modes
- Test with different frame sizes

**Reliability Test:**
- Introduce errors (simulate CRC failures)
- Verify retransmission
- Measure error recovery time

**Stress Test:**
- Multiple concurrent connections
- Large data transfers
- Connection/disconnection cycles

## Debugging

### Verbose Mode

Enable verbose output:
```bash
./mercury -v -x shm -s 0
```

### Log Analysis

Key log messages:
- `FSM State: ...` - State machine transitions
- `ARQ initialized: frame_size=...` - Frame size information
- `Received ... bytes` - Data reception
- `Sent ... bytes` - Data transmission
- `CRC error` - CRC validation failures

### Common Issues

**Connection Timeout:**
- Check callsigns match
- Verify both stations are running
- Check network connectivity (for TCP)

**CRC Errors:**
- Check frame size matches modem mode
- Verify buffer alignment
- Check for buffer overflows

**No Data Received:**
- Verify buffers are connected
- Check thread status
- Verify modem is receiving data

## Test Utilities Reference

### arq_test_client

Interactive ARQ test client.

**Commands:**
- `MYCALL <callsign>` - Set callsign
- `LISTEN ON/OFF` - Enable/disable listening
- `CONNECT <src> <dst>` - Connect to remote
- `DISCONNECT` - Disconnect
- `SEND <message>` - Send data
- `QUIT` - Exit

### loopback_test

Creates loopback between TX and RX buffers.

**Usage:**
```bash
./loopback_test [modulation_mode]
```

### broadcast_test

Simple broadcast test client.

**Usage:**
```bash
./broadcast_test [host] [port]
```

## Automated Testing

### Test Scripts

Create test scripts for automated testing:

```bash
#!/bin/bash
# test_arq.sh

# Start modem
./mercury -x shm -s 0 -p 8300 &
MODEM_PID=$!

sleep 2

# Run test client
echo "MYCALL TEST1" | ./utils/arq_test_client 8300

# Cleanup
kill $MODEM_PID
```

### Continuous Integration

For CI/CD, use:
- Unit tests for individual components
- Integration tests for full system
- Performance benchmarks
- Regression tests

## Performance Benchmarks

### Expected Performance

**ARQ Mode:**
- Connection establishment: < 2 seconds
- Data throughput: Depends on modulation mode
- Latency: < 500ms per frame

**Broadcast Mode:**
- No connection overhead
- Higher throughput (no ACK overhead)
- No reliability guarantees

### Measurement Tools

Use system tools to measure:
- `time` - Execution time
- `perf` - Performance profiling
- `valgrind` - Memory analysis
- `strace` - System call tracing

## Troubleshooting

See [Troubleshooting Guide](troubleshooting.md) for common issues and solutions.

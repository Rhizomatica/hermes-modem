# Troubleshooting Guide

## Common Issues and Solutions

### ARQ Connection Issues

#### Connection Timeout

**Symptoms:**
- Connection request sent but no response
- Timeout after 10 seconds

**Possible Causes:**
1. Remote station not listening
2. Callsign mismatch
3. Network connectivity issues (TCP)
4. Modem not receiving/transmitting

**Solutions:**
1. Verify remote station is running with `LISTEN ON`
2. Check callsigns match exactly (case-sensitive)
3. Test TCP connectivity: `telnet localhost 8300`
4. Check modem initialization and audio I/O

#### CRC Errors

**Symptoms:**
- Frequent CRC error messages
- Frames rejected

**Possible Causes:**
1. Frame size mismatch
2. Buffer corruption
3. Modem synchronization issues
4. Audio I/O problems

**Solutions:**
1. Verify frame size matches modem mode
2. Check for buffer overflows
3. Ensure modem is properly synchronized
4. Test audio I/O independently

#### Sequence Number Errors

**Symptoms:**
- NACK messages
- Retransmissions

**Possible Causes:**
1. Frame loss
2. Out-of-order reception
3. Buffer issues

**Solutions:**
1. Check network stability
2. Verify buffer thread safety
3. Increase buffer sizes if needed

### Broadcast Issues

#### No Data Received

**Symptoms:**
- Data sent but not received
- TCP connection established but no data

**Possible Causes:**
1. KISS frame parsing errors
2. Buffer not connected
3. Modem not receiving

**Solutions:**
1. Verify KISS frame format
2. Check buffer connections
3. Test modem reception independently

#### KISS Frame Errors

**Symptoms:**
- Malformed frames
- Incomplete data

**Possible Causes:**
1. Incorrect KISS encoding
2. Buffer truncation
3. Frame size limits

**Solutions:**
1. Verify KISS frame structure
2. Check frame size limits
3. Ensure complete frames in buffers

### Modem Issues

#### No Audio Output

**Symptoms:**
- Modem running but no audio
- PTT not activating

**Possible Causes:**
1. Audio device not configured
2. Permissions issue
3. Audio system not initialized

**Solutions:**
1. Check audio device: `-i` and `-o` options
2. Verify permissions: `ls -l /dev/snd/*`
3. Test audio system: `-z` to list devices

#### Modem Synchronization

**Symptoms:**
- No frames decoded
- Low SNR reported

**Possible Causes:**
1. Frequency offset
2. Timing issues
3. Signal quality

**Solutions:**
1. Adjust frequency
2. Check sample rate matching
3. Improve signal quality

### Buffer Issues

#### Buffer Overflow

**Symptoms:**
- Data loss
- Incomplete frames

**Possible Causes:**
1. Producer faster than consumer
2. Buffer size too small
3. Thread blocking

**Solutions:**
1. Increase buffer sizes
2. Optimize thread priorities
3. Check for thread deadlocks

#### Buffer Underflow

**Symptoms:**
- Empty buffers
- No data available

**Possible Causes:**
1. Producer slower than consumer
2. Thread not running
3. Buffer not initialized

**Solutions:**
1. Check thread status
2. Verify buffer initialization
3. Add flow control

### Thread Issues

#### Deadlock

**Symptoms:**
- System hangs
- No progress

**Possible Causes:**
1. Mutex deadlock
2. Circular dependencies
3. Resource contention

**Solutions:**
1. Review mutex usage
2. Check lock ordering
3. Use timeout locks

#### Thread Not Running

**Symptoms:**
- No data processing
- Missing functionality

**Possible Causes:**
1. Thread creation failed
2. Thread exited early
3. Thread blocked

**Solutions:**
1. Check thread creation return values
2. Review thread exit conditions
3. Check for blocking operations

## Debugging Techniques

### Enable Verbose Mode

```bash
./mercury -v -x shm -s 0
```

### Check Thread Status

```bash
ps -T -p $(pgrep mercury)
```

### Monitor Buffers

Add debug prints to buffer operations:
```c
printf("Buffer size: %zu\n", size_buffer(buffer));
```

### Use GDB

```bash
gdb ./mercury
(gdb) run -x shm -s 0
(gdb) break arq_init
(gdb) continue
```

### Valgrind

Check for memory issues:
```bash
valgrind --leak-check=full ./mercury -x shm -s 0
```

### Strace

Trace system calls:
```bash
strace -e trace=network,file ./mercury -x shm -s 0
```

## Performance Issues

### High CPU Usage

**Possible Causes:**
1. Busy loops
2. Inefficient algorithms
3. Too many threads

**Solutions:**
1. Add sleep in loops
2. Optimize algorithms
3. Reduce thread count

### High Memory Usage

**Possible Causes:**
1. Buffer leaks
2. Large buffers
3. Memory fragmentation

**Solutions:**
1. Check for leaks with valgrind
2. Reduce buffer sizes
3. Use memory pools

### Low Throughput

**Possible Causes:**
1. Small frame sizes
2. High retransmission rate
3. Buffer bottlenecks

**Solutions:**
1. Use larger frames
2. Improve signal quality
3. Increase buffer sizes

## System-Specific Issues

### Linux

**ALSA Issues:**
- Check device permissions
- Verify device exists: `aplay -l`
- Test with: `aplay -D plughw:0,0 test.wav`

**PulseAudio Issues:**
- Check PulseAudio daemon: `pulseaudio --check`
- Restart if needed: `pulseaudio -k && pulseaudio --start`

### Windows

**DirectSound Issues:**
- Check audio drivers
- Verify device availability
- Test with Windows audio tools

**WASAPI Issues:**
- Check exclusive mode settings
- Verify sample rate compatibility
- Test with Windows audio tools

## Getting Help

### Logs

Collect relevant logs:
- Modem output
- System logs
- Error messages

### Information to Provide

When reporting issues, include:
1. Modem version
2. Operating system
3. Audio system used
4. Modulation mode
5. Error messages
6. Steps to reproduce

### Resources

- Check documentation
- Review code comments
- Test with minimal configuration
- Compare with known working setup

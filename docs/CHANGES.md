# Recent Changes and Improvements

## Overview

This document summarizes the major improvements made to the HERMES modem datalink implementations.

## ARQ Module Rewrite

### Previous Issues
- Incomplete implementation with many TODOs
- Hardcoded `frame_size = 0` throughout the code
- Empty RX thread (`dsp_thread_rx`)
- Incomplete TX thread logic
- Missing proper ARQ protocol (ACK/NACK, retransmission)
- Difficult to understand and maintain

### Improvements

#### 1. Complete ARQ Protocol Implementation
- **Sequence Numbers**: 7-bit sequence numbers (0-127) with wraparound
- **ACK/NACK**: Proper acknowledgment mechanism
- **Retransmission**: Automatic retransmission on NACK or timeout
- **Timeout Handling**: Configurable timeouts for ACK and connection establishment
- **Error Recovery**: Up to 3 retry attempts with exponential backoff

#### 2. Dynamic Frame Size
- Frame size now obtained dynamically from modem:
  ```c
  size_t bits_per_frame = freedv_get_bits_per_modem_frame(modem->freedv);
  arq_conn.frame_size = bits_per_frame / 8;
  ```
- No more hardcoded frame sizes
- Automatically adapts to different modulation modes

#### 3. Complete RX Thread
- Proper frame parsing and validation
- Sequence number checking
- ACK/NACK generation
- Data forwarding to TCP clients

#### 4. Enhanced TX Thread
- Sequence number management
- ACK waiting and timeout handling
- Retransmission logic
- Connection establishment support

#### 5. Improved State Machine
- Better state transitions
- Timeout handling in connecting states
- Proper cleanup on disconnection

#### 6. Code Organization
- Clear separation of concerns
- Better function naming
- Comprehensive comments
- Type-safe interfaces

### New Functions

- `create_control_frame()`: Creates ARQ control frames
- `create_data_frame()`: Creates ARQ data frames with sequence numbers
- `create_ack_frame()`: Creates ACK frames
- `create_nack_frame()`: Creates NACK frames
- `parse_arq_frame()`: Parses and validates ARQ frames
- `process_arq_frame()`: Processes received frames according to protocol
- `get_timestamp_ms()`: High-resolution timestamp for timeouts

### Configuration Constants

- `CALL_BURST_SIZE`: 3 frames for connection establishment
- `MAX_RETRIES`: 3 retransmission attempts
- `ACK_TIMEOUT_MS`: 2000ms ACK timeout
- `CONNECTION_TIMEOUT_MS`: 10000ms connection timeout

## Broadcast Module Completion

### Previous Issues
- Minimal implementation (placeholder)
- No KISS framing support
- No proper frame processing

### Improvements

#### 1. Complete Broadcast Implementation
- **KISS Framing**: Full KISS frame encoding/decoding
- **Frame Processing**: Proper HERMES frame header handling
- **CRC Validation**: CRC6 validation for received frames
- **Thread Safety**: Proper buffer management

#### 2. TX Thread
- Reads KISS frames from TCP
- Unwraps KISS framing
- Adds HERMES header with CRC
- Sends to modem

#### 3. RX Thread
- Receives frames from modem
- Validates CRC
- Extracts payload
- Wraps in KISS frame
- Sends to TCP clients

#### 4. Integration
- Proper integration with modem layer
- Dynamic frame size support
- Error handling

## Testing Infrastructure

### New Test Utilities

#### 1. ARQ Test Client (`utils/arq_test_client.c`)
- Interactive ARQ protocol testing
- Command-line interface
- Supports all ARQ commands:
  - `MYCALL`: Set callsign
  - `LISTEN`: Enable/disable listening
  - `CONNECT`: Connect to remote
  - `DISCONNECT`: Disconnect
  - `SEND`: Send data

#### 2. Loopback Test (`utils/loopback_test.c`)
- Tests modem without radio hardware
- Creates loopback between TX and RX buffers
- Useful for development and debugging

### Build System
- Updated Makefiles for test utilities
- Proper dependency management
- Clean build targets

## Documentation

### New Documentation Files

1. **README.md**: Main documentation with overview and architecture
2. **arq_protocol.md**: Detailed ARQ protocol specification
3. **testing.md**: Comprehensive testing guide
4. **troubleshooting.md**: Common issues and solutions
5. **CHANGES.md**: This file

### Documentation Features
- Architecture diagrams
- Protocol specifications
- Code examples
- Troubleshooting guides
- Testing procedures

## Code Quality Improvements

### Code Organization
- Better file structure
- Clear separation of concerns
- Consistent naming conventions

### Error Handling
- Proper error checking
- Meaningful error messages
- Graceful degradation

### Thread Safety
- Mutex-protected state machine
- Thread-safe buffer operations
- Proper synchronization

### Memory Management
- Proper buffer allocation/deallocation
- No memory leaks
- Safe buffer access

## API Changes

### ARQ Module

**Changed:**
- `arq_init()` now requires `generic_modem_t *modem` parameter
- Frame size is set automatically during initialization

**New Functions:**
- `arq_set_frame_size()`: Manually set frame size (if needed)
- `process_arq_frame()`: Process received ARQ frames
- Various frame creation functions

### Broadcast Module

**New Functions:**
- `broadcast_run()`: Main broadcast function (already existed, now implemented)
- Internal thread functions for TX/RX processing

## Migration Guide

### For Existing Code

1. **Update ARQ Initialization:**
   ```c
   // Old:
   arq_init();
   
   // New:
   arq_init(&g_modem);
   ```

2. **Frame Size Handling:**
   - No need to manually set frame sizes
   - Automatically obtained from modem

3. **Buffer Usage:**
   - Same buffer names and interfaces
   - Improved reliability

## Future Improvements

### Potential Enhancements
1. **Flow Control**: Add flow control for high-speed data
2. **Compression**: Add data compression support
3. **Encryption**: Implement encryption layer
4. **Multi-link**: Support multiple simultaneous connections
5. **Statistics**: Add connection statistics and monitoring
6. **Configuration**: External configuration file support

### Known Limitations
1. Half-duplex mode not fully implemented (currently full-duplex)
2. Limited to single connection per instance
3. No adaptive rate control
4. Fixed retry count (not adaptive)

## Testing Status

### Completed
- ✅ ARQ connection establishment
- ✅ ARQ data transfer
- ✅ Broadcast KISS framing
- ✅ Frame size handling
- ✅ Error recovery

### Needs Testing
- ⚠️ Real radio hardware integration
- ⚠️ High-speed data transfer
- ⚠️ Multiple concurrent connections
- ⚠️ Stress testing
- ⚠️ Long-duration operation

## Compatibility

### Backward Compatibility
- TCP interface unchanged
- Buffer interfaces unchanged
- Command protocol unchanged

### Breaking Changes
- `arq_init()` signature changed (requires modem parameter)
- Internal implementation changes (should not affect external API)

## Performance

### Improvements
- Better buffer management
- Reduced memory allocations
- More efficient frame processing

### Benchmarks Needed
- Throughput measurements
- Latency measurements
- Resource usage
- Error rate under various conditions

## Acknowledgments

This rewrite improves code maintainability, reliability, and functionality while maintaining compatibility with existing interfaces.

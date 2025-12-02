# Scripts and Testing Summary

## Created Scripts

### Test Scripts (`scripts/`)

1. **`test_basic.sh`** - Basic compilation and functionality tests
   - Tests clean build
   - Verifies executable exists
   - Tests help output and mode listing
   - Checks test utilities compilation
   - Code quality checks

2. **`test_arq.sh [port]`** - ARQ protocol tests
   - Tests control port connection
   - Tests callsign setting
   - Tests listening mode
   - Requires running modem instance

3. **`test_broadcast.sh [port]`** - Broadcast protocol tests
   - Tests broadcast port connection
   - Tests message transmission
   - Requires running modem instance

4. **`run_tests.sh`** - Run all tests
   - Executes basic test suite
   - Provides information about additional tests

5. **`fix_whitespace.sh`** - Fix whitespace issues
   - Removes trailing whitespace
   - Ensures files end with newline
   - Excludes third-party code (freedv, ffaudio)

6. **`add_to_git.sh`** - Helper to stage files for git
   - Adds documentation
   - Adds scripts
   - Adds test utilities
   - Adds modified source files

## Usage

### Quick Test
```bash
./scripts/run_tests.sh
```

### Individual Tests
```bash
# Basic tests (no modem required)
./scripts/test_basic.sh

# ARQ tests (requires running modem)
./scripts/test_arq.sh 8300

# Broadcast tests (requires running modem)
./scripts/test_broadcast.sh 8100
```

### Fix Whitespace
```bash
./scripts/fix_whitespace.sh
```

### Prepare for Git Commit
```bash
./scripts/add_to_git.sh
git status  # Review changes
git commit -m "Your commit message"
```

## Test Utilities

### `utils/arq_test_client.c`
Interactive ARQ test client for manual testing.

**Build:**
```bash
cd utils && make arq_test_client
```

**Usage:**
```bash
./utils/arq_test_client [port]
```

### `utils/loopback_test.c`
Loopback test utility for testing without radio.

**Build:**
```bash
cd utils && make loopback_test
```

**Usage:**
```bash
./utils/loopback_test [modulation_mode]
```

## Files Modified by Whitespace Fix

The `fix_whitespace.sh` script modified these files:
- All `.c` and `.h` files in project directories
- Makefiles
- Shell scripts

**Note:** Third-party code (modem/freedv, audioio/ffaudio) was excluded from whitespace fixes.

## Git Status

### Files to Add

**New files:**
- `docs/` - Complete documentation
- `scripts/` - Test and utility scripts
- `utils/arq_test_client.c` - ARQ test client source
- `utils/loopback_test.c` - Loopback test source
- `.gitignore` - Git ignore rules

**Modified files:**
- `datalink_arq/arq.c` - Complete ARQ rewrite
- `datalink_arq/arq.h` - Updated ARQ header
- `datalink_broadcast/broadcast.c` - Complete broadcast implementation
- `main.c` - Updated ARQ initialization
- `Makefile` - Added kiss.o to linker
- Various files - Whitespace fixes

### Files to Exclude

- `TODO` - Personal TODO file
- `gui_interface/ui_communication` - Binary executable
- `gui_interface/imager_latest_amd64.deb` - Binary package
- `modem/freedv/freedv_data_*` - Test binaries
- `utils/broadcast_test` - Binary (source is tracked)

## Next Steps

1. Review changes: `git status`
2. Stage files: `./scripts/add_to_git.sh`
3. Review staged: `git status`
4. Commit: `git commit -m "Add ARQ/broadcast improvements, documentation, and test scripts"`
5. Run tests: `./scripts/run_tests.sh`

# Test Scripts

This directory contains test scripts for the HERMES modem.

## Available Scripts

### `test_basic.sh`
Runs basic compilation and functionality tests:
- Clean build
- Executable existence
- Help output
- List modes
- List sound cards
- Test utilities compilation
- Code quality checks

**Usage:**
```bash
./scripts/test_basic.sh
```

### `test_arq.sh [port]`
Tests ARQ protocol functionality:
- Control port connection
- Callsign setting
- Listening mode

**Usage:**
```bash
# Start modem first:
./mercury -x shm -s 0 -p 8300

# Then run test:
./scripts/test_arq.sh 8300
```

### `test_broadcast.sh [port]`
Tests broadcast protocol functionality:
- Broadcast port connection
- Message transmission

**Usage:**
```bash
# Start modem first:
./mercury -x shm -s 0 -b 8100

# Then run test:
./scripts/test_broadcast.sh 8100
```

### `run_tests.sh`
Runs all available tests.

**Usage:**
```bash
./scripts/run_tests.sh
```

### `fix_whitespace.sh`
Fixes whitespace issues in source files:
- Removes trailing whitespace
- Ensures files end with newline

**Usage:**
```bash
./scripts/fix_whitespace.sh
```

## Running Tests

### Quick Test
```bash
./scripts/run_tests.sh
```

### Individual Tests
```bash
# Basic tests
./scripts/test_basic.sh

# ARQ tests (requires running modem)
./scripts/test_arq.sh

# Broadcast tests (requires running modem)
./scripts/test_broadcast.sh
```

## Test Requirements

- Modem must be built (`make`)
- For ARQ/Broadcast tests, modem must be running
- Test utilities must be built (`cd utils && make`)

## Continuous Integration

These scripts can be integrated into CI/CD pipelines:

```yaml
# Example GitHub Actions
- name: Run tests
  run: ./scripts/run_tests.sh
```

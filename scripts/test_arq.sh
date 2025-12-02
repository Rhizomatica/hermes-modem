#!/bin/bash
# ARQ protocol test script
# Tests ARQ connection establishment and data transfer

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ARQ_PORT=${1:-8300}
TEST_DURATION=${2:-5}

echo "=========================================="
echo "ARQ Protocol Test"
echo "=========================================="
echo "Port: $ARQ_PORT"
echo "Duration: ${TEST_DURATION}s"
echo ""

# Check if test client exists
if [ ! -f "$PROJECT_ROOT/utils/arq_test_client" ]; then
    echo -e "${YELLOW}Building test client...${NC}"
    cd "$PROJECT_ROOT/utils"
    make arq_test_client
    cd "$PROJECT_ROOT"
fi

# Check if modem is running
if ! pgrep -f "mercury.*-p.*$ARQ_PORT" > /dev/null; then
    echo -e "${YELLOW}Warning: Modem not running on port $ARQ_PORT${NC}"
    echo "Start modem with: ./mercury -x shm -s 0 -p $ARQ_PORT"
    echo ""
    read -p "Continue anyway? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Test 1: Connect to control port
echo "Test 1: Connect to control port..."
timeout 2 bash -c "echo 'MYCALL TEST1' | $PROJECT_ROOT/utils/arq_test_client $ARQ_PORT" > /tmp/arq_test1.log 2>&1
if grep -q "Connected" /tmp/arq_test1.log; then
    echo -e "${GREEN}✓ PASSED${NC}: Control port connection"
else
    echo -e "${RED}✗ FAILED${NC}: Control port connection"
    cat /tmp/arq_test1.log
    exit 1
fi

# Test 2: Set callsign
echo "Test 2: Set callsign..."
echo -e "MYCALL TEST1\r" | timeout 2 nc localhost $ARQ_PORT > /tmp/arq_test2.log 2>&1 || true
if grep -q "OK" /tmp/arq_test2.log; then
    echo -e "${GREEN}✓ PASSED${NC}: Set callsign"
else
    echo -e "${RED}✗ FAILED${NC}: Set callsign"
    cat /tmp/arq_test2.log
    exit 1
fi

# Test 3: Enable listening
echo "Test 3: Enable listening..."
echo -e "LISTEN ON\r" | timeout 2 nc localhost $ARQ_PORT > /tmp/arq_test3.log 2>&1 || true
if grep -q "OK" /tmp/arq_test3.log; then
    echo -e "${GREEN}✓ PASSED${NC}: Enable listening"
else
    echo -e "${RED}✗ FAILED${NC}: Enable listening"
    cat /tmp/arq_test3.log
    exit 1
fi

echo ""
echo -e "${GREEN}All basic ARQ tests passed!${NC}"
echo ""
echo "For interactive testing, run:"
echo "  $PROJECT_ROOT/utils/arq_test_client $ARQ_PORT"

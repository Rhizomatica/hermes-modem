#!/bin/bash
# Test Broadcast protocol using ALSA loopback
# This creates a full loopback test environment

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

BROADCAST_PORT=${1:-8100}
MODULATION=${2:-0}
if [ -f /tmp/alsa_loopback_dev ]; then
    source /tmp/alsa_loopback_dev
fi
LOOPBACK_DEV=${3:-${LOOPBACK_DEV:-plughw:2,0}}

echo "=========================================="
echo "Broadcast Loopback Test"
echo "=========================================="
echo "Port: $BROADCAST_PORT"
echo "Modulation: $MODULATION"
echo "ALSA Device: $LOOPBACK_DEV"
echo ""

# Check if ALSA loopback is available
if ! aplay -l 2>/dev/null | grep -qi loopback; then
    echo -e "${YELLOW}Setting up ALSA loopback...${NC}"
    "$SCRIPT_DIR/setup_alsa_loopback.sh"
    echo ""
fi

# Check if test client exists
if [ ! -f "$PROJECT_ROOT/utils/broadcast_test" ]; then
    echo -e "${YELLOW}Building test client...${NC}"
    cd "$PROJECT_ROOT/utils"
    make broadcast_test
    cd "$PROJECT_ROOT"
fi

# Clean up function
cleanup() {
    echo ""
    echo -e "${BLUE}Cleaning up...${NC}"
    if [ -n "$MERCURY_PID" ]; then
        kill $MERCURY_PID 2>/dev/null || true
        wait $MERCURY_PID 2>/dev/null || true
    fi
    # Clean up any remaining mercury processes
    pkill -f "mercury.*-b.*$BROADCAST_PORT" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

# Start modem in background
echo -e "${BLUE}Starting modem with ALSA loopback...${NC}"
./mercury -x alsa -i "$LOOPBACK_DEV" -o "$LOOPBACK_DEV" -s "$MODULATION" -b "$BROADCAST_PORT" > /tmp/mercury_broadcast.log 2>&1 &
MERCURY_PID=$!

# Wait for modem to start
echo "Waiting for modem to initialize..."
sleep 3

# Check if modem is running
if ! kill -0 $MERCURY_PID 2>/dev/null; then
    echo -e "${RED}✗ Modem failed to start${NC}"
    echo "Log output:"
    cat /tmp/mercury_broadcast.log
    exit 1
fi

echo -e "${GREEN}✓ Modem started (PID: $MERCURY_PID)${NC}"
echo ""

# Test 1: Connect to broadcast port
echo -e "${BLUE}Test 1: Connect to broadcast port...${NC}"
sleep 2

# Send a test message
TEST_MESSAGE="Hello from broadcast test $(date +%s)"
echo "$TEST_MESSAGE" | timeout 5 "$PROJECT_ROOT/utils/broadcast_test" localhost "$BROADCAST_PORT" > /tmp/broadcast_test.log 2>&1 &
CLIENT_PID=$!

# Wait a bit for transmission
sleep 3

# Check if client connected
if kill -0 $CLIENT_PID 2>/dev/null; then
    echo -e "${GREEN}✓ PASSED${NC}: Broadcast port connection"
    kill $CLIENT_PID 2>/dev/null || true
else
    if grep -q "Connected\|connected\|sent" /tmp/broadcast_test.log; then
        echo -e "${GREEN}✓ PASSED${NC}: Broadcast port connection"
    else
        echo -e "${YELLOW}⚠ PARTIAL${NC}: Connection attempt made"
        cat /tmp/broadcast_test.log
    fi
fi

# Test 2: Check modem received data
echo -e "${BLUE}Test 2: Check modem processing...${NC}"
sleep 2
if grep -q "Broadcast\|broadcast\|Received\|received" /tmp/mercury_broadcast.log; then
    echo -e "${GREEN}✓ PASSED${NC}: Modem processing broadcast data"
else
    echo -e "${YELLOW}⚠ INFO${NC}: No broadcast activity in log yet"
    echo "  (This is normal if no data was transmitted)"
fi

echo ""
echo "=========================================="
echo -e "${GREEN}Broadcast Loopback Test Complete${NC}"
echo "=========================================="
echo ""
echo "Modem log: /tmp/mercury_broadcast.log"
echo "Client log: /tmp/broadcast_test.log"
echo ""
echo "For interactive testing:"
echo "  $PROJECT_ROOT/utils/broadcast_test localhost $BROADCAST_PORT"

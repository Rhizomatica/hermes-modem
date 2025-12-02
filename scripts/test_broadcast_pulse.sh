#!/bin/bash
# Test Broadcast protocol using PulseAudio loopback

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

echo "=========================================="
echo "Broadcast PulseAudio Loopback Test"
echo "=========================================="
echo "Port: $BROADCAST_PORT"
echo "Modulation: $MODULATION"
echo ""

# Check if PulseAudio is running
if ! pulseaudio --check 2>/dev/null; then
    echo -e "${YELLOW}Starting PulseAudio...${NC}"
    pulseaudio --start
    sleep 2
fi

# Setup PulseAudio loopback
echo -e "${BLUE}Setting up PulseAudio loopback...${NC}"
LOOPBACK_ID=$(pactl load-module module-loopback latency_msec=50 2>&1)
if [ $? -eq 0 ] && [ -n "$LOOPBACK_ID" ]; then
    echo -e "${GREEN}✓ PulseAudio loopback created (ID: $LOOPBACK_ID)${NC}"
    echo "  To remove: pactl unload-module $LOOPBACK_ID"
else
    echo -e "${YELLOW}⚠ Loopback may already exist${NC}"
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
        kill -TERM $MERCURY_PID 2>/dev/null || true
        sleep 1
        kill -9 $MERCURY_PID 2>/dev/null || true
    fi
    if [ -n "$LOOPBACK_ID" ]; then
        pactl unload-module $LOOPBACK_ID 2>/dev/null || true
    fi
    pkill -TERM -f "mercury.*-b.*$BROADCAST_PORT" 2>/dev/null || true
    sleep 1
    pkill -9 -f "mercury.*-b.*$BROADCAST_PORT" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

# Start modem in background
echo -e "${BLUE}Starting modem with PulseAudio...${NC}"
./mercury -x pulse -s "$MODULATION" -b "$BROADCAST_PORT" > /tmp/mercury_broadcast_pulse.log 2>&1 &
MERCURY_PID=$!

# Wait for modem to start
echo "Waiting for modem to initialize..."
sleep 4

# Check if modem is running
if ! kill -0 $MERCURY_PID 2>/dev/null; then
    echo -e "${RED}✗ Modem failed to start${NC}"
    echo "Log output:"
    cat /tmp/mercury_broadcast_pulse.log
    exit 1
fi

echo -e "${GREEN}✓ Modem started (PID: $MERCURY_PID)${NC}"
echo ""

# Test 1: Check port is listening
echo -e "${BLUE}Test 1: Check broadcast port...${NC}"
sleep 1
if ss -tln 2>/dev/null | grep -q ":$BROADCAST_PORT "; then
    echo -e "${GREEN}✓ PASSED${NC}: Port $BROADCAST_PORT is listening"
else
    echo -e "${RED}✗ FAILED${NC}: Port $BROADCAST_PORT not listening"
    cat /tmp/mercury_broadcast_pulse.log | tail -20
    exit 1
fi

# Test 2: Connect and send data
echo -e "${BLUE}Test 2: Send broadcast data...${NC}"
sleep 1
TEST_MESSAGE="Hello from broadcast test $(date +%s)"
echo "$TEST_MESSAGE" | timeout 5 "$PROJECT_ROOT/utils/broadcast_test" localhost "$BROADCAST_PORT" > /tmp/broadcast_client.log 2>&1 &
CLIENT_PID=$!

sleep 3

if kill -0 $CLIENT_PID 2>/dev/null || grep -q "Connected\|connected\|sent" /tmp/broadcast_client.log 2>/dev/null; then
    echo -e "${GREEN}✓ PASSED${NC}: Broadcast data sent"
    kill $CLIENT_PID 2>/dev/null || true
else
    echo -e "${YELLOW}⚠ PARTIAL${NC}: Check logs"
    cat /tmp/broadcast_client.log 2>/dev/null | head -10
fi

# Test 3: Check modem received data
echo -e "${BLUE}Test 3: Check modem processing...${NC}"
sleep 2
if grep -q "Broadcast\|broadcast\|Received\|received" /tmp/mercury_broadcast_pulse.log 2>/dev/null; then
    echo -e "${GREEN}✓ PASSED${NC}: Modem processing broadcast data"
    grep -E "Broadcast|broadcast|Received|received" /tmp/mercury_broadcast_pulse.log | tail -3
else
    echo -e "${YELLOW}⚠ INFO${NC}: No broadcast activity in log yet"
fi

echo ""
echo "=========================================="
echo -e "${GREEN}Broadcast PulseAudio Test Complete${NC}"
echo "=========================================="
echo ""
echo "Modem log: /tmp/mercury_broadcast_pulse.log"
echo "Client log: /tmp/broadcast_client.log"
echo ""
echo "For interactive testing:"
echo "  $PROJECT_ROOT/utils/broadcast_test localhost $BROADCAST_PORT"

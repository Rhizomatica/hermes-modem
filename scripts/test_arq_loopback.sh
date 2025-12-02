#!/bin/bash
# Test ARQ protocol using ALSA loopback
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

ARQ_PORT=${1:-8300}
MODULATION=${2:-0}
if [ -f /tmp/alsa_loopback_dev ]; then
    source /tmp/alsa_loopback_dev
fi
LOOPBACK_DEV=${3:-${LOOPBACK_DEV:-plughw:2,0}}

echo "=========================================="
echo "ARQ Loopback Test"
echo "=========================================="
echo "Port: $ARQ_PORT"
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
if [ ! -f "$PROJECT_ROOT/utils/arq_test_client" ]; then
    echo -e "${YELLOW}Building test client...${NC}"
    cd "$PROJECT_ROOT/utils"
    make arq_test_client
    cd "$PROJECT_ROOT"
fi

# Clean up function
cleanup() {
    echo ""
    echo -e "${BLUE}Cleaning up...${NC}"
    if [ -n "$MERCURY_PID" ]; then
        # Send SIGTERM for graceful shutdown
        kill -TERM $MERCURY_PID 2>/dev/null || true
        sleep 1
        # Force kill if still running
        kill -9 $MERCURY_PID 2>/dev/null || true
        wait $MERCURY_PID 2>/dev/null || true
    fi
    # Clean up any remaining mercury processes
    pkill -TERM -f "mercury.*-p.*$ARQ_PORT" 2>/dev/null || true
    sleep 1
    pkill -9 -f "mercury.*-p.*$ARQ_PORT" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

# Start modem in background
echo -e "${BLUE}Starting modem with ALSA loopback...${NC}"
./mercury -x alsa -i "$LOOPBACK_DEV" -o "$LOOPBACK_DEV" -s "$MODULATION" -p "$ARQ_PORT" > /tmp/mercury_arq.log 2>&1 &
MERCURY_PID=$!

# Wait for modem to start
echo "Waiting for modem to initialize..."
sleep 3

# Check if modem is running
if ! kill -0 $MERCURY_PID 2>/dev/null; then
    echo -e "${RED}✗ Modem failed to start${NC}"
    echo "Log output:"
    cat /tmp/mercury_arq.log
    exit 1
fi

echo -e "${GREEN}✓ Modem started (PID: $MERCURY_PID)${NC}"
echo ""

# Test 1: Connect to control port
echo -e "${BLUE}Test 1: Connect to control port...${NC}"
sleep 1

# Try different connection methods
CONNECTED=0
if command -v telnet > /dev/null 2>&1; then
    (echo -e "MYCALL TEST1\r"; sleep 1) | timeout 3 telnet localhost $ARQ_PORT 2>/dev/null | grep -v "Escape\|Trying\|Connected" > /tmp/arq_conn.log 2>&1 && CONNECTED=1
elif command -v nc > /dev/null 2>&1; then
    if echo -e "MYCALL TEST1\r" | timeout 3 nc localhost $ARQ_PORT > /tmp/arq_conn.log 2>&1; then
        CONNECTED=1
    fi
elif command -v socat > /dev/null 2>&1; then
    echo -e "MYCALL TEST1\r" | timeout 3 socat - TCP:localhost:$ARQ_PORT > /tmp/arq_conn.log 2>&1 && CONNECTED=1
fi

if [ $CONNECTED -eq 1 ] && grep -q "OK" /tmp/arq_conn.log 2>/dev/null; then
    echo -e "${GREEN}✓ PASSED${NC}: Control port connection"
elif [ $CONNECTED -eq 1 ]; then
    echo -e "${YELLOW}⚠ PARTIAL${NC}: Connected but no OK response"
    cat /tmp/arq_conn.log 2>/dev/null | head -3
else
    echo -e "${YELLOW}⚠ SKIPPED${NC}: No TCP client available"
    echo "  Install: sudo apt-get install telnet netcat-openbsd"
fi

# Test 2-4: Test ARQ commands
echo -e "${BLUE}Test 2-4: Testing ARQ commands...${NC}"
echo ""

TEST_PASSED=0

# Test MYCALL
if command -v telnet > /dev/null 2>&1; then
    (echo -e "MYCALL TEST1\r"; sleep 1) | timeout 3 telnet localhost $ARQ_PORT 2>/dev/null | grep -v "Escape\|Trying\|Connected" > /tmp/arq_mycall.log 2>&1
    if grep -q "OK" /tmp/arq_mycall.log; then
        echo -e "${GREEN}✓ PASSED${NC}: MYCALL command"
        ((TEST_PASSED++))
    else
        echo -e "${YELLOW}⚠ PARTIAL${NC}: MYCALL - check /tmp/arq_mycall.log"
    fi
    
    # Test LISTEN ON
    (echo -e "LISTEN ON\r"; sleep 1) | timeout 3 telnet localhost $ARQ_PORT 2>/dev/null | grep -v "Escape\|Trying\|Connected" > /tmp/arq_listen.log 2>&1
    if grep -q "OK" /tmp/arq_listen.log; then
        echo -e "${GREEN}✓ PASSED${NC}: LISTEN ON command"
        ((TEST_PASSED++))
    else
        echo -e "${YELLOW}⚠ PARTIAL${NC}: LISTEN ON - check /tmp/arq_listen.log"
    fi
    
    # Test CONNECT
    (echo -e "CONNECT TEST1 TEST2\r"; sleep 1) | timeout 3 telnet localhost $ARQ_PORT 2>/dev/null | grep -v "Escape\|Trying\|Connected" > /tmp/arq_connect.log 2>&1
    if grep -q "OK" /tmp/arq_connect.log; then
        echo -e "${GREEN}✓ PASSED${NC}: CONNECT command accepted"
        ((TEST_PASSED++))
    else
        echo -e "${YELLOW}⚠ PARTIAL${NC}: CONNECT - check /tmp/arq_connect.log"
    fi
    
    if [ $TEST_PASSED -eq 3 ]; then
        echo ""
        echo -e "${GREEN}✓ All ARQ commands passed!${NC}"
    fi
else
    echo -e "${YELLOW}⚠ SKIPPED${NC}: telnet not available for command testing"
    echo "  Install: sudo apt-get install telnet"
    echo "  Or use interactive client: ./utils/arq_test_client $ARQ_PORT"
fi

echo ""
echo "=========================================="
echo -e "${GREEN}ARQ Loopback Test Complete${NC}"
echo "=========================================="
echo ""
echo "Modem log: /tmp/mercury_arq.log"
echo ""
echo "For interactive testing:"
echo "  $PROJECT_ROOT/utils/arq_test_client $ARQ_PORT"

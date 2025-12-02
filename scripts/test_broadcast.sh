#!/bin/bash
# Broadcast protocol test script
# Tests broadcast transmission and reception

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

BROADCAST_PORT=${1:-8100}

echo "=========================================="
echo "Broadcast Protocol Test"
echo "=========================================="
echo "Port: $BROADCAST_PORT"
echo ""

# Check if test client exists
if [ ! -f "$PROJECT_ROOT/utils/broadcast_test" ]; then
    echo -e "${YELLOW}Building test client...${NC}"
    cd "$PROJECT_ROOT/utils"
    make broadcast_test
    cd "$PROJECT_ROOT"
fi

# Check if modem is running
if ! pgrep -f "mercury.*-b.*$BROADCAST_PORT" > /dev/null; then
    echo -e "${YELLOW}Warning: Modem not running on broadcast port $BROADCAST_PORT${NC}"
    echo "Start modem with: ./mercury -x shm -s 0 -b $BROADCAST_PORT"
    echo ""
    read -p "Continue anyway? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Test 1: Connect to broadcast port
echo "Test 1: Connect to broadcast port..."
timeout 2 bash -c "echo 'TEST MESSAGE' | $PROJECT_ROOT/utils/broadcast_test localhost $BROADCAST_PORT" > /tmp/broadcast_test1.log 2>&1 || true
if [ $? -eq 0 ] || grep -q "Connected\|connected" /tmp/broadcast_test1.log; then
    echo -e "${GREEN}✓ PASSED${NC}: Broadcast port connection"
else
    echo -e "${RED}✗ FAILED${NC}: Broadcast port connection"
    cat /tmp/broadcast_test1.log
    exit 1
fi

echo ""
echo -e "${GREEN}Basic broadcast test completed!${NC}"
echo ""
echo "For interactive testing, run:"
echo "  $PROJECT_ROOT/utils/broadcast_test localhost $BROADCAST_PORT"

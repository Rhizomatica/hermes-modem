#!/bin/bash
# Full loopback test - tests both ARQ and Broadcast
# Uses ALSA loopback for complete end-to-end testing

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

MODULATION=${1:-0}
LOOPBACK_DEV=${2:-plughw:1,0}

echo "=========================================="
echo "Full Loopback Test Suite"
echo "=========================================="
echo "Modulation: $MODULATION"
echo "ALSA Device: $LOOPBACK_DEV"
echo ""

# Setup ALSA loopback
echo -e "${BLUE}Setting up ALSA loopback...${NC}"
"$SCRIPT_DIR/setup_alsa_loopback.sh"
echo ""

# Run ARQ test
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Running ARQ Loopback Test${NC}"
echo -e "${BLUE}========================================${NC}"
"$SCRIPT_DIR/test_arq_loopback.sh" 8300 "$MODULATION" "$LOOPBACK_DEV"
ARQ_RESULT=$?

echo ""
echo ""

# Run Broadcast test
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Running Broadcast Loopback Test${NC}"
echo -e "${BLUE}========================================${NC}"
"$SCRIPT_DIR/test_broadcast_loopback.sh" 8100 "$MODULATION" "$LOOPBACK_DEV"
BROADCAST_RESULT=$?

echo ""
echo "=========================================="
echo "Test Summary"
echo "=========================================="

if [ $ARQ_RESULT -eq 0 ]; then
    echo -e "${GREEN}✓ ARQ Test: PASSED${NC}"
else
    echo -e "${RED}✗ ARQ Test: FAILED${NC}"
fi

if [ $BROADCAST_RESULT -eq 0 ]; then
    echo -e "${GREEN}✓ Broadcast Test: PASSED${NC}"
else
    echo -e "${RED}✗ Broadcast Test: FAILED${NC}"
fi

if [ $ARQ_RESULT -eq 0 ] && [ $BROADCAST_RESULT -eq 0 ]; then
    echo ""
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo ""
    echo -e "${RED}Some tests failed${NC}"
    exit 1
fi

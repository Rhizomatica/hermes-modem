#!/bin/bash
# Run all tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

echo "=========================================="
echo "Running HERMES Modem Test Suite"
echo "=========================================="
echo ""

# Run basic tests
echo "Running basic tests..."
"$SCRIPT_DIR/test_basic.sh"
BASIC_RESULT=$?

echo ""
echo "=========================================="
echo ""

# Note: ARQ and Broadcast tests require running modem
# They are provided for manual testing

echo "Additional test scripts available:"
echo "  scripts/test_arq.sh [port]      - Test ARQ protocol"
echo "  scripts/test_broadcast.sh [port] - Test broadcast protocol"
echo ""

if [ $BASIC_RESULT -eq 0 ]; then
    echo "✓ All basic tests passed!"
    exit 0
else
    echo "✗ Some tests failed"
    exit 1
fi

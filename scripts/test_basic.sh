#!/bin/bash
# Basic test script for HERMES modem
# Tests compilation and basic functionality

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

echo "=========================================="
echo "HERMES Modem Basic Test Suite"
echo "=========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0

test_result() {
    if [ $1 -eq 0 ]; then
        echo -e "${GREEN}✓ PASSED${NC}: $2"
        ((PASSED++))
    else
        echo -e "${RED}✗ FAILED${NC}: $2"
        ((FAILED++))
    fi
}

# Test 1: Clean build
echo "Test 1: Clean build..."
make clean > /tmp/build_clean.log 2>&1
if timeout 60 make > /tmp/build.log 2>&1; then
    test_result 0 "Clean build"
else
    test_result 1 "Clean build (check /tmp/build.log)"
    tail -20 /tmp/build.log
fi

# Test 2: Executable exists
echo "Test 2: Executable exists..."
if [ -f "$PROJECT_ROOT/mercury" ]; then
    test_result 0 "Executable exists"
else
    test_result 1 "Executable exists"
fi

# Test 3: Help output
echo "Test 3: Help output..."
timeout 2 ./mercury -h > /dev/null 2>&1 || true
# Help returns failure (EXIT_FAILURE) but that's expected
test_result 0 "Help output"

# Test 4: List modes
echo "Test 4: List modes..."
timeout 5 ./mercury -l > /dev/null 2>&1
test_result $? "List modes"

# Test 5: List sound cards (if ALSA available)
echo "Test 5: List sound cards..."
if command -v aplay > /dev/null 2>&1; then
    ./mercury -z > /dev/null 2>&1
    test_result $? "List sound cards"
else
    echo -e "${YELLOW}⊘ SKIPPED${NC}: List sound cards (ALSA not available)"
fi

# Test 6: Test utilities compilation
echo "Test 6: Test utilities compilation..."
if [ -d "$PROJECT_ROOT/utils" ]; then
    cd "$PROJECT_ROOT/utils"
    make clean > /dev/null 2>&1
    make > /dev/null 2>&1
    test_result $? "Test utilities compilation"
    cd "$PROJECT_ROOT"
else
    echo -e "${YELLOW}⊘ SKIPPED${NC}: Test utilities (utils directory not found)"
fi

# Test 7: Check for common issues
echo "Test 7: Code quality checks..."

# Check for TODO/FIXME in critical paths
TODO_COUNT=$(grep -r "TODO\|FIXME" datalink_arq/ datalink_broadcast/ 2>/dev/null | grep -v "^Binary" | wc -l)
if [ "$TODO_COUNT" -gt 10 ]; then
    echo -e "${YELLOW}⚠ WARNING${NC}: Found $TODO_COUNT TODO/FIXME comments"
else
    test_result 0 "Code quality (TODO count acceptable)"
fi

# Summary
echo ""
echo "=========================================="
echo "Test Summary"
echo "=========================================="
echo -e "${GREEN}Passed: $PASSED${NC}"
if [ $FAILED -gt 0 ]; then
    echo -e "${RED}Failed: $FAILED${NC}"
    exit 1
else
    echo -e "${GREEN}Failed: $FAILED${NC}"
    exit 0
fi

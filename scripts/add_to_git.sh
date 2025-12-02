#!/bin/bash
# Helper script to add new files to git
# This adds documentation, scripts, and test utilities

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

echo "Adding files to git..."

# Add documentation
git add docs/
git add .gitignore

# Add scripts
git add scripts/

# Add test utilities source
git add utils/arq_test_client.c
git add utils/loopback_test.c
git add utils/Makefile

# Add modified source files
git add datalink_arq/arq.c
git add datalink_arq/arq.h
git add datalink_broadcast/broadcast.c
git add main.c
git add Makefile

echo ""
echo "Files staged. Review with: git status"
echo ""
echo "To commit:"
echo "  git commit -m 'Add ARQ/broadcast improvements, documentation, and test scripts'"

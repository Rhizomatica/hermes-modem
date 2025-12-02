#!/bin/bash
# Fix whitespace issues in source files
# Removes trailing whitespace and ensures files end with newline

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

echo "Fixing whitespace issues..."

# Find all C and H files
find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.sh" -o -name "Makefile" \) \
    ! -path "./modem/freedv/*" \
    ! -path "./audioio/ffaudio/*" \
    ! -path "./audioio/ffbase/*" \
    -exec sed -i 's/[[:space:]]*$//' {} \;

# Ensure files end with newline
find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.sh" -o -name "Makefile" \) \
    ! -path "./modem/freedv/*" \
    ! -path "./audioio/ffaudio/*" \
    ! -path "./audioio/ffbase/*" \
    -exec sh -c 'if [ -s "$1" ] && [ "$(tail -c 1 "$1")" != "" ]; then echo >> "$1"; fi' _ {} \;

echo "Done!"

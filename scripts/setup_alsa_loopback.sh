#!/bin/bash
# Setup ALSA loopback device for testing
# This creates a virtual sound card that loops back output to input

set -e

echo "Setting up ALSA loopback..."

# Check if module is already loaded
if lsmod | grep -q snd_aloop; then
    echo "ALSA loopback module already loaded"
else
    # Try to load the module
    if sudo modprobe snd-aloop; then
        echo "✓ ALSA loopback module loaded"
    else
        echo "✗ Failed to load ALSA loopback module"
        echo "You may need to run: sudo modprobe snd-aloop"
        exit 1
    fi
fi

# Wait a moment for device to appear
sleep 1

# Check if loopback device exists
if [ -c /dev/snd/pcmC0D0p ] || [ -c /dev/snd/pcmC1D0p ]; then
    echo "✓ ALSA loopback device found"
    
    # List available loopback devices
    echo ""
    echo "Available loopback devices:"
    LOOPBACK_CARD=$(aplay -l 2>/dev/null | grep -i loopback | head -1 | sed 's/.*card \([0-9]*\):.*/\1/')
    if [ -n "$LOOPBACK_CARD" ]; then
        echo "  Found loopback on card $LOOPBACK_CARD"
        echo "  Use device: plughw:$LOOPBACK_CARD,0"
        echo "LOOPBACK_DEV=plughw:$LOOPBACK_CARD,0" > /tmp/alsa_loopback_dev
    else
        aplay -l 2>/dev/null | grep -i loopback || echo "  (check with: aplay -l)"
        echo ""
        echo "Use device: plughw:1,0 (or check aplay -l for correct device)"
    fi
else
    echo "⚠ Warning: Loopback device not found in /dev/snd/"
    echo "Try: aplay -l to see available devices"
fi

echo ""
echo "To use loopback in mercury:"
echo "  ./mercury -x alsa -i plughw:1,0 -o plughw:1,0 -s 0"

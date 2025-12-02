#!/bin/bash
# Setup PulseAudio loopback for testing
# Alternative to ALSA loopback

set -e

echo "Setting up PulseAudio loopback..."

# Check if PulseAudio is running
if ! pulseaudio --check 2>/dev/null; then
    echo "Starting PulseAudio..."
    pulseaudio --start
    sleep 2
fi

# Check if loopback module is loaded
if pactl list modules short | grep -q module-loopback; then
    echo "PulseAudio loopback module already loaded"
    echo "To remove existing loopbacks:"
    echo "  pactl unload-module module-loopback"
else
    # Create loopback
    echo "Creating PulseAudio loopback..."
    LOOPBACK_ID=$(pactl load-module module-loopback latency_msec=50 2>&1)
    
    if [ $? -eq 0 ]; then
        echo "✓ PulseAudio loopback created (ID: $LOOPBACK_ID)"
        echo ""
        echo "To remove loopback:"
        echo "  pactl unload-module $LOOPBACK_ID"
    else
        echo "✗ Failed to create PulseAudio loopback"
        exit 1
    fi
fi

echo ""
echo "To use PulseAudio in mercury:"
echo "  ./mercury -x pulse -s 0"
echo ""
echo "Note: PulseAudio doesn't require explicit device names"
echo "      It will use the default sink/source with loopback"

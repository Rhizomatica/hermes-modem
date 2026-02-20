#!/usr/bin/env bash
set -euo pipefail

# Wrapper intended for systemd usage (see modem.service in hermes-net / hermes-installer).
# Defaults match the HERMES reference integration:
# - VARA-style TCP TNC on 8300/8301 for uucpd
# - Broadcast TCP port on 8100
# - ALSA backend using "dsp" PCM (provided by /etc/asound.conf in typical images)

MERCURY_BIN="${MERCURY_BIN:-./mercury}"

AUDIO_BACKEND="${AUDIO_BACKEND:-alsa}"
CAPTURE_DEV="${CAPTURE_DEV:-dsp}"
PLAYBACK_DEV="${PLAYBACK_DEV:-dsp}"

ARQ_TCP_BASE_PORT="${ARQ_TCP_BASE_PORT:-8300}"
BROADCAST_TCP_PORT="${BROADCAST_TCP_PORT:-8100}"

FREEDV_VERBOSITY="${FREEDV_VERBOSITY:-0}"
RX_INPUT_CHANNEL="${RX_INPUT_CHANNEL:-left}"

exec "${MERCURY_BIN}" \
  -x "${AUDIO_BACKEND}" \
  -i "${CAPTURE_DEV}" \
  -o "${PLAYBACK_DEV}" \
  -p "${ARQ_TCP_BASE_PORT}" \
  -b "${BROADCAST_TCP_PORT}" \
  -f "${FREEDV_VERBOSITY}" \
  -k "${RX_INPUT_CHANNEL}" \
  "$@"


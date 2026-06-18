#!/usr/bin/env bash
# tests/bench/run_audio.sh — QDX USB Audio path test.
#
# Part of the local HITL bench plan (docs/local-hwil-plan.md). This test proves
# the bench host can play audio into and capture audio from the QDX over its
# USB Audio Class device.  No RX888, no RF path — just the QDX connected via USB.
#
# Prerequisites:
#   - python3 with pyserial installed (pip install pyserial)
#   - aplay, arecord (alsa-utils), sox
#   - QDX connected and enumerated as both serial and audio device
#
# Env vars:
#   QDX_PORT       serial port path (REQUIRED, e.g. /dev/ttyACM0)
#   QDX_BAUD       baud rate (default 9600; irrelevant for USB CDC)
#   QDX_CARD       override ALSA card number (default: auto-discover)
#   QDX_TONE_FREQ  tone frequency in Hz (default 1500)
#   QDX_TONE_DUR   tone duration in seconds (default 2)
#
# Verdict line: "AUDIO OK ..." / "AUDIO FAIL — ...". Exit 0/1.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

fail() { echo "AUDIO FAIL — $*"; exit 1; }

# --- Preflight checks --------------------------------------------------------
command -v python3 >/dev/null || fail "python3 not found"
python3 -c "import serial" 2>/dev/null \
    || fail "pyserial not installed (pip install pyserial)"
command -v aplay   >/dev/null || fail "aplay not found (install alsa-utils)"
command -v arecord >/dev/null || fail "arecord not found (install alsa-utils)"
command -v sox     >/dev/null || fail "sox not found (install sox)"

[ -n "${QDX_PORT:-}" ] \
    || fail "QDX_PORT not set (e.g. export QDX_PORT=/dev/ttyACM0)"
[ -e "$QDX_PORT" ] \
    || fail "QDX_PORT=$QDX_PORT does not exist — is the QDX connected?"

# --- Run test -----------------------------------------------------------------
exec python3 "$HERE/audio_test.py"

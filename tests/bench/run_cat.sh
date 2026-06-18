#!/usr/bin/env bash
# tests/bench/run_cat.sh — QDX CAT serial control test.
#
# Part of the local HITL bench plan (docs/local-hwil-plan.md). This is the
# first QDX-touching test: it proves the bench host can set frequency, key/
# unkey PTT, and read responses from the QDX over its USB Virtual COM Port.
# No RX888, no RF path — just the QDX connected via USB.
#
# Prerequisites:
#   - python3 with pyserial installed (pip install pyserial)
#   - QDX connected and enumerated as a serial device
#
# Env vars:
#   QDX_PORT         serial port path (REQUIRED, e.g. /dev/ttyACM0)
#   QDX_BAUD         baud rate (default 9600; irrelevant for USB CDC)
#   QDX_FREQ_OFFSET  Hz offset for set-frequency test (default 1000)
#
# Verdict line: "CAT OK ..." / "CAT FAIL — ...". Exit 0/1.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

fail() { echo "CAT FAIL — $*"; exit 1; }

# --- Preflight checks -----------------------------------------------------
command -v python3 >/dev/null || fail "python3 not found"
python3 -c "import serial" 2>/dev/null \
    || fail "pyserial not installed (pip install pyserial)"

[ -n "${QDX_PORT:-}" ] \
    || fail "QDX_PORT not set (e.g. export QDX_PORT=/dev/ttyACM0)"
[ -e "$QDX_PORT" ] \
    || fail "QDX_PORT=$QDX_PORT does not exist — is the QDX connected?"

# --- Run test --------------------------------------------------------------
exec python3 "$HERE/cat_test.py"

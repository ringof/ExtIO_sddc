#!/usr/bin/env bash
#
# soak_test.sh — Multi-hour soak test wrapper for SDDC_FX3 firmware.
#
# Uploads firmware via rx888_stream, verifies the device is alive,
# then invokes fx3_cmd's soak subcommand for randomized stress testing.
#
# Usage:
#   ./soak_test.sh --firmware path/to/SDDC_FX3.img [--hours N] [--seed S]
#
# Options:
#   --firmware PATH        Firmware .img to upload (required)
#   --hours N              Soak duration in hours (default: 1, fractional OK)
#   --seed S               PRNG seed for reproducibility (default: time-based)
#   --rx888-stream PATH    Path to rx888_stream binary (default: search PATH)
#

set -euo pipefail

FIRMWARE=""
HOURS="1"
SEED=""
RX888_STREAM=""
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FX3_CMD="$SCRIPT_DIR/fx3_cmd"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --firmware)        FIRMWARE="$2"; shift 2 ;;
        --hours)           HOURS="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --rx888-stream)    RX888_STREAM="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^$/s/^# \?//p' "$0"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---- Locate rx888_stream ----

find_rx888_stream() {
    if [[ -n "$RX888_STREAM" ]]; then
        [[ -x "$RX888_STREAM" ]] && return 0
        echo "Bail out! --rx888-stream '$RX888_STREAM' not found or not executable"
        exit 1
    fi
    local submod="$SCRIPT_DIR/rx888_stream"
    if [[ -x "$submod" ]]; then RX888_STREAM="$submod"; return 0; fi
    local submod_bin="$SCRIPT_DIR/rx888_tools/rx888_stream"
    if [[ -x "$submod_bin" ]]; then RX888_STREAM="$submod_bin"; return 0; fi
    if command -v rx888_stream &>/dev/null; then
        RX888_STREAM="$(command -v rx888_stream)"; return 0
    fi
    return 1
}

# ---- Preflight ----

if [[ ! -x "$FX3_CMD" ]]; then
    echo "Error: fx3_cmd not found at $FX3_CMD — run 'make' in tests/ first" >&2
    exit 1
fi

if ! find_rx888_stream; then
    echo "Error: rx888_stream not found. Build it or pass --rx888-stream PATH" >&2
    exit 1
fi

if [[ -z "$FIRMWARE" ]]; then
    DEFAULT_FW="$SCRIPT_DIR/../SDDC_FX3/SDDC_FX3.img"
    if [[ -f "$DEFAULT_FW" ]]; then
        FIRMWARE="$(cd "$(dirname "$DEFAULT_FW")" && pwd)/$(basename "$DEFAULT_FW")"
    else
        echo "Error: No firmware specified. Use --firmware path/to/SDDC_FX3.img" >&2
        exit 1
    fi
fi

if [[ ! -f "$FIRMWARE" ]]; then
    echo "Error: Firmware file not found: $FIRMWARE" >&2
    exit 1
fi

# ---- Upload firmware ----

echo "# Uploading firmware: $FIRMWARE"
timeout 15 "$RX888_STREAM" -f "$FIRMWARE" -s 32000000 \
    > /dev/null 2>/dev/null &
UPLOAD_PID=$!
sleep 4
kill "$UPLOAD_PID" 2>/dev/null || true
wait "$UPLOAD_PID" 2>/dev/null || true
sleep 2

# ---- Sanity check ----

if ! lsusb -d 04b4:00f1 &>/dev/null; then
    echo "Error: device not found at PID 0x00F1 after firmware upload" >&2
    exit 1
fi

echo "# Device ready. Starting soak test..."
echo "# Hours: $HOURS  Seed: ${SEED:-auto}"

# ---- Run soak ----

exec "$FX3_CMD" soak "$HOURS" ${SEED:+"$SEED"}

#!/usr/bin/env bash
#
# soak_test.sh — Multi-hour soak test wrapper for SDDC_FX3 firmware.
#
# Uploads firmware via rx888_stream, verifies the device is alive,
# then invokes fx3_cmd's soak subcommand for randomized stress testing.
#
# On exit (pass or fail), resets the RX888mk2 back to bootloader state
# via usbreset so the device is ready for the next run without a manual
# power cycle.
#
# Usage:
#   ./soak_test.sh --firmware path/to/SDDC_FX3.img [--hours N] [--seed S]
#
# Options:
#   --firmware PATH        Firmware .img to upload (required)
#   --hours N              Soak duration in hours (default: 1, fractional OK)
#   --seed S               PRNG seed for reproducibility (default: time-based)
#   --rx888-stream PATH    Path to rx888_stream binary (default: search PATH)
#   --no-reset             Skip USB reset on exit (leave device as-is)
#

set -euo pipefail

FIRMWARE=""
HOURS="1"
SEED=""
RX888_STREAM=""
USB_RESET=1
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FX3_CMD="$SCRIPT_DIR/fx3_cmd"

# VID:PID for the RX888mk2 in application mode (firmware loaded)
APP_VID_PID="04b4:00f1"
# VID:PID for the FX3 bootloader (DFU / pre-firmware state)
BOOT_VID_PID="04b4:00f3"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --firmware)        FIRMWARE="$2"; shift 2 ;;
        --hours)           HOURS="$2"; shift 2 ;;
        --seed)            SEED="$2"; shift 2 ;;
        --rx888-stream)    RX888_STREAM="$2"; shift 2 ;;
        --no-reset)        USB_RESET=0; shift ;;
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

# ---- USB reset helper ----
#
# Rationale: `usbreset <VID:PID>` on the RX888mk2 kicks the Cypress FX3
# back to its bootloader state (PID 00F3), equivalent to a power cycle.
# This was verified empirically: the FX3 drops its RAM-loaded firmware,
# the blue LED returns to its initial state, and lsusb shows 04b4:00f3.
#
# This is the most reliable software-only recovery for a wedged device,
# stronger than a libusb reset_device() call.  uhubctl (VBUS power
# cycling) is preferable in theory but requires per-port power switching
# support in the USB hub hardware, which most built-in controllers lack.
#
# usbreset is part of the usbutils package (apt install usbutils).

usb_reset_device() {
    if [[ $USB_RESET -eq 0 ]]; then
        return
    fi
    if ! command -v usbreset &>/dev/null; then
        echo "# Warning: usbreset not found — skipping device reset" >&2
        echo "#   Install with: sudo apt install usbutils" >&2
        return
    fi
    # Try application PID first, then bootloader PID
    if lsusb -d "$APP_VID_PID" &>/dev/null; then
        echo "# Resetting device ($APP_VID_PID) back to bootloader state..."
        usbreset "$APP_VID_PID" 2>/dev/null || true
        sleep 2
        if lsusb -d "$BOOT_VID_PID" &>/dev/null; then
            echo "# Device reset to bootloader ($BOOT_VID_PID) — ready for next run"
        else
            echo "# Warning: device not found at bootloader PID after reset" >&2
        fi
    elif lsusb -d "$BOOT_VID_PID" &>/dev/null; then
        echo "# Device already in bootloader state ($BOOT_VID_PID)"
    else
        echo "# Warning: device not found at either PID" >&2
    fi
}

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

if ! lsusb -d "$APP_VID_PID" &>/dev/null; then
    echo "Error: device not found at PID 0x00F1 after firmware upload" >&2
    exit 1
fi

echo "# Device ready. Starting soak test..."
echo "# Hours: $HOURS  Seed: ${SEED:-auto}"
if [[ $USB_RESET -eq 1 ]]; then
    echo "# USB reset on exit: enabled (--no-reset to disable)"
fi

# ---- Run soak ----

SOAK_RC=0
"$FX3_CMD" soak "$HOURS" ${SEED:+"$SEED"} || SOAK_RC=$?

# ---- Cleanup: reset device to bootloader state ----

if [[ $SOAK_RC -ne 0 ]]; then
    echo "# Soak test exited with code $SOAK_RC"
fi
usb_reset_device

exit "$SOAK_RC"

#!/usr/bin/env bash
#
# fw_test.sh — Automated firmware test for SDDC_FX3 on RX888mk2 hardware.
#
# Requires:
#   - fx3_cmd (built from tests/Makefile)
#   - rx888_stream (from https://github.com/ringof/rx888_tools)
#   - RX888mk2 connected via USB with firmware loaded
#
# Output: TAP (Test Anything Protocol)
#
# Usage:
#   ./fw_test.sh [--stream-seconds N] [--rx888-stream PATH] [--skip-stream]
#

set -euo pipefail

# ---- Configuration ----

STREAM_SECONDS=5
RX888_STREAM="rx888_stream"
SKIP_STREAM=0
SAMPLE_RATE=64000000
FX3_CMD="$(dirname "$0")/fx3_cmd"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stream-seconds) STREAM_SECONDS="$2"; shift 2 ;;
        --rx888-stream)   RX888_STREAM="$2"; shift 2 ;;
        --skip-stream)    SKIP_STREAM=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--stream-seconds N] [--rx888-stream PATH] [--skip-stream]"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ---- Helpers ----

TEST_NUM=0
PASS_COUNT=0
FAIL_COUNT=0

tap_ok() {
    TEST_NUM=$((TEST_NUM + 1))
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok $TEST_NUM - $1"
}

tap_fail() {
    TEST_NUM=$((TEST_NUM + 1))
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "not ok $TEST_NUM - $1"
    if [[ -n "${2:-}" ]]; then
        echo "#   $2"
    fi
}

tap_skip() {
    TEST_NUM=$((TEST_NUM + 1))
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok $TEST_NUM - $1 # SKIP $2"
}

run_cmd() {
    # Run fx3_cmd, capture output. Returns 0 if PASS, 1 if FAIL.
    local output
    output=$("$FX3_CMD" "$@" 2>&1) || true
    echo "$output"
    [[ "$output" == PASS* ]]
}

# ---- Preflight ----

if [[ ! -x "$FX3_CMD" ]]; then
    echo "Bail out! fx3_cmd not found — run 'make' in tests/ first"
    exit 1
fi

# ---- Test Plan ----

# Count tests: 1 probe + 1 gpio + 1 adc + 2 att + 2 vga + 1 stop
#            + 3 stale commands + optional streaming (3 checks)
PLANNED=11
if [[ $SKIP_STREAM -eq 0 ]]; then
    PLANNED=$((PLANNED + 3))
fi
echo "1..$PLANNED"

# ---- 1. Device probe ----

output=$(run_cmd test) && {
    # Parse hwconfig from output
    hwconfig=$(echo "$output" | grep -oP 'hwconfig=0x\K[0-9A-Fa-f]+')
    if [[ "$hwconfig" == "04" ]]; then
        tap_ok "device probe: RX888r2 detected (hwconfig=0x04)"
    else
        tap_fail "device probe: unexpected hwconfig=0x$hwconfig (expected 0x04)" "$output"
    fi
} || {
    tap_fail "device probe: fx3_cmd test failed" "$output"
}

# ---- 2. GPIO test ----

# Set LEDs on, dither off, randomizer off
output=$(run_cmd gpio 0x1C00) && {
    tap_ok "gpio: set LED_YELLOW|LED_RED|LED_BLUE"
} || {
    tap_fail "gpio: write failed" "$output"
}

# ---- 3. ADC clock ----

output=$(run_cmd adc $SAMPLE_RATE) && {
    tap_ok "adc: set clock to $SAMPLE_RATE Hz"
} || {
    tap_fail "adc: set clock failed" "$output"
}

# ---- 4. Attenuator sweep (spot check min and max) ----

output=$(run_cmd att 0) && {
    tap_ok "att: set 0 (minimum)"
} || {
    tap_fail "att: set 0 failed" "$output"
}

output=$(run_cmd att 63) && {
    tap_ok "att: set 63 (maximum)"
} || {
    tap_fail "att: set 63 failed" "$output"
}

# ---- 5. VGA sweep (spot check min and max) ----

output=$(run_cmd vga 0) && {
    tap_ok "vga: set 0 (minimum)"
} || {
    tap_fail "vga: set 0 failed" "$output"
}

output=$(run_cmd vga 255) && {
    tap_ok "vga: set 255 (maximum)"
} || {
    tap_fail "vga: set 255 failed" "$output"
}

# ---- 6. Stop (ensure clean state before streaming test) ----

output=$(run_cmd stop) && {
    tap_ok "stop: clean state"
} || {
    tap_fail "stop: failed" "$output"
}

# ---- 7. Stale command tests (post-R82xx removal) ----
# These should STALL. The "raw" subcommand treats STALL as PASS.

output=$(run_cmd raw 0xB4) && {
    tap_ok "stale TUNERINIT (0xB4): expected STALL"
} || {
    tap_fail "stale TUNERINIT (0xB4): unexpected result" "$output"
}

output=$(run_cmd raw 0xB5) && {
    tap_ok "stale TUNERTUNE (0xB5): expected STALL"
} || {
    tap_fail "stale TUNERTUNE (0xB5): unexpected result" "$output"
}

output=$(run_cmd raw 0xB8) && {
    tap_ok "stale TUNERSTDBY (0xB8): expected STALL"
} || {
    tap_fail "stale TUNERSTDBY (0xB8): unexpected result" "$output"
}

# ---- 8. Streaming test ----

if [[ $SKIP_STREAM -eq 1 ]]; then
    tap_skip "stream: data capture" "streaming tests skipped"
    tap_skip "stream: byte count" "streaming tests skipped"
    tap_skip "stream: non-zero data" "streaming tests skipped"
else
    if ! command -v "$RX888_STREAM" &>/dev/null; then
        tap_skip "stream: data capture" "rx888_stream not found in PATH"
        tap_skip "stream: byte count" "rx888_stream not found in PATH"
        tap_skip "stream: non-zero data" "rx888_stream not found in PATH"
    else
        TMPFILE=$(mktemp /tmp/fw_test_stream.XXXXXX)
        trap "rm -f '$TMPFILE'" EXIT

        # Run rx888_stream for N seconds, capturing raw samples
        timeout $((STREAM_SECONDS + 5)) \
            "$RX888_STREAM" -s "$SAMPLE_RATE" 2>/dev/null \
            | head -c $((SAMPLE_RATE * 2 * STREAM_SECONDS)) \
            > "$TMPFILE" &
        STREAM_PID=$!

        sleep "$STREAM_SECONDS"
        kill "$STREAM_PID" 2>/dev/null || true
        wait "$STREAM_PID" 2>/dev/null || true

        BYTE_COUNT=$(stat -c%s "$TMPFILE" 2>/dev/null || echo 0)

        # Test 8a: did we get any data at all?
        if [[ "$BYTE_COUNT" -gt 0 ]]; then
            tap_ok "stream: data capture ($BYTE_COUNT bytes in ${STREAM_SECONDS}s)"
        else
            tap_fail "stream: data capture (0 bytes received)"
        fi

        # Test 8b: is the byte count in the right ballpark?
        # Expected: sample_rate * 2 bytes/sample * seconds
        EXPECTED=$((SAMPLE_RATE * 2 * STREAM_SECONDS))
        LOW=$((EXPECTED * 50 / 100))    # 50% of expected (generous lower bound)
        if [[ "$BYTE_COUNT" -ge "$LOW" ]]; then
            PERCENT=$((BYTE_COUNT * 100 / EXPECTED))
            tap_ok "stream: byte count ${PERCENT}% of expected ($BYTE_COUNT / $EXPECTED)"
        else
            tap_fail "stream: byte count too low ($BYTE_COUNT < $LOW expected)"
        fi

        # Test 8c: data is not all zeros
        NONZERO=$(od -An -tx1 -N 4096 "$TMPFILE" | tr -d ' \n0' | wc -c)
        if [[ "$NONZERO" -gt 0 ]]; then
            tap_ok "stream: non-zero data present"
        else
            tap_fail "stream: first 4096 bytes are all zero (ADC not sampling?)"
        fi

        rm -f "$TMPFILE"
    fi
fi

# ---- Summary ----

echo "# $PASS_COUNT passed, $FAIL_COUNT failed out of $TEST_NUM tests"
if [[ $FAIL_COUNT -gt 0 ]]; then
    exit 1
fi
exit 0

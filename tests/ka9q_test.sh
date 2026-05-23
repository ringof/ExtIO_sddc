#!/usr/bin/env bash
#
# ka9q_test.sh — Phase 1 ka9q-radio integration soak for RX888mk2 firmware.
#
# Drives the docker/ka9q-radio container and cycles the radiod session
# (start → stream → stop), correlating captured radiod logs with firmware
# state, and periodically forces a full firmware reload.  This exercises the
# STARTADC / STARTFX3 / STOPFX3 paths through the real host stack and
# confirms the device returns to a usable, ADC-parked idle state between
# sessions (issue #131).
#
# REQUIRES real hardware: an attached RX888mk2 and privileged Docker with
# USB passthrough.  Never runs in CI.  See PLAN-KA9Q-HARNESS.md.
#
# Design notes:
#   * radiod runs inside a persistent, otherwise-idle container (CMD
#     overridden to `sleep infinity`), launched/stopped per cycle via
#     `docker exec`.  This keeps dbus/avahi/wisdom warm across cycles.
#   * START health is judged WITHOUT touching the device from the host:
#     probing with fx3_cmd would steal the interface claim and crash
#     radiod.  We use "radiod process still alive after warmup + no fatal
#     log markers".  The host only talks to the device AFTER radiod stops.
#   * STOP health: radiod gone → device claimable again → GETSTATS shows
#     GPIF idle and a frozen DMA count.
#   * force_reload uses `fx3_cmd reload` (RESETFX3 + usbreset → re-upload →
#     verify), which leaves the device freshly flashed at the app PID.
#
# Usage:
#   ./ka9q_test.sh [options]
# Options (all have env-var equivalents in parentheses):
#   --firmware PATH     firmware image (FIRMWARE; default ../SDDC_FX3/SDDC_FX3.img)
#   --duration SECS     total run time (DURATION; default 3600)
#   --reload-interval SECS  force_reload cadence (RELOAD_INTERVAL; default 420)
#   --stream-secs SECS  radiod run time per cycle (STREAM_SECS; default 10)
#   --image NAME        docker image (IMAGE; default ka9q-radio)
#   --container NAME    docker container (CONTAINER; default ka9q-radio-soak)
#   --keep-container    do not stop/remove the container on exit
#   -h, --help

set -u

# ---- Configuration ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FX3_CMD="$SCRIPT_DIR/fx3_cmd"
FIRMWARE="${FIRMWARE:-$PROJECT_ROOT/SDDC_FX3/SDDC_FX3.img}"
DURATION="${DURATION:-3600}"
RELOAD_INTERVAL="${RELOAD_INTERVAL:-420}"
STREAM_SECS="${STREAM_SECS:-10}"
IMAGE="${IMAGE:-ka9q-radio}"
CONTAINER="${CONTAINER:-ka9q-radio-soak}"
KEEP_CONTAINER=0

# Timing knobs (seconds)
SETTLE=3            # let radiod claim + init before the first liveness check
STOP_TIMEOUT=20     # wait for radiod to exit after SIGINT
FREE_TIMEOUT=10     # wait for the device to become host-claimable after stop

VID=04b4
PID_APP=00f1
PID_BOOT=00f3

RADIOD_CONF=/etc/radio/radiod@rx888-test.conf
LOG_IN=/tmp/ka9q_radiod.log         # logfile path inside the container

# radiod log markers that mean a hard failure.  Best-effort and overridable
# (KA9Q_FATAL_PATTERN) since radiod's exact wording isn't pinned here.
FATAL_PATTERN="${KA9Q_FATAL_PATTERN:-error|cannot|can.t|fatal|No such device|not found|failed|overrun|timeout|LIBUSB|usb_|Unable}"

# ---- Arg parsing ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --firmware)        FIRMWARE="$2"; shift 2 ;;
        --duration)        DURATION="$2"; shift 2 ;;
        --reload-interval) RELOAD_INTERVAL="$2"; shift 2 ;;
        --stream-secs)     STREAM_SECS="$2"; shift 2 ;;
        --image)           IMAGE="$2"; shift 2 ;;
        --container)       CONTAINER="$2"; shift 2 ;;
        --keep-container)  KEEP_CONTAINER=1; shift ;;
        -h|--help)
            sed -n '3,/^set -u/{/^set -u/d;s/^# \?//p}' "$0"; exit 0 ;;
        *) echo "Bail out! unknown option: $1"; exit 2 ;;
    esac
done

# ---- TAP state ----
TEST_NUM=0
PASS_COUNT=0
FAIL_COUNT=0

tap_ok()   { TEST_NUM=$((TEST_NUM+1)); PASS_COUNT=$((PASS_COUNT+1)); echo "ok $TEST_NUM - $1"; }
tap_fail() {
    TEST_NUM=$((TEST_NUM+1)); FAIL_COUNT=$((FAIL_COUNT+1)); echo "not ok $TEST_NUM - $1"
    if [[ -n "${2:-}" ]]; then
        echo "  ---"; echo "$2" | sed 's/^/  /'; echo "  ..."
    fi
}
note() { echo "# $*"; }

# ---- Device / radiod helpers ----
dev_present_app()  { lsusb -d "$VID:$PID_APP"  >/dev/null 2>&1; }
dev_present_boot() { lsusb -d "$VID:$PID_BOOT" >/dev/null 2>&1; }
# Device is "free" (radiod not holding the interface) iff fx3_cmd can claim it.
dev_free()         { "$FX3_CMD" test >/dev/null 2>&1; }
radiod_running()   { docker exec "$CONTAINER" pgrep -x radiod >/dev/null 2>&1; }
radiod_log()       { docker exec "$CONTAINER" cat "$LOG_IN" 2>/dev/null; }

# Parse a decimal field (dma|gpif|pib|...) from `fx3_cmd stats`.
stats_field() {
    local out
    out="$("$FX3_CMD" stats 2>/dev/null)" || return 1
    echo "$out" | sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" | head -1
}

start_radiod() {
    docker exec "$CONTAINER" sh -c "rm -f $LOG_IN; radiod $RADIOD_CONF > $LOG_IN 2>&1" &
    # The above blocks for the radiod lifetime in a background shell job;
    # we control radiod via pkill, then reap the job.  Detached exec
    # (-d) is avoided so a failed `docker exec` surfaces; radiod's own
    # output is redirected to the in-container logfile regardless.
    RADIOD_EXEC_PID=$!
}

stop_radiod() {
    docker exec "$CONTAINER" pkill -INT -x radiod >/dev/null 2>&1 || true
    local waited=0
    while (( waited < STOP_TIMEOUT*2 )); do
        radiod_running || break
        sleep 0.5; waited=$((waited+1))
    done
    if radiod_running; then
        note "radiod did not exit on SIGINT — sending SIGKILL"
        docker exec "$CONTAINER" pkill -KILL -x radiod >/dev/null 2>&1 || true
        sleep 1
    fi
    # Reap the background docker-exec job.
    wait "${RADIOD_EXEC_PID:-}" 2>/dev/null || true
}

wait_dev_free() {
    local waited=0
    while (( waited < FREE_TIMEOUT*2 )); do
        dev_free && return 0
        sleep 0.5; waited=$((waited+1))
    done
    return 1
}

# ---- Cleanup ----
container_started_by_us=0
cleanup() {
    note "cleanup: stopping radiod and parking the device"
    stop_radiod 2>/dev/null || true
    # Leave the device usable and ADC-parked (NOT in DFU): stop streaming
    # and assert SHDN standby.  Mirrors fw_test.sh / soak teardowns.
    if dev_present_app; then
        "$FX3_CMD" stop  >/dev/null 2>&1 || true
        "$FX3_CMD" gpio 0x0820 >/dev/null 2>&1 || true   # LED_BLUE | SHDWN
    fi
    if (( KEEP_CONTAINER == 0 )) && (( container_started_by_us == 1 )); then
        docker stop "$CONTAINER" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

# ---- Preflight ----
if [[ ! -x "$FX3_CMD" ]]; then
    echo "Bail out! fx3_cmd not found at $FX3_CMD — run 'make' in tests/ first"; exit 1
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "Bail out! docker not found on PATH"; exit 1
fi
if ! command -v lsusb >/dev/null 2>&1; then
    echo "Bail out! lsusb not found (install usbutils)"; exit 1
fi
if [[ ! -f "$FIRMWARE" ]]; then
    echo "Bail out! firmware image not found: $FIRMWARE"; exit 1
fi
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Bail out! docker image '$IMAGE' not found — build it:"
    echo "#   docker build -t $IMAGE docker/ka9q-radio/"
    exit 1
fi
if ! dev_present_app && ! dev_present_boot; then
    echo "Bail out! no RX888 on USB (VID $VID, PID $PID_APP/$PID_BOOT)"; exit 1
fi

note "firmware:        $FIRMWARE"
note "duration:        ${DURATION}s   reload every ${RELOAD_INTERVAL}s   stream ${STREAM_SECS}s/cycle"
note "image/container: $IMAGE / $CONTAINER"

# ---- Start the idle container (reuse if already running) ----
if docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    note "reusing running container '$CONTAINER'"
else
    mkdir -p "$PROJECT_ROOT/wisdom"
    note "starting idle container (first run generates FFTW wisdom — may take minutes)"
    if ! docker run --rm -d --name "$CONTAINER" --privileged \
            -v /dev/bus/usb:/dev/bus/usb \
            -v /run/udev:/run/udev:ro \
            -v "$PROJECT_ROOT/SDDC_FX3:/firmware" \
            -v "$PROJECT_ROOT/wisdom:/var/lib/ka9q-radio" \
            --network host \
            -e FFTW_RIGOR="${FFTW_RIGOR:-measure}" \
            "$IMAGE" sleep infinity >/dev/null; then
        echo "Bail out! failed to start container"; exit 1
    fi
    container_started_by_us=1
    # Wait for entrypoint setup (dbus/avahi/wisdom) to settle.
    sleep 3
fi

# ---- Establish a known-good loaded state up front (also tests reload once) ----
note "initial force_reload to establish a clean baseline"
if "$FX3_CMD" -F "$FIRMWARE" reload | tail -1 | grep -q '^PASS'; then
    tap_ok "baseline reload: device re-flashed and healthy"
else
    tap_fail "baseline reload failed" "$("$FX3_CMD" -F "$FIRMWARE" reload 2>&1 | tail -20)"
    echo "1..$TEST_NUM"; exit 1
fi

# ---- Main cycle loop ----
start_ts=$(date +%s)
last_reload_ts=$start_ts
cycle=0

while :; do
    now=$(date +%s)
    (( now - start_ts >= DURATION )) && break
    cycle=$((cycle+1))

    # --- Start radiod ---
    start_radiod
    sleep "$SETTLE"
    if ! radiod_running; then
        tap_fail "cycle $cycle: radiod died during init" "$(radiod_log | tail -25)"
        stop_radiod; wait_dev_free || true
        continue
    fi
    # --- Stream ---
    remain=$((STREAM_SECS - SETTLE)); (( remain < 0 )) && remain=0
    sleep "$remain"
    if ! radiod_running; then
        tap_fail "cycle $cycle: radiod died mid-stream" "$(radiod_log | tail -25)"
        stop_radiod; wait_dev_free || true
        continue
    fi
    # --- Scan log for fatal markers ---
    log="$(radiod_log)"
    if echo "$log" | grep -iqE "$FATAL_PATTERN"; then
        tap_fail "cycle $cycle: fatal marker in radiod log" \
                 "$(echo "$log" | grep -iE "$FATAL_PATTERN" | head -10)"
        stop_radiod; wait_dev_free || true
        continue
    fi

    # --- Stop radiod, verify the device returns idle ---
    stop_radiod
    if radiod_running; then
        tap_fail "cycle $cycle: radiod would not stop" "$(radiod_log | tail -25)"
        continue
    fi
    if ! wait_dev_free; then
        tap_fail "cycle $cycle: device not host-claimable after radiod stop" \
                 "fx3_cmd test still failing ${FREE_TIMEOUT}s after stop"
        continue
    fi
    gpif="$(stats_field gpif)"; dma1="$(stats_field dma)"
    sleep 0.2
    dma2="$(stats_field dma)"
    if [[ -z "$gpif" || -z "$dma1" || -z "$dma2" ]]; then
        tap_fail "cycle $cycle: GETSTATS unreadable after stop" "$("$FX3_CMD" stats 2>&1 | tail -5)"
        continue
    fi
    if [[ "$gpif" != "0" && "$gpif" != "1" && "$gpif" != "255" ]]; then
        tap_fail "cycle $cycle: GPIF not idle after stop (gpif=$gpif)" ""
        continue
    fi
    if [[ "$dma1" != "$dma2" ]]; then
        tap_fail "cycle $cycle: DMA still advancing after stop ($dma1 -> $dma2)" ""
        continue
    fi
    tap_ok "cycle $cycle: radiod start/stream/stop OK; device idle (gpif=$gpif dma=$dma1)"

    # --- Periodic forced firmware reload ---
    now=$(date +%s)
    if (( now - last_reload_ts >= RELOAD_INTERVAL )); then
        if "$FX3_CMD" -F "$FIRMWARE" reload | tail -1 | grep -q '^PASS'; then
            tap_ok "cycle $cycle: force_reload — device re-flashed and healthy"
        else
            tap_fail "cycle $cycle: force_reload failed" \
                     "$("$FX3_CMD" -F "$FIRMWARE" reload 2>&1 | tail -20)"
        fi
        last_reload_ts=$(date +%s)
    fi
done

# ---- Summary ----
elapsed=$(( $(date +%s) - start_ts ))
note "ran ${elapsed}s, $cycle cycles, $PASS_COUNT passed, $FAIL_COUNT failed"
echo "1..$TEST_NUM"
[[ "$FAIL_COUNT" -eq 0 ]]

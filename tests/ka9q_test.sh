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
READY_TIMEOUT="${READY_TIMEOUT:-60}"  # max wait for radiod to stream + register mDNS
                                       # (restart bring-up in rx888_setup can run ~30s; 30s was too tight)
STOP_TIMEOUT=20     # wait for radiod to exit after SIGINT
FREE_TIMEOUT=10     # wait for the device to become host-claimable after stop

VID=04b4
PID_APP=00f1
PID_BOOT=00f3

RADIOD_CONF=/etc/radio/radiod@rx888-test.conf
LOG_IN=/tmp/ka9q_radiod.log         # logfile path inside the container

# Phase 2 data-plane check: use ka9q's `powers` tool to pull a power
# spectrum from radiod (it dynamically creates a spectrum channel via the
# status group — no conf change needed) and assert the spectrum looks like
# LIVE RF, not a dead/frozen ADC.  Runs inside the container (mDNS resolves
# the status group); consumes radiod's status/data multicast only (no USB),
# so it's safe while radiod holds the device.  Auto-skipped if `powers`
# isn't in the image (rebuild to enable).
#
# Live-RF metric (scale-independent): broadband noise keeps MOST bins within
# a dynamic range of the spectrum's own peak; a shut-down/frozen ADC FFTs to
# a lone DC spike with the rest near -inf, so very few bins qualify.  We
# require >= SPEC_MIN_FRAC of bins within SPEC_DYN_RANGE_DB of the max.
DATA_PLANE="${DATA_PLANE:-1}"
SPEC_GROUP="${SPEC_GROUP:-hf.local}"      # status group (rx888-test.conf: status =)
SPEC_FREQ="${SPEC_FREQ:-10000000}"        # center freq Hz (WWV 10 MHz, in-band)
SPEC_BINS="${SPEC_BINS:-256}"
SPEC_BINWIDTH="${SPEC_BINWIDTH:-10000}"   # Hz/bin => 2.56 MHz span at 256 bins
SPEC_INTERVAL="${SPEC_INTERVAL:-2}"       # powers integration time (s)
SPEC_SSRC="${SPEC_SSRC:-30303}"           # arbitrary SSRC for the temp channel
SPEC_DYN_RANGE_DB="${SPEC_DYN_RANGE_DB:-60}"
SPEC_MIN_FRAC="${SPEC_MIN_FRAC:-0.5}"
# Multicast interface to pin powers' join to. In a bridge-networked container
# radiod sends status on lo ("Multicast enabled on loopback interface lo")
# while powers without an iface joins on the default route's interface
# (eth0) -> command never reaches radiod, powers spends 5-7s in
# "Invalid response, length 0" retries, often exhausting timeout -> no
# spectrum. ka9q resolve_mcast takes a "name,iface" suffix; default to lo,
# set SPEC_IFACE= (empty) to skip on hosts where the iface isn't ambiguous.
SPEC_IFACE="${SPEC_IFACE:-lo}"
SPEC_GROUP_FULL="${SPEC_GROUP}${SPEC_IFACE:+,$SPEC_IFACE}"

# Log scanning is ADVISORY by default: the hard pass/fail gates are radiod
# process-liveness and a clean idle device after stop.  radiod emits several
# known-benign lines every run (see docker/ka9q-radio/README.md) — the
# TUNERSTDBY 0xB8 STALL, the FFTW wisdom fallback, and avahi mDNS name
# collisions across cycles — none of which kill streaming.  We subtract
# those, then warn on anything suspicious that remains.  Set KA9Q_STRICT_LOG=1
# to turn a residual match into a cycle failure.  All patterns overridable.
FATAL_PATTERN="${KA9Q_FATAL_PATTERN:-error|cannot|can.t|fatal|No such device|overrun|abort|Segmentation|core dumped}"
BENIGN_PATTERN="${KA9Q_BENIGN_PATTERN:-0xB8|TUNERSTDBY|import_system_wisdom|name collision|Local name collision}"
STRICT_LOG="${KA9Q_STRICT_LOG:-0}"

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

# Pull one power spectrum via `powers` and evaluate the live-RF metric.
# Echoes "BINS LIVE MAX MIN" (LIVE = bins within SPEC_DYN_RANGE_DB of MAX;
# MAX/MIN in dB, *100 as integers to stay shell-friendly), or nothing on
# failure.  powers prints one CSV line: ts,fstart,fstop,binw,bincount,p0,p1...
capture_spectrum() {
    local csv tries=0 perr=/dev/null
    # KA9Q_SPEC_DEBUG=1 surfaces powers' stderr + raw output so a "no spectrum"
    # can be diagnosed (resolve error / invalid responses / timeout).
    [[ -n "${KA9Q_SPEC_DEBUG:-}" ]] && perr=/dev/stderr
    # Retry a few times: even after the status group is registered, the first
    # mDNS resolve / spectrum-channel creation can lag a beat.
    while (( tries < 3 )); do
        if [[ -n "${KA9Q_SPEC_DEBUG:-}" ]]; then
            note "capture try $((tries+1)): powers -f $SPEC_FREQ -b $SPEC_BINS -w $SPEC_BINWIDTH -s $SPEC_SSRC $SPEC_GROUP_FULL" >&2
        fi
        csv="$(docker exec "$CONTAINER" sh -c \
            "timeout $((SPEC_INTERVAL + 8)) powers -c 1 -i $SPEC_INTERVAL \
             -f $SPEC_FREQ -b $SPEC_BINS -w $SPEC_BINWIDTH -s $SPEC_SSRC $SPEC_GROUP_FULL \
             2>$perr" 2>"$perr" | grep -E '^[0-9].*,.*,' | tail -1)"
        [[ -n "$csv" ]] && break
        tries=$((tries+1)); sleep 1
    done
    [[ -z "$csv" ]] && return 1
    echo "$csv" | awk -F, -v dyn="$SPEC_DYN_RANGE_DB" '
        NF >= 6 {
            n = 0; max = -1e18; min = 1e18;
            for (i = 6; i <= NF; i++) {
                v = $i;
                if (v !~ /^-?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?$/) continue;  # skip inf/nan
                val[n++] = v + 0;
                if (v + 0 > max) max = v + 0;
                if (v + 0 < min) min = v + 0;
            }
            if (n == 0) exit 1;
            live = 0;
            for (j = 0; j < n; j++) if (val[j] >= max - dyn) live++;
            printf "%d %d %d %d\n", n, live, int(max*100), int(min*100);
        }'
}

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

# Classify a RUNNING radiod's bring-up state from its log + mDNS. The mDNS
# resolve is the "real" check, not just a log grep: radiod can log
# "Established under name 'hf.local'" yet have the name actually lost to a
# Local name collision, so it never resolves and `powers` gets nothing. We
# require the group to BOTH be logged AND actually resolve.
#   SETUP   - no "rx888 running" yet (still in / wedged in rx888_setup)
#   NO_MDNS - streaming, but $SPEC_GROUP not registered/resolvable
#   READY   - streaming and the status group resolves
radiod_bringup_state() {
    local log="$1"
    grep -q 'rx888 running' <<<"$log" || { echo SETUP; return; }
    (( DATA_PLANE != 1 )) && { echo READY; return; }
    if grep -q "Established under name '${SPEC_GROUP}'" <<<"$log" \
       && docker exec "$CONTAINER" avahi-resolve -n "$SPEC_GROUP" >/dev/null 2>&1; then
        echo READY
    else
        echo NO_MDNS
    fi
}

# Wait until radiod is actually serving. Sets READY_REASON to the final state
# (LAUNCHING|EXITED|SETUP|NO_MDNS|READY) for the caller's diagnostics. Returns
# 0 only on READY. Bails early on EXITED (a crash won't recover by waiting).
# Polls the fresh per-cycle log (start_radiod truncates it).
READY_REASON=""
wait_radiod_ready() {
    local t=0 st seen=0 log
    while (( t < READY_TIMEOUT )); do
        if radiod_running; then
            seen=1
            log="$(radiod_log)"
            st="$(radiod_bringup_state "$log")"
        elif (( seen )); then
            st=EXITED          # was running, now gone -> crashed in bring-up
        else
            st=LAUNCHING       # not spawned yet; keep waiting
        fi
        READY_REASON="$st"
        [[ "$st" == READY  ]] && return 0
        [[ "$st" == EXITED ]] && return 1
        sleep 1; t=$((t+1))
    done
    return 1
}

# Build a human-readable diagnosis of a non-READY radiod for TAP failure
# output, so a failure says WHAT happened instead of just "timed out".
readiness_diag() {
    local reason="$1" pid log markers
    pid="$(docker exec "$CONTAINER" pgrep -x radiod 2>/dev/null | head -1)"
    log="$(radiod_log)"
    case "$reason" in
        LAUNCHING) echo "radiod never started — no process within ${READY_TIMEOUT}s" ;;
        EXITED)    echo "radiod EXITED during bring-up (process gone) — see markers below" ;;
        SETUP)     echo "radiod SETUP-stuck — pid ${pid:-?} alive but no 'rx888 running' after ${READY_TIMEOUT}s (wedged in rx888_setup / USB claim)" ;;
        NO_MDNS)   echo "radiod NO_MDNS — streaming, but status group '${SPEC_GROUP}' not registered/resolvable after ${READY_TIMEOUT}s (avahi name collision?)" ;;
        *)         echo "radiod not ready (${reason})" ;;
    esac
    markers="$(grep -nE 'Error claiming|device setup returned|rx888 running|Established under name|Local name collision|aborted|usb_init' <<<"$log")"
    [[ -n "$markers" ]] && { echo "--- markers ---"; echo "$markers"; }
    echo "--- last 20 log lines ---"
    tail -20 <<<"$log"
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
    # Wait briefly for radiod's mDNS name to be WITHDRAWN before the next
    # cycle. A clean SIGINT triggers radiod's D-Bus disconnect, which makes
    # avahi auto-tear-down that client's entry groups — including
    # $SPEC_GROUP — so this normally clears quickly on its own. No daemon
    # bounce: an earlier "harden the avahi reset" unconditionally killed
    # and respawned avahi-daemon here, which produced zombie daemonize
    # parents (PID 1 = `sleep infinity` doesn't reap) and a respawned
    # daemon that was alive-but-wedged on D-Bus — so the next cycle's
    # avahi-resolve / getaddrinfo both timed out, manifesting as "powers
    # returned no spectrum" despite radiod READY. If the name still
    # resolves, log it and continue rather than tear the daemon down.
    local w=0
    while (( w < 10 )); do
        docker exec "$CONTAINER" avahi-resolve -n "$SPEC_GROUP" >/dev/null 2>&1 || break
        sleep 0.5; w=$((w+1))
    done
    if docker exec "$CONTAINER" avahi-resolve -n "$SPEC_GROUP" >/dev/null 2>&1; then
        note "warning: $SPEC_GROUP still resolves after stop — stale registration persists (not bouncing avahi)"
    fi
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
# On INT/TERM, exit so the loop stops; the EXIT trap runs cleanup exactly
# once. (Trapping cleanup directly on INT let the main loop continue after
# the container was already stopped, spamming "No such container".)
trap cleanup EXIT
trap 'exit 130' INT TERM

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
    note "starting idle container"
    # Bridge network (NOT --network host): keeps ka9q multicast on the
    # container's own lo/eth0 so radiod and powers always agree on the
    # interface (a multi-homed host with --network host can land them on
    # different interfaces; powers has no --iface flag). estimate FFTW rigor
    # keeps radiod's first start fast/deterministic for the soak.
    if ! docker run --rm -d --name "$CONTAINER" --privileged \
            -v /dev/bus/usb:/dev/bus/usb \
            -v /run/udev:/run/udev:ro \
            -v "$PROJECT_ROOT/SDDC_FX3:/firmware" \
            -v "$PROJECT_ROOT/wisdom:/var/lib/ka9q-radio" \
            -e FFTW_RIGOR="${FFTW_RIGOR:-estimate}" \
            "$IMAGE" sleep infinity >/dev/null; then
        echo "Bail out! failed to start container"; exit 1
    fi
    container_started_by_us=1
    # Wait for entrypoint setup (dbus/avahi/wisdom) to settle.
    sleep 3
fi

# ---- Data-plane availability: need `powers` in the image ----
if [[ "$DATA_PLANE" == "1" ]]; then
    if docker exec "$CONTAINER" sh -c 'command -v powers' >/dev/null 2>&1; then
        note "data-plane: powers spectrum @ ${SPEC_FREQ}Hz, ${SPEC_BINS} bins; live-RF needs >= ${SPEC_MIN_FRAC} of bins within ${SPEC_DYN_RANGE_DB}dB of peak"
    else
        note "data-plane: 'powers' not in image — skipping (rebuild to enable)"
        DATA_PLANE=0
    fi
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

    # --- Start radiod and wait until it is actually serving ---
    start_radiod
    if ! wait_radiod_ready; then
        # Only backtrace when actually wedged in setup (SETUP); EXITED/NO_MDNS
        # aren't hangs, so a gdb capture there is noise.
        if [[ -n "${KA9Q_GDB_ON_STALL:-}" && "$READY_REASON" == SETUP ]]; then
            # Capture WHERE the stalled radiod is stuck, before we kill it.
            docker exec "$CONTAINER" sh -c 'command -v gdb >/dev/null 2>&1 || { apt-get update -qq && apt-get install -y gdb >/dev/null 2>&1; }'
            rpid=$(docker exec "$CONTAINER" pgrep -x radiod 2>/dev/null | head -1)
            if [[ -n "$rpid" ]]; then
                snaps="${KA9Q_GDB_SNAPS:-4}"
                echo "# === radiod STALLED (cycle $cycle, pid $rpid) — $snaps snapshots ~2s apart ===" >&2
                for n in $(seq 1 "$snaps"); do
                    echo "# --- snapshot $n/$snaps ---" >&2
                    if [[ "$n" == "1" ]]; then
                        # first snapshot: all threads for context
                        docker exec "$CONTAINER" gdb -p "$rpid" -batch -ex 'thread apply all bt' 2>&1 | sed 's/^/#   /' >&2
                    else
                        # later snapshots: main thread only, to see if it moves
                        docker exec "$CONTAINER" gdb -p "$rpid" -batch -ex 'thread 1' -ex 'bt' 2>&1 | sed 's/^/#   /' >&2
                    fi
                    sleep 2
                done
            else
                echo "# radiod not running at stall (exited, not hung); log tail:" >&2
                radiod_log | tail -20 | sed 's/^/#   /' >&2
            fi
        fi
        tap_fail "cycle $cycle: radiod not ready (${READY_REASON}) within ${READY_TIMEOUT}s" "$(readiness_diag "$READY_REASON")"
        stop_radiod; wait_dev_free || true
        continue
    fi
    # --- Stream window (with spectrum capture if enabled) ---
    spec=""
    if [[ "$DATA_PLANE" == "1" ]]; then
        spec="$(capture_spectrum)"            # blocks ~SPEC_INTERVAL
    fi
    remain=$STREAM_SECS
    (( DATA_PLANE == 1 )) && remain=$((STREAM_SECS - SPEC_INTERVAL))
    (( remain > 0 )) && sleep "$remain"
    if ! radiod_running; then
        tap_fail "cycle $cycle: radiod died mid-stream" "$(radiod_log | tail -25)"
        stop_radiod; wait_dev_free || true
        continue
    fi
    # --- Data-plane assertion: spectrum must look like live RF. ---
    spec_msg=""
    if [[ "$DATA_PLANE" == "1" ]]; then
        if [[ -z "$spec" ]]; then
            # radiod was confirmed READY (running + 'rx888 running' + group
            # resolves) before we got here, so this is a data-plane/resolve
            # problem, NOT radiod being down.
            tap_fail "cycle $cycle: powers returned no spectrum despite radiod READY (data-plane/resolve issue, not radiod-down)" \
                     "$(echo "${SPEC_GROUP} resolves to: $(docker exec "$CONTAINER" avahi-resolve -n "$SPEC_GROUP" 2>&1)"; radiod_log | tail -15)"
            stop_radiod; wait_dev_free || true
            continue
        fi
        read -r s_n s_live s_max s_min <<<"$spec"
        # live fraction = s_live / s_n, compared to SPEC_MIN_FRAC (awk float)
        if ! awk -v live="$s_live" -v n="$s_n" -v f="$SPEC_MIN_FRAC" \
                 'BEGIN { exit !(n > 0 && live >= f * n) }'; then
            tap_fail "cycle $cycle: spectrum not live-RF ($s_live/$s_n bins within ${SPEC_DYN_RANGE_DB}dB of peak; max=$((s_max/100))dB min=$((s_min/100))dB)" \
                     "likely frozen/shut-down ADC (DC spike) or no RF; $(radiod_log | tail -8)"
            stop_radiod; wait_dev_free || true
            continue
        fi
        spec_msg="; spec=${s_live}/${s_n}bins max=$((s_max/100))dB"
    fi
    # --- Advisory log scan: flag suspicious lines, minus known-benign ones.
    # radiod survived warmup (checked above) so it is not a hard failure;
    # only fail the cycle when KA9Q_STRICT_LOG=1. ---
    log="$(radiod_log)"
    residual="$(echo "$log" | grep -iE "$FATAL_PATTERN" | grep -ivE "$BENIGN_PATTERN")"
    if [[ -n "$residual" ]]; then
        if [[ "$STRICT_LOG" == "1" ]]; then
            tap_fail "cycle $cycle: suspicious radiod log (strict)" \
                     "$(echo "$residual" | head -10)"
            stop_radiod; wait_dev_free || true
            continue
        fi
        note "WARN cycle $cycle: suspicious radiod log line(s):"
        echo "$residual" | head -5 | sed 's/^/#   /'
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
    tap_ok "cycle $cycle: radiod start/stream/stop OK${spec_msg}; device idle (gpif=$gpif dma=$dma1)"

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

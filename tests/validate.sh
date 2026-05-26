#!/usr/bin/env bash
#
# validate.sh — end-to-end RX888mk2 firmware validation. Three sequential
# stages against an attached RX888, each releasing the USB device before the
# next, ending in one overall PASS/FAIL. Hardware-only (privileged USB),
# never CI.
#
#   Stage 1  fw_test.sh    vendor-command + data-flow correctness (TAP)
#   Stage 2  soak_test.sh  long-run stability (default 300 s)
#   Stage 3  ka9q-radio    runs under the real host AND produces real output:
#            3A  ka9q_smoke.sh  streams + sane floor + texture + fs/2 alias
#            3B  ka9q_test.sh   kill-and-return (radiod restarts and re-streams)
#                               + clean idle after stop (#131)
#
# Usage:
#   ./validate.sh [options]
#   --firmware PATH    firmware image (default ../SDDC_FX3/SDDC_FX3.img)
#   --stages LIST      comma list of stages to run (default 1,2,3)
#   --soak-secs N      Stage 2 duration in seconds (default 300)
#   --skip-soak        skip Stage 2 (same as dropping 2 from --stages)
#   --container NAME   Stage 3A container (default ka9q-radio)
#   --image NAME       docker image (default ka9q-radio)
#   --keep-going       run remaining stages even after a failure
#   -h, --help
#
# Exit 0 iff every stage that ran passed.

set -u

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)
FX3_CMD="$DIR/fx3_cmd"

FIRMWARE="${FIRMWARE:-$ROOT/SDDC_FX3/SDDC_FX3.img}"
SOAK_SECS="${SOAK_SECS:-300}"
STAGES="1,2,3"
SKIP_SOAK=0
CONTAINER="${CONTAINER:-ka9q-radio}"
IMAGE="${IMAGE:-ka9q-radio}"
KEEP_GOING=0
CYCLE_SECS=12          # Stage 3B per-cycle stream window

usage(){ sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --firmware)  FIRMWARE="$2"; shift 2 ;;
        --stages)    STAGES="$2"; shift 2 ;;
        --soak-secs) SOAK_SECS="$2"; shift 2 ;;
        --skip-soak) SKIP_SOAK=1; shift ;;
        --container) CONTAINER="$2"; shift 2 ;;
        --image)     IMAGE="$2"; shift 2 ;;
        --keep-going) KEEP_GOING=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "validate: unknown option: $1" >&2; exit 2 ;;
    esac
done

want_stage(){ case ",$STAGES," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

RESULTS=()
overall=0
record(){  # $1=label $2=rc
    if [ "$2" = 0 ]; then RESULTS+=("PASS  $1"); echo "## $1: PASS"
    else RESULTS+=("FAIL  $1"); echo "## $1: FAIL"; overall=1; fi
}
summary(){
    echo; echo "===== validate.sh summary ====="
    printf '  %s\n' "${RESULTS[@]}"
    if [ "$overall" = 0 ]; then echo "OVERALL: PASS"; else echo "OVERALL: FAIL"; fi
}
# Stop at the first failure unless --keep-going.
gate(){ [ "$1" = 0 ] || [ "$KEEP_GOING" = 1 ] || { summary; exit 1; }; }

# ---- Preflight ----
[ -f "$FIRMWARE" ] || { echo "Bail out! firmware not found: $FIRMWARE"; exit 1; }
command -v lsusb >/dev/null 2>&1 || { echo "Bail out! lsusb not found (install usbutils)"; exit 1; }
lsusb -d 04b4: 2>/dev/null | grep -q . || { echo "Bail out! no RX888 (VID 04b4) on USB"; exit 1; }
if [ ! -x "$FX3_CMD" ]; then
    echo "validate: building fx3_cmd..."; make -C "$DIR" >/dev/null || { echo "Bail out! make -C tests failed"; exit 1; }
fi

echo "validate: firmware=$FIRMWARE  stages=$STAGES  soak=${SOAK_SECS}s"

# ---- Stage 1: fw_test ----
if want_stage 1; then
    echo "### Stage 1 — fw_test.sh (vendor-command + data-flow correctness)"
    "$DIR/fw_test.sh" --firmware "$FIRMWARE"; rc=$?
    record "Stage 1 (fw_test)" "$rc"; gate "$rc"
fi

# ---- Stage 2: soak ----
if want_stage 2 && [ "$SKIP_SOAK" = 0 ]; then
    hours=$(awk "BEGIN{printf \"%.5f\", $SOAK_SECS/3600}")
    echo "### Stage 2 — soak_test.sh (${SOAK_SECS}s = ${hours}h; full validation uses hours)"
    "$DIR/soak_test.sh" --firmware "$FIRMWARE" --hours "$hours"; rc=$?
    record "Stage 2 (soak)" "$rc"; gate "$rc"
fi

# ---- Stage 3: ka9q-radio ----
if want_stage 3; then
    docker image inspect "$IMAGE" >/dev/null 2>&1 || {
        echo "Bail out! docker image '$IMAGE' not found — build it:"
        echo "    docker build -t $IMAGE docker/ka9q-radio/"; summary; exit 1; }

    # 3A — runs + produces real output
    echo "### Stage 3A — ka9q_smoke.sh (runs under ka9q-radio + real output)"
    "$ROOT/docker/ka9q-radio/ka9q.sh" start >/dev/null 2>&1 || true
    ready=0
    for _ in $(seq 1 30); do
        docker logs "$CONTAINER" 2>&1 | grep -q "rx888 running" && { ready=1; break; }
        sleep 1
    done
    if [ "$ready" = 1 ]; then
        "$DIR/ka9q_smoke.sh" -c "$CONTAINER"; rc=$?
    else
        echo "FAIL: radiod did not reach 'rx888 running' within 30s"; rc=1
    fi
    "$ROOT/docker/ka9q-radio/ka9q.sh" stop >/dev/null 2>&1 || true
    record "Stage 3A (real output)" "$rc"; gate "$rc"

    # 3B — kill-and-return + clean idle (a few short start/stop cycles)
    echo "### Stage 3B — ka9q_test.sh (kill-and-return + clean idle, #131)"
    dur=$(( CYCLE_SECS * 3 + 20 ))
    "$DIR/ka9q_test.sh" --firmware "$FIRMWARE" --duration "$dur" \
        --reload-interval "$(( dur + 100 ))" --stream-secs "$CYCLE_SECS"; rc=$?
    record "Stage 3B (kill-and-return)" "$rc"; gate "$rc"
fi

summary
exit "$overall"

#!/usr/bin/env bash
# tests/bench/run_g0_wspr.sh — Rung G0 (WSPR): off-hardware audio encode/decode.
#
# Mirrors run_g0_ft8.sh for WSPR. Proves the WSPR AUDIO path with no hardware:
# render audio with wsprsimwav, resample to wsprd's 12 kHz input via sox,
# decode with wsprd, assert the known message. Decoding from rendered audio
# (not c2) exercises the same audio->decoder path the bench uses.
#
# Checks:
#   1. FIXTURE (authoritative): each fixtures/wspr/<name>.wav (any rate) is
#      resampled to 12 kHz and decoded; the message in <name>.expected must
#      appear.
#   2. SELF-LOOP (mechanics): wsprsimwav renders G0_MSG -> sox 12 kHz -> wsprd.
#
# Verdict line: "G0 WSPR OK ..." / "G0 WSPR FAIL — ...". Exit 0/1.
#
# Build deps (first run): gfortran + libfftw3-dev (compiles wsprsimwav+wsprd).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/.build"
FIX_DIR="$HERE/fixtures/wspr"
G0_MSG="${G0_MSG:-T1ABC FN20 30}"
SRC="${WSPR_CUI_DIR:-$BUILD/wspr-cui}"
WSPR_CUI_URL="${WSPR_CUI_URL:-https://github.com/jj1bdx/wspr-cui.git}"
WSPR_CUI_SHA="${WSPR_CUI_SHA:-839b86fd9d81a9f1d23ac8b70de6817b8c053305}"

fail() { echo "G0 WSPR FAIL — $*"; exit 1; }

# --- Build wsprsimwav + wsprd (pinned) -----------------------------------
WSWAV="$SRC/wsprd/wsprsimwav"; WD="$SRC/wsprd/wsprd"
if [ ! -x "$WSWAV" ] || [ ! -x "$WD" ]; then
    command -v gfortran >/dev/null || fail "gfortran required (apt install gfortran)"
    if [ ! -d "$SRC" ]; then
        git clone "$WSPR_CUI_URL" "$SRC" >/dev/null 2>&1 || fail "clone wspr-cui failed"
        git -C "$SRC" checkout -q "$WSPR_CUI_SHA" || fail "checkout $WSPR_CUI_SHA failed"
    fi
    make -C "$SRC/wsprd" wsprsimwav wsprd >/dev/null 2>&1 \
        || fail "build failed (need libfftw3-dev)"
fi

WORK="$BUILD/wspr_work"; mkdir -p "$WORK"
OUT="$HERE/out"; mkdir -p "$OUT"
checks=0; fixture_checks=0

# wsprd writes scratch files (hashtable.txt, ALL_WSPR.TXT, ...) into cwd, so
# run it from $WORK and read its stdout for the decode line.
decode() {  # $1 = 12 kHz wav, $2 = expected message
    local wav="$1" expected="$2" out
    out="$(cd "$WORK" && "$WD" -f 14.0956 "$wav" 2>/dev/null || true)"
    grep -Fq -- "$expected" <<<"$out" || { echo "$out" | tail -3; return 1; }
}

to12k() {  # $1 = src wav, echoes path to a 12 kHz S16_LE mono copy
    local src="$1"
    local dst="$WORK/$(basename "${src%.wav}")_12k.wav"
    if [ "$(soxi -r "$src" 2>/dev/null)" = "12000" ]; then
        printf '%s' "$src"
    else
        sox "$src" -r 12000 -c 1 -b 16 -e signed-integer "$dst"
        printf '%s' "$dst"
    fi
}

# --- Check 1: authoritative fixtures -------------------------------------
shopt -s nullglob
for wav in "$FIX_DIR"/*.wav; do
    exp_file="${wav%.wav}.expected"
    [ -f "$exp_file" ] || fail "fixture $(basename "$wav") has no .expected ground-truth file"
    expected="$(head -n1 "$exp_file")"
    decode "$(to12k "$wav")" "$expected" \
        || fail "fixture $(basename "$wav"): expected \"$expected\" not decoded"
    echo "G0: fixture $(basename "$wav") -> decoded \"$expected\""
    checks=$((checks+1)); fixture_checks=$((fixture_checks+1))
done

# --- Check 2: self-loop mechanics ----------------------------------------
# Render to the visible out/ dir so the operator has a real WSPR audio file to
# inspect and independently decode (e.g. `wsprd out/g0_wspr_selfloop_12k.wav`
# with their own WSJT-X). 48 kHz is the QDX TX rate; 12 kHz is wsprd's input.
self48="$OUT/g0_wspr_selfloop_48k.wav"
self12="$OUT/g0_wspr_selfloop_12k.wav"
# NB: wsprsimwav returns a spurious nonzero exit even on success, so validate
# by its output file rather than its exit status.
"$WSWAV" -a -6 -o "$self48" "$G0_MSG" >/dev/null 2>&1 || true
[ -s "$self48" ] || fail "wsprsimwav produced no output for \"$G0_MSG\""
sox "$self48" -r 12000 -c 1 -b 16 -e signed-integer "$self12"
decode "$self12" "$G0_MSG" || fail "self-loop: \"$G0_MSG\" did not round-trip"
echo "G0: self-loop round-trip OK -> \"$G0_MSG\""
echo "G0: WSPR audio written -> $self48 (48 kHz TX)"
echo "G0:                       $self12 (12 kHz; verify yourself: wsprd $self12)"
checks=$((checks+1))

# --- Verdict -------------------------------------------------------------
if [ "$fixture_checks" -eq 0 ]; then
    echo "G0 WSPR OK ($checks check(s), self-loop only — NO authoritative fixture present;" \
         "drop a fixtures/wspr/<name>.wav + .expected to harden this rung)"
else
    echo "G0 WSPR OK ($checks check(s): $fixture_checks fixture + self-loop)"
fi
exit 0

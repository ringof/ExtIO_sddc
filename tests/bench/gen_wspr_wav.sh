#!/usr/bin/env bash
# tests/bench/gen_wspr_wav.sh — generate a known-content WSPR audio WAV.
#
# Uses wspr-cui's `wsprsimwav` (jj1bdx, based on WSJT-X 2.7.1): it does the
# authoritative WSPR protocol encoding AND renders 48 kHz S16_LE mono audio
# with raised-cosine envelope shaping — no hand-rolled DSP. sox is used ONLY
# for rate conversion when a non-native rate is requested.
#
# Usage:  gen_wspr_wav.sh "<CALL GRID DBM>" <out.wav> [rate_hz] [level_dbfs]
# Example: gen_wspr_wav.sh "T1ABC FN20 30" wspr.wav            # 48 kHz (QDX TX rate)
#          gen_wspr_wav.sh "T1ABC FN20 30" wspr12.wav 12000    # 12 kHz (wsprd input)
#
# Build deps (first run): gfortran + libfftw3-dev (to compile wsprsimwav).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${WSPR_CUI_DIR:-$HERE/.build/wspr-cui}"
WSPR_CUI_URL="${WSPR_CUI_URL:-https://github.com/jj1bdx/wspr-cui.git}"
WSPR_CUI_SHA="${WSPR_CUI_SHA:-839b86fd9d81a9f1d23ac8b70de6817b8c053305}"

msg="${1:?usage: gen_wspr_wav.sh \"<CALL GRID DBM>\" <out.wav> [rate_hz] [level_dbfs]}"
out="${2:?usage: gen_wspr_wav.sh \"<CALL GRID DBM>\" <out.wav> [rate_hz] [level_dbfs]}"
rate="${3:-48000}"
level="${4:--6}"

WSWAV="$SRC/wsprd/wsprsimwav"
if [ ! -x "$WSWAV" ]; then
    command -v gfortran >/dev/null \
        || { echo "error: gfortran required to build wsprsimwav (apt install gfortran)" >&2; exit 1; }
    if [ ! -d "$SRC" ]; then
        git clone "$WSPR_CUI_URL" "$SRC" >/dev/null 2>&1 \
            || { echo "error: could not clone wspr-cui ($WSPR_CUI_URL)" >&2; exit 1; }
        git -C "$SRC" checkout -q "$WSPR_CUI_SHA" \
            || { echo "error: could not check out wspr-cui $WSPR_CUI_SHA" >&2; exit 1; }
    fi
    make -C "$SRC/wsprd" wsprsimwav >/dev/null 2>&1 \
        || { echo "error: wsprsimwav build failed (need libfftw3-dev)" >&2; exit 1; }
fi

# NB: wsprsimwav returns a spurious nonzero exit even on success, so validate
# by its output file rather than its exit status.
if [ "$rate" = "48000" ]; then
    "$WSWAV" -a "$level" -o "$out" "$msg" >/dev/null 2>&1 || true
    [ -s "$out" ] || { echo "error: wsprsimwav produced no output" >&2; exit 1; }
else
    tmp="$(mktemp --suffix=.wav)"
    "$WSWAV" -a "$level" -o "$tmp" "$msg" >/dev/null 2>&1 || true
    [ -s "$tmp" ] || { echo "error: wsprsimwav produced no output" >&2; rm -f "$tmp"; exit 1; }
    sox "$tmp" -r "$rate" -c 1 -b 16 -e signed-integer "$out"
    rm -f "$tmp"
fi
echo "wrote $out  (WSPR \"$msg\" @ ${rate} Hz mono, ${level} dBFS)"

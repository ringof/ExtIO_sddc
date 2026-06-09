#!/usr/bin/env bash
# tests/bench/gen_ft8_wav.sh — generate a known-content FT8 WAV with ft8_lib.
#
# Usage:  gen_ft8_wav.sh "<message>" <out.wav> [audio_hz]
# Example: gen_ft8_wav.sh "CQ T1ABC FN20" cq.wav 1500
#
# Output is a 15 s, 12 kHz mono WAV — the format ft8_lib/wsprd expect and the
# format the G0 harness decodes.
#
# NOTE ON INDEPENDENCE: this uses the same encoder family (ft8_lib) as the G0
# decoder. Great for dev/self-loop WAVs. For an *authoritative* regression
# fixture (the one committed under fixtures/ft8/), prefer an INDEPENDENT
# encoder — WSJT-X `ft8sim` — so a shared ft8_lib encode+decode bug cannot mask
# a failure. See tests/bench/README.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="${FT8_LIB_DIR:-$HERE/.build/ft8_lib}"
FT8_LIB_URL="${FT8_LIB_URL:-https://github.com/kgoba/ft8_lib.git}"
FT8_LIB_SHA="${FT8_LIB_SHA:-9fec6ca39886edbf96f4f5e71edc76da5074e871}"

msg="${1:?usage: gen_ft8_wav.sh \"<message>\" <out.wav> [audio_hz]}"
out="${2:?usage: gen_ft8_wav.sh \"<message>\" <out.wav> [audio_hz]}"
hz="${3:-1500}"

# Build ft8_lib once (pinned), reusing tests/bench/.build if G0 already did.
if [ ! -x "$LIB/gen_ft8" ]; then
    if [ ! -d "$LIB" ]; then
        git clone "$FT8_LIB_URL" "$LIB" >/dev/null 2>&1 \
            || { echo "error: could not clone ft8_lib ($FT8_LIB_URL)" >&2; exit 1; }
        git -C "$LIB" checkout -q "$FT8_LIB_SHA" \
            || { echo "error: could not check out ft8_lib $FT8_LIB_SHA" >&2; exit 1; }
    fi
    make -C "$LIB" gen_ft8 >/dev/null 2>&1 || make -C "$LIB" >/dev/null 2>&1 \
        || { echo "error: ft8_lib build failed" >&2; exit 1; }
fi

"$LIB/gen_ft8" "$msg" "$out" "$hz"
echo "wrote $out  (FT8 \"$msg\" @ ${hz} Hz, 15 s / 12 kHz mono)"

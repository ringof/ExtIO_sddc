#!/usr/bin/env bash
# tests/bench/run_g0_ft8.sh — Rung G0 (FT8): off-hardware encode/decode self-test.
#
# Part of the local HITL bench plan (docs/local-hwil-plan.md). G0 is the
# foundation rung: it proves the FT8 encode->decode toolchain and the
# single-line verdict mechanics with NO hardware, NO RF, NO QDX/RX888 — so it
# can run anywhere, including hosted CI (build.yml).
#
# It runs up to two checks and prints exactly one verdict line, then exits
# 0 (pass) or 1 (fail):
#
#   1. FIXTURE check (authoritative): for each committed fixture WAV under
#      fixtures/ft8/<name>.wav with a sibling <name>.expected (the message a
#      reference tool such as WSJT-X encoded into it), decode with ft8_lib and
#      assert the expected message text appears. This is the real regression
#      assertion — ground truth comes from an INDEPENDENT encoder, so a shared
#      ft8_lib encode+decode bug cannot mask a failure.
#
#   2. SELF-LOOP check (mechanics smoke): encode a known message with ft8_lib's
#      gen_ft8, decode it back with decode_ft8, assert round-trip. This proves
#      the harness plumbing even before authoritative fixtures exist, but it is
#      same-tool and therefore NOT a correctness authority on its own.
#
# Verdict line (grep-stable, the contract a `/goal` evaluator reads):
#   "G0 FT8 OK ..."   / "G0 FT8 FAIL ..."
#
# Env overrides:
#   FT8_LIB_DIR  prebuilt ft8_lib checkout (with gen_ft8/decode_ft8). If unset,
#                this script clones+builds it under .build/ft8_lib.
#   G0_MSG       self-loop message (default "CQ T1ABC FN20").
#   FT8_LIB_URL  ft8_lib git URL (default kgoba/ft8_lib).
#   FT8_LIB_SHA  ft8_lib commit to pin (default: the validated SHA below).
#                Pinned for reproducibility, mirroring the ka9q-radio SHA pin
#                in docker/ka9q-radio/Dockerfile. Bump deliberately.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/.build"
FIX_DIR="$HERE/fixtures/ft8"
G0_MSG="${G0_MSG:-CQ T1ABC FN20}"
FT8_LIB_URL="${FT8_LIB_URL:-https://github.com/kgoba/ft8_lib.git}"
FT8_LIB_SHA="${FT8_LIB_SHA:-9fec6ca39886edbf96f4f5e71edc76da5074e871}"

fail() { echo "G0 FT8 FAIL — $*"; exit 1; }

# --- Obtain gen_ft8 / decode_ft8 -----------------------------------------
if [ -n "${FT8_LIB_DIR:-}" ]; then
    LIB="$FT8_LIB_DIR"
else
    LIB="$BUILD/ft8_lib"
    if [ ! -d "$LIB" ]; then
        mkdir -p "$BUILD"
        echo "G0: cloning ft8_lib@$FT8_LIB_SHA -> $LIB"
        git clone "$FT8_LIB_URL" "$LIB" >/dev/null 2>&1 \
            || fail "could not clone ft8_lib ($FT8_LIB_URL) — check network policy"
        git -C "$LIB" checkout -q "$FT8_LIB_SHA" \
            || fail "could not check out pinned ft8_lib SHA $FT8_LIB_SHA"
    fi
fi

GEN="$LIB/gen_ft8"
DEC="$LIB/decode_ft8"
if [ ! -x "$GEN" ] || [ ! -x "$DEC" ]; then
    echo "G0: building ft8_lib in $LIB"
    make -C "$LIB" gen_ft8 decode_ft8 >/dev/null 2>&1 \
        || make -C "$LIB" >/dev/null 2>&1 \
        || fail "ft8_lib build failed"
fi
[ -x "$GEN" ] && [ -x "$DEC" ] || fail "gen_ft8/decode_ft8 missing after build"

mkdir -p "$BUILD/gen"
checks=0
fixture_checks=0

# --- Check 1: authoritative fixtures -------------------------------------
shopt -s nullglob
for wav in "$FIX_DIR"/*.wav; do
    base="${wav%.wav}"
    exp_file="$base.expected"
    [ -f "$exp_file" ] || fail "fixture $(basename "$wav") has no .expected ground-truth file"
    expected="$(head -n1 "$exp_file")"
    out="$("$DEC" "$wav" 2>/dev/null || true)"
    if ! grep -Fq -- "$expected" <<<"$out"; then
        echo "$out" | tail -3
        fail "fixture $(basename "$wav"): expected \"$expected\" not decoded"
    fi
    echo "G0: fixture $(basename "$wav") -> decoded \"$expected\""
    checks=$((checks+1)); fixture_checks=$((fixture_checks+1))
done

# --- Check 2: self-loop mechanics ----------------------------------------
selfwav="$BUILD/gen/selfloop.wav"
"$GEN" "$G0_MSG" "$selfwav" 1500 >/dev/null 2>&1 || fail "gen_ft8 failed for \"$G0_MSG\""
out="$("$DEC" "$selfwav" 2>/dev/null || true)"
grep -Fq -- "$G0_MSG" <<<"$out" || { echo "$out" | tail -3; fail "self-loop: \"$G0_MSG\" did not round-trip"; }
echo "G0: self-loop round-trip OK -> \"$G0_MSG\""
checks=$((checks+1))

# --- Verdict -------------------------------------------------------------
if [ "$fixture_checks" -eq 0 ]; then
    echo "G0 FT8 OK ($checks check(s), self-loop only — NO authoritative fixture present;" \
         "drop a WSJT-X-authored fixtures/ft8/<name>.wav + .expected to harden this rung)"
else
    echo "G0 FT8 OK ($checks check(s): $fixture_checks fixture + self-loop)"
fi
exit 0

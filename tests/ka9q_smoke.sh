#!/usr/bin/env bash
#
# ka9q_smoke.sh — one-shot "is the firmware really running the radio?" check,
# using the whole-band noise-floor spectrum as the proof. No receiver UI, no
# listening: sweep 0 .. fs/2 from radiod via the patched `powers` (through
# hf_sweep.sh, so it's calibrated and edge-artifact-free) and assert it looks
# like a LIVE, textured thermal floor. A dead / frozen / shut-down ADC FFTs to
# a featureless flat line; a genuinely streaming one shows a ~-130 dB floor
# with several dB of natural variance, the ADC DC spike at f=0, and the fs/2
# (32.4 MHz) Nyquist alias spike. Renders a PNG so you can also just look and
# see the whole band.
#
# This is the fast go/no-go counterpart to ka9q_test.sh (which is a multi-cycle
# start/stop soak). Run it after `./docker/ka9q-radio/ka9q.sh start`. A full
# 0 .. fs/2 sweep at the defaults takes a couple of minutes.
#
# Usage: ka9q_smoke.sh [options]
#   -a LO_HZ      band start        (default 0)
#   -z HI_HZ      band stop         (default 32400000 = fs/2, full 1st Nyquist)
#   -i SECS       integration       (default 5)
#   -c CONTAINER  docker container  (default ka9q-radio)
#   -g GROUP      radiod status grp (default hf.local)
#   -I IFACE      multicast iface   (default lo; empty = let kernel pick).
#                                   Forwarded to hf_sweep.sh so powers joins
#                                   on the same interface radiod sends on —
#                                   see docs/docker.md §2.
#   -o FILE       CSV output        (default a temp file)
#
# Exit: 0 = PASS (firmware streaming a live spectrum), 1 = FAIL.
#
#   tests/ka9q_smoke.sh                 # full 0..fs/2 sweep + PNG
#   tests/ka9q_smoke.sh -a 5000000 -z 6000000   # faster sub-band check

set -u
CONTAINER=ka9q-radio; GROUP=hf.local
LO=0; HI=32400000; INT=5; OUT=; SSRC=30303
IFACE="${IFACE:-lo}"

# Liveness thresholds (dB). A live HF floor sits well inside [-150,-100] with
# many dB of bin-to-bin variance; a frozen/zeroed ADC collapses the spread.
MEAN_LO="${MEAN_LO:--150}"; MEAN_HI="${MEAN_HI:--100}"; MIN_SPREAD="${MIN_SPREAD:-5}"

# fs/2 Nyquist-alias gate (RX888-specific, antenna-independent proof the ADC
# is genuinely sampling at fs): require a peak within +/-ALIAS_WINDOW of
# ALIAS_FREQ (fs/2 = 32.4 MHz for 64.8 Msps) at least ALIAS_MIN_DB above the
# floor mean. Auto-skipped if ALIAS_FREQ is outside the swept band; set
# ALIAS_MIN_DB=0 to disable. NOTE: coupled to the firmware's DC->Nyquist
# alias; revisit if a future firmware nulls the ADC DC offset.
ALIAS_FREQ="${ALIAS_FREQ:-32400000}"
ALIAS_WINDOW="${ALIAS_WINDOW:-200000}"
ALIAS_MIN_DB="${ALIAS_MIN_DB:-20}"

while getopts "a:z:i:c:g:I:o:h" opt; do
    case "$opt" in
        a) LO=$OPTARG ;; z) HI=$OPTARG ;; i) INT=$OPTARG ;;
        c) CONTAINER=$OPTARG ;; g) GROUP=$OPTARG ;; I) IFACE=$OPTARG ;;
        o) OUT=$OPTARG ;;
        h) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 2 ;;
    esac
done

DIR=$(cd "$(dirname "$0")" && pwd)
SWEEP="$DIR/hf_sweep.sh"
[ -x "$SWEEP" ] || { echo "FAIL: hf_sweep.sh not found/executable at $SWEEP"; exit 1; }

# Container must be up and able to run a command.
if ! docker exec "$CONTAINER" true 2>/dev/null; then
    echo "FAIL: container '$CONTAINER' is not running."
    echo "      Start it first:  ./docker/ka9q-radio/ka9q.sh start"
    exit 1
fi

OUT=${OUT:-$(mktemp /tmp/ka9q_smoke.XXXXXX.csv)}
echo "ka9q smoke: sweeping $(awk "BEGIN{printf \"%.3f-%.3f\",$LO/1e6,$HI/1e6}") MHz via powers (per-tile progress below)..." >&2
# Keep the sweep's stderr (its per-tile "# tile N ..." progress) visible; only
# the CSV (stdout, via -o) is silenced. The sweep is the slow part (~minutes).
"$SWEEP" -a "$LO" -z "$HI" -i "$INT" -c "$CONTAINER" -g "$GROUP" -I "$IFACE" \
         -s "$SSRC" -o "$OUT" -p >/dev/null || true

# Reduce the spectrum to mean / stdev / min / max.
read -r N MEAN SD MN MX <<EOF
$(awk -F, '!/^#/&&NF==2 { n++; s+=$2; ss+=$2*$2;
      if(n==1||$2<mn)mn=$2; if(n==1||$2>mx)mx=$2 }
   END{ if(n>0){ v=ss/n-(s/n)^2; if(v<0)v=0;
        printf "%d %.2f %.2f %.2f %.2f", n, s/n, sqrt(v), mn, mx }
        else print "0 0 0 0 0" }' "$OUT")
EOF

SPREAD=$(awk -v a="$MX" -v b="$MN" 'BEGIN{printf "%.2f", a-b}')
PNG="${OUT%.csv}.png"; [ "$PNG" = "$OUT" ] && PNG="$OUT.png"

echo "ka9q smoke: bins=$N mean=${MEAN}dB spread=${SPREAD}dB (min=${MN} max=${MX}) sd=${SD}dB"
[ -f "$PNG" ] && echo "ka9q smoke: spectrum plotted -> $PNG"

fail() { echo "FAIL: $1"; exit 1; }

echo "ka9q smoke: [1/4] streaming — spectrum returned ($N bins)"
[ "$N" -gt 0 ] 2>/dev/null || fail "no spectrum returned — is radiod streaming, and is 'powers' in the image?"
echo "ka9q smoke: [2/4] floor mean ${MEAN}dB within sane window [${MEAN_LO},${MEAN_HI}]"
awk -v m="$MEAN" -v lo="$MEAN_LO" -v hi="$MEAN_HI" 'BEGIN{exit !(m>=lo && m<=hi)}' \
    || fail "floor mean ${MEAN}dB outside sane window [${MEAN_LO},${MEAN_HI}] — no real samples?"
echo "ka9q smoke: [3/4] variance — spread ${SPREAD}dB >= ${MIN_SPREAD}dB (not a flat line)"
awk -v s="$SPREAD" -v t="$MIN_SPREAD" 'BEGIN{exit !(s>=t)}' \
    || fail "spectrum is a flat line (spread ${SPREAD}dB < ${MIN_SPREAD}dB) — ADC frozen / not sampling?"

# fs/2 Nyquist-alias gate (only if the alias freq is inside the swept band and enabled).
echo "ka9q smoke: [4/4] fs/2 Nyquist alias near $(awk "BEGIN{printf \"%.3f\",$ALIAS_FREQ/1e6}") MHz"
if awk -v f="$ALIAS_FREQ" -v lo="$LO" -v hi="$HI" -v t="$ALIAS_MIN_DB" 'BEGIN{exit !(t>0 && f>=lo && f<=hi)}'; then
    ALIAS_MAX=$(awk -F, -v lo="$((ALIAS_FREQ-ALIAS_WINDOW))" -v hi="$((ALIAS_FREQ+ALIAS_WINDOW))" \
        '!/^#/&&NF==2 && $1>=lo && $1<=hi { if(n++==0||$2>mx)mx=$2 } END{ if(n>0)printf "%.2f",mx; else print "NA" }' "$OUT")
    [ "$ALIAS_MAX" = "NA" ] && fail "no bins within +/-${ALIAS_WINDOW}Hz of fs/2 (${ALIAS_FREQ}Hz) — can't check the Nyquist alias"
    ALIAS_MARGIN=$(awk -v a="$ALIAS_MAX" -v m="$MEAN" 'BEGIN{printf "%.1f", a-m}')
    echo "ka9q smoke:       alias peak ${ALIAS_MAX}dB, margin ${ALIAS_MARGIN}dB over floor mean (need >= ${ALIAS_MIN_DB})"
    awk -v a="$ALIAS_MAX" -v m="$MEAN" -v t="$ALIAS_MIN_DB" 'BEGIN{exit !((a-m)>=t)}' \
        || fail "fs/2 Nyquist alias absent/weak (margin ${ALIAS_MARGIN}dB < ${ALIAS_MIN_DB}dB) — not a live RX888 ADC sampling at fs?"
else
    echo "ka9q smoke:       skipped (fs/2 not in swept band, or disabled)"
fi

echo "PASS: firmware is streaming a live spectrum (radiod -> rx888.so -> FX3 -> ADC)."
exit 0

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
#   -o FILE       CSV output        (default a temp file)
#
# Exit: 0 = PASS (firmware streaming a live spectrum), 1 = FAIL.
#
#   tests/ka9q_smoke.sh                 # full 0..fs/2 sweep + PNG
#   tests/ka9q_smoke.sh -a 5000000 -z 6000000   # faster sub-band check

set -u
CONTAINER=ka9q-radio; GROUP=hf.local
LO=0; HI=32400000; INT=5; OUT=; SSRC=30303

# Liveness thresholds (dB). A live HF floor sits well inside [-150,-100] with
# many dB of bin-to-bin variance; a frozen/zeroed ADC collapses the spread.
MEAN_LO=-150; MEAN_HI=-100; MIN_SPREAD=5

while getopts "a:z:i:c:g:o:h" opt; do
    case "$opt" in
        a) LO=$OPTARG ;; z) HI=$OPTARG ;; i) INT=$OPTARG ;;
        c) CONTAINER=$OPTARG ;; g) GROUP=$OPTARG ;; o) OUT=$OPTARG ;;
        h) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
echo "ka9q smoke: sweeping $(awk "BEGIN{printf \"%.3f-%.3f\",$LO/1e6,$HI/1e6}") MHz via powers ..." >&2
"$SWEEP" -a "$LO" -z "$HI" -i "$INT" -c "$CONTAINER" -g "$GROUP" -s "$SSRC" \
         -o "$OUT" -p >/dev/null 2>&1 || true

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

[ "$N" -gt 0 ] 2>/dev/null || fail "no spectrum returned — is radiod streaming, and is 'powers' in the image?"
awk -v m="$MEAN" -v lo="$MEAN_LO" -v hi="$MEAN_HI" 'BEGIN{exit !(m>=lo && m<=hi)}' \
    || fail "floor mean ${MEAN}dB outside sane window [${MEAN_LO},${MEAN_HI}] — no real samples?"
awk -v s="$SPREAD" -v t="$MIN_SPREAD" 'BEGIN{exit !(s>=t)}' \
    || fail "spectrum is a flat line (spread ${SPREAD}dB < ${MIN_SPREAD}dB) — ADC frozen / not sampling?"

echo "PASS: firmware is streaming a live spectrum (radiod -> rx888.so -> FX3 -> ADC)."
exit 0

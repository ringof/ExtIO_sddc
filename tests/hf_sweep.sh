#!/usr/bin/env bash
#
# hf_sweep.sh — full-HF power spectrum via ka9q `powers`, emitted as
# `freq_hz,power_dB` CSV (one line per bin) for plotting / post-processing.
#
# Uses a single wide `powers` spectrum channel (DC..~32.4 MHz by default),
# which covers all of HF in one call — no ghost-driving the ka9q-web UI.
# Requires the patched `powers` in the running ka9q-radio container
# (RADIO_FREQUENCY sent as double, RESOLUTION_BW decoded as float — see
# docker/ka9q-radio/patches/). Without those patches the spectrum is
# untuned (half-flat) and the frequency axis is wrong.
#
# Usage: hf_sweep.sh [options]
#   -c CONTAINER  docker container name        (default ka9q-radio)
#   -g GROUP      radiod status group          (default hf.local)
#   -f CENTER_HZ  spectrum center frequency    (default 16200000)
#   -w BIN_HZ     bin width, Hz                 (default 100000)
#   -b BINS       bin count (span = BINS*BIN_HZ)(default 324 -> 32.4 MHz)
#   -s SSRC       temp spectrum SSRC           (default 30303)
#   -i SECS       integration time             (default 5)
#   -o FILE       write CSV to FILE            (default stdout)
#
# Examples:
#   tests/hf_sweep.sh                          # whole band, 100 kHz bins
#   tests/hf_sweep.sh -w 10000 -b 3240         # whole band, 10 kHz bins
#   tests/hf_sweep.sh -o sweep.csv && gnuplot -p -e \
#     "set datafile separator ','; plot 'sweep.csv' using (\$1/1e6):2 with lines"

set -u
CONTAINER=ka9q-radio; GROUP=hf.local; CENTER=16200000
BIN_HZ=100000; BINS=324; SSRC=30303; INT=5; OUT=

while getopts "c:g:f:w:b:s:i:o:h" opt; do
    case "$opt" in
        c) CONTAINER=$OPTARG ;; g) GROUP=$OPTARG ;; f) CENTER=$OPTARG ;;
        w) BIN_HZ=$OPTARG ;;   b) BINS=$OPTARG ;;  s) SSRC=$OPTARG ;;
        i) INT=$OPTARG ;;      o) OUT=$OPTARG ;;
        h) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 2 ;;
    esac
done

# Run powers; keep the CSV data line (leading timestamp digit + many commas),
# ignoring the cosmetic "Invalid response, length 0" first poll.
line="$(docker exec "$CONTAINER" powers -c 1 -i "$INT" -s "$SSRC" \
            -f "$CENTER" -b "$BINS" -w "$BIN_HZ" "$GROUP" 2>/dev/null \
        | grep -E '^[0-9].*,.*,' | tail -1)"

if [ -z "$line" ]; then
    echo "hf_sweep: no spectrum returned by powers (is radiod running, powers patched?)" >&2
    exit 1
fi

# powers CSV: ts, start_hz, stop_hz, bin_hz, bin_count, p0, p1, ... p(n-1)
# Bin i frequency = start_hz + i*bin_hz.
emit() {
    printf '%s\n' "$line" | awk -F', *' '
        NF >= 6 {
            start = $2; bw = $4; n = $5;
            for (i = 0; i < n; i++) printf "%.0f,%s\n", start + i*bw, $(6+i);
        }'
}

if [ -n "$OUT" ]; then
    { echo "# freq_hz,power_dB"; emit; } > "$OUT"
    echo "wrote $OUT ($BINS bins, $(awk "BEGIN{printf \"%.1f\", $BINS*$BIN_HZ/1e6}") MHz span)" >&2
else
    echo "# freq_hz,power_dB"
    emit
fi

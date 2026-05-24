#!/usr/bin/env bash
#
# hf_sweep.sh — power spectrum across a band via ka9q `powers`, emitted as
# `freq_hz,power_dB` CSV (one line per bin) for plotting / post-processing.
#
# Auto-tiles with OVERLAP + TRIM: a single `powers` spectrum channel tops
# out near 16000 bins (the 64 KB status datagram limit), so a wide band at
# fine resolution is covered by stepping `powers` across it. Each tile is
# requested GUARD bins wider on each side (clamped at 0 / Nyquist) and only
# its clean interior [clean_lo, clean_hi) is emitted — so each tile's edge
# bins, which can carry channel-edge / DC-leakage artifacts, are discarded
# and every output sample comes from a tile interior. Adjacent clean
# intervals butt together (half-open), giving contiguous, artifact-free
# coverage. Band-EDGE features are real and remain: the ADC DC offset at
# the low end and the Nyquist roll-up near 32.4 MHz. Sweep a sub-band
# (e.g. -a 100000 -z 30000000) to drop them.
#
# Requires the patched `powers` in the ka9q-radio container (RADIO_FREQUENCY
# double / RESOLUTION_BW float — docker/ka9q-radio/patches/).
#
# Usage: hf_sweep.sh [options]
#   -a START_HZ   band start            (default 0)
#   -z STOP_HZ    band stop             (default 32400000 = RX888 1st Nyquist)
#   -w BIN_HZ     bin width, Hz         (default 100)
#   -m MAXBINS    max bins per tile     (default 16000)
#   -G GUARD      edge bins trimmed/side(default 1000 = 100 kHz @ 100 Hz)
#   -c CONTAINER  docker container      (default ka9q-radio)
#   -g GROUP      radiod status group   (default hf.local)
#   -s SSRC       temp spectrum SSRC    (default 30303)
#   -i SECS       integration per tile  (default 5)
#   -o FILE       write CSV to FILE     (default stdout)
#   -p            also render a PNG plot (gnuplot) next to the CSV
#
#   tests/hf_sweep.sh -o hf100.csv -p          # CSV + hf100.png
#   tests/hf_sweep.sh -a 100000 -z 30000000 -p # usable band, plotted

set -u
START=0; STOP=32400000; BIN_HZ=100; MAXBINS=16000; GUARD=1000
NYQ=32400000
CONTAINER=ka9q-radio; GROUP=hf.local; SSRC=30303; INT=5; OUT=; PLOT=0

while getopts "a:z:w:m:G:c:g:s:i:o:ph" opt; do
    case "$opt" in
        a) START=$OPTARG ;; z) STOP=$OPTARG ;; w) BIN_HZ=$OPTARG ;;
        m) MAXBINS=$OPTARG ;; G) GUARD=$OPTARG ;; c) CONTAINER=$OPTARG ;;
        g) GROUP=$OPTARG ;; s) SSRC=$OPTARG ;; i) INT=$OPTARG ;; o) OUT=$OPTARG ;;
        p) PLOT=1 ;;
        h) sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 2 ;;
    esac
done

# One tile: request [rlo .. rlo+rb*bw], emit only bins whose freq lands in
# [clo, chi). powers returns bins low-to-high with bin 0 at rlo (its base =
# center - (rb/2)*bw == rlo), so bin i freq = rlo + i*bw.
run_tile() {                              # $1=rlo $2=rb $3=clo $4=chi
    local rlo=$1 rb=$2 clo=$3 chi=$4 center out
    center=$(( rlo + (rb/2)*BIN_HZ ))
    out="$(docker exec "$CONTAINER" powers -c 1 -i "$INT" -s "$SSRC" \
              -f "$center" -w "$BIN_HZ" -b "$rb" "$GROUP" 2>/dev/null \
           | grep -E '^[0-9].*,.*,' | tail -1)"
    if [ -z "$out" ]; then
        echo "hf_sweep: WARNING no data for tile rlo=$rlo bins=$rb" >&2
        return 1
    fi
    printf '%s\n' "$out" | awk -F', *' -v rlo="$rlo" -v bw="$BIN_HZ" -v clo="$clo" -v chi="$chi" '
        NF>=6 { n=$5; for(i=0;i<n;i++){ f=rlo+i*bw; if(f>=clo && f<chi) printf "%.0f,%s\n", f, $(6+i) } }'
}

clean=$(( MAXBINS - 2*GUARD ))
if [ "$clean" -le 0 ]; then
    echo "hf_sweep: GUARD ($GUARD) too large for MAXBINS ($MAXBINS)" >&2; exit 2
fi

sweep() {
    echo "# freq_hz,power_dB"
    local clo=$START tile=0 chi rlo rhi rb
    while [ "$clo" -lt "$STOP" ]; do
        tile=$((tile+1))
        chi=$(( clo + clean*BIN_HZ )); [ "$chi" -gt "$STOP" ] && chi=$STOP
        rlo=$(( clo - GUARD*BIN_HZ )); [ "$rlo" -lt 0 ] && rlo=0
        rhi=$(( chi + GUARD*BIN_HZ )); [ "$rhi" -gt "$NYQ" ] && rhi=$NYQ
        rb=$(( (rhi - rlo) / BIN_HZ )); [ "$rb" -gt "$MAXBINS" ] && rb=$MAXBINS
        printf '# tile %d: clean %s MHz (%d-bin req @ %s MHz)\n' "$tile" \
            "$(awk "BEGIN{printf \"%.3f-%.3f\", $clo/1e6, $chi/1e6}")" "$rb" \
            "$(awk "BEGIN{printf \"%.3f\", ($rlo+($rb/2)*$BIN_HZ)/1e6}")" >&2
        run_tile "$rlo" "$rb" "$clo" "$chi"
        clo=$chi
    done
}

# Plotting needs the CSV in a file; use a temp if no -o was given.
if [ "$PLOT" = 1 ] && [ -z "$OUT" ]; then OUT="$(mktemp /tmp/hf_sweep.XXXXXX.csv)"; fi

if [ -n "$OUT" ]; then sweep > "$OUT" && echo "# wrote $OUT" >&2; else sweep; fi

if [ "$PLOT" = 1 ]; then
    if ! command -v gnuplot >/dev/null 2>&1; then
        echo "hf_sweep: gnuplot not found — CSV is at $OUT" >&2; exit 0
    fi
    case "$OUT" in *.csv) PNG="${OUT%.csv}.png" ;; *) PNG="$OUT.png" ;; esac
    gnuplot -e "set datafile separator ','; \
        set terminal pngcairo size 1600,520; set output '$PNG'; \
        set xlabel 'Frequency (MHz)'; set ylabel 'Power (dB)'; set grid; \
        set title 'RX888 sweep: $OUT'; \
        plot '$OUT' using (\$1/1e6):2 with lines lc rgb 'navy' notitle" \
      && echo "# plotted -> $PNG" >&2
fi

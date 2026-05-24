#!/usr/bin/env bash
#
# hf_sweep.sh — power spectrum across a band via ka9q `powers`, emitted as
# `freq_hz,power_dB` CSV (one line per bin) for plotting / post-processing.
#
# Auto-tiles: a single `powers` spectrum channel tops out near 16000 bins
# (the 64 KB status datagram limit), so to cover a wide band at fine
# resolution this steps `powers` across the band in tiles of <= MAXBINS
# bins and concatenates them. One tile when the whole band fits.
#
# Requires the patched `powers` in the running ka9q-radio container
# (RADIO_FREQUENCY sent as double, RESOLUTION_BW decoded as float — see
# docker/ka9q-radio/patches/). Without those the spectrum is untuned and
# the frequency axis is wrong.
#
# Usage: hf_sweep.sh [options]
#   -a START_HZ   band start            (default 0)
#   -z STOP_HZ    band stop             (default 32400000 = RX888 1st Nyquist)
#   -w BIN_HZ     bin width, Hz         (default 100)
#   -m MAXBINS    max bins per tile     (default 16000)
#   -c CONTAINER  docker container      (default ka9q-radio)
#   -g GROUP      radiod status group   (default hf.local)
#   -s SSRC       temp spectrum SSRC    (default 30303)
#   -i SECS       integration per tile  (default 5)
#   -o FILE       write CSV to FILE     (default stdout)
#
# 0..32.4 MHz at 100 Hz = 324000 bins -> ~21 tiles -> ~2 min at -i 5.
#   tests/hf_sweep.sh -o hf100.csv
#   gnuplot -p -e "set datafile separator ','; plot 'hf100.csv' u (\$1/1e6):2 w l"

set -u
START=0; STOP=32400000; BIN_HZ=100; MAXBINS=16000
CONTAINER=ka9q-radio; GROUP=hf.local; SSRC=30303; INT=5; OUT=

while getopts "a:z:w:m:c:g:s:i:o:h" opt; do
    case "$opt" in
        a) START=$OPTARG ;; z) STOP=$OPTARG ;; w) BIN_HZ=$OPTARG ;;
        m) MAXBINS=$OPTARG ;; c) CONTAINER=$OPTARG ;; g) GROUP=$OPTARG ;;
        s) SSRC=$OPTARG ;; i) INT=$OPTARG ;; o) OUT=$OPTARG ;;
        h) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 2 ;;
    esac
done

# One tile: center it so powers' bin 0 lands exactly on tile_start, then
# label bins start+i*bw (powers' base = center - (bins/2)*bw == tile_start).
run_tile() {                                  # $1=tile_start $2=tile_bins
    local ts=$1 nb=$2 center out
    center=$(( ts + (nb/2)*BIN_HZ ))
    out="$(docker exec "$CONTAINER" powers -c 1 -i "$INT" -s "$SSRC" \
              -f "$center" -w "$BIN_HZ" -b "$nb" "$GROUP" 2>/dev/null \
           | grep -E '^[0-9].*,.*,' | tail -1)"
    if [ -z "$out" ]; then
        echo "hf_sweep: WARNING no data for tile at $ts Hz ($nb bins)" >&2
        return 1
    fi
    printf '%s\n' "$out" | awk -F', *' -v ts="$ts" -v bw="$BIN_HZ" '
        NF>=6 { n=$5; for(i=0;i<n;i++) printf "%.0f,%s\n", ts + i*bw, $(6+i) }'
}

total_bins=$(( (STOP - START) / BIN_HZ ))
ntiles=$(( (total_bins + MAXBINS - 1) / MAXBINS ))
echo "# hf_sweep ${START}-${STOP} Hz @ ${BIN_HZ} Hz/bin = ${total_bins} bins in ${ntiles} tile(s), -i ${INT}s each" >&2

sweep() {
    echo "# freq_hz,power_dB"
    local ts=$START tile=0 remaining nb
    while [ "$ts" -lt "$STOP" ]; do
        tile=$((tile+1))
        remaining=$(( (STOP - ts) / BIN_HZ ))
        nb=$(( remaining < MAXBINS ? remaining : MAXBINS ))
        [ "$nb" -le 0 ] && break
        printf '# tile %d/%d: %s MHz (%d bins)\n' "$tile" "$ntiles" \
            "$(awk "BEGIN{printf \"%.3f-%.3f\", $ts/1e6, ($ts+$nb*$BIN_HZ)/1e6}")" "$nb" >&2
        run_tile "$ts" "$nb"
        ts=$(( ts + nb*BIN_HZ ))
    done
}

if [ -n "$OUT" ]; then sweep > "$OUT" && echo "# wrote $OUT" >&2; else sweep; fi

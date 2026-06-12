#!/usr/bin/env bash
#
# vhf_fm_tune.sh — tune the RX888 R828D VHF front-end to bring an FM-broadcast
# station down to the 4.57 MHz IF, where the ka9q-radio WBFM receiver defined in
# docker/ka9q-radio/rx888-vhf-fm.conf is parked.
#
# Runs CONCURRENTLY with radiod (EP0 control vs EP1 bulk). It deliberately does
# NOT pass --adc or --load: radiod owns the ADC sample clock (CLKA) and the
# firmware upload. This wrapper only drives the VHF front-end — VHF_EN, the
# CLKB tuner reference, and the R828D PLL — leaving radiod's stream untouched.
#
# Usage:
#   ./vhf_fm_tune.sh <fm_freq_hz> [extra vhf_tune.py args...]
#   ./vhf_fm_tune.sh 100300000              # 100.3 MHz -> 4.57 MHz IF
#   ./vhf_fm_tune.sh 88500000 --ref 16000000
#
# If it reports that GETSTATS doesn't expose GPIO (older firmware), pass the HF
# GPIO word explicitly, e.g.:  ./vhf_fm_tune.sh 100300000 --base 0x00000
#
# Then listen:  ./docker/ka9q-radio/ka9q.sh monitor fm-pcm.local
# Or retune the receiver within the captured ~8 MHz window:  control hf.local

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <fm_freq_hz> [extra vhf_tune.py args...]" >&2
    echo "  e.g. $0 100300000      # 100.3 MHz FM" >&2
    exit 2
fi

RF_HZ="$1"; shift

echo "Tuning R828D to ${RF_HZ} Hz -> IF 4.570 MHz (ka9q WBFM channel at 4570k)."
echo "Front-end only; radiod keeps streaming on CLKA."
exec python3 "$SCRIPT_DIR/vhf_tune.py" "$RF_HZ" --persist "$@"
